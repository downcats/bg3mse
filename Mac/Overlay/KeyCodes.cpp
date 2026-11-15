#include <Overlay/KeyCodes.h>

#include <iterator>

namespace bg3se::mac::overlay {

namespace {

struct Key
{
    uint16_t Code;
    ImGuiKey Overlay;
    SDL_Scancode Scancode;
};

// One row for each key of an Apple keyboard, by its virtual key code.
constexpr Key kKeys[] = {
    { 0x00, ImGuiKey_A, SDL_SCANCODE_A },
    { 0x01, ImGuiKey_S, SDL_SCANCODE_S },
    { 0x02, ImGuiKey_D, SDL_SCANCODE_D },
    { 0x03, ImGuiKey_F, SDL_SCANCODE_F },
    { 0x04, ImGuiKey_H, SDL_SCANCODE_H },
    { 0x05, ImGuiKey_G, SDL_SCANCODE_G },
    { 0x06, ImGuiKey_Z, SDL_SCANCODE_Z },
    { 0x07, ImGuiKey_X, SDL_SCANCODE_X },
    { 0x08, ImGuiKey_C, SDL_SCANCODE_C },
    { 0x09, ImGuiKey_V, SDL_SCANCODE_V },
    { 0x0a, ImGuiKey_Oem102, SDL_SCANCODE_NONUSBACKSLASH },    // the key next to left shift on an ISO keyboard
    { 0x0b, ImGuiKey_B, SDL_SCANCODE_B },
    { 0x0c, ImGuiKey_Q, SDL_SCANCODE_Q },
    { 0x0d, ImGuiKey_W, SDL_SCANCODE_W },
    { 0x0e, ImGuiKey_E, SDL_SCANCODE_E },
    { 0x0f, ImGuiKey_R, SDL_SCANCODE_R },
    { 0x10, ImGuiKey_Y, SDL_SCANCODE_Y },
    { 0x11, ImGuiKey_T, SDL_SCANCODE_T },
    { 0x12, ImGuiKey_1, SDL_SCANCODE_1 },
    { 0x13, ImGuiKey_2, SDL_SCANCODE_2 },
    { 0x14, ImGuiKey_3, SDL_SCANCODE_3 },
    { 0x15, ImGuiKey_4, SDL_SCANCODE_4 },
    { 0x16, ImGuiKey_6, SDL_SCANCODE_6 },
    { 0x17, ImGuiKey_5, SDL_SCANCODE_5 },
    { 0x18, ImGuiKey_Equal, SDL_SCANCODE_EQUALS },
    { 0x19, ImGuiKey_9, SDL_SCANCODE_9 },
    { 0x1a, ImGuiKey_7, SDL_SCANCODE_7 },
    { 0x1b, ImGuiKey_Minus, SDL_SCANCODE_MINUS },
    { 0x1c, ImGuiKey_8, SDL_SCANCODE_8 },
    { 0x1d, ImGuiKey_0, SDL_SCANCODE_0 },
    { 0x1e, ImGuiKey_RightBracket, SDL_SCANCODE_RIGHTBRACKET },
    { 0x1f, ImGuiKey_O, SDL_SCANCODE_O },
    { 0x20, ImGuiKey_U, SDL_SCANCODE_U },
    { 0x21, ImGuiKey_LeftBracket, SDL_SCANCODE_LEFTBRACKET },
    { 0x22, ImGuiKey_I, SDL_SCANCODE_I },
    { 0x23, ImGuiKey_P, SDL_SCANCODE_P },
    { 0x24, ImGuiKey_Enter, SDL_SCANCODE_RETURN },
    { 0x25, ImGuiKey_L, SDL_SCANCODE_L },
    { 0x26, ImGuiKey_J, SDL_SCANCODE_J },
    { 0x27, ImGuiKey_Apostrophe, SDL_SCANCODE_APOSTROPHE },
    { 0x28, ImGuiKey_K, SDL_SCANCODE_K },
    { 0x29, ImGuiKey_Semicolon, SDL_SCANCODE_SEMICOLON },
    { 0x2a, ImGuiKey_Backslash, SDL_SCANCODE_BACKSLASH },
    { 0x2b, ImGuiKey_Comma, SDL_SCANCODE_COMMA },
    { 0x2c, ImGuiKey_Slash, SDL_SCANCODE_SLASH },
    { 0x2d, ImGuiKey_N, SDL_SCANCODE_N },
    { 0x2e, ImGuiKey_M, SDL_SCANCODE_M },
    { 0x2f, ImGuiKey_Period, SDL_SCANCODE_PERIOD },
    { 0x30, ImGuiKey_Tab, SDL_SCANCODE_TAB },
    { 0x31, ImGuiKey_Space, SDL_SCANCODE_SPACE },
    { 0x32, ImGuiKey_GraveAccent, SDL_SCANCODE_GRAVE },
    { 0x33, ImGuiKey_Backspace, SDL_SCANCODE_BACKSPACE },
    { 0x35, ImGuiKey_Escape, SDL_SCANCODE_ESCAPE },
    { 0x36, ImGuiKey_RightSuper, SDL_SCANCODE_RGUI },
    { 0x37, ImGuiKey_LeftSuper, SDL_SCANCODE_LGUI },
    { 0x38, ImGuiKey_LeftShift, SDL_SCANCODE_LSHIFT },
    { 0x39, ImGuiKey_CapsLock, SDL_SCANCODE_CAPSLOCK },
    { 0x3a, ImGuiKey_LeftAlt, SDL_SCANCODE_LALT },
    { 0x3b, ImGuiKey_LeftCtrl, SDL_SCANCODE_LCTRL },
    { 0x3c, ImGuiKey_RightShift, SDL_SCANCODE_RSHIFT },
    { 0x3d, ImGuiKey_RightAlt, SDL_SCANCODE_RALT },
    { 0x3e, ImGuiKey_RightCtrl, SDL_SCANCODE_RCTRL },
    { 0x40, ImGuiKey_F17, SDL_SCANCODE_F17 },
    { 0x41, ImGuiKey_KeypadDecimal, SDL_SCANCODE_KP_PERIOD },
    { 0x43, ImGuiKey_KeypadMultiply, SDL_SCANCODE_KP_MULTIPLY },
    { 0x45, ImGuiKey_KeypadAdd, SDL_SCANCODE_KP_PLUS },
    { 0x47, ImGuiKey_NumLock, SDL_SCANCODE_NUMLOCKCLEAR },     // "clear" on an Apple keypad
    { 0x48, ImGuiKey_None, SDL_SCANCODE_VOLUMEUP },
    { 0x49, ImGuiKey_None, SDL_SCANCODE_VOLUMEDOWN },
    { 0x4a, ImGuiKey_None, SDL_SCANCODE_MUTE },
    { 0x4b, ImGuiKey_KeypadDivide, SDL_SCANCODE_KP_DIVIDE },
    { 0x4c, ImGuiKey_KeypadEnter, SDL_SCANCODE_KP_ENTER },
    { 0x4e, ImGuiKey_KeypadSubtract, SDL_SCANCODE_KP_MINUS },
    { 0x4f, ImGuiKey_F18, SDL_SCANCODE_F18 },
    { 0x50, ImGuiKey_F19, SDL_SCANCODE_F19 },
    { 0x51, ImGuiKey_KeypadEqual, SDL_SCANCODE_KP_EQUALS },
    { 0x52, ImGuiKey_Keypad0, SDL_SCANCODE_KP_0 },
    { 0x53, ImGuiKey_Keypad1, SDL_SCANCODE_KP_1 },
    { 0x54, ImGuiKey_Keypad2, SDL_SCANCODE_KP_2 },
    { 0x55, ImGuiKey_Keypad3, SDL_SCANCODE_KP_3 },
    { 0x56, ImGuiKey_Keypad4, SDL_SCANCODE_KP_4 },
    { 0x57, ImGuiKey_Keypad5, SDL_SCANCODE_KP_5 },
    { 0x58, ImGuiKey_Keypad6, SDL_SCANCODE_KP_6 },
    { 0x59, ImGuiKey_Keypad7, SDL_SCANCODE_KP_7 },
    { 0x5a, ImGuiKey_F20, SDL_SCANCODE_F20 },
    { 0x5b, ImGuiKey_Keypad8, SDL_SCANCODE_KP_8 },
    { 0x5c, ImGuiKey_Keypad9, SDL_SCANCODE_KP_9 },
    { 0x5d, ImGuiKey_None, SDL_SCANCODE_INTERNATIONAL3 },      // JIS: yen
    { 0x5e, ImGuiKey_None, SDL_SCANCODE_INTERNATIONAL1 },      // JIS: ro (underscore)
    { 0x5f, ImGuiKey_None, SDL_SCANCODE_KP_COMMA },            // JIS: the keypad's comma
    { 0x60, ImGuiKey_F5, SDL_SCANCODE_F5 },
    { 0x61, ImGuiKey_F6, SDL_SCANCODE_F6 },
    { 0x62, ImGuiKey_F7, SDL_SCANCODE_F7 },
    { 0x63, ImGuiKey_F3, SDL_SCANCODE_F3 },
    { 0x64, ImGuiKey_F8, SDL_SCANCODE_F8 },
    { 0x65, ImGuiKey_F9, SDL_SCANCODE_F9 },
    { 0x66, ImGuiKey_None, SDL_SCANCODE_LANG2 },               // JIS: eisu
    { 0x67, ImGuiKey_F11, SDL_SCANCODE_F11 },
    { 0x68, ImGuiKey_None, SDL_SCANCODE_LANG1 },               // JIS: kana
    { 0x69, ImGuiKey_F13, SDL_SCANCODE_F13 },
    { 0x6a, ImGuiKey_F16, SDL_SCANCODE_F16 },
    { 0x6b, ImGuiKey_F14, SDL_SCANCODE_F14 },
    { 0x6d, ImGuiKey_F10, SDL_SCANCODE_F10 },
    { 0x6e, ImGuiKey_Menu, SDL_SCANCODE_APPLICATION },
    { 0x6f, ImGuiKey_F12, SDL_SCANCODE_F12 },
    { 0x71, ImGuiKey_F15, SDL_SCANCODE_F15 },
    { 0x72, ImGuiKey_Insert, SDL_SCANCODE_INSERT },            // "help" on an Apple keyboard, where a PC keyboard has insert
    { 0x73, ImGuiKey_Home, SDL_SCANCODE_HOME },
    { 0x74, ImGuiKey_PageUp, SDL_SCANCODE_PAGEUP },
    { 0x75, ImGuiKey_Delete, SDL_SCANCODE_DELETE },
    { 0x76, ImGuiKey_F4, SDL_SCANCODE_F4 },
    { 0x77, ImGuiKey_End, SDL_SCANCODE_END },
    { 0x78, ImGuiKey_F2, SDL_SCANCODE_F2 },
    { 0x79, ImGuiKey_PageDown, SDL_SCANCODE_PAGEDOWN },
    { 0x7a, ImGuiKey_F1, SDL_SCANCODE_F1 },
    { 0x7b, ImGuiKey_LeftArrow, SDL_SCANCODE_LEFT },
    { 0x7c, ImGuiKey_RightArrow, SDL_SCANCODE_RIGHT },
    { 0x7d, ImGuiKey_DownArrow, SDL_SCANCODE_DOWN },
    { 0x7e, ImGuiKey_UpArrow, SDL_SCANCODE_UP },
};

Key const* Find(uint16_t keyCode)
{
    for (auto const& key : kKeys) {
        if (key.Code == keyCode) return &key;
    }
    return nullptr;
}

}

ImGuiKey ImGuiKeyOf(uint16_t keyCode)
{
    auto key = Find(keyCode);
    return key ? key->Overlay : ImGuiKey_None;
}

SDL_Scancode ScancodeOf(uint16_t keyCode)
{
    auto key = Find(keyCode);
    return key ? key->Scancode : SDL_SCANCODE_UNKNOWN;
}

int KeyCodeOf(SDL_Scancode scancode)
{
    if (scancode == SDL_SCANCODE_UNKNOWN) return -1;
    for (auto const& key : kKeys) {
        if (key.Scancode == scancode) return key.Code;
    }
    return -1;
}

uint16_t PositionalKeyCode(uint16_t keyCode, bool iso)
{
    if (!iso) return keyCode;
    if (keyCode == 0x0a) return 0x32;
    if (keyCode == 0x32) return 0x0a;
    return keyCode;
}

SDL_Keycode SdlKeycodeOf(SDL_Scancode scancode, uint32_t layoutCharacter)
{
    // SDL's default keymap (SDL_keyboard.c): a character for these, the
    // scancode's keycode for the others. SDL's UpdateKeymap gives every one
    // of these the layout's character, where the layout has one.
    SDL_Keycode character = 0;
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) character = 'a' + (scancode - SDL_SCANCODE_A);
    else if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) character = '1' + (scancode - SDL_SCANCODE_1);
    else {
        switch (scancode) {
        case SDL_SCANCODE_0: character = '0'; break;
        case SDL_SCANCODE_RETURN: character = SDLK_RETURN; break;
        case SDL_SCANCODE_ESCAPE: character = SDLK_ESCAPE; break;
        case SDL_SCANCODE_BACKSPACE: character = SDLK_BACKSPACE; break;
        case SDL_SCANCODE_TAB: character = SDLK_TAB; break;
        case SDL_SCANCODE_SPACE: character = SDLK_SPACE; break;
        case SDL_SCANCODE_MINUS: character = '-'; break;
        case SDL_SCANCODE_EQUALS: character = '='; break;
        case SDL_SCANCODE_LEFTBRACKET: character = '['; break;
        case SDL_SCANCODE_RIGHTBRACKET: character = ']'; break;
        case SDL_SCANCODE_BACKSLASH: character = '\\'; break;
        case SDL_SCANCODE_NONUSHASH: character = '#'; break;
        case SDL_SCANCODE_SEMICOLON: character = ';'; break;
        case SDL_SCANCODE_APOSTROPHE: character = '\''; break;
        case SDL_SCANCODE_GRAVE: character = '`'; break;
        case SDL_SCANCODE_COMMA: character = ','; break;
        case SDL_SCANCODE_PERIOD: character = '.'; break;
        case SDL_SCANCODE_SLASH: character = '/'; break;
        case SDL_SCANCODE_DELETE: character = SDLK_DELETE; break;
        // No character of SDL's own, the layout's when it has one.
        case SDL_SCANCODE_NONUSBACKSLASH:
        case SDL_SCANCODE_INTERNATIONAL1:
        case SDL_SCANCODE_INTERNATIONAL3:
        case SDL_SCANCODE_LANG1:
        case SDL_SCANCODE_LANG2:
            return layoutCharacter;
        default:
            return SDL_SCANCODE_TO_KEYCODE(scancode);
        }
    }
    return layoutCharacter ? (SDL_Keycode)layoutCharacter : character;
}

ImGuiKey ImGuiKeyOf(uint16_t keyCode, uint32_t layoutCharacter)
{
    auto key = Find(keyCode);
    if (!key) return ImGuiKey_None;
    // ImGui_ImplSDL2_KeyEventToImGuiKey: by the keycode where it names one of
    // these; by the scancode for the others.
    SDL_Keycode keycode = SdlKeycodeOf(key->Scancode, layoutCharacter);
    if (keycode >= 'a' && keycode <= 'z') return (ImGuiKey)(ImGuiKey_A + (keycode - 'a'));
    if (keycode >= '0' && keycode <= '9') return (ImGuiKey)(ImGuiKey_0 + (keycode - '0'));
    switch (keycode) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_COMMA: return ImGuiKey_Comma;
    case SDLK_PERIOD: return ImGuiKey_Period;
    case SDLK_SEMICOLON: return ImGuiKey_Semicolon;
    default: break;
    }
    // Keys whose keycode is their scancode's are named as before.
    if (keycode == SDL_SCANCODE_TO_KEYCODE(key->Scancode)) return key->Overlay;
    switch (key->Scancode) {
    case SDL_SCANCODE_GRAVE: return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_NONUSBACKSLASH: return ImGuiKey_Oem102;
    case SDL_SCANCODE_BACKSLASH: return ImGuiKey_Backslash;
    case SDL_SCANCODE_SEMICOLON: return ImGuiKey_Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    default: return ImGuiKey_None;
    }
}

uint32_t ModifierBitOf(uint16_t keyCode)
{
    switch (keyCode) {
    case 0x36: return kDeviceRightCommand;
    case 0x37: return kDeviceLeftCommand;
    case 0x38: return kDeviceLeftShift;
    case 0x39: return kFlagCapsLock;
    case 0x3a: return kDeviceLeftOption;
    case 0x3b: return kDeviceLeftControl;
    case 0x3c: return kDeviceRightShift;
    case 0x3d: return kDeviceRightOption;
    case 0x3e: return kDeviceRightControl;
    default: return 0;
    }
}

uint16_t SdlModifiersOf(uint32_t flags)
{
    uint16_t mod = KMOD_NONE;
    // Which of the two keys, when the event says; the left one when it only
    // says that the modifier is down (an event made by software may).
    auto pair = [&](uint32_t either, uint32_t left, uint32_t right, uint16_t sdlLeft, uint16_t sdlRight) {
        if (!(flags & either)) return;
        if (flags & left) mod |= sdlLeft;
        if (flags & right) mod |= sdlRight;
        if (!(flags & (left | right))) mod |= sdlLeft;
    };
    pair(kFlagShift, kDeviceLeftShift, kDeviceRightShift, KMOD_LSHIFT, KMOD_RSHIFT);
    pair(kFlagControl, kDeviceLeftControl, kDeviceRightControl, KMOD_LCTRL, KMOD_RCTRL);
    pair(kFlagOption, kDeviceLeftOption, kDeviceRightOption, KMOD_LALT, KMOD_RALT);
    pair(kFlagCommand, kDeviceLeftCommand, kDeviceRightCommand, KMOD_LGUI, KMOD_RGUI);
    if (flags & kFlagCapsLock) mod |= KMOD_CAPS;
    return mod;
}

}
