#!/bin/sh
# Fetch the dependencies the Mac build needs into External/ (the same ones, at
# the same versions, pull-externals.bat fetches on Windows), build protobuf,
# and run upstream's generators. Safe to re-run. NoesisGUI headers are not
# fetched: pass their Include directory to CMake as BG3MSE_NOESIS_INCLUDE_DIR.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
ext="$root/External"

fetch() {  # fetch <dir> <url> <ref>
    if [ ! -d "$ext/$1/.git" ]; then
        git clone --quiet "$2" "$ext/$1"
    fi
    # A clone from before the version asked for changed does not have it yet.
    git -C "$ext/$1" checkout --quiet "$3" 2>/dev/null || {
        git -C "$ext/$1" fetch --quiet --tags origin
        git -C "$ext/$1" checkout --quiet "$3"
    }
    echo "External/$1 @ $3"
}

fetch glm        https://github.com/g-truc/glm                 1.0.3
fetch lua        https://github.com/Norbyte/lua-dos            4fff4e19954f927a0478055306dadaf71bba02b2
# The commit pull-externals.bat resets to (ImGui 1.93 with Norbyte's changes).
fetch imgui      https://github.com/Norbyte/imgui              0c44880d5f783e83d0e086a9dfc5d8caf055051a
fetch optick     https://github.com/Norbyte/optick             a84a9ac6b23a4cf4ab21ac3d98e05aa475dab34c
fetch rapidjson  https://github.com/tencent/rapidjson          297637bc0a0f00dc504dbc1a47ebdaa577f8ae19
fetch lz4        https://github.com/lz4/lz4                    v1.10.0
fetch SDL        https://github.com/libsdl-org/SDL             release-2.30.6
fetch FastLZ     https://github.com/ariya/FastLZ               0.5.0
# pull-externals.bat takes the archived repository's last commit.
fetch tinycrypt  https://github.com/intel/tinycrypt            cf24c907a61c01bc2c4e1ee0ae24457c27a840a2
fetch stb        https://github.com/nothings/stb               7023e273f1513b68b8d4086077c6faca555d50df
fetch bcdec      https://github.com/iOrange/bcdec              80859ed3b7afb1c527a2a99d70c61457bea72d0c
fetch protobuf   https://github.com/protocolbuffers/protobuf   v36.1
# The abseil release protobuf v36.1 fetches (cmake/dependencies.cmake).
fetch abseil-cpp https://github.com/abseil/abseil-cpp          20250512.1
# The SPIR-V grammar (MIT): which operands of an instruction are ids (Mac/Shaders).
fetch SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers vulkan-sdk-1.4.321.0
# SPIR-V to Metal Shading Language (Apache-2.0), for mod shaders (Mac/Shaders).
fetch SPIRV-Cross   https://github.com/KhronosGroup/SPIRV-Cross   vulkan-sdk-1.4.321.0

# The two packages Norbyte's Lua debug adapter uses (LuaDebugger/packages.config),
# for its macOS build (Mac/LuaDebugger): Newtonsoft.Json at his version, and
# Google.Protobuf at the version of the protoc above, which writes the
# adapter's protocol source. Kept as a folder NuGet restores from, so the
# build itself fetches nothing. The adapter is built only when CMake is given
# a .NET 10 SDK (BG3MSE_DOTNET), which is not fetched here.
package() {  # package <id> <version> <sha256>
    file="$ext/nuget/$1.$2.nupkg"
    if [ ! -f "$file" ] || [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" != "$3" ]; then
        mkdir -p "$ext/nuget"
        curl -fsSL -o "$file" "https://api.nuget.org/v3-flatcontainer/$1/$2/$1.$2.nupkg"
        if [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" != "$3" ]; then
            echo "External/nuget/$1.$2.nupkg is not the package expected (its SHA-256 differs)." >&2
            rm -f "$file"
            exit 1
        fi
    fi
    echo "External/nuget/$1.$2.nupkg"
}

package google.protobuf 3.36.1 e599da30244bfcab4e6ceba7c654668e337b586cd0ca12cba4826c7103695201
package newtonsoft.json 13.0.4 f09081d457405baf35a973fa0c50d6bf272ed683f2568c5a620a49da952f6529

# libprotobuf-lite (what the extender links on Windows), abseil and protoc,
# built from those sources as static libraries with hidden symbols. As on
# Windows the libraries are built with protobuf's defaults (RTTI on) and the
# extender's sources see them with GOOGLE_PROTOBUF_NO_RTTI (CMakeLists.txt).
# Homebrew's abseil, protobuf and zlib must not be picked up. The build runs
# under the machine-wide build lock with four jobs, like every other build.
#
# They are linked into libbg3mse.dylib, so they are compiled for the oldest
# macOS it runs on: BG3MSE_MACOS_MINIMUM, by default the value CMakeLists.txt
# gives it. A build made for another system is made again.
pb_build="$ext/protobuf-build"
min_macos=${BG3MSE_MACOS_MINIMUM:-$(sed -n 's/^set(BG3MSE_MACOS_MINIMUM "\([0-9.]*\)".*/\1/p' "$root/CMakeLists.txt")}
if [ -z "$min_macos" ]; then
    echo "The oldest macOS to build for is not known: CMakeLists.txt does not set BG3MSE_MACOS_MINIMUM as expected." >&2
    exit 1
fi
built_for=$(sed -n 's/^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]*=//p' "$pb_build/CMakeCache.txt" 2>/dev/null || true)
if [ ! -x "$pb_build/protoc" ] || [ ! -f "$pb_build/libprotobuf-lite.a" ] || [ "$built_for" != "$min_macos" ]; then
    cmake -S "$ext/protobuf" -B "$pb_build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$min_macos" \
        -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_C_VISIBILITY_PRESET=hidden \
        -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
        -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
        -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        -DABSL_PROPAGATE_CXX_STD=ON \
        -Dprotobuf_FORCE_FETCH_DEPENDENCIES=ON \
        -DFETCHCONTENT_SOURCE_DIR_ABSL="$ext/abseil-cpp" \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_BUILD_LIBUPB=OFF \
        -Dprotobuf_WITH_ZLIB=OFF \
        -Dprotobuf_INSTALL=OFF
    lockf -k /private/tmp/bg3mse-build.lock \
        cmake --build "$pb_build" -j 4 --target libprotobuf-lite protoc
fi
echo "External/protobuf-build: $("$pb_build/protoc" --version), for macOS $min_macos"

cd "$root/BG3Extender"
python3 make_enumerations.py
python3 make_events.py
python3 make_property_map.py
echo "Generated BG3Extender/GameDefinitions/Generated/"
