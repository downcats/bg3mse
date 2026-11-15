# The updater (Mac/Updater/Updater.h): libbg3mse-loader.dylib, which the game
# loads and which loads the newest verified build; bg3mse-sign, which makes
# the update key and signs a channel's manifest; and their tests. Included
# from the root CMakeLists.txt.

# Whether a release updates itself. ON: at every start the loader fetches
# the channel's manifest from BG3MSE_UPDATER_URL, and a newer build it
# lists. OFF: the loader is built with no way to the network (FetchNone.cpp
# in place of the system's URL loading and libcurl, and no update key), so
# it loads the build beside it and nothing else; the release's
# manifest.json says so (self_update false).
option(BG3MSE_SELF_UPDATE "A release updates itself from its channel; OFF: its loader cannot reach the network" ON)

# The public half of the update key, 128 hexadecimal digits, as
# `bg3mse-sign keygen` prints it. Without it the loader is built, loads
# the build beside it, and fetches nothing.
set(BG3MSE_UPDATER_PUBLIC_KEY "" CACHE STRING "The update key's public half (bg3mse-sign keygen); empty: no updates")
# The channel manifest is <BG3MSE_UPDATER_URL><channel>/Manifest.json, the
# asset Manifest.json of the release tagged channel-<channel>.
set(BG3MSE_UPDATER_URL "https://github.com/downcats/bg3mse/releases/download/channel-" CACHE STRING
    "Where channel manifests are fetched from (ManifestURL in ScriptExtenderUpdaterConfig.json overrides it)")
string(LENGTH "${BG3MSE_UPDATER_PUBLIC_KEY}" key_length)
if(NOT BG3MSE_UPDATER_PUBLIC_KEY MATCHES "^[0-9a-fA-F]*$" OR NOT (key_length EQUAL 0 OR key_length EQUAL 128))
    message(FATAL_ERROR "BG3MSE_UPDATER_PUBLIC_KEY is 128 hexadecimal digits (bg3mse-sign keygen prints it), or empty")
endif()
if(NOT BG3MSE_UPDATER_URL MATCHES "^https://[^\" \\\\]+$")
    message(FATAL_ERROR "BG3MSE_UPDATER_URL '${BG3MSE_UPDATER_URL}' is not an https URL")
endif()
if(NOT BG3MSE_SELF_UPDATE)
    message(STATUS "Updater: BG3MSE_SELF_UPDATE is OFF, the loader cannot fetch anything (an update key is not used)")
elseif(NOT BG3MSE_UPDATER_PUBLIC_KEY)
    message(STATUS "Updater: no BG3MSE_UPDATER_PUBLIC_KEY, the loader fetches no updates")
endif()
# Written in lower case, as bg3mse-sign prints it (publish.sh looks for it in the loader).
string(TOLOWER "${BG3MSE_UPDATER_PUBLIC_KEY}" updater_key)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/updater/Key.h" @ONLY CONTENT [=[
// Written by Mac/Updater/Updater.cmake.
#define BG3MSE_UPDATER_KEY "@updater_key@"
#define BG3MSE_UPDATER_URL "@BG3MSE_UPDATER_URL@"
]=])

# The version stamps (Stamp.h), which the installer reads too, and nothing else of the updater.
add_library(bg3mse_updater_stamp STATIC Mac/Updater/Stamp.cpp)
target_include_directories(bg3mse_updater_stamp PUBLIC Mac)
target_compile_options(bg3mse_updater_stamp PRIVATE ${BG3MSE_WARNINGS})

set(updater_sources
    Mac/Updater/Config.cpp
    Mac/Updater/Files.cpp
    Mac/Updater/Manifest.cpp
    Mac/Updater/Notice.cpp
    Mac/Updater/Signature.cpp
    Mac/Updater/Updater.cpp
    Mac/Updater/Zip.cpp)
add_library(bg3mse_updater STATIC ${updater_sources} Mac/Updater/Fetch.mm Mac/Updater/FetchCurl.cpp)
set_source_files_properties(Mac/Updater/Fetch.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
# The system's libcurl carries IPv4Only (Fetch.h); its log lines go to the platform's log.
target_link_libraries(bg3mse_updater PUBLIC bg3mse_updater_stamp bg3se_tinycrypt z curl bg3mse_platform "-framework Foundation" "-framework CoreFoundation")
# The same without a fetch: the loader of a release that does not update itself.
add_library(bg3mse_updater_offline STATIC ${updater_sources} Mac/Updater/FetchNone.cpp)
target_link_libraries(bg3mse_updater_offline PUBLIC bg3mse_updater_stamp bg3se_tinycrypt z bg3mse_platform "-framework CoreFoundation")
foreach(library bg3mse_updater bg3mse_updater_offline)
    target_include_directories(${library} PUBLIC Mac)
    target_include_directories(${library} PRIVATE "${EXT}/rapidjson/include" "${EXT}/tinycrypt/lib/include")
    target_compile_options(${library} PRIVATE ${BG3MSE_WARNINGS})
endforeach()

# The loader. It exports Norbyte's updater API and nothing else: the
# SEUpdater* functions, SESetGameVersion and SEUpdate (BG3Updater/Exports.def). Its
# initializer (LoaderEntry.cpp) is the image's last, as libbg3mse.dylib's is:
# a library of its own, loaded whole as the last thing linked.
add_library(bg3mse_loader_entry STATIC Mac/Updater/LoaderEntry.cpp)
target_compile_options(bg3mse_loader_entry PRIVATE ${BG3MSE_WARNINGS})
# SELF_UPDATE OFF: built on bg3mse_updater_offline, with no key.
function(bg3mse_add_loader target self_update)
    add_library(${target} SHARED Mac/Updater/Loader.cpp)
    if(self_update)
        set(updater bg3mse_updater)
        target_compile_definitions(${target} PRIVATE BG3MSE_SELF_UPDATE=1)
    else()
        set(updater bg3mse_updater_offline)
        target_compile_definitions(${target} PRIVATE BG3MSE_SELF_UPDATE=0)
    endif()
    target_link_libraries(${target} PRIVATE ${updater} bg3mse_platform bg3mse_version "-framework CoreFoundation"
        "-framework CoreGraphics" "-Wl,-force_load,$<TARGET_FILE:bg3mse_loader_entry>")
    add_dependencies(${target} bg3mse_loader_entry)
    target_compile_options(${target} PRIVATE ${BG3MSE_WARNINGS})
    target_link_options(${target} PRIVATE "LINKER:-exported_symbol,_SEUpdater*" "LINKER:-exported_symbol,_SESetGameVersion"
        "LINKER:-exported_symbol,_SEUpdate")
    set_target_properties(${target} PROPERTIES MACHO_CURRENT_VERSION "${BG3MSE_VERSION}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DDYLIB=$<TARGET_FILE:${target}> -DLAST_INITIALIZER=InitializeLoader
            "-DEXPORTS=_SE(Updater[A-Za-z]+|SetGameVersion|Update)" -DMACOS_MINIMUM=${BG3MSE_MACOS_MINIMUM}
            -P ${CMAKE_SOURCE_DIR}/Mac/Tools/CheckDylib.cmake
        VERBATIM)
endfunction()
bg3mse_add_loader(bg3mse_loader ${BG3MSE_SELF_UPDATE})
set_target_properties(bg3mse_loader PROPERTIES OUTPUT_NAME bg3mse-loader)
target_compile_definitions(bg3mse_loader PRIVATE "BG3MSE_UPDATER_KEY_HEADER=\"${CMAKE_BINARY_DIR}/updater/Key.h\"")

add_executable(bg3mse-sign Mac/Updater/SignTool.cpp)
target_link_libraries(bg3mse-sign PRIVATE bg3mse_updater)
target_compile_options(bg3mse-sign PRIVATE ${BG3MSE_WARNINGS})

# --- Tests ---------------------------------------------------------------------
#
# All offline: a server on 127.0.0.1 in the test serves the manifests and
# zips, signed with a key the test makes and throws away. The loader under
# test is built apart, and takes that key from the environment; the shipped
# loader has no such way in (bg3mse_loader_has_no_test_key).

bg3mse_add_loader(bg3mse_loader_test ON)
set_target_properties(bg3mse_loader_test PROPERTIES OUTPUT_NAME bg3mse-loader-test)
target_compile_definitions(bg3mse_loader_test PRIVATE BG3MSE_UPDATER_TEST_KEY=1)
# And the loader of a release that does not update itself, whichever kind
# this build ships: as such a release ships it, and as the tests run it.
bg3mse_add_loader(bg3mse_loader_offline OFF)
set_target_properties(bg3mse_loader_offline PROPERTIES OUTPUT_NAME bg3mse-loader-offline)
bg3mse_add_loader(bg3mse_loader_offline_test OFF)
set_target_properties(bg3mse_loader_offline_test PROPERTIES OUTPUT_NAME bg3mse-loader-offline-test)
target_compile_definitions(bg3mse_loader_offline_test PRIVATE BG3MSE_UPDATER_TEST_KEY=1)

# Stand-ins for libbg3mse.dylib of three versions, and for the game.
foreach(version 0.1.0 0.2.0 0.3.0)
    string(REPLACE "." "_" name "${version}")
    add_library(bg3mse_updater_fake_${name} SHARED Mac/Updater/Tests/FakeExtender.c)
    target_compile_definitions(bg3mse_updater_fake_${name} PRIVATE "FAKE_VERSION=\"${version}\"")
    target_include_directories(bg3mse_updater_fake_${name} PRIVATE Mac)
    list(APPEND fakes "$<TARGET_FILE:bg3mse_updater_fake_${name}>")
endforeach()
add_executable(bg3mse_updater_stand_in Mac/Updater/Tests/StandIn.c)

add_executable(bg3mse_updater_tests Mac/Updater/Tests/UpdaterTests.cpp Mac/Updater/Tests/Server.cpp)
# The updater, and with it the installer: its launch option's install and the
# updater share the data directory.
target_link_libraries(bg3mse_updater_tests PRIVATE bg3mse_installer bg3mse_updater)
target_include_directories(bg3mse_updater_tests PRIVATE "${EXT}/tinycrypt/lib/include")
target_compile_options(bg3mse_updater_tests PRIVATE ${BG3MSE_WARNINGS})
add_dependencies(bg3mse_updater_tests bg3mse_loader_test bg3mse_loader_offline_test bg3mse_updater_stand_in bg3mse-launch bg3mse_updater_api_tests
    bg3mse_updater_fake_0_1_0 bg3mse_updater_fake_0_2_0 bg3mse_updater_fake_0_3_0)
add_test(NAME bg3mse_updater_tests COMMAND bg3mse_updater_tests
    "--scratch=${CMAKE_BINARY_DIR}/updater_tests"
    "--loader=$<TARGET_FILE:bg3mse_loader_test>"
    "--offline-loader=$<TARGET_FILE:bg3mse_loader_offline_test>"
    "--stand-in=$<TARGET_FILE:bg3mse_updater_stand_in>"
    "--launcher=$<TARGET_FILE:bg3mse-launch>"
    "--fake-0.1.0=$<TARGET_FILE:bg3mse_updater_fake_0_1_0>"
    "--fake-0.2.0=$<TARGET_FILE:bg3mse_updater_fake_0_2_0>"
    "--fake-0.3.0=$<TARGET_FILE:bg3mse_updater_fake_0_3_0>"
    "--cmake=${CMAKE_COMMAND}"
    "--zip-script=${CMAKE_SOURCE_DIR}/Mac/Release/Zip.cmake"
    "--api-tests=$<TARGET_FILE:bg3mse_updater_api_tests>")

# The key, the signed manifest and the refusals of bg3mse-sign.
add_test(NAME bg3mse_updater_sign_tool COMMAND "${CMAKE_COMMAND}"
    "-DSIGN=$<TARGET_FILE:bg3mse-sign>" "-DZIP_SCRIPT=${CMAKE_SOURCE_DIR}/Mac/Release/Zip.cmake"
    "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    -P "${CMAKE_SOURCE_DIR}/Mac/Updater/Tests/SignTool.cmake")

# The shipped loader takes no key from its environment.
add_test(NAME bg3mse_loader_has_no_test_key COMMAND "${CMAKE_COMMAND}"
    "-DLOADER=$<TARGET_FILE:bg3mse_loader>" "-DTEST_LOADER=$<TARGET_FILE:bg3mse_loader_test>"
    -P "${CMAKE_SOURCE_DIR}/Mac/Updater/Tests/NoTestKey.cmake")

# The loader of a release that does not update itself has no way to the
# network, nor have the programs installed beside a loader (Tests/NoNetwork.cmake).
set(offline_loaders "$<TARGET_FILE:bg3mse_loader_offline>|$<TARGET_FILE:bg3mse_loader_offline_test>")
if(NOT BG3MSE_SELF_UPDATE)
    string(APPEND offline_loaders "|$<TARGET_FILE:bg3mse_loader>")
endif()
add_test(NAME bg3mse_loader_offline_has_no_network COMMAND "${CMAKE_COMMAND}"
    "-DOFFLINE=${offline_loaders}" "-DONLINE=$<TARGET_FILE:bg3mse_loader_test>"
    "-DTOOLS=$<TARGET_FILE:bg3mse-install>|$<TARGET_FILE:bg3mse-launch>|$<TARGET_FILE:bg3mse_installer_app>"
    -P "${CMAKE_SOURCE_DIR}/Mac/Updater/Tests/NoNetwork.cmake")

# The loader's API found as the extender finds it, through the shim.
add_executable(bg3mse_updater_api_tests Mac/Updater/Tests/ApiTests.cpp)
target_link_libraries(bg3mse_updater_api_tests PRIVATE bg3mse_shim)
target_compile_options(bg3mse_updater_api_tests PRIVATE ${BG3MSE_WARNINGS})
add_dependencies(bg3mse_updater_api_tests bg3mse_loader_test)
add_test(NAME bg3mse_updater_api_tests COMMAND bg3mse_updater_api_tests
    "$<TARGET_FILE:bg3mse_loader_test>" "${CMAKE_BINARY_DIR}/updater_api_tests")

# The loader carries the build's version, as the other dylibs do (Mac/Release/Stamp.cmake).
add_test(NAME bg3mse_loader_dylib_version COMMAND "${CMAKE_COMMAND}"
    "-DIMAGE=$<TARGET_FILE:bg3mse_loader>" "-DVERSION=${BG3MSE_VERSION}" "-DCOMMIT=${BG3MSE_COMMIT}"
    "-DEXTENDER=${BG3MSE_EXTENDER_VERSION}" "-DCHANNEL=${BG3MSE_CHANNEL}" -DDYLIB=ON
    -P "${CMAKE_SOURCE_DIR}/Mac/Release/Stamp.cmake")
