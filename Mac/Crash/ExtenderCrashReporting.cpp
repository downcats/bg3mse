// Norbyte's extender as the crash reporter's host: what
// Extender/Shared/CrashReporter.cpp takes from the extender, handed to the
// Mac reporter (Crash/CrashReporter.h), and the two functions the extender
// starts and stops crash reporting with. Compiled as Norbyte's sources are.
//
// What the reporter calls at the time of a crash is everything here but
// ReportDirectory, which it calls once, when it is prepared. It runs on one
// of bg3mse's exception threads while the faulting thread is stopped, or,
// for a C++ exception leaving a guarded block, on the throwing thread. So
// none of it allocates, takes a lock or calls a function of the game's: text
// is formatted into the reporter's buffers, where Norbyte builds
// std::strings, and what he reads through the game's string table is taken
// another way (ModuleList).

#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Server/ExtensionStateServer.h>
#include <Extender/Shared/ExtensionState.h>
#include <Extender/Shared/API.h>
#include <Extender/Version.h>
#include <Osiris/Shared/NodeHooks.h>
#include <resource.h>

#include <Crash/CrashReporter.h>
#include <Platform/Config.h>

BEGIN_SE()

extern char const* BuildDate;

namespace {

using mac::FaultRecord;
using mac::crash::ReportWriter;
using mac::crash::TextBuffer;
namespace tag = mac::crash::tag;

bool ForceCrashReporting()
{
    return gExtender && gExtender->GetConfig().ForceCrashReporting;
}

// CrashReporter::ShouldReportCrashes, the other way round: one atomic read.
bool NativeApiUsed()
{
    return gExtender && gExtender->GetExternalAPI().WasActivated();
}

// Where CrashReporter::GetMiniDumpPath gives a file in the temporary
// directory, for one report that the next replaces: here reports are kept, in
// a directory of bg3mse's own (Crash/CrashReporter.h).
size_t ReportDirectory(char* out, size_t size)
{
    std::string path = mac::DataDirectory() + "/Crashes";
    if (path.size() >= size) return 0;
    memcpy(out, path.c_str(), path.size() + 1);
    return path.size();
}

// ExcludedSymbols: "Wrapper frames are included in the stack trace for any
// server/client worker thread crash, so we have to filter them to make sure
// that we don't receive a report for every (unrelated) game crash."
size_t ExcludedFunctions(void const** out, size_t max)
{
    void const* const functions[] = {
        reinterpret_cast<void const*>(&NodeVMTWrapper::s_WrappedIsValid),
        reinterpret_cast<void const*>(&NodeVMTWrapper::s_WrappedPushDownTuple),
        reinterpret_cast<void const*>(&NodeVMTWrapper::s_WrappedPushDownTupleDelete),
        reinterpret_cast<void const*>(&NodeVMTWrapper::s_WrappedInsertTuple),
        reinterpret_cast<void const*>(&NodeVMTWrapper::s_WrappedDeleteTuple),
        reinterpret_cast<void const*>(&NodeVMTWrapper::s_WrappedCallQuery),
        reinterpret_cast<void const*>(&CustomFunctionInjector::StaticCallWrapper),
        reinterpret_cast<void const*>(&CustomFunctionInjector::StaticQueryWrapper),
        reinterpret_cast<void const*>(&OsirisWrappers::CallWrapper),
        reinterpret_cast<void const*>(&OsirisWrappers::QueryWrapper),
        reinterpret_cast<void const*>(&OsirisWrappers::ErrorWrapper),
        reinterpret_cast<void const*>(&OsirisWrappers::AssertWrapper),
    };
    size_t count = std::min(std::size(functions), max);
    for (size_t i = 0; i < count; i++) out[i] = functions[i];
    return count;
}

// The head of CrashReporter::CreateCrashReportFile.
void BuildSections(ReportWriter& report)
{
    report.AddSection(tag::ExtenderVersion, RES_DLL_VERSION_STRING);

    if (gExtender) {
        auto const& ver = gExtender->GetGameVersion();
        char gameVer[64];
        int length = snprintf(gameVer, sizeof(gameVer), "%u.%u.%u.%u", (unsigned)ver.Major, (unsigned)ver.Minor,
            (unsigned)ver.Revision, (unsigned)ver.Build);
        report.AddSection(tag::GameVersion, std::string_view(gameVer, (size_t)length));
    }
    report.AddSection(tag::BuildDate, BuildDate);
    report.AddSection(tag::ReleaseChannel, SE_BUILD_CHANNEL);

#if !defined(NDEBUG)
    report.AddSection(tag::BuildType, "Debug");
#elif defined(SE_IS_DEVELOPER_BUILD)
    report.AddSection(tag::BuildType, "Devel");
#else
    report.AddSection(tag::BuildType, "Release");
#endif

#if defined(SE_CERTIFICATION_ID)
    report.AddSection(tag::Certification, SE_CERTIFICATION_ID);
#endif
}

// BacktraceDumper::DumpLuaBacktrace(ExtensionStateBase&).
void DumpLuaBacktrace(ExtensionStateBase& state, TextBuffer& text)
{
    auto lua = state.GetLua();
    if (!lua || !lua->GetState()) return;

    auto L = lua->GetState();

    // Every frame, as Norbyte's loop gives them: its stop at
    // MaxLuaDisplayBacktraceLines is never reached (its counter is tested
    // twice a frame, and the test that would stop sees only odd counts). The
    // trace goes into a buffer of fixed size, which nothing allocates at the
    // time of a crash: what does not fit in it is left out.
    lua_Debug ar;
    for (int i = 0; !text.Truncated(); i++) {
        memset(&ar, 0, sizeof(ar));
        if (lua_getstack(L, i, &ar) == 0) {
            break;
        }

        // Norbyte asks for "fnuSl". The f is left out: it only pushes the
        // function onto the Lua stack of the stopped thread, one slot a
        // frame and never popped, growing that stack (an allocation, while
        // the stopped thread may hold the allocator's lock) when it is full;
        // nothing reads it, and the text is the same without it.
        lua_getinfo(L, "nuSl", &ar);

        if (ar.source) {
            if (ar.source[0] == '=') {
                text.Append("[C++ code]");
            } else {
                text.Append(ar.source);
            }
        } else {
            text.Append("(unknown)");
        }

        if (ar.currentline > 0) {
            text.Appendf(":%d", ar.currentline);
        }

        if (ar.name) {
            text.Append(" ");
            text.Append(ar.name);
        }

        text.Append("\n");
    }
}

// BacktraceDumper::DumpLuaBacktrace(): the state whose thread crashed.
// GetOwningThread is that thread's GetCurrentThreadId, which here is its
// Mach port, as FaultRecord::Thread is.
void LuaBacktrace(FaultRecord const& fault, TextBuffer& text)
{
    if (!gExtender) return;

    if (gExtender->GetServer().HasExtensionState() && gExtender->GetServer().GetExtensionState().GetOwningThread() == fault.Thread) {
        DumpLuaBacktrace(gExtender->GetServer().GetExtensionState(), text);
        return;
    }

    if (gExtender->GetClient().HasExtensionState() && gExtender->GetClient().GetExtensionState().GetOwningThread() == fault.Thread) {
        DumpLuaBacktrace(gExtender->GetClient().GetExtensionState(), text);
    }
}

// BacktraceDumper::DumpModuleList.
//
// Norbyte writes each module's UUID from ModuleUUIDString, a FixedString
// whose characters come from the game's string table through a call of the
// game's (ls::gst::Get). That function is written for the game's threads: on
// a thread's first call it sets up a thread-local variable of the game's,
// which allocates, and takes one of the game's thread slots for good. Here
// the UUID is taken from the Guid beside the string, ModuleUUID, of which
// the string is the text: Guid::ToString writes it into a buffer and calls
// nothing. (The build checks that this file refers to none of
// FixedStringBase's functions: Mac/Tools/CheckCrashHost.cmake.)
void ModuleList(TextBuffer& text)
{
    auto modManager = GetStaticSymbols().GetModManagerClient();
    if (!modManager) return;

    for (auto const& mod : modManager->LoadOrderedModules) {
        char uuid[Guid::StringLength];
        mod.Info.ModuleUUID.ToString(uuid);
        text.Append(std::string_view(uuid, sizeof(uuid)));
        text.Append("\t");
        text.Append(std::string_view(mod.Info.Name.data(), mod.Info.Name.size()));
        text.Appendf("\t%u.%u.%u.%u\r\n", (unsigned)mod.Info.ModVersion.Major(), (unsigned)mod.Info.ModVersion.Minor(),
            (unsigned)mod.Info.ModVersion.Revision(), (unsigned)mod.Info.ModVersion.Build());
    }
}

// The tail of CrashReporter::CreateCrashReportFile.
void StateSections(ReportWriter& report)
{
    auto clientState = GetStaticSymbols().GetClientState();
    if (clientState) {
        auto state = *clientState;
        report.AddSection(tag::ClientState, std::string_view((char*)&state, sizeof(state)));
    }

    auto serverState = GetStaticSymbols().GetServerState();
    if (serverState) {
        auto state = *serverState;
        report.AddSection(tag::ServerState, std::string_view((char*)&state, sizeof(state)));
    }

    auto startTime = gExtender ? gExtender->GetStartTime() : 0;
    if (startTime) {
        auto runtime = GetTickCount64() - startTime;
        report.AddSection(tag::SessionLength, std::string_view((char*)&runtime, sizeof(runtime)));
    }
}

// CrashReporter::OnLogMessage.
void OnLogMessage(DebugMessageType type, char const* msg)
{
    if (!mac::crash::Started()) return;
    if (type != DebugMessageType::Warning && type != DebugMessageType::Error) return;

    mac::crash::NoteLogMessage(msg);
}

}

// CrashReporter::Initialize. What it does beyond this has no counterpart
// here: its own two functions need no registering (the reporter does not run
// on the crashing thread's stack), the terminate handler is the reporter's
// (Crash/CrashReporter.h), and the symbol tables it loads are in the images
// (or, for a stripped release, in its dSYM files: Mac/Release).
//
// Norbyte's code calls this at the start of its start-up and again at every
// change of the game's state, to be the process's top-level handler again
// after the game set its own. The Mac reporter is started each time for the
// same reason: Start puts its handler in front of the one the game has
// installed since (Crash/CrashReporter.h).
void InitCrashReporting()
{
    if (!mac::crash::Start()) return;

    if (gCoreLibPlatformInterface.GlobalConsole) {
        gCoreLibPlatformInterface.GlobalConsole->SetLogCallback(&OnLogMessage);
    }
}

// CrashReporter::Shutdown.
void ShutdownCrashReporting()
{
    if (!mac::crash::Started()) return;

    if (gCoreLibPlatformInterface.GlobalConsole) {
        gCoreLibPlatformInterface.GlobalConsole->SetLogCallback(nullptr);
    }

    mac::crash::Stop();
}

END_SE()

constinit bg3se::mac::crash::Host const bg3se::mac::crash::gHost{
    .ForceCrashReporting = &bg3se::ForceCrashReporting,
    .NativeApiUsed = &bg3se::NativeApiUsed,
    .ReportDirectory = &bg3se::ReportDirectory,
    .ExcludedFunctions = &bg3se::ExcludedFunctions,
    .BuildSections = &bg3se::BuildSections,
    .LuaBacktrace = &bg3se::LuaBacktrace,
    .ModuleList = &bg3se::ModuleList,
    .StateSections = &bg3se::StateSections,
};

// Ext.Debug.Crash in a release build (Mac/Shim/windows.h).
bool bg3se::mac::DebugCrashAllowed()
{
    return bg3se::gExtender && bg3se::gExtender->GetConfig().DeveloperMode && FeatureEnabled("debug_crash");
}
