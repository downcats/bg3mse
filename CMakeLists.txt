# macOS (Apple Silicon) build of the Script Extender.
# The Windows build is BG3Tools.sln; this file does not affect it.
#
#   ./Mac/setup.sh
#   cmake -S . -B build -DBG3MSE_NOESIS_INCLUDE_DIR=/path/to/NoesisGUI-NativeSDK/Include
#   cmake --build build -j 2 && ctest --test-dir build
#
# Give -j a number: a bare -j runs every compile at once. 2 on a 16 GB machine:
# Norbyte's sources are heavy, and the largest of them
# (BG3Extender/Lua/Shared/Proxies/LuaObjectProxies.cpp) has been measured at
# 2.3 to 3.0 GB of compiler memory; the next two take 1.5 and 1.3 GB.
#
# Switches (both OFF by default):
#   -DBG3MSE_CAMERA=ON   builds libbg3mse-camera.dylib (Mac/Camera, GPL-3.0-or-later), which libbg3mse.dylib loads when installed next to it
#   -DBG3MSE_GFX=ON      compiles the graphics fixes (Mac/Gfx) into libbg3mse.dylib
#
# -DBG3MSE_GAME_APP=/path/to/Baldur's\ Gate\ 3.app makes ctest also run the check
# tools on that game binary (read from disk; the game is not started).

cmake_minimum_required(VERSION 3.24)

# The oldest macOS the build runs on. The default is the game's own minimum
# (LC_BUILD_VERSION of its arm64 image: minos 11.0), so that libbg3mse.dylib
# loads wherever the game starts; with BG3MSE_GAME_APP, ctest compares the two.
# It is the deployment target of everything compiled here, which CMake reads
# when project() enables the languages: it is set before that, and a value
# given later would only be a cache entry nothing uses. The libraries
# Mac/setup.sh builds (protobuf, abseil) are compiled for the same system:
# the script reads the default from the line below.
set(BG3MSE_MACOS_MINIMUM "11.0" CACHE STRING "The oldest macOS the build runs on (the default is the game's own minimum)")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${BG3MSE_MACOS_MINIMUM}" CACHE STRING "Set from BG3MSE_MACOS_MINIMUM" FORCE)

project(bg3mse LANGUAGES C CXX OBJC OBJCXX ASM)

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    message(FATAL_ERROR "This build targets arm64 macOS only.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BG3MSE_NOESIS_INCLUDE_DIR "" CACHE PATH "Include directory of the NoesisGUI Native SDK 3.1.7")
set(BG3MSE_GAME_APP "" CACHE PATH "The game's .app; when set, ctest also checks that every hook and site would install on its binary")
if(NOT EXISTS "${BG3MSE_NOESIS_INCLUDE_DIR}/NsGui/GridLength.h")
    message(FATAL_ERROR "Set BG3MSE_NOESIS_INCLUDE_DIR to the NoesisGUI Native SDK's Include directory "
                        "(got '${BG3MSE_NOESIS_INCLUDE_DIR}').")
endif()

# The game links NoesisGUI built with clang. The SDK headers declare
# TypeProperty::GetCopy before Get, which matches MSVC's vtable order only: the
# game's vtable has Get(ptr) before Get(ptr, dest). The build compiles against a
# copy of the headers with that order restored.
#
# The copy is brought up to date, not made again: a file is written only when
# its content would change, so configuring a second time touches nothing (the
# precompiled headers stay valid, and a build running at the same time reads
# files that do not change under it).
set(NOESIS_INCLUDE "${CMAKE_BINARY_DIR}/noesis_include")
set(NOESIS_PATCHED "")
# Replaces `old` by `new` in the text of an SDK header. The patched text is
# kept in NOESIS_TEXT_<file> until the copy below writes it.
function(noesis_patch file old new)
    string(MAKE_C_IDENTIFIER "${file}" id)
    if(file IN_LIST NOESIS_PATCHED)
        set(text "${NOESIS_TEXT_${id}}")
    else()
        file(READ "${BG3MSE_NOESIS_INCLUDE_DIR}/${file}" text)
        list(APPEND NOESIS_PATCHED "${file}")
        set(NOESIS_PATCHED "${NOESIS_PATCHED}" PARENT_SCOPE)
    endif()
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "NoesisGUI headers: expected text not found in ${file} (different SDK version?)")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    set(NOESIS_TEXT_${id} "${text}" PARENT_SCOPE)
endfunction()
noesis_patch(NsCore/TypeProperty.h
"    /// Copies the value of the pointer to the given address
    virtual void GetCopy(const void* ptr, void* dest) const;

    /// Gets a pointer to the value of the property
    virtual const void* Get(const void* ptr) const;
"
"    /// Gets a pointer to the value of the property
    virtual const void* Get(const void* ptr) const;

    /// Copies the value of the pointer to the given address
    virtual void GetCopy(const void* ptr, void* dest) const;
")
noesis_patch(NsCore/TypePropertyImpl.h
    "    void Get(const void* ptr, void* dest) const override;"
    "    void GetCopy(const void* ptr, void* dest) const override;")
noesis_patch(NsCore/TypePropertyImpl.inl
    "void TypePropertyFunction<C,T>::Get(const void* ptr, void* dest) const"
    "void TypePropertyFunction<C,T>::GetCopy(const void* ptr, void* dest) const")

file(GLOB_RECURSE noesis_files RELATIVE "${BG3MSE_NOESIS_INCLUDE_DIR}" "${BG3MSE_NOESIS_INCLUDE_DIR}/*")
foreach(file IN LISTS noesis_files)
    if(file IN_LIST NOESIS_PATCHED)
        string(MAKE_C_IDENTIFIER "${file}" id)
        set(current "")
        if(EXISTS "${NOESIS_INCLUDE}/${file}")
            file(READ "${NOESIS_INCLUDE}/${file}" current)
        endif()
        if(NOT "${current}" STREQUAL "${NOESIS_TEXT_${id}}")
            file(WRITE "${NOESIS_INCLUDE}/${file}" "${NOESIS_TEXT_${id}}")
        endif()
    else()
        get_filename_component(dir "${NOESIS_INCLUDE}/${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${dir}")
        file(COPY_FILE "${BG3MSE_NOESIS_INCLUDE_DIR}/${file}" "${NOESIS_INCLUDE}/${file}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
# What an earlier configure copied from another SDK directory and this one does not have.
file(GLOB_RECURSE noesis_present RELATIVE "${NOESIS_INCLUDE}" "${NOESIS_INCLUDE}/*")
list(REMOVE_ITEM noesis_present ${noesis_files})
foreach(file IN LISTS noesis_present)
    file(REMOVE "${NOESIS_INCLUDE}/${file}")
endforeach()

set(EXT "${CMAKE_SOURCE_DIR}/External")
foreach(dep glm lua imgui optick rapidjson lz4 SDL FastLZ tinycrypt stb bcdec protobuf abseil-cpp
        protobuf-build/protoc protobuf-build/libprotobuf-lite.a)
    if(NOT EXISTS "${EXT}/${dep}")
        message(FATAL_ERROR "External/${dep} is missing: run Mac/setup.sh first.")
    endif()
endforeach()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/BG3Extender/GameDefinitions/Generated/Enumerations.inl")
    message(FATAL_ERROR "Generated sources are missing: run Mac/setup.sh first.")
endif()
# protobuf and abseil are linked into the image: built for a later macOS than
# the image's, they may use what the older system does not have. (The check
# after the link reads every object of them: Mac/Tools/CheckDylib.cmake.)
set(protobuf_built_for "")
if(EXISTS "${EXT}/protobuf-build/CMakeCache.txt")
    file(STRINGS "${EXT}/protobuf-build/CMakeCache.txt" protobuf_built_for REGEX "^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]+=")
    string(REGEX REPLACE "^[^=]*=" "" protobuf_built_for "${protobuf_built_for}")
endif()
if(NOT protobuf_built_for MATCHES "^[0-9.]+$" OR protobuf_built_for VERSION_GREATER BG3MSE_MACOS_MINIMUM)
    message(FATAL_ERROR "External/protobuf-build was built for macOS '${protobuf_built_for}', not for ${BG3MSE_MACOS_MINIMUM} "
                        "(BG3MSE_MACOS_MINIMUM) or earlier: run Mac/setup.sh, which builds it again.")
endif()

# The protocol sources, generated into the build directory with the protoc
# Mac/setup.sh builds, as generate-proto.bat generates them on Windows.
set(PROTOC "${EXT}/protobuf-build/protoc")
set(PROTO_OUT "${CMAKE_BINARY_DIR}/proto")
set(PROTO_SOURCES)
foreach(proto Extender/Shared/ExtenderProtocol Lua/Debugger/LuaDebug Osiris/Debugger/osidebug)
    get_filename_component(dir "${proto}" DIRECTORY)
    add_custom_command(
        OUTPUT "${PROTO_OUT}/${proto}.pb.cc" "${PROTO_OUT}/${proto}.pb.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROTO_OUT}/${dir}"
        COMMAND "${PROTOC}" "--proto_path=${CMAKE_SOURCE_DIR}/BG3Extender" "--proto_path=${EXT}/protobuf/src"
                "--cpp_out=${PROTO_OUT}"
                "${CMAKE_SOURCE_DIR}/BG3Extender/${proto}.proto"
        DEPENDS "${CMAKE_SOURCE_DIR}/BG3Extender/${proto}.proto" "${PROTOC}"
        VERBATIM)
    list(APPEND PROTO_SOURCES "${PROTO_OUT}/${proto}.pb.cc")
endforeach()
add_custom_target(bg3se_proto ALL DEPENDS ${PROTO_SOURCES})

# Upstream's headers, compiled as upstream's "Game Release" configuration does.
add_library(bg3se_headers INTERFACE)
target_include_directories(bg3se_headers SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/Mac/Shim"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/BG3Extender"
    "${EXT}/glm"
    "${EXT}/lua/src"
    "${EXT}/imgui"
    "${EXT}/optick/src"
    "${EXT}/rapidjson/include"
    "${EXT}/SDL/include"
    "${NOESIS_INCLUDE}")
target_compile_definitions(bg3se_headers INTERFACE
    OSI_EOCAPP OSIINTERFACE_EXPORTS OSI_EXTENSION_BUILD
    ENABLE_SDL ENABLE_IMGUI ENABLE_UI NS_STATIC_LIBRARY NDEBUG
    GLM_FORCE_ALIGNED_GENTYPES _MSC_EXTENSIONS)
# -fno-strict-aliasing: whoever includes Norbyte's headers compiles their
# inline code, which is written for a compiler without type-based alias
# analysis (see BG3SE_DIALECT).
target_compile_options(bg3se_headers INTERFACE -fms-extensions -fno-strict-aliasing)

# The build's flavour: what publish.ps1 writes into Extender/BuildInfo.h and
# CoreLib/Config.h for the channel it publishes. Release, what players get:
# no SE_IS_DEVELOPER_BUILD (no Optick, no "experimental version" notice in the
# main menu) and no SE_RELEASE_ASSERTS (se_assert is compiled out). Devel and
# Nightly define both. The files in the repository are publish.ps1's output
# for Devel on Norbyte's machine; the generated pair is put ahead of them on
# every include path that has them (checked at the end of the configuration).
# SE_CERTIFICATION_ID is left undefined, as publish.ps1 leaves it without
# Norbyte's certification file.
set(BG3MSE_CHANNEL "Release" CACHE STRING "The Script Extender's build flavour: Release, Devel or Nightly")
set_property(CACHE BG3MSE_CHANNEL PROPERTY STRINGS Release Devel Nightly)
if(NOT BG3MSE_CHANNEL MATCHES "^(Release|Devel|Nightly)$")
    message(FATAL_ERROR "BG3MSE_CHANNEL is Release, Devel or Nightly, not '${BG3MSE_CHANNEL}'")
endif()
set(FLAVOUR_DIR "${CMAKE_BINARY_DIR}/flavour")
if(BG3MSE_CHANNEL STREQUAL "Release")
    set(FLAVOUR_DEVELOPER "#undef SE_IS_DEVELOPER_BUILD")
    set(FLAVOUR_ASSERTS "#undef SE_RELEASE_ASSERTS")
else()
    set(FLAVOUR_DEVELOPER "#define SE_IS_DEVELOPER_BUILD")
    set(FLAVOUR_ASSERTS "#define SE_RELEASE_ASSERTS")
endif()
file(CONFIGURE OUTPUT "${FLAVOUR_DIR}/Extender/BuildInfo.h" @ONLY CONTENT
    "${FLAVOUR_DEVELOPER}\n#define SE_BUILD_CHANNEL \"${BG3MSE_CHANNEL}\"\n#undef SE_CERTIFICATION_ID\n#define BG3MSE_FLAVOUR_BUILD_INFO\n")
file(CONFIGURE OUTPUT "${FLAVOUR_DIR}/CoreLib/Config.h" @ONLY CONTENT
    "${FLAVOUR_ASSERTS}\n#define BG3MSE_FLAVOUR_CONFIG\n")
target_include_directories(bg3se_headers SYSTEM BEFORE INTERFACE "${FLAVOUR_DIR}")
# A target whose own include path has Norbyte's directories must have the
# flavour's ahead of them.
function(bg3mse_check_flavour_paths)
    get_property(targets DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        foreach(property INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
            get_target_property(paths ${target} ${property})
            if(NOT paths)
                continue()
            endif()
            list(FIND paths "${FLAVOUR_DIR}" flavour)
            foreach(norbyte "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/BG3Extender")
                list(FIND paths "${norbyte}" at)
                if(at GREATER -1 AND (flavour EQUAL -1 OR flavour GREATER at))
                    message(FATAL_ERROR "${target}: ${property} has ${norbyte} without ${FLAVOUR_DIR} ahead of it")
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL bg3mse_check_flavour_paths)
message(STATUS "Script Extender build flavour: ${BG3MSE_CHANNEL}")

option(BG3MSE_CAMERA "Build the camera module, libbg3mse-camera.dylib (Mac/Camera)" OFF)
option(BG3MSE_GFX "Compile the graphics fixes (Mac/Gfx) into libbg3mse.dylib" OFF)
option(BG3MSE_TESTS "Build the tests, the developer tools and the release target; OFF builds only what a release ships" ON)

set(BG3MSE_WARNINGS -Wall -Wextra -Wno-unused-parameter)

# Debug information and the version stamp, for every target defined below (Mac/Release).
include(Mac/Release/Configure.cmake)

# The Mach exception messages the fault handling sends and receives: MIG
# stubs generated from the SDK's mach_exc.defs (libSystem exports only the
# client of exc.defs, whose codes are 32-bit).
if(CMAKE_OSX_SYSROOT)
    set(BG3MSE_SDK "${CMAKE_OSX_SYSROOT}")
else()
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE BG3MSE_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
find_program(BG3MSE_MIG mig REQUIRED)
set(MACH_EXC_DEFS "${BG3MSE_SDK}/usr/include/mach/mach_exc.defs")
set(MACH_EXC_DIR "${CMAKE_BINARY_DIR}/mach_exc")
file(MAKE_DIRECTORY "${MACH_EXC_DIR}")
add_custom_command(
    OUTPUT "${MACH_EXC_DIR}/mach_exc.h" "${MACH_EXC_DIR}/mach_excUser.c" "${MACH_EXC_DIR}/mach_excServer.h" "${MACH_EXC_DIR}/mach_excServer.c"
    COMMAND "${BG3MSE_MIG}" -arch arm64 -isysroot "${BG3MSE_SDK}"
        -header mach_exc.h -user mach_excUser.c -sheader mach_excServer.h -server mach_excServer.c "${MACH_EXC_DEFS}"
    WORKING_DIRECTORY "${MACH_EXC_DIR}"
    DEPENDS "${MACH_EXC_DEFS}"
    VERBATIM)

# The platform layer: symbols, code search, hooks, settings, the log and
# fault handling. It includes nothing of the Script Extender, so the camera
# module can link it.
add_library(bg3mse_platform STATIC
    Mac/Platform/Log.cpp
    Mac/Platform/Process.cpp
    Mac/Platform/Symbols.cpp
    Mac/Platform/CodeShape.cpp
    Mac/Platform/CodeSearch.cpp
    Mac/Platform/Config.cpp
    Mac/Platform/CodeWriter.cpp
    Mac/Platform/CodePatchC.cpp
    Mac/Platform/Islands.cpp
    Mac/Platform/HookPlanner.cpp
    Mac/Platform/EntryHook.cpp
    Mac/Platform/FunctionExtents.cpp
    Mac/Platform/FaultCapture.cpp
    Mac/Platform/FaultGuard.cpp
    Mac/Platform/FaultGuardRun.S
    Mac/Platform/FaultObserver.cpp
    Mac/Platform/macho_symbols.c
    "${MACH_EXC_DIR}/mach_excUser.c"
    "${MACH_EXC_DIR}/mach_excServer.c")
target_include_directories(bg3mse_platform PUBLIC Mac)
target_include_directories(bg3mse_platform PRIVATE "${MACH_EXC_DIR}")
target_compile_options(bg3mse_platform PRIVATE ${BG3MSE_WARNINGS})

# The Detours transaction API upstream's code calls (Mac/Shim/detours.h), on
# the platform layer's entry hooks. A library of its own: it needs the Windows
# names of Mac/Shim, which bg3mse_platform (linked by the camera module too)
# does not include, and nothing of the Script Extender.
add_library(bg3mse_detours STATIC Mac/Shim/detours.cpp)
target_include_directories(bg3mse_detours SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/Mac/Shim")
target_link_libraries(bg3mse_detours PUBLIC bg3mse_platform)
target_compile_options(bg3mse_detours PRIVATE ${BG3MSE_WARNINGS})

# Which operands of a SPIR-V instruction are ids, from Khronos' grammar.
set(BG3MSE_SPIRV_GRAMMAR "${EXT}/SPIRV-Headers/include/spirv/unified1/spirv.core.grammar.json")
if(NOT EXISTS "${BG3MSE_SPIRV_GRAMMAR}")
    message(FATAL_ERROR "External/SPIRV-Headers is missing: run Mac/setup.sh")
endif()
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/generated/spirv_operands.inc"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/Mac/Shaders/make_spirv_operands.py" "${BG3MSE_SPIRV_GRAMMAR}"
            "${CMAKE_BINARY_DIR}/generated/spirv_operands.inc"
    DEPENDS "${CMAKE_SOURCE_DIR}/Mac/Shaders/make_spirv_operands.py" "${BG3MSE_SPIRV_GRAMMAR}"
    VERBATIM)

# SPIRV-Cross (Khronos, Apache-2.0): a mod's Vulkan shader to Metal source.
if(NOT EXISTS "${EXT}/SPIRV-Cross/spirv_msl.cpp")
    message(FATAL_ERROR "External/SPIRV-Cross is missing: run Mac/setup.sh")
endif()
add_library(bg3mse_spirv_cross STATIC
    "${EXT}/SPIRV-Cross/spirv_cfg.cpp"
    "${EXT}/SPIRV-Cross/spirv_cross.cpp"
    "${EXT}/SPIRV-Cross/spirv_cross_parsed_ir.cpp"
    "${EXT}/SPIRV-Cross/spirv_cross_util.cpp"
    "${EXT}/SPIRV-Cross/spirv_glsl.cpp"
    "${EXT}/SPIRV-Cross/spirv_msl.cpp"
    "${EXT}/SPIRV-Cross/spirv_parser.cpp")
target_include_directories(bg3mse_spirv_cross SYSTEM PUBLIC "${EXT}/SPIRV-Cross")
# Its names stay out of the dylib's exports and apart from any other copy in the process.
target_compile_definitions(bg3mse_spirv_cross PUBLIC SPIRV_CROSS_NAMESPACE_OVERRIDE=bg3mse_spirv_cross)
target_compile_options(bg3mse_spirv_cross PRIVATE -w -fvisibility=hidden -fvisibility-inlines-hidden)

# Everything of libbg3mse.dylib except its entry point, so the tool and the
# tests run the same code the game does.
add_library(bg3mse_core STATIC
    "${CMAKE_BINARY_DIR}/generated/spirv_operands.inc"
    Mac/Shaders/GameInterface.cpp
    Mac/Shaders/InterfaceData.cpp
    Mac/Shaders/MetalFunctions.mm
    Mac/Shaders/ShaderMapping.cpp
    Mac/Shaders/SpirvIdentity.cpp
    Mac/Shaders/SpirvShader.cpp
    Mac/Shaders/SpirvTextureTable.cpp
    Mac/Shaders/TranslatedFunctions.mm
    Mac/Shaders/Translation.cpp
    Mac/Shaders/TranslationCache.cpp
    Mac/Platform/DebugLog.cpp
    Mac/Platform/FileReaderHook.cpp
    Mac/Shaders/PakFile.cpp
    Mac/Shaders/Provision.cpp
    Mac/Shaders/ShaderProvision.cpp
    Mac/Shaders/identity.c
    Mac/Shaders/identity_data.c
    Mac/Shaders/lsf.c
    Mac/Shaders/lsf_write.c
    Mac/Shaders/lsf_relayout.c
    "${EXT}/lz4/lib/lz4.c")
target_include_directories(bg3mse_core SYSTEM PUBLIC "${EXT}/lz4/lib")
target_include_directories(bg3mse_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(bg3mse_core PUBLIC bg3mse_platform bg3mse_detours bg3se_headers z "-framework Foundation" "-framework Metal")
target_link_libraries(bg3mse_core PRIVATE bg3mse_spirv_cross)
# These include what they use, not the Script Extender's headers.
set_source_files_properties(Mac/Shaders/MetalFunctions.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-fobjc-arc")
set_source_files_properties(Mac/Shaders/SpirvShader.cpp Mac/Shaders/TranslatedFunctions.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_precompile_headers(bg3mse_core PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/BG3Extender/stdafx.h>")
target_compile_options(bg3mse_core PRIVATE ${BG3MSE_WARNINGS})

if(BG3MSE_GFX)
    add_library(bg3mse_gfx STATIC
        Mac/Gfx/Gfx.cpp
        Mac/Gfx/GfxLog.cpp
        Mac/Gfx/GfxPatch.cpp
        Mac/Gfx/GfxSites.cpp
        Mac/Gfx/GfxJitter.cpp
        Mac/Gfx/GfxJitterSample.cpp
        Mac/Gfx/GfxJitterStubs.S
        Mac/Gfx/GfxMetal.mm
        Mac/Gfx/GfxMetalCalls.cpp
        Mac/Gfx/GfxShaderCheck.mm
        Mac/Gfx/GfxTemporal.mm
        Mac/Gfx/GfxTemporalLogic.cpp
        Mac/Gfx/GfxShadows.mm
        Mac/Gfx/GfxUiRing.mm)
    # MetalFX came with macOS 13 and the dylib loads on 11: linked weakly, and
    # used only where the system has it (Mac/Gfx/GfxTemporal.mm).
    target_link_libraries(bg3mse_gfx PUBLIC bg3mse_core "-framework Foundation" "-framework Metal" "-weak_framework MetalFX")
    target_compile_definitions(bg3mse_gfx INTERFACE BG3MSE_GFX=1)
    target_precompile_headers(bg3mse_gfx REUSE_FROM bg3mse_core)
    # The Objective-C++ files include what they use (Gfx/GfxLog.h).
    set_source_files_properties(Mac/Gfx/GfxMetal.mm Mac/Gfx/GfxMetalCalls.cpp Mac/Gfx/GfxShaderCheck.mm Mac/Gfx/GfxShadows.mm Mac/Gfx/GfxTemporal.mm
        Mac/Gfx/GfxUiRing.mm
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_options(bg3mse_gfx PRIVATE ${BG3MSE_WARNINGS})
    set(BG3MSE_MAIN_LIBS bg3mse_gfx)
else()
    set(BG3MSE_MAIN_LIBS bg3mse_core)
endif()

# libbg3mse.dylib. What it is linked from is said at the end of this file,
# where every library exists ("What libbg3mse.dylib is made of").
add_library(bg3mse SHARED Mac/Main.cpp)
# Its version (Mac/Release/Configure.cmake).
target_link_libraries(bg3mse PRIVATE bg3mse_version)
set_target_properties(bg3mse PROPERTIES MACHO_CURRENT_VERSION "${BG3MSE_VERSION}")
# The image's initializer (Mac/Entry.cpp) must be the last initializer of the
# image, and initializers run in the order of the files on the link line: it
# is a library of its own, loaded whole as the last thing linked.
add_library(bg3mse_entry STATIC Mac/Entry.cpp)
target_compile_options(bg3mse_entry PRIVATE ${BG3MSE_WARNINGS})
# Loaded for its initializer. It exports Norbyte's native API
# (Extender/Shared/APIExports.h) and nothing else ("What libbg3mse.dylib is
# made of").
# ... binds every import to an OS library at link time, runs that initializer
# last, and is built, with the libraries from outside this build that are
# linked into it, for BG3MSE_MACOS_MINIMUM (Mac/Tools/CheckDylib.cmake).
set(BG3MSE_ARCHIVE_LIST "${CMAKE_BINARY_DIR}/bg3mse_archives.txt")
add_custom_command(TARGET bg3mse POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DDYLIB=$<TARGET_FILE:bg3mse> -DLAST_INITIALIZER=InitializeImage
        -DEXPORTED=$<TARGET_PROPERTY:bg3mse,BG3MSE_EXPORTED>
        -DMIN_HEADER_PADDING=$<TARGET_PROPERTY:bg3mse,BG3MSE_MIN_HEADER_PADDING>
        -DMACOS_MINIMUM=${BG3MSE_MACOS_MINIMUM} -DARCHIVE_LIST=${BG3MSE_ARCHIVE_LIST}
        -P ${CMAKE_SOURCE_DIR}/Mac/Tools/CheckDylib.cmake
    VERBATIM)
target_precompile_headers(bg3mse REUSE_FROM bg3mse_core)

add_executable(bg3mse-shaders Mac/Tools/ShaderTool.cpp Mac/Tools/ShaderSurvey.cpp)
target_link_libraries(bg3mse-shaders PRIVATE ${BG3MSE_MAIN_LIBS})
target_precompile_headers(bg3mse-shaders REUSE_FROM bg3mse_core)

# The Win32 compatibility layer Norbyte's sources are built against (Mac/Shim):
# what it implements.
add_library(bg3mse_shim STATIC
    Mac/Shim/Win32Strings.cpp
    Mac/Shim/Win32Console.cpp
    Mac/Shim/ConsoleScreen.cpp
    Mac/Shim/OwnStdout.cpp
    Mac/Shim/Win32Process.cpp
    Mac/Shim/Win32Files.cpp
    Mac/Shim/Win32Modules.cpp
    Mac/Shim/Win32Mutexes.cpp
    Mac/Shim/Win32Keys.cpp
    Mac/Shim/Win32MessageBox.cpp
    Mac/Shim/Win32Resources.cpp
    Mac/Shim/Win32Sockets.cpp
    Mac/Shim/Win32SocketsPosix.cpp)
target_include_directories(bg3mse_shim PUBLIC Mac/Shim)
# The keyboard's state (Mac/Shim/Win32Keys.cpp) and the system's alerts
# (Mac/Shim/Win32MessageBox.cpp).
target_link_libraries(bg3mse_shim PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
target_compile_options(bg3mse_shim PRIVATE ${BG3MSE_WARNINGS})

# Norbyte's Script Extender: the ClCompile lists of CoreLib/CoreLib.vcxproj and
# BG3Extender/BG3Extender.vcxproj (less the stdafx.cpp of each, which only
# creates MSVC's precompiled header), compiled as their "Release" and "Game
# Release" configurations are (C++ latest, their preprocessor definitions),
# with clang's default warnings. RTTI is on: clang needs it for typeid, which
# MSVC allows without. Built by default, and linked into libbg3mse.dylib once
# nothing it needs is undefined ("What libbg3mse.dylib is made of").
#
# Two static libraries, as in the solution, where BG3Extender links
# CoreLib.lib: both projects compile a tinyxml2.cpp (the same file twice), and
# a link takes the extender's own object and never needs the library's member.
#
# Left out of the Mac build, each a Windows-only subsystem:
#   BG3Extender/Extender/Client/SDL.cpp           the SDL hooks (the Mac game has no SDL): Mac/Overlay/CocoaInput.cpp
#                                                 defines SDLManager for the Mac game's input
#   BG3Extender/Extender/Shared/DWriteWrapper.cpp the DWrite.dll loader proxy
#   BG3Extender/Extender/Shared/CrashReporter.cpp minidumps and the external crash reporter
#                                                 (Mac/Crash is its Mac counterpart)
# and the DX11 and Vulkan ImGui backends, Extender/Client/IMGUI/{DX11,Vulkan}.inl,
# in whose place IMGUI.cpp includes the Metal backend (Mac/Overlay/MetalBackend.h).
#
# The sources are written in MSVC's dialect, and are compiled in it:
#   -fms-compatibility            what MSVC accepts beyond -fms-extensions: a
#                                 function pointer converts to void* and back,
#                                 which the hook wrappers and the property maps
#                                 rely on
#   -fms-compatibility-version    the compiler of the projects' toolset (v145);
#                                 without one, char16_t and char32_t are not
#                                 keywords in that mode. It does not define _MSC_VER here.
#   -fgnuc-version                the macOS SDK's headers need __GNUC__, which
#                                 that mode leaves undefined; 4.2.1 is the value
#                                 clang defines without it
#   -fdelayed-template-parsing    a template's body is parsed when it is
#                                 instantiated, as MSVC does: Norbyte's templates
#                                 name functions and types declared after them.
#                                 clang reports the option as deprecated after
#                                 C++20, once per source.
#   -Wno-error=invalid-token-paste  PERF_REPORT (Extender/Shared/ExtenderConfig.h)
#                                 pastes "(" onto its first argument, which forms
#                                 no token. MSVC ignores the ##; clang does the
#                                 same once that is a warning and not an error.
#   -Wno-error=c++11-narrowing    a bool member is initialised with { .0f }
#                                 (GameDefinitions/GuidResources.h), which MSVC
#                                 does not count as narrowing. Still a warning.
# -fno-strict-aliasing: Norbyte's sources are written for MSVC, which does no
# type-based alias analysis; they read one object through pointers of another
# type, and clang at -O2 may otherwise drop such reads and writes.
set(BG3SE_DIALECT -fms-compatibility -fms-compatibility-version=19.50 -fgnuc-version=4.2.1 -fdelayed-template-parsing
    -fno-strict-aliasing -Wno-error=invalid-token-paste -Wno-error=c++11-narrowing)
add_library(bg3se_corelib STATIC
    CoreLib/Base/Base.cpp
    CoreLib/Console.cpp
    CoreLib/Crypto.cpp
    CoreLib/MurmurHash3.cpp
    CoreLib/SymbolMapper.cpp
    CoreLib/tinyxml2.cpp
    CoreLib/Utils.cpp)
# Norbyte's own directories are not system directories for these two targets
# (they are for bg3se_headers), so that clang's warnings in Norbyte's headers
# are reported. CoreLib/ itself: its sources include "stdafx.h" from
# subdirectories, which MSVC's precompiled header settings resolve.
target_include_directories(bg3se_corelib PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/CoreLib")
target_include_directories(bg3se_corelib SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/Mac/Shim"
    "${EXT}/glm"
    "${EXT}/rapidjson/include"
    "${EXT}/tinycrypt/lib/include")
# As for bg3se_headers: glm's aligned types, which CoreLib/Base/BaseTypes.h
# asks for, need the definition before the first glm header and need
# _MSC_EXTENSIONS to see the language extensions -fms-extensions turns on.
target_compile_definitions(bg3se_corelib PRIVATE NDEBUG _LIB GLM_FORCE_ALIGNED_GENTYPES _MSC_EXTENSIONS)
target_compile_options(bg3se_corelib PRIVATE -fms-extensions ${BG3SE_DIALECT})
target_precompile_headers(bg3se_corelib PRIVATE "${CMAKE_SOURCE_DIR}/CoreLib/stdafx.h")
target_include_directories(bg3se_corelib BEFORE PRIVATE "${FLAVOUR_DIR}")
set_source_files_properties(CoreLib/tinyxml2.cpp TARGET_DIRECTORY bg3se_corelib PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_library(bg3se_extender STATIC
    BG3Extender/Extender/Client/ExtensionStateClient.cpp
    BG3Extender/Extender/Client/IMGUI/IMGUI.cpp
    BG3Extender/Extender/Client/ScriptExtenderClient.cpp
    BG3Extender/Extender/Client/VisualHelpers.cpp
    BG3Extender/Extender/ScriptExtender.cpp
    BG3Extender/Extender/Server/ScriptExtenderServer.cpp
    BG3Extender/Extender/Shared/Console.cpp
    BG3Extender/Extender/Shared/ExtenderNet.cpp
    BG3Extender/Extender/Shared/ExtensionState.cpp
    BG3Extender/Extender/Shared/Hooks.cpp
    BG3Extender/Extender/Shared/ScriptHelpers.cpp
    BG3Extender/Extender/Shared/Utils.cpp
    BG3Extender/Extender/Shared/dllmain.cpp
    BG3Extender/Extender/Shared/tinyxml2.cpp
    BG3Extender/GameDefinitions/Base/Base.cpp
    BG3Extender/GameDefinitions/EntitySystem.cpp
    BG3Extender/GameDefinitions/Enumerations.cpp
    BG3Extender/GameDefinitions/GameDefinitionHelpers.cpp
    BG3Extender/GameDefinitions/GameHelpers.cpp
    BG3Extender/GameDefinitions/GlobalFixedStrings.cpp
    BG3Extender/GameDefinitions/Stats/Stats.cpp
    BG3Extender/GameHooks/DataLibraries.cpp
    BG3Extender/GameHooks/DataLibrariesBG3Game.cpp
    BG3Extender/GameHooks/EngineHooks.cpp
    BG3Extender/GameHooks/OsirisWrappers.cpp
    BG3Extender/Lua/Client/LuaClient.cpp
    BG3Extender/Lua/Debugger/LuaDebugMessages.cpp
    BG3Extender/Lua/Debugger/LuaDebugger.cpp
    BG3Extender/Lua/Libs/LuaSharedLibs.cpp
    BG3Extender/Lua/LuaBinding.cpp
    BG3Extender/Lua/LuaSerializers.cpp
    BG3Extender/Lua/Osiris/LuaOsirisBinding.cpp
    BG3Extender/Lua/Server/LuaServer.cpp
    BG3Extender/Lua/Shared/LuaBundle.cpp
    BG3Extender/Lua/Shared/LuaInternalHelpers.cpp
    BG3Extender/Lua/Shared/Proxies/LuaEventRegistrations.cpp
    BG3Extender/Lua/Shared/Proxies/LuaObjectProxies.cpp
    BG3Extender/Lua/Shared/Proxies/LuaTypeInformation.cpp
    BG3Extender/Osiris/Debugger/DebugInterface.cpp
    BG3Extender/Osiris/Debugger/DebugMessages.cpp
    BG3Extender/Osiris/Debugger/Debugger.cpp
    BG3Extender/Osiris/Functions/FunctionLibrary.cpp
    BG3Extender/Osiris/Functions/FunctionUtilities.cpp
    BG3Extender/Osiris/Functions/LuaFunctions.cpp
    BG3Extender/Osiris/OsirisExtender.cpp
    BG3Extender/Osiris/Shared/CustomFunctions.cpp
    BG3Extender/Osiris/Shared/NodeHooks.cpp
    BG3Extender/Osiris/Shared/OsirisHelpers.cpp
    ${PROTO_SOURCES}
    "${EXT}/FastLZ/fastlz.c"
    "${EXT}/lz4/lib/lz4.c")
target_link_libraries(bg3se_extender PUBLIC bg3se_corelib)
# Norbyte's directories, then the dependencies (the include directories of
# bg3se_headers).
target_include_directories(bg3se_extender PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/BG3Extender")
target_include_directories(bg3se_extender SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/Mac/Shim"
    "${PROTO_OUT}"
    "${EXT}/protobuf/src"
    "${EXT}/protobuf/third_party/utf8_range"
    "${EXT}/abseil-cpp"
    "${EXT}/glm"
    "${EXT}/lua/src"
    "${EXT}/imgui"
    "${EXT}/optick/src"
    "${EXT}/rapidjson/include"
    "${EXT}/SDL/include"
    "${NOESIS_INCLUDE}")
# The "Game Release" definitions, less _WINDOWS and _USRDLL (which name a
# Windows DLL build) and the MSVC STL's _SILENCE_* switches, with what
# bg3se_headers adds for glm. protobuf is used without RTTI, as the extender's
# sources see it on Windows (/GR-).
target_compile_definitions(bg3se_extender PRIVATE
    OSI_EOCAPP OSIINTERFACE_EXPORTS OSI_EXTENSION_BUILD
    ENABLE_SDL ENABLE_IMGUI ENABLE_UI NS_STATIC_LIBRARY NDEBUG
    GLM_FORCE_ALIGNED_GENTYPES _MSC_EXTENSIONS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS GOOGLE_PROTOBUF_NO_RTTI=1)
target_compile_options(bg3se_extender PRIVATE -fms-extensions)
# C++ only: lz4.c and fastlz.c are C.
target_compile_options(bg3se_extender PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${BG3SE_DIALECT}>")
target_precompile_headers(bg3se_extender PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/BG3Extender/stdafx.h>")
target_include_directories(bg3se_extender BEFORE PRIVATE "${FLAVOUR_DIR}")
set_source_files_properties(${PROTO_SOURCES} BG3Extender/Extender/Shared/tinyxml2.cpp
    TARGET_DIRECTORY bg3se_extender PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# The solution's other libraries, which the extender links: the ClCompile
# lists and "Release" definitions (NDEBUG, _LIB and what each adds) of
# LuaLib/LuaLib.vcxproj, imgui/imgui.vcxproj, Optick/Optick.vcxproj and
# TinyCrypt/TinyCrypt.vcxproj. Static, with hidden symbols like everything
# else here, in the language mode of the projects (none is set, which is C++14
# for that toolset).

# Lua, from Norbyte's sources (External/lua), compiled as C++ as the project
# does (CompileAs: CompileAsCpp). That makes its functions C++ functions with
# mangled names, and Lua errors C++ exceptions (ldo.c: LUAI_THROW is throw
# unless LUA_USE_LONGJMP is defined, which it must not be here: the guarded
# calls of Mac/Platform/FaultGuard.h allow no longjmp out of them).
#
# On Windows luaconf.h configures itself from _WIN32. Here LUA_USE_POSIX does
# (POSIX functions where Windows has its own: those differences are in the io
# and os libraries, which the extender does not open, Lua/LuaBinding.cpp), and
# Mac/Lua/WindowsConfiguration.h, included in front of every source, sets what
# a script can tell the two configurations apart by as Windows has it.
#
# Not LUA_USE_MACOSX, Lua's configuration for this system: that adds
# LUA_USE_DLOPEN, and with it the loading of C modules from any file a script
# names (loadlib.c). The extender does not open the package library either;
# without the definition the code is not in the image at all
# (Mac/Tools/CheckLuaIsolation.cmake).
#
# One difference no macro reaches: string.pack, string.unpack and
# string.packsize take "l" and "L" as a C long, which is 4 bytes for MSVC and
# 8 here. lstrlib.c is compiled from a copy that says 4, made when its text
# changes (like the NoesisGUI headers above).
set(BG3SE_LUA_SOURCES)
foreach(name lapi lauxlib lbaselib lbitlib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc lgc linit liolib llex
        lmathlib lmem loadlib lobject lopcodes loslib lparser lstate lstring ltable ltablib ltm lundump
        lutf8lib lvm lzio)
    list(APPEND BG3SE_LUA_SOURCES "${EXT}/lua/src/${name}.c")
endforeach()

set(BG3SE_LUA_STRLIB "${CMAKE_BINARY_DIR}/lua_windows/lstrlib.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EXT}/lua/src/lstrlib.c")
file(READ "${EXT}/lua/src/lstrlib.c" strlib)
foreach(option l L)
    if(option STREQUAL "l")
        set(kind Kint)
    else()
        set(kind Kuint)
    endif()
    set(old "    case '${option}': *size = sizeof(long); return ${kind};\n")
    string(FIND "${strlib}" "${old}" at)
    string(FIND "${strlib}" "${old}" last REVERSE)
    if(at EQUAL -1 OR NOT at EQUAL last)
        message(FATAL_ERROR "External/lua/src/lstrlib.c: the size of string.pack's '${option}' is not where it was (another Lua?)")
    endif()
    string(REPLACE "${old}" "    case '${option}': *size = 4; return ${kind};  /* MSVC's long */\n" strlib "${strlib}")
endforeach()
if(strlib MATCHES "sizeof\\(long\\)")
    message(FATAL_ERROR "External/lua/src/lstrlib.c: it uses the size of a long elsewhere too (another Lua?)")
endif()
set(current "")
if(EXISTS "${BG3SE_LUA_STRLIB}")
    file(READ "${BG3SE_LUA_STRLIB}" current)
endif()
if(NOT "${current}" STREQUAL "${strlib}")
    file(WRITE "${BG3SE_LUA_STRLIB}" "${strlib}")
endif()
list(APPEND BG3SE_LUA_SOURCES "${BG3SE_LUA_STRLIB}")

add_library(bg3se_lua STATIC ${BG3SE_LUA_SOURCES})
set_source_files_properties(${BG3SE_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
set_target_properties(bg3se_lua PROPERTIES CXX_STANDARD 14 LINKER_LANGUAGE CXX)
target_compile_definitions(bg3se_lua PRIVATE NDEBUG _LIB LUA_USE_POSIX)
# For the copy of lstrlib.c, which is not next to Lua's headers, and for the configuration header.
target_include_directories(bg3se_lua PRIVATE "${EXT}/lua/src")
target_compile_options(bg3se_lua PRIVATE -include "${CMAKE_SOURCE_DIR}/Mac/Lua/WindowsConfiguration.h")

# Chunks of Lua run through that library, and what the Windows build gives
# for each: one group for every difference the configuration header and the
# copy of lstrlib.c are there for.
add_executable(bg3se_lua_tests Mac/Tests/LuaTests.cpp)
target_include_directories(bg3se_lua_tests SYSTEM PRIVATE "${EXT}/lua/src")
target_link_libraries(bg3se_lua_tests PRIVATE bg3se_lua)
target_compile_options(bg3se_lua_tests PRIVATE ${BG3MSE_WARNINGS})

# ImGui: the core, and the demo window the project also compiles. Its DX11,
# Vulkan, SDL2 and Win32 backends (imgui/backends) are left out with the
# extender's own two rendering backends.
add_library(bg3se_imgui STATIC
    "${EXT}/imgui/imgui.cpp"
    "${EXT}/imgui/imgui_demo.cpp"
    "${EXT}/imgui/imgui_draw.cpp"
    "${EXT}/imgui/imgui_tables.cpp"
    "${EXT}/imgui/imgui_widgets.cpp")
set_target_properties(bg3se_imgui PROPERTIES CXX_STANDARD 14)
target_include_directories(bg3se_imgui PRIVATE "${EXT}/imgui")
target_compile_definitions(bg3se_imgui PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS NDEBUG _LIB)
# Numbers written and read in the "C" locale, as the Windows build's static C
# library has it, whatever locale the game sets (Mac/Shim/ImGuiCLocale.h).
target_compile_options(bg3se_imgui PRIVATE "SHELL:-include ${CMAKE_SOURCE_DIR}/Mac/Shim/ImGuiCLocale.h")
# The clipboard. Norbyte keeps ImGui's own clipboard functions in place of his
# platform backend's (IMGUIManager::InitializeUI). On Windows ImGui's own are
# the system clipboard; on macOS they are only with this definition, which
# makes them the system pasteboard (ApplicationServices) instead of a
# clipboard of ImGui's own that nothing else can see.
target_compile_definitions(bg3se_imgui PRIVATE IMGUI_ENABLE_OSX_DEFAULT_CLIPBOARD_FUNCTIONS)
target_link_libraries(bg3se_imgui INTERFACE "-framework ApplicationServices")

# Optick, the profiler. The extender uses it when Extender/BuildInfo.h defines
# SE_IS_DEVELOPER_BUILD (Extender/Shared/Optick.h sets USE_OPTICK from that,
# whatever the configuration); the file in the repository does, and
# publish.ps1 rewrites it without for the "Release" channel. The project
# excludes the two GPU back ends (optick_gpu.d3d12.cpp, optick_gpu.vulkan.cpp).
add_library(bg3se_optick STATIC
    "${EXT}/optick/src/optick_capi.cpp"
    "${EXT}/optick/src/optick_core.cpp"
    "${EXT}/optick/src/optick_gpu.cpp"
    "${EXT}/optick/src/optick_message.cpp"
    "${EXT}/optick/src/optick_miniz.cpp"
    "${EXT}/optick/src/optick_serialization.cpp"
    "${EXT}/optick/src/optick_server.cpp")
set_target_properties(bg3se_optick PROPERTIES CXX_STANDARD 14)
target_compile_definitions(bg3se_optick PRIVATE NDEBUG _LIB)

# TinyCrypt: ECDSA and SHA-256, which CoreLib/Crypto.cpp verifies signatures with.
add_library(bg3se_tinycrypt STATIC
    "${EXT}/tinycrypt/lib/source/ecc.c"
    "${EXT}/tinycrypt/lib/source/ecc_dh.c"
    "${EXT}/tinycrypt/lib/source/ecc_dsa.c"
    "${EXT}/tinycrypt/lib/source/ecc_platform_specific.c"
    "${EXT}/tinycrypt/lib/source/sha256.c"
    "${EXT}/tinycrypt/lib/source/utils.c")
target_include_directories(bg3se_tinycrypt PRIVATE "${EXT}/tinycrypt/lib/include")
target_compile_definitions(bg3se_tinycrypt PRIVATE NDEBUG _LIB)

# The files Norbyte embeds in the extender as resources (BG3Extender/BG3Extender.rc,
# type SCRIPT_EXTENDER), read at run time through GetExeResource:
#   IDR_LUA_BUILTIN_BUNDLE   Lua.bundle, which ResourceBundler packs from
#                            BG3Extender/LuaScripts before the extender is built
#   IDR_BINARY_MAPPINGS      GameHooks/BinaryMappings.xml
# On macOS a resource is a section of the image (Mac/Shim/Win32Resources.cpp):
# an image that links the extender takes BG3SE_RESOURCE_LINK_OPTIONS and
# depends on BG3SE_RESOURCE_FILES. The numbers come from BG3Extender/resource.h.
add_executable(bg3se_resource_bundler ResourceBundler/ResourceBundler.cpp)
# The standard headers the source uses and MSVC's <filesystem> brings along.
target_compile_options(bg3se_resource_bundler PRIVATE
    "SHELL:-include vector" "SHELL:-include algorithm" "SHELL:-include cstring"
    "SHELL:-include cstdint" "SHELL:-include cstdlib")
file(GLOB_RECURSE BG3SE_LUA_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/BG3Extender/LuaScripts/*")
set(BG3SE_LUA_BUNDLE "${CMAKE_BINARY_DIR}/Lua.bundle")
add_custom_command(
    OUTPUT "${BG3SE_LUA_BUNDLE}"
    COMMAND bg3se_resource_bundler "${CMAKE_SOURCE_DIR}/BG3Extender/LuaScripts" "${BG3SE_LUA_BUNDLE}"
    DEPENDS bg3se_resource_bundler ${BG3SE_LUA_SCRIPTS}
    COMMENT "Packing BG3Extender/LuaScripts into Lua.bundle"
    VERBATIM)
file(READ "${CMAKE_SOURCE_DIR}/BG3Extender/resource.h" resource_h)
foreach(id IDR_LUA_BUILTIN_BUNDLE IDR_BINARY_MAPPINGS)
    if(NOT resource_h MATCHES "#define[ \t]+${id}[ \t]+([0-9]+)")
        message(FATAL_ERROR "BG3Extender/resource.h does not define ${id}")
    endif()
    set(${id} "${CMAKE_MATCH_1}")
endforeach()
set(BG3SE_BINARY_MAPPINGS "${CMAKE_SOURCE_DIR}/BG3Extender/GameHooks/BinaryMappings.xml")
set(BG3SE_RESOURCE_FILES "${BG3SE_LUA_BUNDLE}" "${BG3SE_BINARY_MAPPINGS}")
# For an image that is a target of its own: the bundle is packed before it is linked.
add_custom_target(bg3se_resource_files DEPENDS ${BG3SE_RESOURCE_FILES})
set(BG3SE_RESOURCE_LINK_OPTIONS
    "-Wl,-sectcreate,SCRIPT_EXTENDER,${IDR_LUA_BUILTIN_BUNDLE},${BG3SE_LUA_BUNDLE}"
    "-Wl,-sectcreate,SCRIPT_EXTENDER,${IDR_BINARY_MAPPINGS},${BG3SE_BINARY_MAPPINGS}")

# The layouts of game types on Mac and their proofs against the game's code
# (Mac/Layout): what is decoded from the game binary. It includes nothing of
# the Script Extender.
add_library(bg3mse_layout STATIC
    Mac/Layout/CodeProof.cpp
    Mac/Layout/FunctionCode.cpp
    Mac/Layout/ComponentSizes.cpp
    Mac/Layout/ComponentRegistry.cpp
    Mac/Layout/ComponentMembers.cpp
    Mac/Layout/GameFields.cpp
    Mac/Layout/ObjectSizes.cpp
    Mac/Layout/GameShapes.cpp
    Mac/Layout/OsirisShapes.cpp
    Mac/Layout/VirtualSlots.cpp)
target_link_libraries(bg3mse_layout PUBLIC bg3mse_engine bg3mse_platform)
target_compile_options(bg3mse_layout PRIVATE ${BG3MSE_WARNINGS})

# What this build makes of Norbyte's definitions: the size of every bound
# component type, for the tools that compare it with the game, and the
# assertions that his structures have the Mac shapes. Compiled with
# bg3se_extender's definitions and options, because the answer is only that
# target's answer under them; its include directories are all system
# directories here, so that what the compiler reports for these files is
# about these files.
add_library(bg3mse_layout_bound OBJECT Mac/Layout/BoundComponents.cpp Mac/Layout/BoundMembers.cpp Mac/Layout/BoundObjects.cpp
    Mac/Layout/DefinitionChecks.cpp Mac/Layout/DeclaredVirtuals.cpp
    Mac/Layout/OsirisDefinitionChecks.cpp
    Mac/Layout/NetworkChecks.cpp)
target_include_directories(bg3mse_layout_bound SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_layout_bound PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_layout_bound PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_layout_bound bg3se_proto)

if(BG3MSE_TESTS)
    enable_testing()
endif()

file(GLOB_RECURSE BG3SE_PROTOBUF_DEPS
    "${EXT}/protobuf-build/_deps/absl-build/libabsl_*.a"
    "${EXT}/protobuf-build/third_party/utf8_range/libutf8_*.a")
if(NOT BG3SE_PROTOBUF_DEPS)
    message(FATAL_ERROR "External/protobuf-build has no abseil libraries: run Mac/setup.sh first.")
endif()
# What an image that holds Norbyte's extender is linked from, besides the
# extender itself (which is loaded whole): every object of bg3se_extender, and
# CoreLib, the solution's other libraries, the shim, the lock functions (with
# the layout checks their gate runs), the Detours layer, the platform layer,
# protobuf and abseil as libraries, and of bg3mse_core what the rest needs.
# One list, for libbg3mse.dylib ("What libbg3mse.dylib is made of") and the
# tests linked as it is. A library that others depend on comes after them.
set(BG3SE_IMAGE_OBJECTS bg3mse_binding_rows bg3mse_binding_startup bg3mse_overlay_bound)
set(BG3SE_IMAGE_LIBRARIES
    bg3se_corelib bg3se_lua bg3se_imgui bg3se_optick bg3se_tinycrypt
    bg3mse_shim bg3mse_locks
    bg3mse_binding bg3mse_crash_extender bg3mse_crash_delivery bg3mse_crash
    bg3mse_overlay bg3se_imgui_metal
    bg3mse_engine bg3mse_layout bg3mse_core bg3mse_detours bg3mse_platform)
# What the crash reporter names its build with (MBLD).
list(APPEND BG3SE_IMAGE_LIBRARIES bg3mse_version)
set(BG3SE_IMAGE_ARCHIVES "${EXT}/protobuf-build/libprotobuf-lite.a" ${BG3SE_PROTOBUF_DEPS})
set(BG3SE_IMAGE_FRAMEWORKS CoreFoundation CoreGraphics Foundation AppKit Metal QuartzCore ApplicationServices)
list(APPEND BG3SE_IMAGE_FRAMEWORKS GameController)
list(APPEND BG3SE_IMAGE_FRAMEWORKS Carbon)

add_test(NAME bg3se_lua_as_on_windows COMMAND bg3se_lua_tests)

# A dylib for the tests to load: with its function starts table, and without it
# and without local symbols. The first has header padding for islands.
add_library(bg3mse_test_functions SHARED Mac/Tests/FunctionLibrary.S)
target_link_options(bg3mse_test_functions PRIVATE -Wl,-headerpad,0x1000)
add_library(bg3mse_test_functions_symbols SHARED Mac/Tests/FunctionLibrary.S)
target_link_options(bg3mse_test_functions_symbols PRIVATE -Wl,-no_function_starts -Wl,-x)
set_target_properties(bg3mse_test_functions bg3mse_test_functions_symbols PROPERTIES LINKER_LANGUAGE C)

add_executable(bg3mse_shim_tests Mac/Tests/ShimTests.cpp Mac/Tests/ShimTestsConsole.cpp Mac/Tests/ShimTestsMs.cpp Mac/Tests/ShimTestsPosix.cpp)
target_link_libraries(bg3mse_shim_tests PRIVATE bg3mse_shim)
add_dependencies(bg3mse_shim_tests bg3mse_test_functions)
# Resources of the test executable itself (Mac/Shim/Win32Resources.cpp): each
# is a section made from a file, the segment named for the type.
set(SHIM_TEST_RESOURCES "${CMAKE_BINARY_DIR}/shim_test_resources")
file(CONFIGURE OUTPUT "${SHIM_TEST_RESOURCES}/seven" CONTENT "resource seven\n" @ONLY)
file(CONFIGURE OUTPUT "${SHIM_TEST_RESOURCES}/named" CONTENT "a resource with a name" @ONLY)
file(CONFIGURE OUTPUT "${SHIM_TEST_RESOURCES}/version" CONTENT "of an integer type" @ONLY)
target_link_options(bg3mse_shim_tests PRIVATE
    "-Wl,-sectcreate,SHIM_TEST,7,${SHIM_TEST_RESOURCES}/seven"
    "-Wl,-sectcreate,SHIM_TEST,NAMED,${SHIM_TEST_RESOURCES}/named"
    "-Wl,-sectcreate,16,1,${SHIM_TEST_RESOURCES}/version")
set_property(TARGET bg3mse_shim_tests APPEND PROPERTY LINK_DEPENDS
    "${SHIM_TEST_RESOURCES}/seven" "${SHIM_TEST_RESOURCES}/named" "${SHIM_TEST_RESOURCES}/version")
target_compile_definitions(bg3mse_shim_tests PRIVATE BG3MSE_TEST_FUNCTIONS="$<TARGET_FILE:bg3mse_test_functions>")
target_compile_options(bg3mse_shim_tests PRIVATE ${BG3MSE_WARNINGS})
set_source_files_properties(Mac/Tests/ShimTestsMs.cpp PROPERTIES COMPILE_OPTIONS -fms-extensions)
add_test(NAME bg3mse_shim_tests COMMAND bg3mse_shim_tests)

# Images loaded the ways the extender can be (LoadedThroughUpdater): named by
# the test's executable, opened with dlopen, and a stand-in for the updater
# under the updater's file name, in a directory of its own.
add_library(bg3mse_test_named_image SHARED Mac/Tests/ImageLibrary.c)
add_library(bg3mse_test_opened_image SHARED Mac/Tests/ImageLibrary.c)
add_library(bg3mse_test_updater_image SHARED Mac/Tests/ImageLibrary.c)
set_target_properties(bg3mse_test_updater_image PROPERTIES OUTPUT_NAME bg3mse-loader
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/platform_tests_updater")

add_executable(bg3mse_platform_tests Mac/Tests/PlatformTests.cpp Mac/Tests/HookTargets.S)
target_link_libraries(bg3mse_platform_tests PRIVATE bg3mse_platform bg3mse_test_named_image)
target_compile_options(bg3mse_platform_tests PRIVATE ${BG3MSE_WARNINGS})
add_dependencies(bg3mse_platform_tests bg3mse_test_functions bg3mse_test_functions_symbols
    bg3mse_test_opened_image bg3mse_test_updater_image)
target_compile_definitions(bg3mse_platform_tests PRIVATE
    BG3MSE_TEST_FUNCTIONS="$<TARGET_FILE:bg3mse_test_functions>"
    BG3MSE_TEST_FUNCTIONS_SYMBOLS="$<TARGET_FILE:bg3mse_test_functions_symbols>"
    BG3MSE_TEST_NAMED_IMAGE="$<TARGET_FILE:bg3mse_test_named_image>"
    BG3MSE_TEST_OPENED_IMAGE="$<TARGET_FILE:bg3mse_test_opened_image>"
    BG3MSE_TEST_UPDATER_IMAGE="$<TARGET_FILE:bg3mse_test_updater_image>")
add_test(NAME bg3mse_platform_tests COMMAND bg3mse_platform_tests)

add_executable(bg3mse_detours_tests Mac/Tests/DetoursTests.cpp Mac/Tests/DetoursTargets.S)
target_link_libraries(bg3mse_detours_tests PRIVATE bg3mse_detours)
target_compile_options(bg3mse_detours_tests PRIVATE ${BG3MSE_WARNINGS})
add_dependencies(bg3mse_detours_tests bg3mse_test_functions bg3mse_test_functions_symbols)
target_compile_definitions(bg3mse_detours_tests PRIVATE
    BG3MSE_TEST_FUNCTIONS="$<TARGET_FILE:bg3mse_test_functions>"
    BG3MSE_TEST_FUNCTIONS_SYMBOLS="$<TARGET_FILE:bg3mse_test_functions_symbols>")
add_test(NAME bg3mse_detours_tests COMMAND bg3mse_detours_tests)

add_executable(bg3mse_fault_tests Mac/Tests/FaultTests.cpp Mac/Tests/FaultTargets.S)
target_link_libraries(bg3mse_fault_tests PRIVATE bg3mse_platform)
target_compile_options(bg3mse_fault_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_fault_tests COMMAND bg3mse_fault_tests)

# Norbyte's guarded blocks and crash reporter on the platform layer's fault
# handling (Mac/Crash). The reporter itself includes nothing of the Script
# Extender, so the tests run it; what it takes from the extender is one file,
# compiled as Norbyte's sources are (like bg3mse_layout_bound) and linked into
# libbg3mse.dylib.
add_library(bg3mse_crash STATIC
    Mac/Crash/CrashReport.cpp
    Mac/Crash/CrashReporter.cpp
    Mac/Crash/ThreadList.cpp
    Mac/Crash/GuardedBlockCall.S)
# What becomes of a written report, for Norbyte's extender: a library of its
# own, because the reporter's tests say that for themselves.
add_library(bg3mse_crash_delivery STATIC Mac/Crash/CrashDelivery.cpp)
target_link_libraries(bg3mse_crash_delivery PUBLIC bg3mse_platform)
target_compile_options(bg3mse_crash_delivery PRIVATE ${BG3MSE_WARNINGS})
target_link_libraries(bg3mse_crash PUBLIC bg3mse_platform "-framework CoreFoundation")
# The build a report is of (MBLD).
target_link_libraries(bg3mse_crash PUBLIC bg3mse_version)
target_compile_options(bg3mse_crash PRIVATE ${BG3MSE_WARNINGS})

add_library(bg3mse_crash_extender STATIC Mac/Crash/ExtenderCrashReporting.cpp)
target_link_libraries(bg3mse_crash_extender PUBLIC bg3mse_crash)
target_include_directories(bg3mse_crash_extender SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_crash_extender PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_crash_extender PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20 ${BG3MSE_WARNINGS})
add_dependencies(bg3mse_crash_extender bg3se_proto)
# What it does at the time of a crash calls nothing of the game's string table.
add_test(NAME bg3mse_crash_host_check COMMAND "${CMAKE_COMMAND}"
    "-DARCHIVE=$<TARGET_FILE:bg3mse_crash_extender>"
    -P "${CMAKE_SOURCE_DIR}/Mac/Tools/CheckCrashHost.cmake")

# A dylib whose functions are in no image of the reporter's, for the tests of
# which frames count.
add_library(bg3mse_crash_test_foreign SHARED Mac/Tests/CrashForeign.S)
set_target_properties(bg3mse_crash_test_foreign PROPERTIES LINKER_LANGUAGE C)
add_executable(bg3mse_crash_tests Mac/Tests/CrashTests.cpp Mac/Tests/CrashTestsBlocks.cpp Mac/Tests/CrashTargets.S)
target_include_directories(bg3mse_crash_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/Mac/Shim")
target_link_libraries(bg3mse_crash_tests PRIVATE bg3mse_crash bg3mse_crash_test_foreign)
target_compile_options(bg3mse_crash_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_crash_tests COMMAND bg3mse_crash_tests)
add_executable(bg3mse_crash_delivery_tests Mac/Tests/CrashDeliveryTests.cpp)
target_link_libraries(bg3mse_crash_delivery_tests PRIVATE bg3mse_crash_delivery)
target_compile_options(bg3mse_crash_delivery_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_crash_delivery_tests COMMAND bg3mse_crash_delivery_tests)

# Norbyte's string for the layout tests, compiled as bg3mse_layout_bound is.
add_library(bg3mse_string_tests_norbyte OBJECT Mac/Tests/GameStringNorbyte.cpp)
target_include_directories(bg3mse_string_tests_norbyte SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_string_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_string_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_string_tests_norbyte bg3se_proto)
add_executable(bg3mse_layout_tests Mac/Tests/LayoutTests.cpp $<TARGET_OBJECTS:bg3mse_string_tests_norbyte>)
target_link_libraries(bg3mse_layout_tests PRIVATE bg3mse_layout)
target_compile_options(bg3mse_layout_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_layout_tests COMMAND bg3mse_layout_tests)

# Norbyte's UI code against NoesisGUI's layouts as the game's code has them
# (Mac/Tests/NoesisTests.cpp); his side compiled as bg3mse_layout_bound is.
add_library(bg3mse_noesis_tests_norbyte OBJECT Mac/Tests/NoesisNorbyte.cpp)
target_include_directories(bg3mse_noesis_tests_norbyte SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_noesis_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_noesis_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_noesis_tests_norbyte bg3se_proto)
add_executable(bg3mse_noesis_tests Mac/Tests/NoesisTests.cpp $<TARGET_OBJECTS:bg3mse_noesis_tests_norbyte>)
target_include_directories(bg3mse_noesis_tests PRIVATE "${CMAKE_SOURCE_DIR}/Mac")
target_compile_options(bg3mse_noesis_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_noesis_tests COMMAND bg3mse_noesis_tests)

# Norbyte's spell prototype animations cleared over a set the game made
# (Mac/Tests/PrototypeTests.cpp), compiled as bg3mse_layout_bound is.
add_executable(bg3mse_prototype_tests Mac/Tests/PrototypeTests.cpp)
target_include_directories(bg3mse_prototype_tests PRIVATE "${CMAKE_SOURCE_DIR}/Mac")
target_include_directories(bg3mse_prototype_tests SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_prototype_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_prototype_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_prototype_tests bg3se_proto)
add_test(NAME bg3mse_prototype_tests COMMAND bg3mse_prototype_tests)

# Norbyte's two lock classes for the lock tests, compiled as bg3mse_layout_bound is.
add_library(bg3mse_lock_tests_norbyte OBJECT Mac/Tests/LockTestsNorbyte.cpp)
target_include_directories(bg3mse_lock_tests_norbyte SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_lock_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_lock_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_lock_tests_norbyte bg3se_proto)
add_executable(bg3mse_lock_tests Mac/Tests/LockTests.cpp $<TARGET_OBJECTS:bg3mse_lock_tests_norbyte>)
target_link_libraries(bg3mse_lock_tests PRIVATE bg3mse_locks bg3mse_shim)
target_compile_options(bg3mse_lock_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_lock_tests COMMAND bg3mse_lock_tests)

add_executable(bg3mse_tests Mac/Tests/Tests.cpp)
target_link_libraries(bg3mse_tests PRIVATE bg3mse_core)
target_precompile_headers(bg3mse_tests REUSE_FROM bg3mse_core)
add_test(NAME bg3mse_tests COMMAND bg3mse_tests)

# What keeps mod shader translation from making a player wait at every start:
# translated materials kept between runs (Mac/Shaders/TranslationCache.h),
# their functions compiled when asked for, and the kept Metal interface.
add_executable(bg3mse_shader_startup_tests Mac/Tests/ShaderStartupTests.cpp)
target_link_libraries(bg3mse_shader_startup_tests PRIVATE bg3mse_core)
target_precompile_headers(bg3mse_shader_startup_tests REUSE_FROM bg3mse_core)
add_test(NAME bg3mse_shader_startup_tests COMMAND bg3mse_shader_startup_tests)

if(BG3MSE_GFX)
    add_executable(bg3mse_gfx_tests Mac/Tests/GfxTests.cpp Mac/Tests/GfxMetalTests.mm Mac/Tests/JitterStubProbe.S)
    target_link_libraries(bg3mse_gfx_tests PRIVATE bg3mse_gfx)
    target_precompile_headers(bg3mse_gfx_tests REUSE_FROM bg3mse_core)
    set_source_files_properties(Mac/Tests/GfxMetalTests.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "${BG3MSE_WARNINGS}")
    add_test(NAME bg3mse_gfx_tests COMMAND bg3mse_gfx_tests)
endif()

# The camera module is GPL-3.0-or-later (Mac/Camera/LICENSE): it is built from
# Mac/Camera and the platform layer only, and nothing above links it.
if(BG3MSE_CAMERA)
    add_library(bg3mse_camera_code STATIC
        Mac/Camera/CameraSites.cpp
        Mac/Camera/camera_gamepad.m
        Mac/Camera/camera_patch.c
        Mac/Camera/camera_settings.c
        Mac/Camera/camera_tweaks.c)
    target_link_libraries(bg3mse_camera_code PUBLIC bg3mse_platform "-framework Foundation" "-framework GameController")
    target_compile_options(bg3mse_camera_code PRIVATE ${BG3MSE_WARNINGS})

    add_library(bg3mse_camera SHARED Mac/Camera/CameraModule.cpp)
    set_target_properties(bg3mse_camera PROPERTIES OUTPUT_NAME bg3mse-camera)
    target_link_libraries(bg3mse_camera PRIVATE bg3mse_camera_code)
    target_link_libraries(bg3mse_camera PRIVATE bg3mse_version)
    set_target_properties(bg3mse_camera PROPERTIES MACHO_CURRENT_VERSION "${BG3MSE_VERSION}")
    target_compile_options(bg3mse_camera PRIVATE ${BG3MSE_WARNINGS})
    # Loaded for its constructor; it exports nothing, and imports only from the OS.
    target_link_options(bg3mse_camera PRIVATE -Wl,-exported_symbols_list,/dev/null)
    add_custom_command(TARGET bg3mse_camera POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DDYLIB=$<TARGET_FILE:bg3mse_camera> -DMACOS_MINIMUM=${BG3MSE_MACOS_MINIMUM}
            -P ${CMAKE_SOURCE_DIR}/Mac/Tools/CheckDylib.cmake
        VERBATIM)

    add_executable(bg3mse-camera Mac/Camera/CameraTool.cpp)
    target_link_libraries(bg3mse-camera PRIVATE bg3mse_camera_code)
    target_compile_options(bg3mse-camera PRIVATE ${BG3MSE_WARNINGS})

    # What goes beside the module where it is installed, in <build>/camera as
    # in a release: its licence texts, and the settings file it writes when
    # there is none, from which bg3mse-install makes the player's (the
    # installer is not GPL, and takes the text as data).
    set(BG3MSE_CAMERA_DEFAULTS "${CMAKE_BINARY_DIR}/camera/NativeCameraTweaks.default.toml")
    set(camera_texts "${BG3MSE_CAMERA_DEFAULTS}")
    foreach(file LICENSE COPYING EXCEPTIONS)
        add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/camera/${file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/camera"
            COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/Mac/Camera/${file}" "${CMAKE_BINARY_DIR}/camera/${file}"
            DEPENDS "Mac/Camera/${file}" VERBATIM)
        list(APPEND camera_texts "${CMAKE_BINARY_DIR}/camera/${file}")
    endforeach()
    add_custom_command(OUTPUT "${BG3MSE_CAMERA_DEFAULTS}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/camera"
        COMMAND sh -c "\"$0\" defaults > \"$1.new\" && mv \"$1.new\" \"$1\"" "$<TARGET_FILE:bg3mse-camera>" "${BG3MSE_CAMERA_DEFAULTS}"
        DEPENDS bg3mse-camera VERBATIM)
    add_custom_target(bg3mse_camera_texts ALL DEPENDS ${camera_texts})
    add_test(NAME bg3mse_camera_defaults_file
        COMMAND sh -c "\"$0\" defaults | cmp - \"$1\"" "$<TARGET_FILE:bg3mse-camera>" "${BG3MSE_CAMERA_DEFAULTS}")

    add_executable(bg3mse_camera_tests Mac/Tests/CameraTests.c)
    target_link_libraries(bg3mse_camera_tests PRIVATE bg3mse_camera_code)
    add_test(NAME bg3mse_camera_tests COMMAND bg3mse_camera_tests)
endif()

# Function extents in the game binary and libOsiris.dylib against their symbol tables.
add_executable(bg3mse-game-functions Mac/Tests/GameFunctions.cpp)
target_link_libraries(bg3mse-game-functions PRIVATE bg3mse_platform)
target_compile_options(bg3mse-game-functions PRIVATE ${BG3MSE_WARNINGS})

# The Win32 lock functions and GetCurrentThreadId, on the game's own lock
# functions (Mac/Layout/GameLockCalls.h). A library of its own, like the
# Detours layer: it needs both the Windows names of Mac/Shim and the platform
# layer.
add_library(bg3mse_locks STATIC Mac/Layout/GameLockCalls.cpp Mac/Shim/Win32Locks.cpp)
target_include_directories(bg3mse_locks SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/Mac/Shim")
target_link_libraries(bg3mse_locks PUBLIC bg3mse_layout)
target_compile_options(bg3mse_locks PRIVATE ${BG3MSE_WARNINGS})

# Norbyte's definitions as this build compiles them, against the game binary.
add_executable(bg3mse-layouts Mac/Tools/LayoutTool.cpp $<TARGET_OBJECTS:bg3mse_layout_bound>)
target_link_libraries(bg3mse-layouts PRIVATE bg3mse_locks)
target_compile_options(bg3mse-layouts PRIVATE ${BG3MSE_WARNINGS})

# The game binary named by BG3MSE_GAME_APP: every hook and site must be found
# and would install (each tool exits non-zero otherwise).
if(BG3MSE_GAME_APP)
    add_test(NAME bg3mse_game_functions_check COMMAND bg3mse-game-functions "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_shaders_check COMMAND bg3mse-shaders check "${BG3MSE_GAME_APP}")
    # What translating a mod's shader relies on in the installed game's own functions.
    add_test(NAME bg3mse_game_shaders_interface COMMAND bg3mse-shaders interface-check "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_component_sizes COMMAND bg3mse-layouts components "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_component_members COMMAND bg3mse-layouts members "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_object_sizes COMMAND bg3mse-layouts objects "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_shapes COMMAND bg3mse-layouts shapes "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_vtables COMMAND bg3mse-layouts vtables "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_locks COMMAND bg3mse-layouts locks "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_story_shapes COMMAND bg3mse-layouts story "${BG3MSE_GAME_APP}")
    if(BG3MSE_CAMERA)
        add_test(NAME bg3mse_game_camera_check COMMAND bg3mse-camera check "${BG3MSE_GAME_APP}")
    endif()
endif()

# The engine table (Mac/Engine): everything in the game that upstream's code
# calls, hooks or reads, found by name in the game's symbol table and checked
# against the game's code. It needs the platform layer only.
add_library(bg3mse_engine STATIC
    Mac/Engine/EngineTable.cpp
    Mac/Engine/EngineResolver.cpp
    Mac/Engine/EntryUse.cpp
    Mac/Engine/TypeIndices.cpp
    Mac/Engine/Entries/CoreLibrary.cpp
    Mac/Engine/Entries/Startup.cpp
    Mac/Engine/Entries/Stats.cpp
    Mac/Engine/Entries/World.cpp
    Mac/Engine/Entries/Ui.cpp
    Mac/Engine/Entries/Osiris.cpp
    Mac/Engine/Entries/Overlay.cpp
    Mac/Engine/Entries/Patches.cpp)
target_link_libraries(bg3mse_engine PUBLIC bg3mse_platform)
target_compile_options(bg3mse_engine PRIVATE ${BG3MSE_WARNINGS})

add_executable(bg3mse_engine_tests Mac/Tests/EngineTests.cpp)
target_link_libraries(bg3mse_engine_tests PRIVATE bg3mse_engine)
target_compile_options(bg3mse_engine_tests PRIVATE ${BG3MSE_WARNINGS})
add_dependencies(bg3mse_engine_tests bg3mse_test_functions)
target_compile_definitions(bg3mse_engine_tests PRIVATE BG3MSE_TEST_FUNCTIONS="$<TARGET_FILE:bg3mse_test_functions>")
add_test(NAME bg3mse_engine_tests COMMAND bg3mse_engine_tests)

# bg3mse-engine check <Game.app> prints the whole table for a game and exits
# non-zero when an entry is not in the state the table expects.
add_executable(bg3mse-engine Mac/Tools/EngineTool.cpp)
target_link_libraries(bg3mse-engine PRIVATE bg3mse_engine)
target_compile_options(bg3mse-engine PRIVATE ${BG3MSE_WARNINGS})
if(BG3MSE_GAME_APP)
    add_test(NAME bg3mse_game_engine_check COMMAND bg3mse-engine check "${BG3MSE_GAME_APP}")
endif()

# The native names of the registered types, as MSVC spells them
# (Mac/TypeNames): a header Norbyte's type information is compiled with, and
# the check that every registered class type has the class key the header
# gives it. The check is a file that compiles or does not, built with
# everything else; it is compiled as bg3mse_layout_bound is, with the
# mismatched-tags diagnostic on for the whole file (see the file).
set(CLASS_KEY_CHECK_DIR "${CMAKE_BINARY_DIR}/class_keys")
add_custom_command(
    OUTPUT "${CLASS_KEY_CHECK_DIR}/ClassKeyCheck.inl"
    COMMAND "${CMAKE_COMMAND}"
        "-DMAPS=${CMAKE_SOURCE_DIR}/BG3Extender/GameDefinitions/Generated/PropertyMaps.inl"
        "-DLIST=${CMAKE_SOURCE_DIR}/Mac/TypeNames/DeclaredAsClass.inl"
        "-DOUT=${CLASS_KEY_CHECK_DIR}/ClassKeyCheck.inl"
        -P "${CMAKE_SOURCE_DIR}/Mac/Tools/MakeClassKeyCheck.cmake"
    DEPENDS "${CMAKE_SOURCE_DIR}/BG3Extender/GameDefinitions/Generated/PropertyMaps.inl"
        "${CMAKE_SOURCE_DIR}/Mac/TypeNames/DeclaredAsClass.inl"
        "${CMAKE_SOURCE_DIR}/Mac/Tools/MakeClassKeyCheck.cmake"
    COMMENT "Listing the registered types with their class keys"
    VERBATIM)
add_library(bg3mse_class_key_check OBJECT Mac/TypeNames/ClassKeyCheck.cpp "${CLASS_KEY_CHECK_DIR}/ClassKeyCheck.inl")
target_include_directories(bg3mse_class_key_check PRIVATE "${CLASS_KEY_CHECK_DIR}")
target_include_directories(bg3mse_class_key_check SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_class_key_check PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_class_key_check PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20 -Wmismatched-tags)
add_dependencies(bg3mse_class_key_check bg3se_proto)

# The names themselves, on types of the test's own, in the dialect Norbyte's
# sources are compiled in.
add_executable(bg3mse_type_name_tests Mac/Tests/TypeNameTests.cpp)
target_include_directories(bg3mse_type_name_tests PRIVATE Mac)
target_compile_options(bg3mse_type_name_tests PRIVATE -fms-extensions ${BG3SE_DIALECT}
    -Wno-delayed-template-parsing-in-cxx20 ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_type_name_tests COMMAND bg3mse_type_name_tests)

# The binding (Mac/Binding): what connects the engine table to Norbyte's code.
# StaticSymbols is filled from a resolution of the table, with adapters where
# the game takes its arguments otherwise than Norbyte's prototype passes them
# and bridges where the place he hooks is not a function of his prototype.
# The library includes none of his headers; the rows, one for each member of
# StaticSymbols, are the one file that does, compiled as bg3mse_layout_bound is.
add_library(bg3mse_binding STATIC
    Mac/Binding/CodePatches.cpp
    Mac/Binding/GameVersion.cpp
    Mac/Binding/Prototype.cpp
    Mac/Binding/CallAdapters.cpp
    Mac/Binding/StatsFunctions.cpp
    Mac/Binding/HookBridges.cpp
    Mac/Binding/StaticSymbolBinding.cpp
    Mac/Binding/OsirisBinding.cpp
    Mac/Binding/TemplateBank.cpp
    Mac/Binding/NetMessages.cpp
    Mac/Binding/StatusVfx.cpp
    Mac/Binding/TypeIndexBinding.cpp
    Mac/Binding/Layouts.cpp
    Mac/Binding/Libraries.cpp)
# CoreFoundation: the game bundle's version (Mac/Binding/GameVersion.cpp).
target_link_libraries(bg3mse_binding PUBLIC bg3mse_engine bg3mse_detours bg3mse_layout "-framework CoreFoundation")
target_compile_options(bg3mse_binding PRIVATE ${BG3MSE_WARNINGS})

add_library(bg3mse_binding_rows OBJECT Mac/Binding/StaticSymbolRows.cpp Mac/Binding/OsirisRows.cpp Mac/Binding/BoundTypes.cpp
    Mac/Binding/TypeIndexMaps.cpp Mac/Binding/VirtualDependents.cpp
    # The rows of the slot check and the members of the bound structures,
    # which the binding compares with the game too (Binding/Layouts.h);
    # bg3mse-layouts has them from bg3mse_layout_bound.
    Mac/Layout/DeclaredVirtuals.cpp Mac/Layout/BoundMembers.cpp)
target_include_directories(bg3mse_binding_rows PRIVATE Mac)
target_include_directories(bg3mse_binding_rows SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_binding_rows PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_binding_rows PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_binding_rows bg3se_proto)

# The start-up sequence (Mac/Binding/Startup.h), which calls Norbyte's
# SetupScriptExtender, and its end at the game's exit (Mac/Binding/Shutdown.h):
# linked wherever his code is.
add_library(bg3mse_binding_startup OBJECT Mac/Binding/Startup.cpp Mac/Binding/Shutdown.cpp
    # The gates of the Lua functions that read what a game may not have (Mac/Binding/LuaGates.h).
    Mac/Binding/LuaGates.cpp)
target_include_directories(bg3mse_binding_startup PRIVATE Mac)
target_include_directories(bg3mse_binding_startup SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_binding_startup PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_binding_startup PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_binding_startup bg3se_proto)

# Norbyte's story containers for the binding tests, compiled as bg3mse_layout_bound is.
# StoryContainersNorbyte.cpp reads one container as libc++'s and as Norbyte's,
# which in the game two images do, each compiled without sight of the other:
# the one compiler that sees both here is told not to reason from the types
# (no strict aliasing), or it may order a read through one before a write
# through the other. StoryListsNorbyte.cpp uses his types alone and is
# compiled exactly as his sources are.
add_library(bg3mse_story_tests_norbyte OBJECT Mac/Tests/StoryContainersNorbyte.cpp Mac/Tests/StoryListsNorbyte.cpp)
target_include_directories(bg3mse_story_tests_norbyte SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_story_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_story_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
set_source_files_properties(Mac/Tests/StoryContainersNorbyte.cpp PROPERTIES COMPILE_OPTIONS -fno-strict-aliasing)
add_dependencies(bg3mse_story_tests_norbyte bg3se_proto)

add_executable(bg3mse_binding_tests Mac/Tests/BindingTests.cpp Mac/Tests/BindingTargets.S $<TARGET_OBJECTS:bg3mse_binding_rows>
    $<TARGET_OBJECTS:bg3mse_story_tests_norbyte>)
target_link_libraries(bg3mse_binding_tests PRIVATE bg3mse_binding)
target_compile_options(bg3mse_binding_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_binding_tests COMMAND bg3mse_binding_tests)

# bg3mse-binding check <Game.app> prints what StaticSymbols would hold on a
# game and which hooks of the binding would go on, and exits non-zero when a
# member that has a source in the game would stay null.
add_executable(bg3mse-binding Mac/Tools/BindingTool.cpp $<TARGET_OBJECTS:bg3mse_binding_rows>)
target_link_libraries(bg3mse-binding PRIVATE bg3mse_binding)
target_compile_options(bg3mse-binding PRIVATE ${BG3MSE_WARNINGS})
if(BG3MSE_GAME_APP)
    add_test(NAME bg3mse_game_binding_check COMMAND bg3mse-binding check "${BG3MSE_GAME_APP}")
    add_test(NAME bg3mse_game_type_indices COMMAND bg3mse-binding types "${BG3MSE_GAME_APP}")
endif()

# The component signals (Mac/Binding/ComponentSignals.h): how the game calls
# a subscriber of a component's attached and detached signals, and the check
# of its code before Norbyte's handlers are subscribed.
target_sources(bg3mse_binding PRIVATE Mac/Binding/ComponentSignals.cpp)
# The handler they are subscribed through sees his types
# (Mac/Binding/ComponentSignalHandler.h, included by his sources); the tests
# compile it with his signal as bg3mse_layout_bound is compiled.
add_library(bg3mse_component_signal_tests_norbyte OBJECT Mac/Tests/ComponentSignalsNorbyte.cpp)
target_include_directories(bg3mse_component_signal_tests_norbyte SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_component_signal_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_component_signal_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_component_signal_tests_norbyte bg3se_proto)
add_executable(bg3mse_component_signal_tests Mac/Tests/ComponentSignalTests.cpp Mac/Tests/ComponentSignalTargets.S
    $<TARGET_OBJECTS:bg3mse_component_signal_tests_norbyte>)
target_link_libraries(bg3mse_component_signal_tests PRIVATE bg3mse_binding)
target_compile_options(bg3mse_component_signal_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_component_signal_tests COMMAND bg3mse_component_signal_tests)
if(BG3MSE_GAME_APP)
    add_test(NAME bg3mse_game_component_signals COMMAND bg3mse_component_signal_tests "${BG3MSE_GAME_APP}")
endif()

# What the extender puts on the wire (Mac/Tests/NetWireTests.cpp), compiled
# with Norbyte's definitions and his protocol as bg3se_extender is.
add_executable(bg3mse_net_wire_tests Mac/Tests/NetWireTests.cpp "${PROTO_OUT}/Extender/Shared/ExtenderProtocol.pb.cc")
target_include_directories(bg3mse_net_wire_tests SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_net_wire_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_net_wire_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
target_link_libraries(bg3mse_net_wire_tests PRIVATE "${EXT}/protobuf-build/libprotobuf-lite.a" ${BG3SE_PROTOBUF_DEPS}
    "-framework CoreFoundation")
add_dependencies(bg3mse_net_wire_tests bg3se_proto)
add_test(NAME bg3mse_net_wire_tests COMMAND bg3mse_net_wire_tests)

# The hooks Norbyte's code puts directly on the game's and the story
# library's functions, run on a stand-in called as the engine table says the
# game calls each (Mac/Tests/HookTests.cpp); his detours are compiled as his
# sources are. With the game, the census of the code the patch of
# ls::ModuleSettings::IsModded must cover.
add_library(bg3mse_hook_tests_norbyte OBJECT Mac/Tests/HookTestsNorbyte.cpp)
target_include_directories(bg3mse_hook_tests_norbyte SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_hook_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_hook_tests_norbyte PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20)
add_dependencies(bg3mse_hook_tests_norbyte bg3se_proto)
add_executable(bg3mse_hook_tests Mac/Tests/HookTests.cpp Mac/Tests/HookCallTargets.S $<TARGET_OBJECTS:bg3mse_hook_tests_norbyte>
    $<TARGET_OBJECTS:bg3mse_binding_rows>)
target_link_libraries(bg3mse_hook_tests PRIVATE bg3mse_binding bg3mse_shim)
target_compile_options(bg3mse_hook_tests PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_hook_tests COMMAND bg3mse_hook_tests)

# Every place in the game that answers whether it is modded is one the patch
# of IsModded covers, or one known not to ask.
add_executable(bg3mse_is_modded_sites Mac/Tests/IsModdedSites.cpp $<TARGET_OBJECTS:bg3mse_binding_rows>)
target_link_libraries(bg3mse_is_modded_sites PRIVATE bg3mse_binding)
target_compile_options(bg3mse_is_modded_sites PRIVATE ${BG3MSE_WARNINGS})
if(BG3MSE_GAME_APP)
    add_test(NAME bg3mse_game_is_modded_sites COMMAND bg3mse_is_modded_sites "${BG3MSE_GAME_APP}")
endif()

# Dear ImGui's Metal renderer, as it is (it needs automatic reference
# counting), with the definitions bg3se_imgui is compiled with: the two must
# agree about ImGui's structures.
add_library(bg3se_imgui_metal STATIC "${EXT}/imgui/backends/imgui_impl_metal.mm")
target_include_directories(bg3se_imgui_metal PRIVATE "${EXT}/imgui")
target_compile_definitions(bg3se_imgui_metal PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS NDEBUG _LIB)
# The file names a storage mode the SDK has since deprecated.
target_compile_options(bg3se_imgui_metal PRIVATE -fobjc-arc -Wno-deprecated-declarations)

# The Mac rendering backend and input of Norbyte's ImGui overlay (Mac/Overlay),
# less what needs Norbyte's headers: the checks of what it depends on in the
# game, the event tap and the translation of events, the frame hooks and the
# Metal pass. It needs the engine table, the platform layer, ImGui's headers
# and SDL's (for the values of the events Norbyte's code is given; nothing of
# SDL is linked).
add_library(bg3mse_overlay STATIC
    Mac/Overlay/Dependencies.cpp
    Mac/Overlay/GameDependencies.cpp
    Mac/Overlay/ObjcClassesOfFile.cpp
    Mac/Overlay/ObjcClassesOfRuntime.cpp
    Mac/Overlay/KeyCodes.cpp
    Mac/Overlay/InputEvents.cpp
    Mac/Overlay/FontAtlas.cpp
    Mac/Overlay/ConsoleWindow.cpp
    Mac/Overlay/ColourSpace.cpp
    Mac/Overlay/Controller.cpp
    Mac/Overlay/ControllerHooks.mm
    Mac/Overlay/Pointer.mm
    Mac/Overlay/KeyboardLayout.mm
    Mac/Overlay/TextInput.mm
    Mac/Overlay/EventTap.mm
    Mac/Overlay/FrameHooks.mm
    Mac/Overlay/MetalOverlay.mm)
target_include_directories(bg3mse_overlay SYSTEM PUBLIC "${EXT}/imgui" "${EXT}/SDL/include")
target_compile_definitions(bg3mse_overlay PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(bg3mse_overlay PUBLIC bg3mse_engine bg3se_imgui_metal objc
    "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework Foundation")
target_link_libraries(bg3mse_overlay PUBLIC "-framework CoreGraphics")
target_link_libraries(bg3mse_overlay PUBLIC "-framework GameController")
target_compile_options(bg3mse_overlay PRIVATE ${BG3MSE_WARNINGS})
# The console window shows the screen of the Win32 console (Mac/Shim/ConsoleScreen.h).
target_link_libraries(bg3mse_overlay PUBLIC bg3mse_shim)
set_source_files_properties(Mac/Overlay/EventTap.mm Mac/Overlay/FrameHooks.mm Mac/Overlay/MetalOverlay.mm
    PROPERTIES COMPILE_OPTIONS -fobjc-arc)
set_source_files_properties(Mac/Overlay/ControllerHooks.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
set_source_files_properties(Mac/Overlay/Pointer.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
set_source_files_properties(Mac/Overlay/KeyboardLayout.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
set_source_files_properties(Mac/Overlay/TextInput.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_link_libraries(bg3mse_overlay PUBLIC "-framework Carbon")

# The part of Mac/Overlay that is written against Norbyte's headers: the
# rendering backend IMGUI.cpp creates, the definitions of his SDLManager, and
# the texture loading. Compiled as bg3mse_layout_bound is, with this build's
# warnings for what these files themselves say.
add_library(bg3mse_overlay_bound OBJECT
    Mac/Overlay/CocoaInput.cpp
    Mac/Overlay/GameTextures.cpp
    Mac/Overlay/MetalBackend.cpp)
target_include_directories(bg3mse_overlay_bound PRIVATE "${CMAKE_SOURCE_DIR}/Mac")
target_include_directories(bg3mse_overlay_bound SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_overlay_bound PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_overlay_bound PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20 ${BG3MSE_WARNINGS})
add_dependencies(bg3mse_overlay_bound bg3se_proto)

add_executable(bg3mse_overlay_tests Mac/Tests/OverlayTests.cpp Mac/Tests/OverlayTestsObjc.mm)
target_link_libraries(bg3mse_overlay_tests PRIVATE bg3mse_overlay bg3se_imgui "-framework CoreGraphics")
# The Metal tests drive Norbyte's mirrors of ImGui's textures as the backend
# does: his TextureMirror.inl as it is, with a stand-in for the one header of
# his it asks for.
target_include_directories(bg3mse_overlay_tests PRIVATE Mac/Tests/MirrorStandIn)
target_compile_options(bg3mse_overlay_tests PRIVATE ${BG3MSE_WARNINGS})
set_source_files_properties(Mac/Tests/OverlayTestsObjc.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
add_dependencies(bg3mse_overlay_tests bg3mse_test_functions)
target_compile_definitions(bg3mse_overlay_tests PRIVATE BG3MSE_TEST_FUNCTIONS="$<TARGET_FILE:bg3mse_test_functions>")
add_test(NAME bg3mse_overlay_tests COMMAND bg3mse_overlay_tests)

# bg3mse-overlay check <Game.app> prints every dependency of the overlay for a
# game and exits non-zero when a part of the overlay would be off in it.
add_executable(bg3mse-overlay Mac/Tools/OverlayTool.cpp)
target_link_libraries(bg3mse-overlay PRIVATE bg3mse_overlay)
target_compile_options(bg3mse-overlay PRIVATE ${BG3MSE_WARNINGS})
if(BG3MSE_GAME_APP)
    add_test(NAME bg3mse_game_overlay_check COMMAND bg3mse-overlay check "${BG3MSE_GAME_APP}")
endif()

# --- What libbg3mse.dylib is made of -------------------------------------------
#
# Its own code (Mac/Main.cpp, the platform layer, shader provisioning, the
# graphics fixes when they are on), and Norbyte's extender with everything
# that connects it to the macOS game. A symbol nothing defines fails the link.
#
# The dylib also carries the extender's two resources, room for
# islands in its header (hooks on its own functions, which Norbyte's code
# detours: the bridges of Mac/Binding and the platform's file reader hook),
# and Mac/Main.cpp is compiled to start it when bg3mse.conf says so.
#
# Every library is named here, the ones others depend on last, and nothing is
# left for CMake to add after the image's initializer: see bg3mse_entry.
# The inputs of an image that holds the extender (BG3SE_IMAGE_*), by target.
set_property(SOURCE Mac/Main.cpp APPEND PROPERTY COMPILE_DEFINITIONS BG3MSE_EXTENDER=1)
foreach(objects IN LISTS BG3SE_IMAGE_OBJECTS)
    target_sources(bg3mse PRIVATE $<TARGET_OBJECTS:${objects}>)
endforeach()
set(frameworks "")
foreach(framework IN LISTS BG3SE_IMAGE_FRAMEWORKS)
    list(APPEND frameworks "-framework ${framework}")
endforeach()
# bg3mse_gfx, when it is built in, depends on bg3mse_core: before it.
set(libraries ${BG3SE_IMAGE_LIBRARIES})
if(BG3MSE_GFX)
    list(FIND libraries bg3mse_core core_at)
    list(INSERT libraries ${core_at} bg3mse_gfx)
endif()
target_link_libraries(bg3mse PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:bg3se_extender>"
    ${libraries} ${BG3SE_IMAGE_ARCHIVES} ${frameworks})
# For the check after the link: the libraries from outside this build.
list(JOIN BG3SE_IMAGE_ARCHIVES "\n" archives)
file(CONFIGURE OUTPUT "${BG3MSE_ARCHIVE_LIST}" CONTENT "${archives}\n" @ONLY)
add_dependencies(bg3mse bg3se_extender bg3se_resource_files)
target_link_options(bg3mse PRIVATE ${BG3SE_RESOURCE_LINK_OPTIONS} -Wl,-headerpad,0x1000)
set_property(TARGET bg3mse APPEND PROPERTY LINK_DEPENDS ${BG3SE_RESOURCE_FILES} $<TARGET_FILE:bg3se_extender>)
# What the post-build check requires of the header room asked for above.
set_property(TARGET bg3mse PROPERTY BG3MSE_MIN_HEADER_PADDING 2048)
# Norbyte's native API: the functions of BG3Extender/Exports.def that a
# native module calls, under the same names, for dlsym as on Windows for
# GetProcAddress. The post-build check requires each, and nothing else.
set(exported "")
foreach(function SEUpdaterGetModule SEUpdaterNotifyWhenLoaded SEGetAPIVersion SERegisterLuaStateChangeCallback
        SEScheduleCallbackOnThread SEEnterLuaVM SEExitLuaVM)
    target_link_options(bg3mse PRIVATE "LINKER:-exported_symbol,_${function}")
    list(APPEND exported "_${function}")
endforeach()
list(JOIN exported "," exported)
set_property(TARGET bg3mse PROPERTY BG3MSE_EXPORTED "${exported}")
target_link_libraries(bg3mse PRIVATE "-Wl,-force_load,$<TARGET_FILE:bg3mse_entry>")
add_dependencies(bg3mse bg3mse_entry)

# The dylib as a file that is loaded (Mac/Tests/DylibLoadTests.cpp): loading
# it into another process does nothing, and in a process named like the game
# the script_extender setting decides whether the extender's start-up runs.
add_executable(bg3mse_game_stand_in Mac/Tests/GameStandIn.cpp)
add_executable(bg3mse_dylib_tests Mac/Tests/DylibLoadTests.cpp)
target_compile_options(bg3mse_dylib_tests PRIVATE ${BG3MSE_WARNINGS})
target_link_libraries(bg3mse_dylib_tests PRIVATE bg3mse_version)
add_dependencies(bg3mse_dylib_tests bg3mse bg3mse_game_stand_in)
# What the dylib tests put in place of the system's alerts (Mac/Tests/AlertRecorder.c),
# found next to the stand-in.
add_library(bg3mse_alert_recorder SHARED Mac/Tests/AlertRecorder.c)
target_link_libraries(bg3mse_alert_recorder PRIVATE "-framework CoreFoundation")
add_dependencies(bg3mse_dylib_tests bg3mse_alert_recorder)
add_test(NAME bg3mse_dylib_tests COMMAND bg3mse_dylib_tests $<TARGET_FILE:bg3mse> $<TARGET_FILE:bg3mse_game_stand_in>
    1)

if(BG3MSE_GAME_APP)
    # The dylib does not ask for a later macOS than the game it is loaded into.
    add_test(NAME bg3mse_game_minimum_macos COMMAND "${CMAKE_COMMAND}"
        "-DDYLIB=$<TARGET_FILE:bg3mse>" "-DEXPORTED=$<TARGET_PROPERTY:bg3mse,BG3MSE_EXPORTED>"
        "-DMACOS_MINIMUM=${BG3MSE_MACOS_MINIMUM}" "-DGAME_APP=${BG3MSE_GAME_APP}"
        -P "${CMAKE_SOURCE_DIR}/Mac/Tools/CheckDylib.cmake")
    if(BG3MSE_CAMERA)
        add_test(NAME bg3mse_game_camera_minimum_macos COMMAND "${CMAKE_COMMAND}"
            "-DDYLIB=$<TARGET_FILE:bg3mse_camera>" "-DMACOS_MINIMUM=${BG3MSE_MACOS_MINIMUM}" "-DGAME_APP=${BG3MSE_GAME_APP}"
            -P "${CMAKE_SOURCE_DIR}/Mac/Tools/CheckDylib.cmake")
    endif()
endif()

# bg3mse's Lua is bound inside the dylib and neither exported nor
# imported; its resource sections hold the two files, byte for byte.
add_test(NAME bg3mse_dylib_lua_isolation COMMAND "${CMAKE_COMMAND}"
    "-DIMAGE=$<TARGET_FILE:bg3mse>"
    "-DLUA_ARCHIVE=$<TARGET_FILE:bg3se_lua>"
    -DEXPECT_LUA=ON
    -P "${CMAKE_SOURCE_DIR}/Mac/Tools/CheckLuaIsolation.cmake")
add_test(NAME bg3mse_dylib_resources COMMAND "${CMAKE_COMMAND}"
    "-DIMAGE=$<TARGET_FILE:bg3mse>"
    "-DRESOURCES=SCRIPT_EXTENDER,${IDR_LUA_BUILTIN_BUNDLE},${BG3SE_LUA_BUNDLE}$<SEMICOLON>SCRIPT_EXTENDER,${IDR_BINARY_MAPPINGS},${BG3SE_BINARY_MAPPINGS}"
    "-DSCRATCH=${CMAKE_BINARY_DIR}/resource_check_dylib"
    -P "${CMAKE_SOURCE_DIR}/Mac/Tools/CheckResources.cmake")

# The part of the extender's start-up that needs no game, run without one
# (Mac/Tests/StartupTests.cpp): an executable linked from what
# libbg3mse.dylib links Norbyte's extender from (BG3SE_IMAGE_*), with the
# extender's resources, and the test's own file compiled as his sources
# are. It carries nothing of bg3mse's entry (Mac/Main.cpp, Mac/Entry.cpp)
# and is the same in every configuration.
add_executable(bg3mse_startup_tests Mac/Tests/StartupTests.cpp Mac/Tests/StartupTestsPosix.cpp
    Mac/Tests/StartupLuaDebugAdapter.cpp Mac/Tests/StartupStoryDebugger.cpp Mac/Tests/StartupStoryQueries.cpp
    Mac/Tests/StartupStoryTargets.S)
target_include_directories(bg3mse_startup_tests PRIVATE "${CMAKE_SOURCE_DIR}/Mac")
target_include_directories(bg3mse_startup_tests SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_startup_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_startup_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20 ${BG3MSE_WARNINGS})
foreach(objects IN LISTS BG3SE_IMAGE_OBJECTS)
    target_sources(bg3mse_startup_tests PRIVATE $<TARGET_OBJECTS:${objects}>)
endforeach()
set(frameworks "")
foreach(framework IN LISTS BG3SE_IMAGE_FRAMEWORKS)
    list(APPEND frameworks "-framework ${framework}")
endforeach()
target_link_libraries(bg3mse_startup_tests PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:bg3se_extender>"
    ${BG3SE_IMAGE_LIBRARIES} ${BG3SE_IMAGE_ARCHIVES} ${frameworks})
add_dependencies(bg3mse_startup_tests bg3se_extender bg3se_resource_files bg3se_proto)
target_link_options(bg3mse_startup_tests PRIVATE ${BG3SE_RESOURCE_LINK_OPTIONS})
set_property(TARGET bg3mse_startup_tests APPEND PROPERTY LINK_DEPENDS ${BG3SE_RESOURCE_FILES} $<TARGET_FILE:bg3se_extender>)
add_test(NAME bg3mse_startup_tests COMMAND bg3mse_startup_tests)
# The game's other ends (Mac/Binding/Shutdown.h): a quit while the server
# waits without ticking, and the end of the run loop.
add_test(NAME bg3mse_startup_quit_idle COMMAND bg3mse_startup_tests --exit quit-idle)
add_test(NAME bg3mse_startup_teardown COMMAND bg3mse_startup_tests --exit teardown)
# Norbyte's Lua debugger, started as EnableLuaDebugger starts it, and a
# frontend on another thread that connects over loopback: the handshake,
# a breakpoint hit on the server, the frame's variables, an evaluation,
# continue. On a port of its own, one run at a time.
add_test(NAME bg3mse_startup_lua_debugger COMMAND bg3mse_startup_tests --lua-debugger 39917)
set_tests_properties(bg3mse_startup_lua_debugger PROPERTIES RESOURCE_LOCK bg3mse_lua_debugger_port)
# Norbyte's story debugger, started as EnableDebugger starts it, with a
# story of the test's own behind his hooks and a frontend on another
# thread: identify, the stops at the story's load and the game's start
# and end, a pause, a breakpoint on a rule action, a step, the story's
# synchronisation, the database's contents, evaluation. On a free port.
# At the run loop's end the story layer ends the server's thread.
add_test(NAME bg3mse_startup_story_debugger COMMAND bg3mse_startup_tests --story-debugger --exit teardown)
set_tests_properties(bg3mse_startup_story_debugger PROPERTIES TIMEOUT 300)
# What the start-up harness checks the flavour against.
if(BG3MSE_CHANNEL STREQUAL "Release")
    target_compile_definitions(bg3mse_startup_tests PRIVATE BG3MSE_EXPECTED_CHANNEL="Release" BG3MSE_EXPECTED_DEVELOPER=0)
else()
    target_compile_definitions(bg3mse_startup_tests PRIVATE BG3MSE_EXPECTED_CHANNEL="${BG3MSE_CHANNEL}" BG3MSE_EXPECTED_DEVELOPER=1)
endif()

# Norbyte's merge of virtual texture tile sets, run without a game
# (Mac/Tests/VirtualTextureTests.cpp), linked as bg3mse_startup_tests is;
# given the game's bundle, also on one of its tile sets, which is only read.
add_executable(bg3mse_vt_tests Mac/Tests/VirtualTextureTests.cpp)
target_include_directories(bg3mse_vt_tests PRIVATE "${CMAKE_SOURCE_DIR}/Mac")
target_include_directories(bg3mse_vt_tests SYSTEM PRIVATE $<TARGET_PROPERTY:bg3se_extender,INCLUDE_DIRECTORIES>)
target_compile_definitions(bg3mse_vt_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_DEFINITIONS>)
target_compile_options(bg3mse_vt_tests PRIVATE $<TARGET_PROPERTY:bg3se_extender,COMPILE_OPTIONS>
    -Wno-delayed-template-parsing-in-cxx20 ${BG3MSE_WARNINGS})
foreach(objects IN LISTS BG3SE_IMAGE_OBJECTS)
    target_sources(bg3mse_vt_tests PRIVATE $<TARGET_OBJECTS:${objects}>)
endforeach()
set(frameworks "")
foreach(framework IN LISTS BG3SE_IMAGE_FRAMEWORKS)
    list(APPEND frameworks "-framework ${framework}")
endforeach()
target_link_libraries(bg3mse_vt_tests PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:bg3se_extender>"
    ${BG3SE_IMAGE_LIBRARIES} ${BG3SE_IMAGE_ARCHIVES} ${frameworks})
add_dependencies(bg3mse_vt_tests bg3se_extender bg3se_resource_files bg3se_proto)
target_link_options(bg3mse_vt_tests PRIVATE ${BG3SE_RESOURCE_LINK_OPTIONS})
set_property(TARGET bg3mse_vt_tests APPEND PROPERTY LINK_DEPENDS ${BG3SE_RESOURCE_FILES} $<TARGET_FILE:bg3se_extender>)
if(BG3MSE_GAME_APP)
    add_test(NAME bg3mse_vt_tests COMMAND bg3mse_vt_tests "${BG3MSE_GAME_APP}")
else()
    add_test(NAME bg3mse_vt_tests COMMAND bg3mse_vt_tests)
endif()

# The in-game test pass (Mac/Tests/InGame): the tool that compiles the test
# mod's Lua, writes its package and reads its results back from the log.
set(BG3MSE_INGAME "${CMAKE_SOURCE_DIR}/Mac/Tests/InGame")
add_executable(bg3mse-ingame Mac/Tests/InGame/InGameTool.cpp)
target_include_directories(bg3mse-ingame PRIVATE "${CMAKE_SOURCE_DIR}/Mac")
target_link_libraries(bg3mse-ingame PRIVATE bg3mse_core bg3se_lua)
target_compile_options(bg3mse-ingame PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_ingame_check_resumed COMMAND "${CMAKE_COMMAND}" -DTOOL=$<TARGET_FILE:bg3mse-ingame>
    "-DLOGS=${BG3MSE_INGAME}/Tests/crashed-log.txt;${BG3MSE_INGAME}/Tests/resumed-log.txt"
    -DEXPECTED=${BG3MSE_INGAME}/Tests/resumed.expected -DEXIT=1 -P "${BG3MSE_INGAME}/CheckLog.cmake")
add_test(NAME bg3mse_ingame_check_resumed_alone COMMAND "${CMAKE_COMMAND}" -DTOOL=$<TARGET_FILE:bg3mse-ingame>
    "-DLOGS=${BG3MSE_INGAME}/Tests/resumed-log.txt"
    -DEXPECTED=${BG3MSE_INGAME}/Tests/resumed-alone.expected -DEXIT=1 -P "${BG3MSE_INGAME}/CheckLog.cmake")
add_test(NAME bg3mse_ingame_check_complete COMMAND "${CMAKE_COMMAND}" -DTOOL=$<TARGET_FILE:bg3mse-ingame>
    "-DLOGS=${BG3MSE_INGAME}/Tests/offline-log.txt" -DEXIT=0 -P "${BG3MSE_INGAME}/CheckLog.cmake")
# The reset phase: a run that went through it, one whose launch ended during
# the reset, and that one with the launch that resumed after it.
add_test(NAME bg3mse_ingame_check_reset COMMAND "${CMAKE_COMMAND}" -DTOOL=$<TARGET_FILE:bg3mse-ingame>
    "-DLOGS=${BG3MSE_INGAME}/Tests/reset-log.txt"
    -DEXPECTED=${BG3MSE_INGAME}/Tests/reset.expected -DEXIT=0 -P "${BG3MSE_INGAME}/CheckLog.cmake")
add_test(NAME bg3mse_ingame_check_reset_crashed COMMAND "${CMAKE_COMMAND}" -DTOOL=$<TARGET_FILE:bg3mse-ingame>
    "-DLOGS=${BG3MSE_INGAME}/Tests/reset-crashed-log.txt"
    -DEXPECTED=${BG3MSE_INGAME}/Tests/reset-crashed.expected -DEXIT=1 -P "${BG3MSE_INGAME}/CheckLog.cmake")
add_test(NAME bg3mse_ingame_check_reset_resumed COMMAND "${CMAKE_COMMAND}" -DTOOL=$<TARGET_FILE:bg3mse-ingame>
    "-DLOGS=${BG3MSE_INGAME}/Tests/reset-crashed-log.txt;${BG3MSE_INGAME}/Tests/reset-resumed-log.txt"
    -DEXPECTED=${BG3MSE_INGAME}/Tests/reset-resumed.expected -DEXIT=0 -P "${BG3MSE_INGAME}/CheckLog.cmake")
# A two-machine run: the host's log and the other machine's (Windows line
# endings, no bg3mse line), lined up by machine.
add_test(NAME bg3mse_ingame_check_two_machines COMMAND "${CMAKE_COMMAND}" -DTOOL=$<TARGET_FILE:bg3mse-ingame>
    "-DLOGS=${BG3MSE_INGAME}/Tests/mp-host-log.txt;${BG3MSE_INGAME}/Tests/mp-remote-log.txt"
    -DEXPECTED=${BG3MSE_INGAME}/Tests/mp.expected -DEXIT=0 -P "${BG3MSE_INGAME}/CheckLog.cmake")
# The test mod (Mac/Tests/InGame/Mod): a Script Extender mod that calls the
# Lua API once per function and writes one line per call to the log. Its
# package is written here, in the build directory, to be installed by hand.
set(BG3MSE_INGAME_LUA "${BG3MSE_INGAME}/Mod/Mods/Bg3mseInGameTests/ScriptExtender/Lua")
file(GLOB_RECURSE BG3MSE_INGAME_MOD_FILES CONFIGURE_DEPENDS "${BG3MSE_INGAME}/Mod/*")
file(GLOB_RECURSE BG3MSE_INGAME_LUA_FILES CONFIGURE_DEPENDS "${BG3MSE_INGAME}/*.lua")
set(BG3MSE_INGAME_PAK "${CMAKE_BINARY_DIR}/Bg3mseInGameTests.pak")
add_custom_command(OUTPUT "${BG3MSE_INGAME_PAK}"
    COMMAND bg3mse-ingame lua-check ${BG3MSE_INGAME_LUA_FILES}
    COMMAND bg3mse-ingame pack "${BG3MSE_INGAME}/Mod" "${BG3MSE_INGAME_PAK}"
    DEPENDS bg3mse-ingame ${BG3MSE_INGAME_MOD_FILES}
    VERBATIM)
add_custom_target(bg3mse_ingame_pak ALL DEPENDS "${BG3MSE_INGAME_PAK}")
add_test(NAME bg3mse_ingame_lua_check COMMAND bg3mse-ingame lua-check ${BG3MSE_INGAME_LUA_FILES})
add_test(NAME bg3mse_ingame_pak COMMAND bg3mse-ingame pak-compare "${BG3MSE_INGAME}/Mod" "${BG3MSE_INGAME_PAK}")
# The story tests (Mac/Tests/InGame/StoryMod): a second mod, with a goal of
# its own, kept apart so that the in-game tests' mod changes no story. Its
# package is the mod's folder with the in-game tests' runner copied in.
set(BG3MSE_STORY_MOD "${BG3MSE_INGAME}/StoryMod")
set(BG3MSE_STORY_STAGE "${CMAKE_BINARY_DIR}/story-mod")
set(BG3MSE_STORY_PAK "${CMAKE_BINARY_DIR}/Bg3mseStoryTests.pak")
file(GLOB_RECURSE BG3MSE_STORY_MOD_FILES CONFIGURE_DEPENDS "${BG3MSE_STORY_MOD}/*")
file(GLOB_RECURSE BG3MSE_STORY_LUA_FILES CONFIGURE_DEPENDS "${BG3MSE_STORY_MOD}/*.lua")
add_custom_command(OUTPUT "${BG3MSE_STORY_PAK}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${BG3MSE_STORY_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${BG3MSE_STORY_MOD}" "${BG3MSE_STORY_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${BG3MSE_INGAME_LUA}/Runner.lua"
        "${BG3MSE_STORY_STAGE}/Mods/Bg3mseStoryTests/ScriptExtender/Lua/Runner.lua"
    COMMAND bg3mse-ingame lua-check ${BG3MSE_STORY_LUA_FILES}
    COMMAND bg3mse-ingame pack "${BG3MSE_STORY_STAGE}" "${BG3MSE_STORY_PAK}"
    DEPENDS bg3mse-ingame ${BG3MSE_STORY_MOD_FILES} "${BG3MSE_INGAME_LUA}/Runner.lua"
    VERBATIM)
add_custom_target(bg3mse_story_pak ALL DEPENDS "${BG3MSE_STORY_PAK}")
add_test(NAME bg3mse_ingame_story_pak COMMAND bg3mse-ingame pak-compare "${BG3MSE_STORY_STAGE}" "${BG3MSE_STORY_PAK}")
# The cases that need no session, run in the client and server Lua states of
# the start-up harness, which also report every registered function and event
# that no case names. The files in the order Libraries.lua gives.
if(TARGET bg3mse_startup_tests)
    file(READ "${BG3MSE_INGAME_LUA}/Libraries.lua" libraries)
    string(REGEX MATCHALL "\"[A-Za-z]+\"" libraries "${libraries}")
    set(chunks --lua "${BG3MSE_INGAME_LUA}/Runner.lua")
    foreach(library IN LISTS libraries)
        string(REPLACE "\"" "" library "${library}")
        list(APPEND chunks --lua "${BG3MSE_INGAME_LUA}/Cases/${library}.lua")
    endforeach()
    list(APPEND chunks --lua "${BG3MSE_INGAME_LUA}/Cases/Events.lua" --lua "${BG3MSE_INGAME}/Offline.lua")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BG3MSE_INGAME_LUA}/Libraries.lua")
    add_test(NAME bg3mse_ingame_offline COMMAND bg3mse_startup_tests ${chunks})
endif()

# Norbyte's Lua debug adapter for macOS, and its test with the Lua debugger (Mac/LuaDebugger).
include(Mac/LuaDebugger/LuaDebugger.cmake)

include(Mac/Installer/Installer.cmake)

# What a build without BG3MSE_TESTS makes (Mac/Release/Release.cmake): only
# what a release ships, and what that is built from. Every other target is
# left out of "all"; the libraries the shipped ones link are still built for
# them.
function(bg3mse_leave_out_tests)
    if(BG3MSE_TESTS)
        return()
    endif()
    set(ships bg3mse bg3mse_loader bg3mse-launch bg3mse-install bg3mse_installer_app bg3mse-sign)
    foreach(target bg3mse_camera bg3mse_camera_texts bg3mse_lua_debug_adapter)
        if(TARGET ${target})
            list(APPEND ships ${target})
        endif()
    endforeach()
    set(directories "${CMAKE_SOURCE_DIR}")
    while(directories)
        list(POP_FRONT directories directory)
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(NOT type STREQUAL "INTERFACE_LIBRARY" AND NOT target IN_LIST ships)
                set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL TRUE)
            endif()
        endforeach()
        get_property(below DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        list(APPEND directories ${below})
    endwhile()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL bg3mse_leave_out_tests)

# The release, its tests and tools (Mac/Release).
include(Mac/Release/Release.cmake)

# The updater: the loader the game loads, the signing tool, their tests (Mac/Updater).
include(Mac/Updater/Updater.cmake)
