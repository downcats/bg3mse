#import <Carbon/Carbon.h>

#include <Overlay/KeyboardLayout.h>
#include <Platform/Log.h>

#if !__has_feature(objc_arc)
#error "KeyboardLayout.mm is written for automatic reference counting"
#endif

namespace bg3se::mac::overlay {

namespace {

void const* UnicodeDataOf(TISInputSourceRef source)
{
    auto data = source ? (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : nullptr;
    return data ? CFDataGetBytePtr(data) : nullptr;
}

}

uint32_t LayoutCharacterOf(void const* uchrData, uint32_t keyboardType, uint16_t keyCode)
{
    if (!uchrData) return 0;
    UniChar text[8];
    UniCharCount length = 0;
    UInt32 deadKeyState = 0;
    OSStatus status = UCKeyTranslate(static_cast<UCKeyboardLayout const*>(uchrData), keyCode, kUCKeyActionDown, 0, keyboardType,
        kUCKeyTranslateNoDeadKeysMask, &deadKeyState, 8, &length, text);
    if (status != noErr || length == 0 || text[0] == 0x10) return 0;
    return text[0];
}

void KeyboardLayout::Update()
{
    // As SDL: the layout is looked up again for each key, and read again only when it changed.
    TISInputSourceRef current = TISCopyCurrentKeyboardLayoutInputSource();
    if (current && current == source_) {
        CFRelease(current);
        return;
    }
    if (source_) CFRelease(source_);
    source_ = current;
    data_ = UnicodeDataOf(current);
    if (!data_ && !said_) {
        said_ = true;
        Log(LogLevel::Warning, "overlay: the keyboard layout has no Unicode data; the overlay names letters by their places on a US keyboard");
    }
}

uint32_t KeyboardLayout::CharacterOf(uint16_t keyCode)
{
    Update();
    return LayoutCharacterOf(data_, LMGetKbdType(), keyCode);
}

bool KeyboardLayout::Iso()
{
    return KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
}

KeyboardLayout& KeyboardLayout::Current()
{
    static KeyboardLayout layout;
    return layout;
}

void const* LayoutDataByID(char const* inputSourceID)
{
    NSDictionary* filter = @{ (__bridge NSString*)kTISPropertyInputSourceID : [NSString stringWithUTF8String:inputSourceID] };
    CFArrayRef sources = TISCreateInputSourceList((__bridge CFDictionaryRef)filter, true);
    if (!sources) return nullptr;
    void const* data = nullptr;
    if (CFArrayGetCount(sources) > 0) {
        auto source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, 0);
        CFRetain(source);
        data = UnicodeDataOf(source);
    }
    CFRelease(sources);
    return data;
}

}
