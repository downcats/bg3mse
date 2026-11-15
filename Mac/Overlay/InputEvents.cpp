#include <Overlay/InputEvents.h>

#include <Overlay/KeyCodes.h>

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace bg3se::mac::overlay {

namespace {

// The length of the UTF-8 sequence that starts with `lead`, or 0.
size_t SequenceLength(uint8_t lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xe0) == 0xc0) return 2;
    if ((lead & 0xf0) == 0xe0) return 3;
    if ((lead & 0xf8) == 0xf0) return 4;
    return 0;
}

uint32_t CodePoint(uint8_t const* at, size_t length)
{
    if (length == 1) return at[0];
    uint32_t value = at[0] & (0xff >> (length + 1));
    for (size_t i = 1; i < length; i++) value = (value << 6) | (at[i] & 0x3f);
    return value;
}

bool IsText(uint32_t codePoint)
{
    if (codePoint < 0x20 || codePoint == 0x7f) return false;
    if (codePoint >= 0xf700 && codePoint <= 0xf8ff) return false;      // NSUpArrowFunctionKey ... : keys, not characters
    return true;
}

// The ImGui wheel amounts of a scroll: lines of a wheel as they are, pixels
// of a trackpad as hundredths of them.
void WheelAmounts(CocoaEvent const& event, float& x, float& y)
{
    float scale = event.PreciseScroll ? 0.01f : 1.0f;
    x = event.ScrollX * scale;
    y = event.ScrollY * scale;
}

uint8_t SdlButtonOf(int32_t button)
{
    switch (button) {
    case 0: return SDL_BUTTON_LEFT;
    case 1: return SDL_BUTTON_RIGHT;
    case 2: return SDL_BUTTON_MIDDLE;
    default: return (uint8_t)std::clamp(button + 1, 4, 255);
    }
}

}

bool ToDrawablePixels(ViewGeometry const& g, double viewX, double viewY, float& x, float& y)
{
    if (!(g.ViewWidth > 0) || !(g.ViewHeight > 0) || !(g.DrawableWidth > 0) || !(g.DrawableHeight > 0)) return false;
    double down = g.Flipped ? viewY : g.ViewHeight - viewY;
    x = (float)(viewX * g.DrawableWidth / g.ViewWidth);
    y = (float)(down * g.DrawableHeight / g.ViewHeight);
    return true;
}

bool ToViewPoint(ViewGeometry const& g, float x, float y, double& viewX, double& viewY)
{
    if (!(g.ViewWidth > 0) || !(g.ViewHeight > 0) || !(g.DrawableWidth > 0) || !(g.DrawableHeight > 0)) return false;
    double down = y * g.ViewHeight / g.DrawableHeight;
    viewX = x * g.ViewWidth / g.DrawableWidth;
    viewY = g.Flipped ? down : g.ViewHeight - down;
    return true;
}

size_t TextOfKeyDown(char const* characters, uint32_t modifierFlags, char* out, size_t outSize)
{
    if (!outSize) return 0;
    size_t written = 0;
    out[0] = 0;
    if (!characters || (modifierFlags & (kFlagCommand | kFlagControl))) return 0;
    auto at = reinterpret_cast<uint8_t const*>(characters);
    size_t left = strlen(characters);
    while (left) {
        size_t length = SequenceLength(*at);
        if (!length || length > left) break;        // not UTF-8 from here on
        if (IsText(CodePoint(at, length))) {
            if (written + length >= outSize) break;
            memcpy(out + written, at, length);
            written += length;
        }
        at += length;
        left -= length;
    }
    out[written] = 0;
    return written;
}

void OverlayInputsOf(CocoaEvent const& event, std::vector<OverlayInput>& out)
{
    auto modifiers = [&] {
        constexpr std::pair<ImGuiKey, uint32_t> kModifiers[] = {
            { ImGuiMod_Shift, kFlagShift }, { ImGuiMod_Ctrl, kFlagControl }, { ImGuiMod_Alt, kFlagOption }, { ImGuiMod_Super, kFlagCommand },
        };
        for (auto const& [key, flag] : kModifiers) {
            OverlayInput input;
            input.What = OverlayInput::Kind::Key;
            input.Key = key;
            input.Down = (event.ModifierFlags & flag) != 0;
            out.push_back(input);
        }
    };
    auto key = [&](ImGuiKey which, bool down) {
        if (which == ImGuiKey_None) return;
        OverlayInput input;
        input.What = OverlayInput::Kind::Key;
        input.Key = which;
        input.Down = down;
        input.KeyCode = event.KeyCode;
        out.push_back(input);
    };
    auto position = [&] {
        if (!event.HasPosition) return;
        OverlayInput input;
        input.What = OverlayInput::Kind::MousePosition;
        input.X = event.X;
        input.Y = event.Y;
        out.push_back(input);
    };

    switch (event.Kind) {
    case CocoaEventKind::KeyDown:
    case CocoaEventKind::KeyUp:
        modifiers();
        // A key of the input method's composition is not ImGui's.
        if (event.Kind == CocoaEventKind::KeyDown && event.Composing) break;
        // ImGui repeats a held key itself; the characters of a repeat are text all the same.
        if (!event.Repeat) key(ImGuiKeyOf(PositionalKeyCode(event.KeyCode, event.IsoKeyboard), event.LayoutCharacter), event.Kind == CocoaEventKind::KeyDown);
        if (event.Kind == CocoaEventKind::KeyDown && !event.TextByInputSystem) {
            OverlayInput input;
            input.What = OverlayInput::Kind::Text;
            if (TextOfKeyDown(event.Text, event.ModifierFlags, input.Text, sizeof(input.Text))) out.push_back(input);
        }
        break;
    case CocoaEventKind::FlagsChanged:
        modifiers();
        // Which key it was, for the keys ImGui tells apart; caps lock is a state, not a key held.
        if (uint32_t bit = ModifierBitOf(event.KeyCode); bit && bit != kFlagCapsLock) {
            key(ImGuiKeyOf(event.KeyCode), (event.ModifierFlags & bit) != 0);
        }
        break;
    case CocoaEventKind::MouseDown:
    case CocoaEventKind::MouseUp:
        position();
        if (event.Button >= 0 && event.Button < ImGuiMouseButton_COUNT) {
            OverlayInput input;
            input.What = OverlayInput::Kind::MouseButton;
            input.Button = event.Button;
            input.Down = event.Kind == CocoaEventKind::MouseDown;
            out.push_back(input);
        }
        break;
    case CocoaEventKind::MouseMoved:
        position();
        break;
    case CocoaEventKind::MouseExited:
        // As ImGui's SDL backend takes SDL_WINDOWEVENT_LEAVE: no position,
        // unless a button is held (a drag goes on outside the view).
        if (!event.PressedButtons) {
            OverlayInput input;
            input.What = OverlayInput::Kind::MousePosition;
            input.X = input.Y = -FLT_MAX;
            out.push_back(input);
        }
        break;
    case CocoaEventKind::ScrollWheel: {
        if (event.ScrollCancelled) break;
        OverlayInput input;
        input.What = OverlayInput::Kind::MouseWheel;
        WheelAmounts(event, input.X, input.Y);
        if (input.X != 0 || input.Y != 0) out.push_back(input);
        break;
    }
    default:
        break;
    }
}

void TextInputsOf(char const* utf8, std::vector<OverlayInput>& out)
{
    if (!utf8) return;
    auto at = reinterpret_cast<uint8_t const*>(utf8);
    size_t left = strlen(utf8);
    OverlayInput input;
    input.What = OverlayInput::Kind::Text;
    size_t written = 0;
    auto flush = [&] {
        if (!written) return;
        input.Text[written] = 0;
        out.push_back(input);
        written = 0;
    };
    while (left) {
        size_t length = SequenceLength(*at);
        if (!length || length > left) break;        // not UTF-8 from here on
        if (IsText(CodePoint(at, length))) {
            if (written + length >= sizeof(input.Text)) flush();
            memcpy(input.Text + written, at, length);
            written += length;
        }
        at += length;
        left -= length;
    }
    flush();
}

void OverlayInputQueue::Push(std::span<OverlayInput const> inputs)
{
    if (inputs.empty()) return;
    std::lock_guard _(mutex_);
    queued_.insert(queued_.end(), inputs.begin(), inputs.end());
    if (queued_.size() > kMost) queued_.erase(queued_.begin(), queued_.end() - kMost);
}

void OverlayInputQueue::Replay(ImGuiIO& io)
{
    std::vector<OverlayInput> inputs;
    {
        std::lock_guard _(mutex_);
        inputs.swap(queued_);
    }
    for (auto const& input : inputs) {
        switch (input.What) {
        case OverlayInput::Kind::Key:
            io.AddKeyEvent(input.Key, input.Down);
            if (input.KeyCode >= 0) io.SetKeyEventNativeData(input.Key, input.KeyCode, -1);
            break;
        case OverlayInput::Kind::Text: io.AddInputCharactersUTF8(input.Text); break;
        case OverlayInput::Kind::MousePosition: io.AddMousePosEvent(input.X, input.Y); break;
        case OverlayInput::Kind::MouseButton: io.AddMouseButtonEvent(input.Button, input.Down); break;
        case OverlayInput::Kind::MouseWheel: io.AddMouseWheelEvent(input.X, input.Y); break;
        case OverlayInput::Kind::Focus: io.AddFocusEvent(input.Down); break;
        }
    }
}

void OverlayInputQueue::Clear()
{
    std::lock_guard _(mutex_);
    queued_.clear();
}

size_t OverlayInputQueue::Size()
{
    std::lock_guard _(mutex_);
    return queued_.size();
}

bool OverlayTakes(CocoaEvent const& event, OverlayWants wants)
{
    switch (event.Kind) {
    case CocoaEventKind::KeyDown:
    case CocoaEventKind::KeyUp:
        return wants.TextInput && !(event.ModifierFlags & kFlagCommand);
    case CocoaEventKind::MouseDown:
    case CocoaEventKind::MouseUp:
    case CocoaEventKind::MouseMoved:
    case CocoaEventKind::ScrollWheel:
        return wants.Mouse;
    default:
        return false;
    }
}

bool SdlEventOf(CocoaEvent const& event, uint32_t timestamp, SDL_Event& out)
{
    memset(&out, 0, sizeof(out));
    auto key = [&](bool down) {
        auto scancode = ScancodeOf(PositionalKeyCode(event.KeyCode, event.IsoKeyboard));
        if (scancode == SDL_SCANCODE_UNKNOWN) return false;
        out.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        out.key.timestamp = timestamp;
        out.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        out.key.repeat = event.Repeat ? 1 : 0;
        out.key.keysym.scancode = scancode;
        out.key.keysym.sym = SdlKeycodeOf(scancode, event.LayoutCharacter);
        out.key.keysym.mod = SdlModifiersOf(event.ModifierFlags);
        return true;
    };

    switch (event.Kind) {
    case CocoaEventKind::KeyDown: return key(true);
    case CocoaEventKind::KeyUp: return key(false);
    case CocoaEventKind::FlagsChanged: {
        uint32_t bit = ModifierBitOf(event.KeyCode);
        return bit && key((event.ModifierFlags & bit) != 0);
    }
    case CocoaEventKind::MouseDown:
    case CocoaEventKind::MouseUp: {
        bool down = event.Kind == CocoaEventKind::MouseDown;
        out.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        out.button.timestamp = timestamp;
        out.button.button = SdlButtonOf(event.Button);
        out.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        out.button.clicks = (uint8_t)std::clamp(event.Clicks, 0, 255);
        out.button.x = (Sint32)lroundf(event.X);
        out.button.y = (Sint32)lroundf(event.Y);
        return true;
    }
    case CocoaEventKind::ScrollWheel: {
        if (event.ScrollCancelled) return false;
        float x = 0, y = 0;
        WheelAmounts(event, x, y);
        if (x == 0 && y == 0) return false;
        // SDL counts scrolling to the right as positive, AppKit (and ImGui) as negative.
        out.type = SDL_MOUSEWHEEL;
        out.wheel.timestamp = timestamp;
        out.wheel.preciseX = -x;
        out.wheel.preciseY = y;
        out.wheel.x = (Sint32)lroundf(-x);
        out.wheel.y = (Sint32)lroundf(y);
        out.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        out.wheel.mouseX = (Sint32)lroundf(event.X);
        out.wheel.mouseY = (Sint32)lroundf(event.Y);
        return true;
    }
    case CocoaEventKind::MouseExited:
        out.type = SDL_WINDOWEVENT;
        out.window.timestamp = timestamp;
        out.window.event = SDL_WINDOWEVENT_LEAVE;
        return true;
    default:
        return false;
    }
}

SDL_Event SdlViewportResized(int32_t width, int32_t height, uint32_t timestamp)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_WINDOWEVENT;
    event.window.timestamp = timestamp;
    event.window.event = SDL_WINDOWEVENT_RESIZED;
    event.window.data1 = width;
    event.window.data2 = height;
    return event;
}

bool GameChangeOf(SDL_Event const& event, std::span<uint16_t const> keyboardKeys, GameInputChange& out)
{
    return GameChangeOf(event, keyboardKeys, false, out);
}

bool GameChangeOf(SDL_Event const& event, std::span<uint16_t const> keyboardKeys, bool isoKeyboard, GameInputChange& out)
{
    if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) return false;
    int keyCode = KeyCodeOf(event.key.keysym.scancode);
    if (keyCode >= 0) keyCode = PositionalKeyCode((uint16_t)keyCode, isoKeyboard);     // the swap is its own reverse
    if (keyCode < 0 || (size_t)keyCode >= keyboardKeys.size()) return false;
    // The game fills its table when it sets its input up: all zero, it says nothing yet.
    if (std::all_of(keyboardKeys.begin(), keyboardKeys.end(), [](uint16_t id) { return id == 0; })) return false;
    uint16_t id = keyboardKeys[(size_t)keyCode];
    if (id == kNoGameInput) return false;
    bool down = event.type == SDL_KEYDOWN;
    out = {};
    out.Input = id;
    out.Value[0] = out.Value[1] = down ? 1.0f : 0.0f;
    out.Pressed = down ? 1 : 0;
    return true;
}

void GameChangesOfInjected(SDL_Event const& event, std::span<uint16_t const> keyboardKeys, uint16_t heldModifiers, bool withModifiers,
    bool isoKeyboard, std::vector<GameInputChange>& out)
{
    GameInputChange key;
    if (!GameChangeOf(event, keyboardKeys, isoKeyboard, key)) return;
    constexpr std::pair<uint16_t, SDL_Scancode> kModifierKeys[] = {
        { KMOD_LCTRL, SDL_SCANCODE_LCTRL }, { KMOD_LSHIFT, SDL_SCANCODE_LSHIFT }, { KMOD_LALT, SDL_SCANCODE_LALT },
        { KMOD_LGUI, SDL_SCANCODE_LGUI }, { KMOD_RCTRL, SDL_SCANCODE_RCTRL }, { KMOD_RSHIFT, SDL_SCANCODE_RSHIFT },
        { KMOD_RALT, SDL_SCANCODE_RALT }, { KMOD_RGUI, SDL_SCANCODE_RGUI },
    };
    size_t first = out.size(), downs = 0;
    if (withModifiers) {
        for (auto const& [bit, scancode] : kModifierKeys) {
            if (!(event.key.keysym.mod & bit) || (heldModifiers & bit) || scancode == event.key.keysym.scancode) continue;
            SDL_Event modifier;
            memset(&modifier, 0, sizeof(modifier));
            modifier.type = SDL_KEYDOWN;
            modifier.key.keysym.scancode = scancode;
            GameInputChange down;
            if (!GameChangeOf(modifier, keyboardKeys, down)) continue;
            out.push_back(down);
            downs++;
        }
    }
    out.push_back(key);
    for (size_t i = downs; i-- > 0;) {
        GameInputChange up = out[first + i];
        up.Value[0] = up.Value[1] = 0.0f;
        up.Pressed = 0;
        out.push_back(up);
    }
}

uint32_t InputTicks()
{
    static const auto start = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

}
