// The initializer of libbg3mse-loader.dylib. It is in a file of its own,
// linked as the last file of the image, so that it runs after every other
// initializer of the image, as Mac/Entry.cpp does in libbg3mse.dylib: the
// exit handler it registers must run before the objects those construct
// (the log's) are destroyed, and handlers run in the reverse order of their
// registration.

namespace bg3se::mac::updater {

// Mac/Updater/Loader.cpp.
void StartLoader() noexcept;

}

namespace {

__attribute__((constructor))
void InitializeLoader()
{
    bg3se::mac::updater::StartLoader();
}

}
