#pragma once

#include <mutex>

// The client's extension state, between the thread that replaces it and the
// main thread.
//
// Lua's cancelable input events are fired where the game's events are taken,
// so that a handler can keep one from the game. In the Mac game that is the
// main thread (Overlay/CocoaInput.cpp). The state they are fired on,
// ecl::ExtensionState, is destroyed and made anew by
// ecl::ScriptExtender::ResetExtensionState: on the client thread when the
// game's state changes, and on whichever thread shuts the client down. An
// event that arrives during that would be handled on a state that is being
// destroyed.
//
// The replacement holds the gate, and so does the handling of each event for
// as long as it uses the state: a replacement waits for the handler that is
// running, and an event that arrives during a replacement does not wait (the
// main thread is the one that presents the game's frames) and is handled as
// when there is no state: Lua is not told, the game gets it.

namespace bg3se::mac::overlay {

class StateGate
{
public:
    using Hold = std::unique_lock<std::recursive_mutex>;

    // Around the replacement of the state. Waits for an event that is being
    // handled on another thread. Not for one on this thread: a handler that
    // ends the game replaces the state it runs on, here as on Windows.
    [[nodiscard]] Hold Replace() { return Hold(mutex_); }
    // Around one event's use of the state. Holds the gate (owns_lock) unless
    // the state is being replaced on another thread right now.
    [[nodiscard]] Hold Use() { return Hold(mutex_, std::try_to_lock); }

private:
    std::recursive_mutex mutex_;
};

// The gate of the client's extension state.
inline StateGate& ClientStateGate()
{
    static StateGate gate;
    return gate;
}

}
