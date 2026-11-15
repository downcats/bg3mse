# The release, included at the end of the build, where every target exists:
#
#   cmake --build <dir> --target release
#
# builds everything, then Mac/Release/Assemble.cmake checks that the build
# may be released, runs ctest, and puts the release together in
# <dir>/release (see there). Its tests and tools are here too.

# How the shipped images are signed: "-" is ad hoc, as the linker signs them.
# A Developer ID is the name of its certificate in the keychain
# ("Developer ID Application: <name> (<team>)"); see Mac/Release/Symbols.cmake.
set(BG3MSE_SIGN_IDENTITY "-" CACHE STRING "The codesign identity of the release's images (- signs ad hoc)")
# Notarisation: the name of a notarytool keychain profile (xcrun notarytool
# store-credentials <name>), which holds the Apple ID's credentials. Given
# with a Developer ID, the release target submits what it signed, waits for
# Apple's answer and staples the ticket to bg3mse.app; empty: not notarised.
set(BG3MSE_NOTARY_PROFILE "" CACHE STRING "The notarytool keychain profile to notarise the release with; empty: not notarised")

# A crash report's backtrace, named by the release's dSYM files.
add_executable(bg3mse-symbolicate Mac/Release/Symbolicate.cpp)
target_compile_options(bg3mse-symbolicate PRIVATE ${BG3MSE_WARNINGS})

# --- Tests --------------------------------------------------------------------------

# A report from a stripped dylib is symbolicated with its dSYM: a dylib that
# carries the crash reporter, as libbg3mse.dylib does, with line tables
# whatever BG3MSE_DEBUG_INFO says, and what loads it and has it crash.
add_library(bg3mse_release_crash_fixture SHARED Mac/Tests/ReleaseCrashFixture.cpp)
target_link_libraries(bg3mse_release_crash_fixture PRIVATE bg3mse_crash)
target_compile_options(bg3mse_release_crash_fixture PRIVATE -gline-tables-only ${BG3MSE_WARNINGS})
add_executable(bg3mse_release_crash_driver Mac/Tests/ReleaseCrashDriver.cpp)
target_compile_options(bg3mse_release_crash_driver PRIVATE ${BG3MSE_WARNINGS})
add_test(NAME bg3mse_release_symbolication COMMAND "${CMAKE_COMMAND}"
    "-DFIXTURE=$<TARGET_FILE:bg3mse_release_crash_fixture>"
    "-DDRIVER=$<TARGET_FILE:bg3mse_release_crash_driver>"
    "-DSYMBOLICATE=$<TARGET_FILE:bg3mse-symbolicate>"
    "-DSOURCE=${CMAKE_SOURCE_DIR}/Mac/Tests/ReleaseCrashFixture.cpp"
    "-DSYMBOLS_SCRIPT=${CMAKE_SOURCE_DIR}/Mac/Release/Symbols.cmake"
    "-DSCRATCH=${CMAKE_BINARY_DIR}/release_symbolication"
    -P "${CMAKE_SOURCE_DIR}/Mac/Tests/ReleaseSymbolication.cmake")

# The release holds what it ships and nothing else.
add_test(NAME bg3mse_release_contents COMMAND "${CMAKE_COMMAND}"
    "-DCONTENTS_SCRIPT=${CMAKE_SOURCE_DIR}/Mac/Release/Contents.cmake"
    "-DSCRATCH=${CMAKE_BINARY_DIR}/release_contents"
    -P "${CMAKE_SOURCE_DIR}/Mac/Tests/ReleaseContents.cmake")

# Each dylib carries the build's version, as a tool reads it from the file.
set(stamped bg3mse)
if(TARGET bg3mse_camera)
    list(APPEND stamped bg3mse_camera)
endif()
foreach(target IN LISTS stamped)
    add_test(NAME ${target}_dylib_version COMMAND "${CMAKE_COMMAND}"
        "-DIMAGE=$<TARGET_FILE:${target}>" "-DVERSION=${BG3MSE_VERSION}" "-DCOMMIT=${BG3MSE_COMMIT}"
        "-DEXTENDER=${BG3MSE_EXTENDER_VERSION}" "-DCHANNEL=${BG3MSE_CHANNEL}" -DDYLIB=ON
        -P "${CMAKE_SOURCE_DIR}/Mac/Release/Stamp.cmake")
endforeach()

# --- The release target ---------------------------------------------------------------
#
# Defined once the whole directory tree is configured: it depends on what
# "all" builds (ctest needs all of it), and ships the installer tools when the
# build has them. Those are the executables built from Mac/Installer whose
# names do not say they are tests, and not from Mac/Installer/Tests (the
# installer tests' fixtures). A build without BG3MSE_TESTS has none: the
# target runs the tests.
function(bg3mse_define_release)
    if(NOT BG3MSE_TESTS)
        return()
    endif()
    set(directories "${CMAKE_SOURCE_DIR}")
    set(targets "")
    while(directories)
        list(POP_FRONT directories directory)
        get_property(found DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        list(APPEND targets ${found})
        get_property(below DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        list(APPEND directories ${below})
    endwhile()

    set(built "")
    set(installers "")
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        get_target_property(excluded ${target} EXCLUDE_FROM_ALL)
        if(type STREQUAL "INTERFACE_LIBRARY" OR excluded)
            continue()
        endif()
        list(APPEND built ${target})
        if(NOT type STREQUAL "EXECUTABLE" OR target MATCHES "[Tt]est")
            continue()
        endif()
        get_target_property(sources ${target} SOURCES)
        get_target_property(source_dir ${target} SOURCE_DIR)
        foreach(source IN LISTS sources)
            get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${source_dir}")
            string(FIND "${source}" "${CMAKE_SOURCE_DIR}/Mac/Installer/" at)
            string(FIND "${source}" "${CMAKE_SOURCE_DIR}/Mac/Installer/Tests/" test_at)
            string(FIND "${source}" "${CMAKE_SOURCE_DIR}/Mac/Installer/App/" app_at)
            if(at EQUAL 0 AND NOT test_at EQUAL 0 AND NOT app_at EQUAL 0)
                list(APPEND installers "$<TARGET_FILE:${target}>")
                break()
            endif()
        endforeach()
    endforeach()
    list(JOIN installers "|" installers)

    set(camera "")
    set(camera_defaults "")
    if(TARGET bg3mse_camera)
        set(camera "$<TARGET_FILE:bg3mse_camera>")
        set(camera_defaults "${BG3MSE_CAMERA_DEFAULTS}")
    endif()
    set(loader "")
    if(TARGET bg3mse_loader)
        set(loader "$<TARGET_FILE:bg3mse_loader>")
    endif()
    # Norbyte's Lua debug adapter, when the build has it (Mac/LuaDebugger).
    set(debug_adapter "")
    set(debug_adapter_files "")
    if(TARGET bg3mse_lua_debug_adapter)
        set(debug_adapter "${BG3MSE_DEBUG_ADAPTER_DIR}")
        list(JOIN BG3MSE_DEBUG_ADAPTER_FILES "|" debug_adapter_files)
    endif()
    add_custom_target(release
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_BINARY_DIR}" "-DSCRIPTS=${CMAKE_SOURCE_DIR}/Mac/Release"
            "-DVERSION=${BG3MSE_VERSION}" "-DCOMMIT=${BG3MSE_COMMIT}" "-DEXTENDER=${BG3MSE_EXTENDER_VERSION}"
            "-DCHANNEL=${BG3MSE_CHANNEL}" "-DBUILD_TYPE=${CMAKE_BUILD_TYPE}" "-DDEBUG_INFO=${BG3MSE_DEBUG_INFO}"
            "-DGAME_APP=${BG3MSE_GAME_APP}" "-DMACOS_MINIMUM=${BG3MSE_MACOS_MINIMUM}"
            "-DDYLIB=$<TARGET_FILE:bg3mse>" "-DCAMERA=${camera}" "-DCAMERA_DEFAULTS=${camera_defaults}" "-DINSTALLERS=${installers}"
            "-DLOADER=${loader}" "-DUPDATER_KEY=${BG3MSE_UPDATER_PUBLIC_KEY}" "-DSELF_UPDATE=${BG3MSE_SELF_UPDATE}" "-DUPDATER_URL=${BG3MSE_UPDATER_URL}"
            "-DDEBUG_ADAPTER=${debug_adapter}" "-DDEBUG_ADAPTER_FILES=${debug_adapter_files}"
            "-DDYLIB_TESTS=$<TARGET_FILE:bg3mse_dylib_tests>" "-DSTAND_IN=$<TARGET_FILE:bg3mse_game_stand_in>"
            "-DCTEST=${CMAKE_CTEST_COMMAND}" "-DIDENTITY=${BG3MSE_SIGN_IDENTITY}" "-DGIT=${BG3MSE_GIT}"
            "-DNOTARY_PROFILE=${BG3MSE_NOTARY_PROFILE}" "-DINSTALLER_APP=$<TARGET_FILE:bg3mse_installer_app>"
            -P "${CMAKE_SOURCE_DIR}/Mac/Release/Assemble.cmake"
        USES_TERMINAL VERBATIM)
    add_dependencies(release ${built})
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL bg3mse_define_release)
