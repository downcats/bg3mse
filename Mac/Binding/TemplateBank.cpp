#include <Binding/TemplateBank.h>

#include <atomic>

#include <Engine/EngineResolver.h>

#include <Platform/Log.h>

namespace bg3se::mac::binding {

namespace {

std::atomic<signed char const*> gBankType{ nullptr };
std::atomic<ThreadLocalDescriptor*> gThreadBankType{ nullptr };

}

int CurrentTemplateBankIndex()
{
    auto bankType = gBankType.load(std::memory_order_acquire);
    auto threadBankType = gThreadBankType.load(std::memory_order_acquire);
    if (!bankType || !threadBankType) return -1;
    // The game reads the byte and extends its sign before it indexes.
    signed char index = *bankType;
    if (index == kBankTypeOfThread) index = *static_cast<signed char const*>(threadBankType->Address(threadBankType));
    return index < 0 ? -1 : index;
}

void SetTemplateBankForTest(signed char const* bankType, ThreadLocalDescriptor* threadBankType)
{
    gBankType.store(bankType, std::memory_order_release);
    gThreadBankType.store(threadBankType, std::memory_order_release);
}

void BindTemplateBank(engine::Resolution const& resolution)
{
    auto bankType = resolution.Find(kTemplateBankEntryNames[0]);
    auto threadBankType = resolution.Find(kTemplateBankEntryNames[1]);
    void* global = bankType && bankType->Usable() ? resolution.Address(*bankType) : nullptr;
    void* descriptor = threadBankType && threadBankType->Usable() ? resolution.Address(*threadBankType) : nullptr;
    if (!global || !descriptor) {
        auto const* refused = !global ? bankType : threadBankType;
        Log(LogLevel::Warning, "the current template bank is not known: %s is not resolved%s%s. GetGlobalTemplateBank gives null",
            !global ? "g_GlobalTemplateBankType" : "tls_CurrentBankType", refused ? ": " : "", refused ? refused->Reason.c_str() : "");
        SetTemplateBankForTest(nullptr, nullptr);
        return;
    }
    SetTemplateBankForTest(static_cast<signed char const*>(global), static_cast<ThreadLocalDescriptor*>(descriptor));
}

}
