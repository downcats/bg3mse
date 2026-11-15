// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

#include <Camera/CameraSites.h>
#include <Camera/camera_settings.h>
#include <Platform/Log.h>

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>

// bg3mse-camera: checks a game binary for the camera tweaks without the game,
// and what the camera module makes of its settings.
//
//   bg3mse-camera check <Game.app>      whether the camera hooks would install on this game binary
//   bg3mse-camera defaults              the settings file the camera module writes when there is none
//                                       (the build ships it as camera/NativeCameraTweaks.default.toml)
//   bg3mse-camera settings <file>       what the camera module reads from a settings file: the
//                                       master switch, and how many lines it ignores

using namespace bg3se::mac;

namespace {

int Usage()
{
    fprintf(stderr, "usage: bg3mse-camera check <Game.app>\n"
                    "       bg3mse-camera defaults\n"
                    "       bg3mse-camera settings <file>\n");
    return 2;
}

// As the camera module reads its file (camera_tweaks.c): the first 64 KB, up to a NUL.
int Settings(std::string const& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        return 1;
    }
    std::string text(64 * 1024, '\0');
    text.resize(fread(text.data(), 1, text.size(), f));
    fclose(f);
    CameraSettings s;
    camera_settings_defaults(&s);
    int first = 0;
    int ignored = camera_settings_parse(&s, text.c_str(), &first);
    printf("Enabled: %s\n", s.enabled ? "true" : "false");
    printf("ignored: %d", ignored);
    if (ignored) printf(" (first at line %d)", first);
    printf("\n");
    return 0;
}

int Check(std::string const& app)
{
    std::string binary = app + "/Contents/MacOS/Baldur's Gate 3";
    int fd = open(binary.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "cannot open %s\n", binary.c_str());
        return 1;
    }
    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) return 1;

    GameImage image;
    if (!image.OpenFile({ static_cast<uint8_t const*>(data), (size_t)st.st_size })) {
        fprintf(stderr, "%s has no readable arm64 symbol table\n", binary.c_str());
        return 1;
    }

    auto camera = CheckCameraSites(image);
    for (auto const& fn : camera.Functions) {
        printf("%-50s %5u instructions, shape %016llx%s\n", fn.Name, fn.Found.Instructions, (unsigned long long)fn.Found.Hash,
            !fn.Present ? "  MISSING" : !fn.Expected.Instructions ? "  (not checked)" : fn.Found == fn.Expected ? "" : "  (differs from the checked shape)");
    }
    if (camera.Ok()) {
        auto const& r = camera.Row;
        printf("camera tweaks: would install\n"
               "  functions (offset from image base): update_gcb %#lx  pitch %#lx  camera_input %#lx  client_input %#lx  get_input_value %#lx  floor %#lx  deltaY stub %#lx\n"
               "  call sites: update %#lx (%08x)  pitch %#lx (%08x)  mouse %#lx (%08x)\n"
               "  globals: switches slot %#llx  controller flag %#lx  input manager %#lx  game control %#lx\n",
            (unsigned long)r.update_gcb_fn, (unsigned long)r.pitch_fn, (unsigned long)r.camera_on_input_fn, (unsigned long)r.client_on_input_fn,
            (unsigned long)r.get_input_value_fn, (unsigned long)r.floor_height_fn, (unsigned long)r.mouse_dy_fn,
            (unsigned long)r.update_gcb_call.site, r.update_gcb_call.expected, (unsigned long)r.pitch_call.site, r.pitch_call.expected,
            (unsigned long)r.mouse_dy_call.site, r.mouse_dy_call.expected,
            (unsigned long long)camera.GlobalSwitchesSlotVA, (unsigned long)r.controller_mode_flag, (unsigned long)r.input_manager_ptr, (unsigned long)r.game_control_ptr);
    } else {
        printf("camera tweaks: would NOT install: %s\n", camera.Problem.c_str());
    }
    return camera.Ok() ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    if (argc == 3 && std::string(argv[1]) == "check") return Check(argv[2]);
    if (argc == 2 && std::string(argv[1]) == "defaults") {
        fputs(camera_settings_default_file(), stdout);
        return fflush(stdout) == 0 ? 0 : 1;
    }
    if (argc == 3 && std::string(argv[1]) == "settings") return Settings(argv[2]);
    return Usage();
}
