#include <Platform/CodeWriter.h>
#include <Platform/EntryHook.h>
#include <Platform/Islands.h>
#include <Platform/code_patch.h>

#include <cstdio>

using namespace bg3se::mac;

namespace {

bool Report(std::string const& problem, char* why, size_t whyLen)
{
    if (why && whyLen) snprintf(why, whyLen, "%s", problem.c_str());
    return problem.empty();
}

}

extern "C" bool bg3mse_code_write_word(void* at, uint32_t word, char* why, size_t whyLen)
{
    return Report(WriteCodeWord(at, word), why, whyLen);
}


extern "C" void* bg3mse_jump_pad(void* near, void* target, char* why, size_t whyLen)
{
    std::string problem;
    void* pad = MakeJumpPad(near, target, problem);
    Report(problem, why, whyLen);
    return pad;
}

extern "C" void* bg3mse_entry_hook(void* target, size_t functionBytes, void* replacement, void** original, char* why, size_t whyLen)
{
    std::string problem;
    EntryHook* hook = InstallEntryHook(target, functionBytes, replacement, original, problem);
    Report(problem, why, whyLen);
    return hook;
}
