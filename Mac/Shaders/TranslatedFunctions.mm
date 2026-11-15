// See TranslatedFunctions.h.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <os/lock.h>

#include <Shaders/TranslatedFunctions.h>
#include <Platform/Log.h>

#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#if __has_feature(objc_arc)
#error "TranslatedFunctions.mm uses manual retain/release: its replacement returns a retained function, as the method it replaces does"
#endif

namespace bg3se::mac {

namespace {

NSString* const kPrefix = @"bg3mse_";

// nil for bytes that are not text: nothing is made a key or a source of that.
NSString* TextOf(std::string_view text)
{
    return [[[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] autorelease];
}

using Digest = std::array<uint8_t, CC_SHA256_DIGEST_LENGTH>;

// A source that is kept and not compiled yet.
struct Waiting
{
    std::string Source;
    std::string Material;
    // Shared by the sources that were kept together.
    std::shared_ptr<std::function<void()>> Failed;
    std::shared_ptr<std::atomic<bool>> FailureSaid;
};

os_unfair_lock gLock = OS_UNFAIR_LOCK_INIT;
NSMutableDictionary<NSString*, id<MTLFunction>>* gFunctions;     // guarded by gLock
NSMutableDictionary<NSString*, NSString*>* gMaterials;           // whose each function is; guarded by gLock
NSMutableSet<NSString*>* gSaid;                                  // the materials the log has named; guarded by gLock
// Never destroyed, like the dictionaries: the queue that compiles and the game's own threads may be in them
// while the process leaves through exit().
auto& gWaiting = *new std::unordered_map<std::string, std::shared_ptr<Waiting const>>;    // by the function's name; guarded by gLock
auto& gCompiledFrom = *new std::unordered_map<std::string, Digest>;      // the source each named function was compiled from; guarded by gLock
std::atomic<IMP> gOriginal{ nullptr };
std::atomic<uint64_t> gAsked{ 0 }, gAnswered{ 0 };
std::atomic<bool> gMissed{ false };

using NewFunction = id (*)(id, SEL, NSString*);

Digest DigestOf(std::string const& source)
{
    Digest digest;
    CC_SHA256(source.data(), (CC_LONG)source.size(), digest.data());
    return digest;
}

// The dictionaries exist. Called with gLock held.
void Prepare()
{
    if (!gFunctions) gFunctions = [NSMutableDictionary new];
    if (!gMaterials) gMaterials = [NSMutableDictionary new];
    if (!gSaid) gSaid = [NSMutableSet new];
}

id<MTLLibrary> Compile(id<MTLDevice> device, std::string const& source, std::string& why)
{
    MTLCompileOptions* options = [[MTLCompileOptions new] autorelease];
    options.languageVersion = MTLLanguageVersion2_3;
    // A vertex function's position is marked invariant, as the game's own are, and the game's own were compiled so
    // that it is: with nothing fused or reordered beyond what the source writes. A translated function is compiled
    // the same way, or it puts a vertex a last bit, and where a number of chance is made of it a long way, from
    // where the game's function for the same shader puts it.
    options.preserveInvariance = YES;
    NSError* error = nil;
    NSString* text = TextOf(source);
    if (!text) { why = "the source is not text"; return nil; }
    id<MTLLibrary> library = [device newLibraryWithSource:text options:options error:&error];
    if (!library) why = std::string("the Metal compiler refused the source: ") + (error.localizedDescription.UTF8String ?: "");
    return [library autorelease];
}

// Compiles one source and puts every function of its library that is ours in `found`.
bool CompileInto(id<MTLDevice> device, std::string const& source, NSMutableDictionary<NSString*, id<MTLFunction>>* found, std::string& why)
{
    id<MTLLibrary> library = Compile(device, source, why);
    if (!library) return false;
    for (NSString* name in library.functionNames) {
        if (![name hasPrefix:kPrefix]) continue;
        // Through the library's own method, installed or not.
        IMP original = gOriginal.load(std::memory_order_acquire);
        id<MTLFunction> function = original ? ((NewFunction)original)(library, @selector(newFunctionWithName:), name)
                                            : [library newFunctionWithName:name];
        if (!function) { why = std::string("a compiled function is not in its library: ") + name.UTF8String; return false; }
        found[name] = function;
        [function release];
    }
    return true;
}

// Makes the functions of `found` the ones the game is answered with. `from`:
// the source the function of each name was compiled from, where that is known.
void Publish(NSDictionary<NSString*, id<MTLFunction>>* found, std::string_view material,
    std::unordered_map<std::string, Digest> const* from = nullptr)
{
    NSString* whose = material.empty() ? nil
        : [[[NSString alloc] initWithBytes:material.data() length:material.size() encoding:NSUTF8StringEncoding] autorelease];
    os_unfair_lock_lock(&gLock);
    Prepare();
    [gFunctions addEntriesFromDictionary:found];
    for (NSString* name in found) {
        if (whose) [gMaterials setObject:whose forKey:name];
        std::string key = name.UTF8String;
        gWaiting.erase(key);
        auto digest = from ? from->find(key) : decltype(from->end()){};
        if (from && digest != from->end()) gCompiledFrom[key] = digest->second;
        else gCompiledFrom.erase(key);
    }
    os_unfair_lock_unlock(&gLock);
}

// Compiles each of `sources` (several at a time: the compiler is a service of the system's and takes them
// side by side) into `found`. False, with the first reason in order, when one does not compile.
bool CompileAll(std::span<std::string const* const> sources, NSMutableDictionary<NSString*, id<MTLFunction>>* found, std::string& why)
{
    if (sources.empty()) return true;
    id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
    if (!device) { why = "no Metal device"; return false; }

    size_t count = sources.size();
    std::vector<NSMutableDictionary<NSString*, id<MTLFunction>>*> each(count, nil);
    std::vector<std::string> reasons(count);
    std::vector<uint8_t> compiled(count, 0);
    auto* eachOut = each.data();
    auto* reasonsOut = reasons.data();
    auto* compiledOut = compiled.data();
    auto const* in = sources.data();
    dispatch_apply(count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
        @autoreleasepool {
            eachOut[i] = [NSMutableDictionary new];
            compiledOut[i] = CompileInto(device, *in[i], eachOut[i], reasonsOut[i]);
        }
    });
    bool ok = true;
    for (size_t i = 0; i < count; i++) {
        if (!compiled[i] && ok) {
            ok = false;
            why = reasons[i];
        }
        if (ok) [found addEntriesFromDictionary:each[i]];
        [each[i] release];
    }
    return ok;
}

// The function of a source that is waiting, compiled now; nil when no source waits under that name or it does
// not compile. What it returns is the caller's to release.
//
// `asked`: the game wants the function now. The .lsf and the stubs of a kept material are served before its
// sources are compiled, so there is nothing else to give the game by then: a source that does not compile is
// tried once more, the compiler being a service of the system's that can fail for a moment, and given up
// after that. One that fails in the background stays as it is for the game's own request. Translating the
// material again would not help: what was kept is what this build makes of the same files.
id<MTLFunction> CompileWaiting(NSString* name, bool asked)
{
    std::string key = name.UTF8String;
    for (;;) {
        std::shared_ptr<Waiting const> waiting;
        os_unfair_lock_lock(&gLock);
        id<MTLFunction> function = [[gFunctions objectForKey:name] retain];
        if (!function) {
            if (auto it = gWaiting.find(key); it != gWaiting.end()) waiting = it->second;
        }
        os_unfair_lock_unlock(&gLock);
        if (function || !waiting) return function;

        bool current = false;
        @autoreleasepool {
            std::string why;
            NSMutableDictionary<NSString*, id<MTLFunction>>* found = [NSMutableDictionary dictionary];
            id<MTLFunction> made = nil;
            for (int attempt = 0; !made && attempt < (asked ? 2 : 1); attempt++) {
                [found removeAllObjects];
                why.clear();
                id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
                if (!device) why = "no Metal device";
                else if (CompileInto(device, waiting->Source, found, why) && !(made = found[name])) why = "its source compiles without it";
            }
            Digest digest = DigestOf(waiting->Source);

            // Only while the source that was compiled is still the one that waits under the name: another
            // material's, kept or compiled since under the same name, is not answered with this one's.
            os_unfair_lock_lock(&gLock);
            auto it = gWaiting.find(key);
            current = it != gWaiting.end() && it->second == waiting;
            if (current && made) {
                Prepare();
                [gFunctions setObject:made forKey:name];
                gCompiledFrom[key] = digest;
                gWaiting.erase(it);
                function = [made retain];
            } else if (current && asked) {
                gWaiting.erase(it);     // not asked for again: the source is what it is
            }
            os_unfair_lock_unlock(&gLock);

            if (!made) {
                char const* whose = waiting->Material.empty() ? "a material that was not named" : waiting->Material.c_str();
                if (current && asked) {
                    Log(LogLevel::Error, "[Shaders] %s of %s was kept from an earlier run and does not compile now (%s): what the game draws with it is missing",
                        key.c_str(), whose, why.c_str());
                } else if (current) {
                    Log(LogLevel::Warning, "[Shaders] %s of %s was kept from an earlier run and did not compile in the background (%s): it is tried again when the game asks for it",
                        key.c_str(), whose, why.c_str());
                }
                // What was kept is not kept on: the next start translates the material afresh.
                if (waiting->FailureSaid && !waiting->FailureSaid->exchange(true) && waiting->Failed && *waiting->Failed) (*waiting->Failed)();
            }
        }
        if (current) return function;
        // Something else has come under the name meanwhile: that is what is answered with.
    }
}

// Where the sources that wait are compiled before the game asks: one batch after another.
dispatch_queue_t BackgroundQueue()
{
    static dispatch_queue_t queue = dispatch_queue_create("bg3mse.shaders.compile",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    return queue;
}

// -[_MTLLibrary newFunctionWithName:], with our functions behind the library's own.
id NewFunctionWithName(id self, SEL _cmd, NSString* name)
{
    id function = ((NewFunction)gOriginal.load(std::memory_order_acquire))(self, _cmd, name);
    if (function || ![name isKindOfClass:[NSString class]] || ![name hasPrefix:kPrefix]) return function;

    gAsked.fetch_add(1, std::memory_order_relaxed);
    // The caller owns what a "new" method returns. One whose source still waits is compiled here and now.
    id<MTLFunction> ours = CompileWaiting(name, true);
    // Said at once, so that the log of a short run tells whether the game came
    // for a translated function and whether it got it: the first time for each
    // material, by the material's name.
    if (ours) {
        gAnswered.fetch_add(1, std::memory_order_relaxed);
        NSString* material = nil;
        os_unfair_lock_lock(&gLock);
        NSString* whose = [gMaterials objectForKey:name] ?: @"a material that was not named";
        if (![gSaid containsObject:whose]) {
            [gSaid addObject:whose];
            material = [whose retain];      // kept past the lock, and released below: this thread may have no pool
        }
        os_unfair_lock_unlock(&gLock);
        if (material) {
            Log(LogLevel::Info, "[Shaders] the game took a translated function of %s for the first time (%s)", material.UTF8String,
                name.UTF8String);
            [material release];
        }
    } else if (!gMissed.exchange(true, std::memory_order_relaxed)) {
        Log(LogLevel::Warning, "[Shaders] the game asked its Metal libraries for %s, a translated function that is not there: what it draws with it is missing",
            name.UTF8String);
    }
    return ours;
}

}

bool InstallTranslatedFunctions(std::string& why)
{
    @autoreleasepool {
        if (gOriginal.load(std::memory_order_acquire)) return true;
        id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
        if (!device) { why = "no Metal device"; return false; }

        // The class of a library is the device's to choose: taken from one made here.
        id<MTLLibrary> probe = Compile(device, "#include <metal_stdlib>\nvertex float4 bg3mse_probe() { return float4(0.0); }\n", why);
        if (!probe) return false;
        Method method = class_getInstanceMethod(object_getClass(probe), @selector(newFunctionWithName:));
        if (!method) { why = "a Metal library has no newFunctionWithName:"; return false; }

        os_unfair_lock_lock(&gLock);
        if (!gOriginal.load(std::memory_order_acquire)) {
            Prepare();
            // The replacement reads gOriginal: it is set before the method can reach it.
            gOriginal.store(method_getImplementation(method), std::memory_order_release);
            method_setImplementation(method, (IMP)NewFunctionWithName);
        }
        os_unfair_lock_unlock(&gLock);
        return true;
    }
}

bool AddTranslatedFunctions(std::span<std::string const> sources, std::string& why, std::string_view material)
{
    @autoreleasepool {
        std::vector<std::string const*> all;
        for (auto const& source : sources) all.push_back(&source);
        NSMutableDictionary<NSString*, id<MTLFunction>>* found = [NSMutableDictionary dictionary];
        if (!CompileAll(all, found, why)) return false;
        Publish(found, material);
        return true;
    }
}

bool AddTranslatedFunctions(std::span<TranslatedSource const> sources, std::string& why, std::string_view material)
{
    @autoreleasepool {
        // A function that was compiled from this very source already is the same function: a material's passes
        // share stages, and so do materials.
        std::unordered_map<std::string, Digest> from;
        std::vector<std::string const*> wanted;
        std::vector<TranslatedSource const*> compiledHere;
        os_unfair_lock_lock(&gLock);
        for (auto const& source : sources) {
            auto digest = DigestOf(source.Source);
            if (!from.emplace(source.Name, digest).second) continue;      // twice in one call: once is enough
            auto before = gCompiledFrom.find(source.Name);
            NSString* name = TextOf(source.Name);
            if (name && before != gCompiledFrom.end() && before->second == digest && [gFunctions objectForKey:name]) continue;
            wanted.push_back(&source.Source);
            compiledHere.push_back(&source);
        }
        os_unfair_lock_unlock(&gLock);

        NSMutableDictionary<NSString*, id<MTLFunction>>* found = [NSMutableDictionary dictionary];
        if (!CompileAll(wanted, found, why)) return false;
        for (auto const* source : compiledHere) {
            NSString* name = TextOf(source->Name);
            if (name && found[name]) continue;
            why = "a source compiles without the function it is named for: " + source->Name;
            return false;
        }
        Publish(found, material, &from);
        // Those that were there already are this material's too, for the log.
        if (!material.empty()) {
            NSString* whose = [[[NSString alloc] initWithBytes:material.data() length:material.size() encoding:NSUTF8StringEncoding] autorelease];
            os_unfair_lock_lock(&gLock);
            for (auto const& source : sources) {
                NSString* name = TextOf(source.Name);
                if (name && whose && !found[name]) [gMaterials setObject:whose forKey:name];
            }
            os_unfair_lock_unlock(&gLock);
        }
        return true;
    }
}

void KeepTranslatedSources(std::span<TranslatedSource const> sources, std::string_view material, std::function<void()> failed)
{
    @autoreleasepool {
        auto onFailure = std::make_shared<std::function<void()>>(std::move(failed));
        auto said = std::make_shared<std::atomic<bool>>(false);
        NSString* whose = material.empty() ? nil
            : [[[NSString alloc] initWithBytes:material.data() length:material.size() encoding:NSUTF8StringEncoding] autorelease];
        NSMutableArray<NSString*>* names = [NSMutableArray array];
        // A name that is not text is no function's: left out before anything is locked.
        std::vector<NSString*> named;
        for (auto const& source : sources) named.push_back(TextOf(source.Name));

        os_unfair_lock_lock(&gLock);
        Prepare();
        for (size_t i = 0; i < sources.size(); i++) {
            auto const& source = sources[i];
            NSString* name = named[i];
            if (!name) continue;
            if (whose) [gMaterials setObject:whose forKey:name];
            auto digest = DigestOf(source.Source);
            auto before = gCompiledFrom.find(source.Name);
            if ([gFunctions objectForKey:name]) {
                if (before != gCompiledFrom.end() && before->second == digest) continue;      // compiled from this source already
                // Another source under the name: the function of the one before is not this one's.
                [gFunctions removeObjectForKey:name];
                gCompiledFrom.erase(source.Name);
            }
            auto waiting = std::make_shared<Waiting>();
            waiting->Source = source.Source;
            waiting->Material = std::string(material);
            waiting->Failed = onFailure;
            waiting->FailureSaid = said;
            gWaiting[source.Name] = std::move(waiting);
            [names addObject:name];
        }
        os_unfair_lock_unlock(&gLock);
        if (!names.count) return;

        // Two at a time, below what the game does itself: the game's own shaders go through the same compiler.
        dispatch_async(BackgroundQueue(), ^{
            dispatch_apply(2, DISPATCH_APPLY_AUTO, ^(size_t worker) {
                for (NSUInteger i = worker; i < names.count; i += 2) {
                    [CompileWaiting(names[i], false) release];
                }
            });
        });
    }
}

void WaitForTranslatedSources()
{
    dispatch_sync(BackgroundQueue(), ^{});
}

TranslatedFunctionCounts CountTranslatedFunctions()
{
    TranslatedFunctionCounts counts;
    os_unfair_lock_lock(&gLock);
    counts.Kept = (uint32_t)gFunctions.count;
    counts.Waiting = (uint32_t)gWaiting.size();
    os_unfair_lock_unlock(&gLock);
    counts.Asked = gAsked.load(std::memory_order_relaxed);
    counts.Answered = gAnswered.load(std::memory_order_relaxed);
    return counts;
}

}
