#include <Platform/FaultCapture.h>
#include <Platform/macho_symbols.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <signal.h>

// The same declaration <os/trace_base.h> makes: the linker defines it as the
// Mach-O header of the image being linked.
extern "C" struct mach_header __dso_handle;

namespace bg3se::mac {

char const* FaultKindName(FaultKind kind)
{
    switch (kind) {
    case FaultKind::BadAccess: return "bad access";
    case FaultKind::StackOverflow: return "stack overflow";
    case FaultKind::IllegalInstruction: return "illegal instruction";
    case FaultKind::Arithmetic: return "arithmetic trap";
    case FaultKind::Breakpoint: return "breakpoint trap";
    case FaultKind::Abort: return "abort";
    }
    return "?";
}

uint64_t ThisImageBase()
{
    return reinterpret_cast<uint64_t>(&__dso_handle);
}

bool ReadMemory(uint64_t address, void* out, size_t size)
{
    vm_size_t got = 0;
    kern_return_t kr = vm_read_overwrite(mach_task_self(), (vm_address_t)address, size, (vm_address_t)out, &got);
    return kr == KERN_SUCCESS && got == size;
}

namespace {

// The __TEXT segment's link-time address and size from load commands that
// are readable in place.
bool TextSegment(mach_header_64 const* mh, uint64_t& vmaddr, uint64_t& vmsize)
{
    auto p = reinterpret_cast<uint8_t const*>(mh + 1);
    auto end = p + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        auto lc = reinterpret_cast<load_command const*>(p);
        if (p + sizeof(*lc) > end || lc->cmdsize < sizeof(*lc) || p + lc->cmdsize > end) return false;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64)) {
            auto seg = reinterpret_cast<segment_command_64 const*>(lc);
            if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0) {
                vmaddr = seg->vmaddr;
                vmsize = seg->vmsize;
                return true;
            }
        }
        p += lc->cmdsize;
    }
    return false;
}

// Reads one image's header and load commands with checked reads.
bool ReadImage(uint64_t base, char const* path, ImageTable::Image& image)
{
    alignas(8) uint8_t buffer[32 * 1024];
    auto mh = reinterpret_cast<mach_header_64*>(buffer);
    if (!ReadMemory(base, mh, sizeof(*mh)) || mh->magic != MH_MAGIC_64) return false;
    if (mh->sizeofcmds > sizeof(buffer) - sizeof(*mh)) return false;
    if (!ReadMemory(base + sizeof(*mh), mh + 1, mh->sizeofcmds)) return false;
    uint64_t vmaddr = 0, vmsize = 0;
    if (!TextSegment(mh, vmaddr, vmsize) || vmsize == 0) return false;
    image = { base, base + vmsize, path, mh->filetype == MH_EXECUTE };
    return true;
}

struct ImageList
{
    uint64_t Array;
    uint32_t Count;
    uint64_t Dyld;                  // dyld itself, which the array does not list
    char const* DyldPath;
};

bool ReadImageList(uint64_t infos, ImageList& list)
{
    dyld_all_image_infos all;
    constexpr size_t needed = offsetof(dyld_all_image_infos, dyldPath) + sizeof(all.dyldPath);
    if (!ReadMemory(infos, &all, offsetof(dyld_all_image_infos, infoArray) + sizeof(all.infoArray))) return false;
    list = { reinterpret_cast<uint64_t>(all.infoArray), all.infoArrayCount, 0, nullptr };
    // dyldPath is there from version 15 on (macOS 10.15 and later).
    if (all.version >= 15 && ReadMemory(infos, &all, needed)) {
        list.Dyld = reinterpret_cast<uint64_t>(all.dyldImageLoadAddress);
        list.DyldPath = all.dyldPath;
    }
    return true;
}

}

bool ImageTable::Read()
{
    count_ = 0;
    task_dyld_info_data_t info{};
    mach_msg_type_number_t infoCount = TASK_DYLD_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&info), &infoCount) != KERN_SUCCESS) {
        return false;
    }

    // dyld sets the array to null while it changes it; a list read before and
    // after the copy that is the same was not changed under it.
    for (int attempt = 0; attempt < 100; attempt++) {
        ImageList before{};
        if (!ReadImageList(info.all_image_info_addr, before)) return false;
        if (!before.Array) {
            sched_yield();
            continue;
        }
        count_ = 0;
        dyld_image_info batch[64];
        bool readAll = true;
        for (uint32_t i = 0; i < before.Count && count_ < Capacity; i += 64) {
            uint32_t n = before.Count - i < 64 ? before.Count - i : 64;
            if (!ReadMemory(before.Array + i * sizeof(dyld_image_info), batch, n * sizeof(dyld_image_info))) {
                readAll = false;
                break;
            }
            for (uint32_t j = 0; j < n && count_ < Capacity; j++) {
                if (ReadImage(reinterpret_cast<uint64_t>(batch[j].imageLoadAddress), batch[j].imageFilePath, images_[count_])) {
                    count_++;
                }
            }
        }
        ImageList after{};
        if (readAll && ReadImageList(info.all_image_info_addr, after) && after.Array == before.Array && after.Count == before.Count) {
            if (before.Dyld && count_ < Capacity && ReadImage(before.Dyld, before.DyldPath, images_[count_])) count_++;
            return true;
        }
        sched_yield();
    }
    count_ = 0;
    return false;
}

ImageTable::Image const* ImageTable::Find(uint64_t address) const
{
    for (size_t i = 0; i < count_; i++) {
        if (address >= images_[i].Base && address < images_[i].End) return &images_[i];
    }
    return nullptr;
}

namespace {

// Where the kernel's conversion of a Mach exception to a signal sends it
// (EXC_BAD_ACCESS: SIGSEGV for an unmapped address, SIGBUS otherwise).
int SignalOf(uint32_t exception, int64_t code)
{
    switch (exception) {
    case EXC_BAD_ACCESS: return code == KERN_INVALID_ADDRESS ? SIGSEGV : SIGBUS;
    case EXC_BAD_INSTRUCTION: return SIGILL;
    case EXC_ARITHMETIC: return SIGFPE;
    case EXC_BREAKPOINT: return SIGTRAP;
    }
    return 0;
}

FaultKind KindOfException(uint32_t exception)
{
    switch (exception) {
    case EXC_BAD_INSTRUCTION: return FaultKind::IllegalInstruction;
    case EXC_ARITHMETIC: return FaultKind::Arithmetic;
    case EXC_BREAKPOINT: return FaultKind::Breakpoint;
    default: return FaultKind::BadAccess;
    }
}

FaultKind KindOfSignal(int signal)
{
    switch (signal) {
    case SIGILL: return FaultKind::IllegalInstruction;
    case SIGFPE: return FaultKind::Arithmetic;
    case SIGTRAP: return FaultKind::Breakpoint;
    case SIGABRT: return FaultKind::Abort;
    default: return FaultKind::BadAccess;
    }
}

// The guard region of a thread's stack is below its lowest address; a fault
// there with the stack pointer near the bottom is the stack running out.
constexpr uint64_t kGuardReach = 1024 * 1024;
constexpr uint64_t kStackPointerSlack = 64 * 1024;

void Backtrace(FaultSource const& source, FaultRecord& record)
{
    record.FrameCount = WalkFrames(*source.State, source.StackLow, source.StackHigh, record.Frames, FaultRecord::MaxFrames);
}

}

uint32_t WalkFrames(arm_thread_state64_t const& state, uint64_t stackLow, uint64_t stackHigh, FaultFrame* out, uint32_t max)
{
    uint32_t count = 0;
    auto add = [&](uint64_t pc, uint8_t flags) {
        if (count < max) out[count++] = { pc, 0, nullptr, flags };
    };
    uint64_t pc = arm_thread_state64_get_pc(state);
    uint64_t lr = arm_thread_state64_get_lr(state);
    uint64_t fp = arm_thread_state64_get_fp(state);
    add(pc, 0);

    uint64_t frameRecord[2]{};          // saved fp, return address
    bool haveFirst = fp && fp % 8 == 0 && ReadMemory(fp, frameRecord, sizeof(frameRecord));
    if (lr && (!haveFirst || lr != frameRecord[1])) add(lr, FrameFromLinkRegister);
    if (!haveFirst) return count;

    while (count < max) {
        if (frameRecord[1] == 0) break;
        add(frameRecord[1], 0);
        uint64_t next = frameRecord[0];
        // Callers' frames are at higher addresses; anything else ends the chain.
        if (next <= fp || next % 8 != 0) break;
        if (stackHigh && fp >= stackLow && fp < stackHigh && next >= stackHigh) break;
        fp = next;
        if (!ReadMemory(fp, frameRecord, sizeof(frameRecord))) break;
    }
    return count;
}

void FaultCapture::Capture(FaultSource const& source, FaultRecord& out)
{
    memset(&out, 0, sizeof(out));
    out.Origin = source.Origin;
    out.MachException = source.MachException;
    out.Registers = *source.State;
    out.PC = arm_thread_state64_get_pc(*source.State);
    out.ThreadId = source.ThreadId;
    out.Thread = source.Thread;
    out.StackLow = source.StackLow;
    out.StackHigh = source.StackHigh;

    if (source.MachException) {
        out.Kind = KindOfException(source.MachException);
        out.Signal = SignalOf(source.MachException, source.Code[0]);
        out.Code[0] = source.Code[0];
        out.Code[1] = source.Code[1];
        out.Address = out.Kind == FaultKind::BadAccess ? (uint64_t)source.Code[1] : out.PC;
    } else {
        out.Kind = KindOfSignal(source.Signal);
        out.Signal = source.Signal;
        out.Code[0] = source.SignalCode;
        out.Address = source.Signal == SIGSEGV || source.Signal == SIGBUS ? source.SignalAddress : out.PC;
    }
    if (source.ExceptionState) {
        out.HasExceptionState = true;
        out.ExceptionState = *source.ExceptionState;
    }

    uint64_t sp = arm_thread_state64_get_sp(*source.State);
    if (out.Kind == FaultKind::BadAccess && source.StackLow && out.Address < source.StackLow &&
        out.Address + kGuardReach >= source.StackLow && sp < source.StackLow + kStackPointerSlack) {
        out.Kind = FaultKind::StackOverflow;
    }

    Backtrace(source, out);

    images_.Read();
    uint64_t self = ThisImageBase();
    for (uint32_t i = 0; i < out.FrameCount; i++) {
        auto& frame = out.Frames[i];
        if (auto image = images_.Find(frame.PC)) {
            frame.ImageBase = image->Base;
            frame.ImagePath = image->Path;
            if (image->MainExecutable) frame.Flags |= FrameInMainExecutable;
            if (image->Base == self) frame.Flags |= FrameInThisImage;
        }
    }
}

bool SymbolizeFrame(FaultFrame const& frame, FrameSymbol& symbol)
{
    if (!frame.ImageBase) return false;
    auto mh = reinterpret_cast<mach_header_64 const*>(frame.ImageBase);
    if (mh->magic != MH_MAGIC_64) return false;
    uint64_t textAddr = 0, textSize = 0;
    if (!TextSegment(mh, textAddr, textSize)) return false;
    intptr_t slide = (intptr_t)(frame.ImageBase - textAddr);
    MachoSymtab st;
    if (!macho_symtab_from_image(mh, slide, &st)) return false;

    uint64_t address = frame.PC - (uint64_t)slide;
    if (address < textAddr || address >= textAddr + textSize) return false;
    auto symbols = static_cast<struct nlist_64 const*>(st.symbols);
    struct nlist_64 const* best = nullptr;
    for (uint32_t i = 0; i < st.nsyms; i++) {
        auto const& sym = symbols[i];
        if ((sym.n_type & N_STAB) || (sym.n_type & N_TYPE) != N_SECT) continue;
        if (sym.n_value > address || sym.n_value < textAddr) continue;
        if (sym.n_un.n_strx == 0 || sym.n_un.n_strx >= st.strsize) continue;
        if (!best || sym.n_value > best->n_value) best = &sym;
    }
    if (!best) return false;
    symbol = { st.strings + best->n_un.n_strx, address - best->n_value };
    return true;
}

size_t SymbolizeFrames(FaultFrame const* frames, uint64_t const* lookup, uint32_t count, FrameSymbol* symbols, bool* found,
    uint32_t* order, void const** best, uint64_t deadline)
{
    auto at = [&](uint32_t i) { return lookup ? lookup[i] : frames[i].PC; };
    uint32_t candidates = 0;
    for (uint32_t i = 0; i < count; i++) {
        found[i] = false;
        symbols[i] = {};
        best[i] = nullptr;
        if (frames[i].ImageBase) order[candidates++] = i;
    }
    // By image, and by address within an image: in an image's run, the
    // frames that a symbol can name are those from the first whose address
    // is not below it.
    std::sort(order, order + candidates, [&](uint32_t a, uint32_t b) {
        return frames[a].ImageBase != frames[b].ImageBase ? frames[a].ImageBase < frames[b].ImageBase : at(a) < at(b);
    });

    size_t named = 0;
    for (uint32_t first = 0; first < candidates;) {
        uint64_t base = frames[order[first]].ImageBase;
        uint32_t end = first;
        while (end < candidates && frames[order[end]].ImageBase == base) end++;
        uint32_t run = first;
        first = end;
        if (deadline && mach_absolute_time() > deadline) break;

        auto mh = reinterpret_cast<mach_header_64 const*>(base);
        if (mh->magic != MH_MAGIC_64) continue;
        uint64_t textAddr = 0, textSize = 0;
        if (!TextSegment(mh, textAddr, textSize)) continue;
        intptr_t slide = (intptr_t)(base - textAddr);
        MachoSymtab st;
        if (!macho_symtab_from_image(mh, slide, &st)) continue;

        // SymbolizeFrame's choice for each frame: the symbol with the
        // greatest value not above its address, the first in the table among
        // equals. Each symbol is offered to the first frame of the run it can
        // name; what that frame keeps is then carried up the run, since every
        // later frame's address is at least as high.
        auto address = [&](uint32_t k) { return at(order[k]) - (uint64_t)slide; };
        auto symbolsTable = static_cast<struct nlist_64 const*>(st.symbols);
        for (uint32_t i = 0; i < st.nsyms; i++) {
            auto const& sym = symbolsTable[i];
            if ((sym.n_type & N_STAB) || (sym.n_type & N_TYPE) != N_SECT) continue;
            if (sym.n_value < textAddr) continue;
            if (sym.n_un.n_strx == 0 || sym.n_un.n_strx >= st.strsize) continue;
            uint32_t lo = run, hi = end;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (address(mid) < sym.n_value) lo = mid + 1; else hi = mid;
            }
            if (lo == end) continue;
            auto& kept = best[order[lo]];
            if (!kept || sym.n_value > static_cast<struct nlist_64 const*>(kept)->n_value) kept = &sym;
        }
        for (uint32_t k = run + 1; k < end; k++) {
            auto before = static_cast<struct nlist_64 const*>(best[order[k - 1]]);
            auto& kept = best[order[k]];
            if (before && (!kept || before->n_value > static_cast<struct nlist_64 const*>(kept)->n_value)) kept = before;
        }
        for (uint32_t k = run; k < end; k++) {
            uint32_t i = order[k];
            auto sym = static_cast<struct nlist_64 const*>(best[i]);
            uint64_t looked = address(k);
            if (!sym || looked < textAddr || looked >= textAddr + textSize) continue;
            symbols[i] = { st.strings + sym->n_un.n_strx, frames[i].PC - (uint64_t)slide - sym->n_value };
            found[i] = true;
            named++;
        }
    }
    return named;
}

}
