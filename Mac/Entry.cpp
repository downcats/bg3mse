// The initializer of libbg3mse.dylib: the one function the image runs when
// the game loads it, before the game's main().
//
// It is in a file of its own, linked as the last file of the image, because
// an image's initializers run in the order of its files, and this one must be
// the last of them: what it starts (Mac/Main.cpp) uses objects of other
// files, among them Norbyte's extender's, whose own initializers construct
// them. The build checks the order on the linked image
// (Mac/Tools/CheckDylib.cmake).

namespace bg3se::mac {

// Mac/Main.cpp. Lets nothing escape: an exception leaving an initializer
// would end the game before it starts.
void StartInGame() noexcept;

}

namespace {

__attribute__((constructor))
void InitializeImage()
{
    bg3se::mac::StartInGame();
}

}
