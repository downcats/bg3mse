// A dylib the tests of libbg3mse.dylib insert into the stand-in for the game
// (DYLD_INSERT_LIBRARIES; Mac/Tests/DylibLoadTests.cpp): it takes the place
// of CFUserNotificationDisplayAlert, which the shim's MessageBox shows its
// alert with, so that a test run puts nothing on the screen. Each alert asked
// for is a line "<header>\t<message>\t<default button>" appended to the file
// BG3MSE_TEST_ALERTS names, and is answered at once with its default button.
//
// MessageBox is called from a thread of its own, which the stand-in's end
// would not wait for: with BG3MSE_TEST_ALERT_WAIT set, the process's end
// waits until an alert is recorded, for ten seconds at most.

#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void Text(CFStringRef text, char* buffer, size_t size)
{
    buffer[0] = 0;
    if (text) CFStringGetCString(text, buffer, (CFIndex)size, kCFStringEncodingUTF8);
}

static SInt32 RecordAlert(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef icon, CFURLRef sound, CFURLRef localization,
    CFStringRef header, CFStringRef message, CFStringRef defaultButton, CFStringRef alternateButton, CFStringRef otherButton,
    CFOptionFlags* response)
{
    (void)timeout, (void)flags, (void)icon, (void)sound, (void)localization, (void)alternateButton, (void)otherButton;
    char const* path = getenv("BG3MSE_TEST_ALERTS");
    if (path) {
        char line[4096], parts[3][1024];
        Text(header, parts[0], sizeof(parts[0]));
        Text(message, parts[1], sizeof(parts[1]));
        Text(defaultButton, parts[2], sizeof(parts[2]));
        int length = snprintf(line, sizeof(line), "%s\t%s\t%s\n", parts[0], parts[1], parts[2]);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            (void)!write(fd, line, (size_t)length);
            close(fd);
        }
    }
    if (response) *response = kCFUserNotificationDefaultResponse;
    return 0;
}

__attribute__((used, section("__DATA,__interpose"))) static struct
{
    void const* Replacement;
    void const* Replaced;
} const kInterposed[] = { { (void const*)&RecordAlert, (void const*)&CFUserNotificationDisplayAlert } };

static void WaitForAlert(void)
{
    char const* path = getenv("BG3MSE_TEST_ALERTS");
    if (!path) return;
    struct stat st;
    // The line is one write: once the file has a size, it is all there.
    for (int i = 0; i < 1000 && (stat(path, &st) != 0 || st.st_size == 0); i++) usleep(10000);
}

__attribute__((constructor)) static void Initialize(void)
{
    if (getenv("BG3MSE_TEST_ALERT_WAIT")) atexit(&WaitForAlert);
}
