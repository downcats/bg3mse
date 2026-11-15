#include <Binding/Prototype.h>

#include <cstdio>

namespace bg3se::mac::binding {

namespace {

std::string Where(engine::Place at, uint16_t stackOffset)
{
    if (at != engine::Place::Stack) return engine::Describe(at);
    char text[32];
    snprintf(text, sizeof(text), "[sp+0x%x]", stackOffset);
    return text;
}

int FindSlot(Prototype const& p, engine::Place at, uint16_t stackOffset)
{
    for (int i = 0; i < p.Count; i++) {
        if (p.Args[i].At == at && (at != engine::Place::Stack || p.Args[i].StackOffset == stackOffset)) return i;
    }
    return -1;
}

std::string ResultDisagreement(Prototype const& p, engine::Signature const& game, Through use, unsigned allow)
{
    using engine::Returns;
    // A result written to memory whose address is an ordinary argument is no
    // result in a register.
    Returns produced = game.Result == Returns::IndirectX0 ? Returns::Nothing : game.Result;
    if (p.Result == produced) return {};
    if (p.Result == Returns::Nothing) {
        if (use == Through::Call && produced != Returns::IndirectX8) return {};
        return std::string("the game function returns a result (") + engine::Describe(game.Result) + ") that this prototype would drop";
    }
    if (produced == Returns::Nothing) {
        if (use == Through::Hook && (allow & kResultNotProduced) && (p.Result == Returns::W0 || p.Result == Returns::X0)) return {};
        return std::string("this prototype expects a result (") + engine::Describe(p.Result) + ") and the game function produces none";
    }
    return std::string("this prototype expects its result ") + engine::Describe(p.Result) + ", the game function returns "
        + engine::Describe(game.Result);
}

}

std::string Disagreement(Prototype const& p, engine::Signature const& game, Through use, unsigned allow)
{
    if (!p.Present) return "there is no prototype to compare";
    if (!p.Known) return "the prototype has a parameter or a result whose passing is not known";

    bool taken[engine::kMaxArguments]{};
    auto expect = [&](engine::Place at, uint16_t stackOffset, uint8_t bytes, bool address, int norbyte, bool unread) -> std::string {
        int i = FindSlot(p, at, stackOffset);
        if (i < 0) {
            if (unread) return {};
            return "the game function takes an argument in " + Where(at, stackOffset) + " that this prototype does not pass";
        }
        taken[i] = true;
        if (unread) return {};
        if (p.Args[i].Bytes != bytes) {
            char text[128];
            snprintf(text, sizeof(text), "%s: the game function takes %u byte(s) there, this prototype passes %u",
                Where(at, stackOffset).c_str(), bytes, p.Args[i].Bytes);
            return text;
        }
        // Eight bytes of a value and a pointer to the value travel alike and
        // are not the same thing to pass.
        bool handedOn = address && !p.Args[i].Address && use == Through::Hook && (allow & kPointersAsWords);
        if (p.Args[i].Address != address && !handedOn) {
            return Where(at, stackOffset) + (address
                ? ": the game function takes a pointer there, this prototype passes a value"
                : ": the game function takes a value there, this prototype passes a pointer");
        }
        if (!(allow & kOwnPrototype) && norbyte != i) {
            char text[160];
            if (norbyte < 0) {
                snprintf(text, sizeof(text), "%s: the game function takes something there that Norbyte's prototype has no parameter for",
                    Where(at, stackOffset).c_str());
            } else {
                snprintf(text, sizeof(text), "%s: the game function takes Norbyte's parameter %d there, his prototype passes parameter %d",
                    Where(at, stackOffset).c_str(), norbyte + 1, i + 1);
            }
            return text;
        }
        return {};
    };

    if (game.Result == engine::Returns::IndirectX0) {
        // The address of the result is the first ordinary argument, and the
        // first parameter of Norbyte's prototype.
        if (auto problem = expect(engine::Place::X0, 0, 8, true, 0, false); !problem.empty()) return problem;
    }
    for (auto const& arg : game.Arguments()) {
        uint8_t bytes = arg.How == engine::Passing::Value ? arg.Bytes : 8;
        if (auto problem = expect(arg.At, arg.StackOffset, bytes, arg.How == engine::Passing::Pointer, arg.Norbyte, arg.How == engine::Passing::Unread);
            !problem.empty()) return problem;
    }
    bool allowed = use == Through::Hook && (allow & kParametersNotTaken);
    bool untaken = false;
    for (int i = 0; i < p.Count; i++) {
        if (taken[i]) {
            // Only after everything the function takes.
            if (untaken) return "this prototype passes a parameter the game function does not take before one it takes";
            continue;
        }
        untaken = true;
        if (!allowed || p.Args[i].At == engine::Place::Stack) {
            return "this prototype passes a parameter in " + Where(p.Args[i].At, p.Args[i].StackOffset) + " that the game function does not take";
        }
    }
    return ResultDisagreement(p, game, use, allow);
}

std::string Describe(Prototype const& p)
{
    if (!p.Present) return "(none)";
    if (!p.Known) return "(not known)";
    std::string text = "(";
    for (int i = 0; i < p.Count; i++) {
        char part[48];
        snprintf(part, sizeof(part), "%s%s %s%u", i ? ", " : "", Where(p.Args[i].At, p.Args[i].StackOffset).c_str(),
            p.Args[i].Address ? "address " : "", p.Args[i].Bytes);
        text += part;
    }
    text += ") -> ";
    text += engine::Describe(p.Result);
    return text;
}

}
