// bg3mse.app: the installer a player double-clicks. It is bg3mse-install's
// install (Mac/Installer/Installer.h), run in this process, with the two
// optional features (Extras.h) answered in its window, and then the launch
// option, with a button that copies it, and the steps in Steam. Where bg3mse is installed, it offers
// bg3mse-install's uninstall too (the settings stay).
//
// The release it installs is inside the bundle, where a signed bundle keeps
// each kind of file (Mac/Release/Assemble.cmake lays it out):
//
//   Contents/MacOS/bg3mse                    this program
//   Contents/Helpers/                        bg3mse-install, bg3mse-launch
//   Contents/Frameworks/                     libbg3mse.dylib, libbg3mse-loader.dylib, libbg3mse-camera.dylib
//   Contents/Resources/release/              the release folder's other files, at their paths there
//
// so that an app the system runs from a copy of its own (a downloaded app
// that was not moved is run from a read-only copy of the bundle alone) has
// all of it. The install reads the release from a folder laid out as the
// release folder is: a temporary one, made from the bundle, with each file's
// contents and mode and none of its extended attributes (the quarantine mark
// of a download among them), and removed afterwards.

#import <AppKit/AppKit.h>

#include <Installer/Extras.h>
#include <Installer/Installer.h>
#include <Installer/Launch.h>
#include <Installer/ToolHost.h>
#include <Platform/Log.h>
#include <Release/Version.h>

#include <climits>
#include <copyfile.h>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace bg3se::mac;
namespace installer = bg3se::mac::installer;

namespace {

NSString* String(std::string const& s)
{
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

bool CopyTree(std::string const& from, std::string const& to, std::string& problem)
{
    struct stat st{};
    if (lstat(from.c_str(), &st) != 0) {
        problem = from + ": " + strerror(errno);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to.c_str(), 0755) != 0 && errno != EEXIST) {
            problem = to + ": " + strerror(errno);
            return false;
        }
        DIR* dir = opendir(from.c_str());
        if (!dir) {
            problem = from + ": " + strerror(errno);
            return false;
        }
        bool ok = true;
        while (dirent* e = readdir(dir)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            if (!CopyTree(from + "/" + name, to + "/" + name, problem)) {
                ok = false;
                break;
            }
        }
        closedir(dir);
        return ok;
    }
    if (!S_ISREG(st.st_mode)) {
        problem = from + " is not a file";
        return false;
    }
    if (copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_DATA | COPYFILE_STAT) != 0) {
        problem = to + ": " + strerror(errno);
        return false;
    }
    return true;
}

// The release folder, made from the bundle in a new temporary folder.
bool LayOutRelease(std::string& folder, std::string& problem)
{
    std::string contents = [[NSBundle mainBundle] bundlePath].UTF8String + std::string("/Contents");
    std::string templ = std::string(NSTemporaryDirectory().UTF8String) + "bg3mse-release.XXXXXX";
    if (!mkdtemp(templ.data())) {
        problem = "cannot make a temporary folder: " + std::string(strerror(errno));
        return false;
    }
    folder = templ;
    struct Piece
    {
        std::string From, To;
    } pieces[] = {
        { contents + "/Resources/release", folder },
        { contents + "/Helpers/" + installer::kInstallTool, folder + "/" + installer::kInstallTool },
        { contents + "/Helpers/" + installer::kLauncher, folder + "/" + installer::kLauncher },
        { contents + "/Frameworks/" + installer::kLibrary, folder + "/" + installer::kLibrary },
        { contents + "/Frameworks/" + installer::kLoaderLibrary, folder + "/" + installer::kLoaderLibrary },
        { contents + "/Frameworks/" + installer::kCameraLibrary,
          folder + "/" + installer::kCameraDirectory + "/" + installer::kCameraLibrary },
    };
    for (auto const& p : pieces) {
        struct stat st{};
        if (lstat(p.From.c_str(), &st) != 0) {
            if (p.To == folder + "/" + installer::kCameraDirectory + "/" + installer::kCameraLibrary) continue;
            problem = "the app is not whole: " + p.From + " is missing";
            return false;
        }
        if (!CopyTree(p.From, p.To, problem)) return false;
    }
    return true;
}

void RemoveTree(std::string const& path)
{
    [[NSFileManager defaultManager] removeItemAtPath:String(path) error:nil];
}

}

@interface Installer : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation Installer {
    NSWindow* _window;
    NSStackView* _stack;
    NSButton* _camera;
    NSButton* _upscaling;
    NSButton* _install;
    NSTextView* _output;
    // The reading end of the running verb's output, kept until the verb has ended.
    NSFileHandle* _reading;
    std::string _release;
    std::string _launchLine;
    // While a verb runs, the app does not quit or close: its release folder is what the verb copies from.
    BOOL _running;
    BOOL _quitWhenDone;
}

- (NSTextField*)label:(NSString*)text
{
    NSTextField* label = [NSTextField wrappingLabelWithString:text];
    label.selectable = NO;
    [label.widthAnchor constraintEqualToConstant:460].active = YES;
    return label;
}

- (NSButton*)button:(NSString*)title action:(SEL)action
{
    NSButton* button = [NSButton buttonWithTitle:title target:self action:action];
    return button;
}

- (void)show:(NSArray<NSView*>*)views
{
    for (NSView* v in [_stack.arrangedSubviews copy]) [_stack removeView:v];
    for (NSView* v in views) [_stack addArrangedSubview:v];
    [_window layoutIfNeeded];
}

- (NSScrollView*)outputView
{
    NSScrollView* scroll = [NSTextView scrollableTextView];
    _output = scroll.documentView;
    _output.editable = NO;
    _output.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    [scroll.widthAnchor constraintEqualToConstant:460].active = YES;
    [scroll.heightAnchor constraintEqualToConstant:200].active = YES;
    return scroll;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 500, 200)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"bg3mse";
    _window.delegate = self;
    _stack = [NSStackView stackViewWithViews:@[]];
    _stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _stack.alignment = NSLayoutAttributeLeading;
    _stack.spacing = 12;
    _stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    _window.contentView = _stack;

    std::string problem;
    installer::Host host = installer::ToolHost();
    if (!LayOutRelease(_release, problem)) {
        [self failed:String(problem)];
    } else if (host.DataDirectory.empty()) {
        [self failed:@"There is no home folder to install bg3mse in."];
    } else {
        // What is on now is what the window starts with.
        installer::Options options;
        installer::Extras extras(options, host, host.DataDirectory, _release, {}, std::nullopt);
        _camera = [NSButton checkboxWithTitle:@"Native Camera Tweaks" target:nil action:nil];
        _camera.state = extras.IsOn(installer::Extra::Camera) ? NSControlStateValueOn : NSControlStateValueOff;
        _upscaling = [NSButton checkboxWithTitle:@"MetalFX Temporal (turn on FSR in game)" target:nil action:nil];
        _upscaling.state = extras.IsOn(installer::Extra::Upscaling) ? NSControlStateValueOn : NSControlStateValueOff;
        NSMutableArray<NSView*>* views = [NSMutableArray array];
        [views addObject:[self label:[NSString stringWithFormat:@"Install bg3mse %s for Baldur's Gate 3.", version::kVersion]]];
        [views addObject:_camera];
        [views addObject:_upscaling];
        std::string unavailable = extras.Unavailable(installer::Extra::Upscaling);
        if (!unavailable.empty()) {
            _upscaling.enabled = NO;
            _upscaling.state = NSControlStateValueOff;
            [views addObject:[self label:unavailable.starts_with("requires macOS 13") ? @"MetalFX Temporal requires macOS 13 or later."
                                                                                 : @"MetalFX Temporal isn't in this release."]];
        }
        _install = [self button:@"Install" action:@selector(install:)];
        _install.keyEquivalent = @"\r";
        [views addObject:_install];
        // Installed before: it can be taken out again.
        std::string launcher = host.DataDirectory + "/" + installer::kBinDirectory + "/" + installer::kLauncher;
        if (access(launcher.c_str(), F_OK) == 0) [views addObject:[self button:@"Uninstall" action:@selector(uninstall:)]];
        [self show:views];
    }
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)install:(id)sender
{
    installer::Options options;
    options.What = installer::Verb::Install;
    options.From = _release;
    options.Camera = _camera.state == NSControlStateValueOn;
    if (_upscaling.enabled) options.Upscaling = _upscaling.state == NSControlStateValueOn;
    else options.Defaults = true;
    [self run:options];
}

- (void)uninstall:(id)sender
{
    installer::Options options;
    options.What = installer::Verb::Uninstall;
    [self run:options];
}

// The verb on a queue of its own, its output shown as it is written.
- (void)run:(installer::Options)options
{
    bool installs = options.What == installer::Verb::Install;
    NSScrollView* scroll = [self outputView];
    [self show:@[ [self label:installs ? @"Installing…" : @"Uninstalling…"], scroll ]];
    _running = YES;
    [_window standardWindowButton:NSWindowCloseButton].enabled = NO;

    int fds[2];
    if (pipe(fds) != 0) {
        [self failed:@"Cannot start the install."];
        return;
    }
    NSFileHandle* reading = _reading = [[NSFileHandle alloc] initWithFileDescriptor:fds[0] closeOnDealloc:YES];
    NSTextView* output = _output;
    reading.readabilityHandler = ^(NSFileHandle* handle) {
        NSData* data = handle.availableData;
        if (!data.length) {
            handle.readabilityHandler = nil;
            return;
        }
        NSString* text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
        dispatch_async(dispatch_get_main_queue(), ^{
            [output.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text
                                                                                         attributes:@{ NSFontAttributeName: output.font,
                                                                                                       NSForegroundColorAttributeName: NSColor.textColor }]];
            [output scrollToEndOfDocument:nil];
        });
    };
    std::string release = _release;
    int writing = fds[1];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        installer::Host host = installer::ToolHost();
        FILE* out = fdopen(writing, "w");
        setvbuf(out, nullptr, _IOLBF, 0);
        host.Out = out;
        int status = installer::Run(options, host);
        fclose(out);
        char resolved[PATH_MAX];
        std::string data = realpath(host.DataDirectory.c_str(), resolved) ? resolved : host.DataDirectory;
        std::string line = installer::LaunchOption(data + "/" + installer::kBinDirectory + "/" + installer::kLauncher);
        RemoveTree(release);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_release.clear();
            self->_running = NO;
            [self->_window standardWindowButton:NSWindowCloseButton].enabled = YES;
            if (self->_quitWhenDone) {
                [NSApp replyToApplicationShouldTerminate:YES];
                return;
            }
            if (installs && (status == 0 || status == installer::kInstalledWithoutExtras)) [self installed:line extras:status == 0];
            else if (status != 0) [self failed:installs ? @"Install failed." : @"Uninstall failed."];
            else [self uninstalled];
        });
    });
}

- (void)installed:(std::string)line extras:(bool)extras
{
    _launchLine = line;
    NSTextField* option = [NSTextField textFieldWithString:String(line)];
    option.editable = NO;
    option.selectable = YES;
    option.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    [option.widthAnchor constraintEqualToConstant:460].active = YES;
    NSButton* copy = [self button:@"Copy" action:@selector(copyLine:)];
    NSButton* done = [self button:@"Done" action:@selector(done:)];
    done.keyEquivalent = @"\r";
    NSMutableArray<NSView*>* views = [NSMutableArray arrayWithObject:[self label:@"bg3mse is installed. Copy this launch option:"]];
    [views addObjectsFromArray:@[ option, copy ]];
    [views addObject:[self label:@"Paste it into Launch Options under Properties > General in Steam, then press Play."]];
    if (!extras) [views addObject:[self label:@"Some optional features couldn't be turned on."]];
    [views addObject:done];
    [self show:views];
}

- (void)uninstalled
{
    NSButton* done = [self button:@"Done" action:@selector(done:)];
    done.keyEquivalent = @"\r";
    [self show:@[
        [self label:@"bg3mse is uninstalled, remember to clear the game's launch options."],
        done,
    ]];
}

- (void)failed:(NSString*)why
{
    NSMutableArray<NSView*>* views = [NSMutableArray arrayWithObject:[self label:why]];
    if (_output) [views addObject:_output.enclosingScrollView];
    [views addObject:[self button:@"Quit" action:@selector(done:)]];
    [self show:views];
}

- (void)copyLine:(id)sender
{
    NSPasteboard* board = [NSPasteboard generalPasteboard];
    [board clearContents];
    [board setString:String(_launchLine) forType:NSPasteboardTypeString];
    if ([sender isKindOfClass:[NSButton class]]) ((NSButton*)sender).title = @"Copied";
}

- (void)done:(id)sender
{
    [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app
{
    return YES;
}

- (BOOL)windowShouldClose:(NSWindow*)window
{
    return !_running;
}

// Asked to quit (logging out, say) while a verb runs: once it has ended.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app
{
    if (!_running) return NSTerminateNow;
    _quitWhenDone = YES;
    return NSTerminateLater;
}

- (void)applicationWillTerminate:(NSNotification*)notification
{
    if (!_release.empty()) RemoveTree(_release);
}

@end

int main()
{
    // A write to the output's pipe with nobody reading fails; it does not end the app.
    signal(SIGPIPE, SIG_IGN);
    SetLogStream(nullptr);
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        Installer* delegate = [Installer new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
