#pragma once

#include <Platform/FaultRecord.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// The crash report file, as Norbyte's CrashDataWriter writes it
// (Extender/Shared/CrashReporter.cpp): the signature 'CRSH', then sections,
// each a four-character tag, the length of its body as 32 bits, and the
// body. Tags and lengths are little-endian 32-bit values, a tag being the
// value MSVC gives the character constant ('CRSH' is 0x43525348, so the file
// starts with the bytes "HSRC").
//
// Sections Norbyte's reporter writes, with the same tags and bodies here:
//
//   VERS  the format's version, 1, as 32 bits
//   SVER  the extender's version string         GVER  the game's, "a.b.c.d"
//   SDAT  the build date                        CHAN  the release channel
//   BLDT  "Debug", "Devel" or "Release"         CERT  the certification id
//   TRAC  the backtrace as text, one frame a line: at most 13 frames, and
//         after the thirteenth a line "(...)", which Norbyte's loop writes
//         whether or not there were more
//   LUAT  the Lua backtrace of the faulting thread's state
//   MODS  the loaded mods, one a line: uuid, name, version
//   ERRL  the warnings and errors logged so far
//   CSTA, SSTA  the client's and server's game state    SLEN  milliseconds since the extender started
//
// His MDMP section is a Windows minidump. This platform has no minidump
// writer; what a developer needs from one is in the stack file (below) and
// in sections of its own, each text, one "key=value" record a line (in MBTR and MIMG, several keys on a
// line, the last running to the end of the line):
//
//   MFLT  the fault: cause ("fault", "c++ exception left a guarded block" or
//         "std::terminate"), what Norbyte's rule makes of the crash
//         (extender_related: yes or no; norbytes_rule: "reports", or why his
//         reporter would leave it to the game's) and the count crash
//         reporting was disabled by, kind, origin, signal, Mach exception and
//         codes, faulting address, pc, thread, the thread's stack bounds, and
//         the stack file written with the report (see below): its name, the
//         address and length of the memory in it and its flags (B, C, U, S
//         for StackBoundsKnown, StackCutAtLimit, StackEndsUnreadable and
//         StackStartsLater, "-" for each that is not set), or stack_file=none;
//         for the two causes that are a C++ exception, last, the exception's
//         type (exception=, "none" when std::terminate was called without
//         one) and, for a std::exception, exception_what=
//   MREG  the faulting thread's registers: x0..x28, fp, lr, sp, pc, cpsr, and
//         far and esr when the fault was seen as a signal; q0..q31 (128 bits
//         each), fpsr and fpcr when they are the fault's, which they are for a
//         fault caught as a Mach exception on a thread that is not the
//         reporting one
//   MBTR  the whole backtrace: frame index, pc, the base of the image it is
//         in, flags (L: from the link register; M: in the main executable;
//         E: in the extender's image), and the nearest symbol with the offset
//         from it, or "-"
//   MIMG  every loaded image: base, slide, size of __TEXT, LC_UUID, flags
//         (M, E as above) and path. pc - slide is the address in the file;
//         the UUID says which build of the file.
//   MSYS  the system and the process: OS version and build, machine model,
//         the executable, the identifier and version of the bundle it is in,
//         the CPU, its cores (all, performance, efficiency), the memory in
//         bytes, and the process id
//   MBLD  bg3mse's build (Release/Version.h): version, commit,
//         extender_version, channel, and the UUID of the image the reporter is
//         in, which is its dSYM's
//   MMAP  the memory map: for the faulting address, pc, lr, sp, fp and each
//         of x0..x28 that points into mapped memory, a line "where=<which>
//         value=" with the region it is in (bounds, protection now/most,
//         sharing mode, the system's tag for it, the image whose __TEXT it
//         is), or region=none and the next region; then the address space by
//         tag, "summary tag= regions= virtual= resident=" (bytes), and the
//         "total"
//   MTHR  every thread of the process (Crash/ThreadList.h), last: a line
//         "threads total= listed= max_frames=", then for each thread a line
//         "thread=<n> id= port= role= captured= run_state= suspend_count=
//         frames= name=" (role: other, crashed, reporter or fault-handling;
//         only "other" threads are suspended and read, the crashed one being
//         MREG's and MBTR's), "queue thread=<n> address= label=" when it runs
//         for a dispatch queue, "registers thread=<n> x0= .. x28= fp= lr= sp=
//         pc= cpsr=" and its frames, "frame thread=<n> index= pc= image=
//         flags= symbol=", as MBTR's
//
// The backtrace text has, after Norbyte's parts, "Threads (<total>):" and
// each thread's name, queue and id with its first frames, as TRAC's are.
// Everything here is for a thread that must not allocate or take a lock (a
// guard filter or a fatal-fault observer, Platform/FaultGuard.h): text is put
// together in buffers the caller owns, and the file is written with open,
// write, close and rename.

namespace bg3se::mac::crash {

constexpr uint32_t Tag(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a << 24 | (uint32_t)(uint8_t)b << 16 | (uint32_t)(uint8_t)c << 8 | (uint32_t)(uint8_t)d;
}

namespace tag {
constexpr uint32_t Signature = Tag('C', 'R', 'S', 'H');
constexpr uint32_t Header = Tag('V', 'E', 'R', 'S');
constexpr uint32_t ExtenderVersion = Tag('S', 'V', 'E', 'R');
constexpr uint32_t GameVersion = Tag('G', 'V', 'E', 'R');
constexpr uint32_t BuildDate = Tag('S', 'D', 'A', 'T');
constexpr uint32_t Certification = Tag('C', 'E', 'R', 'T');
constexpr uint32_t ReleaseChannel = Tag('C', 'H', 'A', 'N');
constexpr uint32_t BuildType = Tag('B', 'L', 'D', 'T');
constexpr uint32_t StackTrace = Tag('T', 'R', 'A', 'C');
constexpr uint32_t LuaStackTrace = Tag('L', 'U', 'A', 'T');
constexpr uint32_t LoadedModules = Tag('M', 'O', 'D', 'S');
constexpr uint32_t ErrorLog = Tag('E', 'R', 'R', 'L');
constexpr uint32_t ServerState = Tag('S', 'S', 'T', 'A');
constexpr uint32_t ClientState = Tag('C', 'S', 'T', 'A');
constexpr uint32_t SessionLength = Tag('S', 'L', 'E', 'N');

constexpr uint32_t MacFault = Tag('M', 'F', 'L', 'T');
constexpr uint32_t MacRegisters = Tag('M', 'R', 'E', 'G');
constexpr uint32_t MacBacktrace = Tag('M', 'B', 'T', 'R');
constexpr uint32_t MacImages = Tag('M', 'I', 'M', 'G');
constexpr uint32_t MacSystem = Tag('M', 'S', 'Y', 'S');
constexpr uint32_t MacBuild = Tag('M', 'B', 'L', 'D');
constexpr uint32_t MacMemory = Tag('M', 'M', 'A', 'P');
constexpr uint32_t MacThreads = Tag('M', 'T', 'H', 'R');
}

constexpr uint32_t kReportVersion = 1;

// Text put together in a buffer of fixed size. What does not fit is dropped,
// and Truncated() says so.
class TextBuffer
{
public:
    TextBuffer(char* data, size_t capacity) : data_(data), capacity_(capacity) {}
    template <size_t N>
    explicit TextBuffer(char (&data)[N]) : TextBuffer(data, N) {}

    void Clear();
    void Append(std::string_view text);
    void Appendf(char const* fmt, ...) __attribute__((format(printf, 2, 3)));

    std::string_view View() const { return { data_, size_ }; }
    bool Empty() const { return size_ == 0; }
    bool Truncated() const { return truncated_; }

private:
    char* data_;
    size_t capacity_;
    size_t size_{ 0 };
    bool truncated_{ false };
};

// --- report files -------------------------------------------------------------
//
// A report is three files in the reports' directory, named for the time of
// the crash: crash_<time>.crpt, the report, crash_<time>.bt, the backtrace as
// text, and crash_<time>.stack, the faulting thread's stack memory (when it
// could be written). The directory keeps the newest kReportsKept reports.

inline constexpr unsigned kReportsKept = 10;
inline constexpr char kReportPrefix[] = "crash_";
inline constexpr char kReportSuffix[] = ".crpt";
inline constexpr char kBacktraceSuffix[] = ".bt";
inline constexpr char kStackSuffix[] = ".stack";
// The length of a report's time, "2026-11-10_21-30-05", without its end.
inline constexpr size_t kReportTimeLength = 19;

// The time in a report's name: the calendar time `utcOffset` seconds east of
// `time` (seconds since 1970, UTC). Written to `out` with its end. It asks
// the system nothing, so it may be called while another thread is stopped.
void FormatReportTime(int64_t time, long utcOffset, char out[kReportTimeLength + 1]);

// Removes the oldest reports of `directory`, each with all its files, until
// at most `keep` are left; a file that is no report's is left alone. Not for
// the time of a crash: it allocates.
void PruneReports(char const* directory, unsigned keep);

// Writes a report file. Nothing is buffered: a section is in the file when
// AddSection returns.
class ReportWriter
{
public:
    // Creates the file, replacing one of that name; a symbolic link there is
    // not followed. False when it cannot be created.
    bool Open(char const* path);
    // The signature and the VERS section.
    void AddHeader();
    void AddSection(uint32_t tag, std::string_view body);
    // A section written in parts: its head, then Write as often as needed,
    // then EndSection, which writes its length into the head. One at a time.
    void BeginSection(uint32_t tag);
    void EndSection();
    // The bytes as they are, for a file that is not made of sections.
    void Write(std::string_view bytes);
    // The bytes at `offset` from the file's start, over what is there.
    void WriteAt(uint64_t offset, std::string_view bytes);
    // False when the file was not open or anything failed to be written.
    bool Close();

private:
    int fd_{ -1 };
    bool failed_{ false };
    // The offset of the open section's head; -1 when none is open.
    int64_t section_{ -1 };
};

// --- the stack file -----------------------------------------------------------
//
// What Norbyte's minidump would give of the crashing thread beyond its
// registers: the memory of its stack, from just below the stack pointer up
// to the top of the stack, so that the values on it (locals, saved
// registers, the return addresses of MBTR) can be looked at. No core dump of
// the process is written. The file is a header and the memory after it:
//
//   - the memory starts kStackRedZone bytes below the faulting stack
//     pointer, where a function that calls nothing may keep its locals, and
//     ends at the top of the stack when the record knows the stack's bounds
//     and the stack pointer is in them or less than 64 KiB below them (in the
//     guard of a stack that overflowed); otherwise it ends
//     kStackLimit bytes later. Never more than kStackLimit bytes;
//   - it is read a page at a time with checked reads (Platform/FaultCapture.h),
//     which fail instead of faulting. Pages that cannot be read at its start
//     (the guard below a stack that overflowed) are left out, and the memory
//     begins at the first that can; a page that cannot be read after that
//     ends it. Base and Length say what is in the file.
//
// An address A of the thread's stack, as a frame pointer or a pointer to a
// local in MREG or a frame record, is at file offset HeaderSize + (A - Base)
// when Base <= A < Base + Length.

inline constexpr uint64_t kStackLimit = 1 << 20;
inline constexpr uint64_t kStackRedZone = 128;
inline constexpr uint32_t kStackSignature = Tag('M', 'S', 'T', 'K');
inline constexpr uint32_t kStackVersion = 1;

enum StackFileFlags : uint32_t
{
    // StackLow and StackHigh are the thread's stack, and the memory was to
    // run to StackHigh.
    StackBoundsKnown = 1,
    // The memory was cut at kStackLimit bytes.
    StackCutAtLimit = 2,
    // The memory ends at a page that could not be read.
    StackEndsUnreadable = 4,
    // Pages at its start could not be read and are left out.
    StackStartsLater = 8,
};

// Little-endian, as written; HeaderSize is its size, for later versions to grow.
struct StackFileHeader
{
    uint32_t Signature;             // kStackSignature: the bytes "KTSM"
    uint32_t Version;               // kStackVersion
    uint32_t HeaderSize;            // sizeof(StackFileHeader)
    uint32_t Flags;                 // StackFileFlags
    uint64_t ThreadId;              // as MFLT's thread_id
    uint32_t ThreadPort;            // as MFLT's thread_port
    uint32_t Reserved;
    uint64_t StackPointer;          // sp at the fault, as MREG's
    uint64_t Base;                  // the address of the first byte after the header
    uint64_t Length;                // the bytes of memory after the header
    uint64_t StackLow, StackHigh;   // the thread's stack, 0 and 0 when not known
};
static_assert(sizeof(StackFileHeader) == 72);

// Writes the stack file of `fault` into `file`, which is open and empty, and
// leaves its header in `header`. False when no memory could be read: the
// file then holds a header with a Length of 0. Reads and writes only, so it
// may be called while the faulting thread is stopped.
bool WriteStackFile(ReportWriter& file, FaultRecord const& fault, StackFileHeader& header);

}
