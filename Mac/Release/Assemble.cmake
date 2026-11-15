# The release target's work (Mac/Release/Release.cmake passes the build's
# values): checks that the build may be released, runs its tests, and puts
# the release together in <build>/release:
#
#   bg3mse-<version>/               what a player gets
#     libbg3mse.dylib               stripped and signed (Mac/Release/Symbols.cmake)
#     libbg3mse-loader.dylib        the updater's loader, what the game loads (Mac/Updater)
#     <installer>                   the installer tools, when the build has them (Mac/Installer):
#                                   bg3mse-install, bg3mse-launch
#     camera/libbg3mse-camera.dylib the camera module, with its licence (GPL-3.0-or-later) beside it
#     camera/LICENSE, COPYING, EXCEPTIONS
#     camera/NativeCameraTweaks.default.toml   the settings file the camera module writes when there
#                                   is none; bg3mse-install makes the player's from it
#     lua-debugger/                 Norbyte's Lua debug adapter, when the build has it (Mac/LuaDebugger):
#                                   LuaDebugger, the program an editor is given, and the assemblies it runs
#     licenses/                     the licences of everything linked into the images
#     manifest.json                 versions, the game build checked, every other file's size and SHA-256
#   bg3mse-<version>-dSYM/          the debug symbols of each image, kept for crash reports
#   bg3mse-<version>-app/bg3mse.app what a player double-clicks (Mac/Installer/App): the
#                                   installer, with the release folder in its bundle
#   bg3mse-<version>.zip            the app: what a player downloads
#   bg3mse-<version>-updater-payload.zip
#                                   the release folder: what the updater fetches, when it updates itself
#   bg3mse-<version>-dSYM.zip       the symbols
#                                   (all three by Mac/Release/Zip.cmake)
#   SHA256SUMS                      the zips, for shasum -a 256 -c
#
# Signing: every image with IDENTITY ("-": ad hoc), the executables and the
# app with the hardened runtime when it is a Developer ID. With a Developer ID
# and NOTARY_PROFILE (a notarytool keychain profile), the images and the app
# are sent to Apple's notary service, the target waits for the answer, and
# the ticket is stapled to bg3mse.app before anything is zipped.
#
# A release is made from a clean checkout's commit, the Release flavour, the
# build type Release, line tables on, and a game binary the game checks ran
# against; anything else is refused before
# anything is written.

foreach(name SOURCE_DIR BINARY_DIR SCRIPTS VERSION COMMIT EXTENDER CHANNEL BUILD_TYPE DYLIB DYLIB_TESTS STAND_IN CTEST)
    if(NOT DEFINED ${name} OR "${${name}}" STREQUAL "")
        message(FATAL_ERROR "release: ${name} is not set")
    endif()
endforeach()
include("${SCRIPTS}/Stamp.cmake")

function(refuse why)
    message(FATAL_ERROR "release refused: ${why}")
endfunction()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        list(JOIN ARGN " " command)
        message(FATAL_ERROR "release: ${command} failed (${rc}):\n${out}${err}")
    endif()
    set(run_output "${out}" PARENT_SCOPE)
endfunction()

# --- What may be released -------------------------------------------------------

if(NOT CHANNEL STREQUAL "Release")
    refuse("this build is the ${CHANNEL} flavour, a developer build; configure with -DBG3MSE_CHANNEL=Release")
endif()
if(NOT BUILD_TYPE STREQUAL "Release")
    refuse("the build type is '${BUILD_TYPE}', not Release (CMAKE_BUILD_TYPE)")
endif()
if(NOT DEBUG_INFO)
    refuse("the build has no line tables, and its dSYM files would name no file or line; configure with -DBG3MSE_DEBUG_INFO=ON")
endif()
if(NOT GAME_APP OR NOT EXISTS "${GAME_APP}/Contents/Info.plist")
    refuse("no game to check against; configure with -DBG3MSE_GAME_APP=<Baldur's Gate 3.app>, so that ctest runs the game checks")
endif()
if(LOADER AND SELF_UPDATE AND NOT UPDATER_KEY)
    refuse("the loader has no update key and would never update; configure with -DBG3MSE_UPDATER_PUBLIC_KEY=<bg3mse-sign keygen's output>, "
        "or with -DBG3MSE_SELF_UPDATE=OFF for a release that does not update itself")
endif()
if(NOTARY_PROFILE AND IDENTITY STREQUAL "-")
    refuse("an ad hoc signature cannot be notarised; give the Developer ID with -DBG3MSE_SIGN_IDENTITY=\"Developer ID Application: ...\"")
endif()
if(NOT IDENTITY STREQUAL "-")
    execute_process(COMMAND security find-identity -v -p codesigning OUTPUT_VARIABLE identities ERROR_QUIET)
    string(FIND "${identities}" "\"${IDENTITY}\"" at)
    if(at EQUAL -1)
        refuse("the keychain has no valid code signing identity named \"${IDENTITY}\" (security find-identity -v -p codesigning lists them)")
    endif()
endif()
if(COMMIT STREQUAL "unknown" OR NOT GIT)
    refuse("the source tree is not a git checkout: a release names the commit it was built from")
endif()
run("${GIT}" -C "${SOURCE_DIR}" rev-parse --verify HEAD)
string(STRIP "${run_output}" head)
if(NOT head STREQUAL COMMIT)
    refuse("the build was configured at ${COMMIT} and HEAD is ${head}; configure and build again")
endif()
run("${GIT}" -C "${SOURCE_DIR}" status --porcelain)
if(NOT run_output STREQUAL "")
    refuse("the tree has changes that are not committed:\n${run_output}")
endif()

set(images "${DYLIB}")
if(CAMERA)
    list(APPEND images "${CAMERA}")
endif()
if(LOADER)
    list(APPEND images "${LOADER}")
endif()
foreach(image IN LISTS images)
    bg3mse_read_stamp("${image}" stamp)
    foreach(field VERSION COMMIT EXTENDER CHANNEL)
        if(NOT stamp_${field} STREQUAL "${${field}}")
            refuse("${image} carries ${field} '${stamp_${field}}', the build '${${field}}': build again")
        endif()
    endforeach()
    if(NOT stamp_CURRENT_VERSION STREQUAL VERSION)
        refuse("${image}'s current version is '${stamp_CURRENT_VERSION}', not ${VERSION}")
    endif()
endforeach()

# --- The build's tests ----------------------------------------------------------------

message(STATUS "release: running the tests")
execute_process(COMMAND "${CTEST}" --test-dir "${BINARY_DIR}" --output-on-failure RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    refuse("ctest failed")
endif()

# --- The images -------------------------------------------------------------------

set(name "bg3mse-${VERSION}")
set(out "${BINARY_DIR}/release")
set(dir "${out}/${name}")
set(symbols "${out}/${name}-dSYM")
# The folder holds this release only: what an earlier version made goes (publish.sh takes the one release there).
file(GLOB earlier LIST_DIRECTORIES true "${out}/bg3mse-*")
file(REMOVE_RECURSE ${earlier} "${out}/SHA256SUMS")
file(MAKE_DIRECTORY "${dir}" "${symbols}")

# bg3mse-launch, under the hardened runtime, keeps the DYLD_ variables it is
# started with (Steam's own, when it sets them) for the game, and may load
# what they name into itself, as it does without the hardened runtime
# (Mac/Installer/Launch.entitlements).
function(ship image subdir executable)
    get_filename_component(file "${image}" NAME)
    set(entitlements "")
    if(file STREQUAL "bg3mse-launch")
        set(entitlements "${SOURCE_DIR}/Mac/Installer/Launch.entitlements")
    endif()
    if(subdir)
        set(target "${dir}/${subdir}/${file}")
    else()
        set(target "${dir}/${file}")
    endif()
    run("${CMAKE_COMMAND}" "-DIMAGE=${image}" "-DOUTPUT=${target}" "-DDSYM=${symbols}/${file}.dSYM"
        "-DIDENTITY=${IDENTITY}" "-DEXECUTABLE=${executable}" "-DENTITLEMENTS=${entitlements}" -P "${SCRIPTS}/Symbols.cmake")
endfunction()

ship("${DYLIB}" "" OFF)
if(LOADER)
    ship("${LOADER}" "" OFF)
endif()
if(CAMERA)
    ship("${CAMERA}" camera OFF)
endif()
string(REPLACE "|" ";" installers "${INSTALLERS}")
foreach(installer IN LISTS installers)
    if(installer)
        ship("${installer}" "" ON)
    endif()
endforeach()

# The Lua debug adapter, for mod authors: what an editor runs to debug the
# Lua of a game started with EnableLuaDebugger. Assemblies of .NET and a
# shell script, nothing to strip or sign. The session it is taken through
# with the Lua debugger is one of the tests above.
if(DEBUG_ADAPTER)
    string(REPLACE "|" ";" adapter_files "${DEBUG_ADAPTER_FILES}")
    foreach(file IN LISTS adapter_files)
        if(NOT EXISTS "${DEBUG_ADAPTER}/${file}")
            refuse("${DEBUG_ADAPTER}/${file}, a part of the Lua debug adapter, is missing")
        endif()
        if(file STREQUAL "LuaDebugger")
            set(permissions OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        else()
            set(permissions OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
        endif()
        file(COPY "${DEBUG_ADAPTER}/${file}" DESTINATION "${dir}/lua-debugger" FILE_PERMISSIONS ${permissions})
    endforeach()
endif()

# The shipped dylib, stripped and signed again, loads and starts as the
# build's does (Mac/Tests/DylibLoadTests.cpp).
message(STATUS "release: the dylib tests on the shipped libbg3mse.dylib")
execute_process(COMMAND "${DYLIB_TESTS}" "${dir}/libbg3mse.dylib" "${STAND_IN}" 1 RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    refuse("the dylib tests fail on the shipped libbg3mse.dylib")
endif()

# --- Licences -----------------------------------------------------------------------
#
# Norbyte's licence for his code, bg3mse's for its own (the platform layer is
# in both images), and the licence of each library linked into
# libbg3mse.dylib, from its copy in External. Lua keeps its licence at the
# end of lua.h only. Optick is on the link line of every flavour, and only a
# developer flavour uses it: its licence is shipped when the dylib has its code.
# SPIRV-Cross is compiled in (Apache-2.0 OR MIT, shipped as Apache-2.0), with
# Khronos's SPIR-V headers, which are under SPIRV-Headers' licence as is the
# grammar a table of the shader translation is made from (Mac/Shaders).

file(MAKE_DIRECTORY "${dir}/licenses/third-party")
file(COPY_FILE "${SOURCE_DIR}/LICENSE" "${dir}/licenses/bg3se.txt")
file(COPY_FILE "${SOURCE_DIR}/Mac/LICENSE" "${dir}/licenses/bg3mse.txt")
if(CAMERA)
    foreach(file LICENSE COPYING EXCEPTIONS)
        file(COPY_FILE "${SOURCE_DIR}/Mac/Camera/${file}" "${dir}/camera/${file}")
    endforeach()
    if(NOT CAMERA_DEFAULTS OR NOT EXISTS "${CAMERA_DEFAULTS}")
        refuse("the camera module's default settings file (${CAMERA_DEFAULTS}) was not made")
    endif()
    file(COPY_FILE "${CAMERA_DEFAULTS}" "${dir}/camera/NativeCameraTweaks.default.toml")
endif()

set(third_party
    "abseil|abseil-cpp/LICENSE"
    "bcdec|bcdec/LICENSE"
    "FastLZ|FastLZ/LICENSE.MIT"
    "glm|glm/copying.txt"
    "imgui|imgui/LICENSE.txt"
    "lz4|lz4/lib/LICENSE"
    "protobuf|protobuf/LICENSE"
    "rapidjson|rapidjson/license.txt"
    "SPIRV-Cross|SPIRV-Cross/LICENSE"
    "SPIRV-Headers|SPIRV-Headers/LICENSE"
    "stb|stb/LICENSE"
    "tinycrypt|tinycrypt/LICENSE"
    "utf8_range|protobuf/third_party/utf8_range/LICENSE")
find_program(nm_PATH nm REQUIRED)
run("${nm_PATH}" "${DYLIB}")
if(run_output MATCHES "[Oo]ptick")
    list(APPEND third_party "optick|optick/LICENSE")
endif()
foreach(entry IN LISTS third_party)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 component)
    list(GET entry 1 file)
    if(NOT EXISTS "${SOURCE_DIR}/External/${file}")
        refuse("External/${file}, the licence of ${component}, is missing")
    endif()
    file(COPY_FILE "${SOURCE_DIR}/External/${file}" "${dir}/licenses/third-party/${component}.txt")
endforeach()
# The debug adapter's two packages: Google.Protobuf is protobuf's, whose
# licence is above; Newtonsoft.Json's is in its package.
if(DEBUG_ADAPTER)
    file(GLOB newtonsoft "${SOURCE_DIR}/External/nuget/newtonsoft.json.*.nupkg")
    list(LENGTH newtonsoft count)
    if(NOT count EQUAL 1)
        refuse("External/nuget does not hold the one Newtonsoft.Json package the debug adapter was built with")
    endif()
    set(unpacked "${out}/newtonsoft")
    file(REMOVE_RECURSE "${unpacked}")
    file(ARCHIVE_EXTRACT INPUT "${newtonsoft}" DESTINATION "${unpacked}" PATTERNS LICENSE.md)
    if(NOT EXISTS "${unpacked}/LICENSE.md")
        refuse("${newtonsoft} has no LICENSE.md")
    endif()
    file(COPY_FILE "${unpacked}/LICENSE.md" "${dir}/licenses/third-party/Newtonsoft.Json.txt")
    file(REMOVE_RECURSE "${unpacked}")
endif()
file(READ "${SOURCE_DIR}/External/lua/src/lua.h" lua)
if(NOT lua MATCHES "\n/\\*+\n(\\* Copyright \\(C\\) [^\n]*Lua\\.org, PUC-Rio\\.\n(\\*[^\n]*\n)+)\\*+/\n")
    refuse("External/lua/src/lua.h has no copyright notice at its end where Lua's is")
endif()
string(REGEX REPLACE "(^|\n)\\* ?" "\\1" lua "${CMAKE_MATCH_1}")
file(WRITE "${dir}/licenses/third-party/lua.txt" "${lua}")

# --- What it holds ---------------------------------------------------------------
#
# What is shipped and nothing else (Mac/Release/Contents.cmake): a file the
# build made for its tests, put in by mistake, is refused here.

set(shipped_third_party "")
foreach(entry IN LISTS third_party)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 component)
    list(APPEND shipped_third_party "${component}")
endforeach()
if(DEBUG_ADAPTER)
    list(APPEND shipped_third_party Newtonsoft.Json)
endif()
list(APPEND shipped_third_party lua)
list(JOIN shipped_third_party "|" shipped_third_party)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DDIR=${dir}" "-DTHIRD_PARTY=${shipped_third_party}" "-DADAPTER_FILES=${DEBUG_ADAPTER_FILES}"
    -P "${SCRIPTS}/Contents.cmake" RESULT_VARIABLE contents_rc OUTPUT_VARIABLE contents_out ERROR_VARIABLE contents_err)
if(NOT contents_rc EQUAL 0)
    refuse("the release would ship what it should not (Mac/Release/Contents.cmake):\n${contents_out}${contents_err}")
endif()

# --- The manifest -------------------------------------------------------------------

run(plutil -extract CFBundleVersion raw "${GAME_APP}/Contents/Info.plist")
string(STRIP "${run_output}" game_build)

file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
list(SORT files COMPARE STRING)
set(json "{}")
string(JSON json SET "${json}" name "\"${name}\"")
string(JSON json SET "${json}" version "\"${VERSION}\"")
string(JSON json SET "${json}" commit "\"${COMMIT}\"")
string(JSON json SET "${json}" script_extender_version "\"${EXTENDER}\"")
string(JSON json SET "${json}" channel "\"${CHANNEL}\"")
string(JSON json SET "${json}" game_build_checked "\"${game_build}\"")
string(JSON json SET "${json}" macos_minimum "\"${MACOS_MINIMUM}\"")
string(JSON json SET "${json}" architecture "\"arm64\"")
# Whether the installed copy updates itself (BG3MSE_SELF_UPDATE); what to do on a newer game follows from it (Release/GameAdvice.h).
if(SELF_UPDATE)
    string(JSON json SET "${json}" self_update true)
else()
    string(JSON json SET "${json}" self_update false)
endif()
string(JSON json SET "${json}" files "[]")
set(index 0)
foreach(file IN LISTS files)
    file(SHA256 "${dir}/${file}" hash)
    file(SIZE "${dir}/${file}" size)
    set(entry "{}")
    string(JSON entry SET "${entry}" path "\"${file}\"")
    string(JSON entry SET "${entry}" size "${size}")
    string(JSON entry SET "${entry}" sha256 "\"${hash}\"")
    execute_process(COMMAND dwarfdump --uuid --arch arm64 "${dir}/${file}" OUTPUT_VARIABLE uuid ERROR_QUIET RESULT_VARIABLE rc)
    if(rc EQUAL 0 AND uuid MATCHES "UUID: ([0-9A-F-]+) \\(arm64\\)")
        string(JSON entry SET "${entry}" uuid "\"${CMAKE_MATCH_1}\"")
    endif()
    string(JSON json SET "${json}" files ${index} "${entry}")
    math(EXPR index "${index} + 1")
endforeach()
file(WRITE "${dir}/manifest.json" "${json}\n")

# --- The installer app ----------------------------------------------------------------
#
# bg3mse.app (Mac/Installer/App/InstallerApp.mm): its program, and the
# release folder inside the bundle where a signed bundle keeps each kind of
# file: the tools in Contents/Helpers, the libraries in Contents/Frameworks,
# the rest in Contents/Resources/release at its path in the folder. The Lua
# debug adapter is for mod authors and stays in the updater's payload zip.

set(installer_dir "${out}/${name}-app")
set(app "${installer_dir}/bg3mse.app")
file(REMOVE_RECURSE "${installer_dir}")
if(NOT INSTALLER_APP)
    refuse("the build has no installer app (bg3mse_installer_app)")
endif()
file(MAKE_DIRECTORY "${app}/Contents/MacOS" "${app}/Contents/Helpers" "${app}/Contents/Frameworks" "${app}/Contents/Resources/release")
configure_file("${SOURCE_DIR}/Mac/Installer/App/Info.plist.in" "${app}/Contents/Info.plist" @ONLY)
run(plutil -lint "${app}/Contents/Info.plist")
run("${CMAKE_COMMAND}" "-DIMAGE=${INSTALLER_APP}" "-DOUTPUT=${app}/Contents/MacOS/bg3mse" "-DDSYM=${symbols}/bg3mse.app.dSYM"
    "-DIDENTITY=${IDENTITY}" -DEXECUTABLE=ON -P "${SCRIPTS}/Symbols.cmake")
foreach(file IN LISTS files ITEMS manifest.json)
    if(file MATCHES "^lua-debugger/")
        continue()
    endif()
    get_filename_component(base "${file}" NAME)
    if(file STREQUAL "bg3mse-install" OR file STREQUAL "bg3mse-launch")
        set(to "${app}/Contents/Helpers")
    elseif(base MATCHES "\\.dylib$")
        set(to "${app}/Contents/Frameworks")
    else()
        get_filename_component(sub "${file}" DIRECTORY)
        set(to "${app}/Contents/Resources/release/${sub}")
    endif()
    file(COPY "${dir}/${file}" DESTINATION "${to}")
endforeach()
set(sign codesign --force --sign "${IDENTITY}")
if(NOT IDENTITY STREQUAL "-")
    list(APPEND sign --timestamp --options runtime)
endif()
run(${sign} "${app}")
run(codesign --verify --deep --strict "${app}")
message(STATUS "release: ${app}, signed (${IDENTITY})")

# --- Notarisation ---------------------------------------------------------------------
#
# One submission holds the release folder and the app; Apple's ticket names
# every image in it. The ticket is stapled to the app, so that it opens with
# no network; the images of the zip, which the updater fetches without the
# quarantine mark, are known to Gatekeeper online.

if(NOTARY_PROFILE)
    set(submission "${out}/notarisation")
    file(REMOVE_RECURSE "${submission}")
    file(MAKE_DIRECTORY "${submission}/${name}-notarisation")
    run(ditto "${dir}" "${submission}/${name}-notarisation/${name}")
    run(ditto "${app}" "${submission}/${name}-notarisation/bg3mse.app")
    run(ditto -c -k --keepParent "${submission}/${name}-notarisation" "${submission}/${name}-notarisation.zip")
    message(STATUS "release: sending ${name} to Apple's notary service (profile ${NOTARY_PROFILE}); this waits for the answer")
    execute_process(COMMAND xcrun notarytool submit "${submission}/${name}-notarisation.zip" --keychain-profile "${NOTARY_PROFILE}"
        --wait --output-format json RESULT_VARIABLE rc OUTPUT_VARIABLE answer ERROR_VARIABLE err)
    string(JSON status ERROR_VARIABLE no_status GET "${answer}" status)
    string(JSON id ERROR_VARIABLE no_id GET "${answer}" id)
    if(NOT rc EQUAL 0 OR NOT status STREQUAL "Accepted")
        if(id)
            execute_process(COMMAND xcrun notarytool log "${id}" --keychain-profile "${NOTARY_PROFILE}" OUTPUT_VARIABLE log ERROR_QUIET)
        endif()
        refuse("Apple's notary service did not accept ${name} (${status}):\n${answer}${err}\n${log}")
    endif()
    run(xcrun stapler staple "${app}")
    run(xcrun stapler validate "${app}")
    run(spctl --assess --type execute -vv "${app}")
    file(REMOVE_RECURSE "${submission}")
    set(notarised "notarised (submission ${id}), ticket stapled to bg3mse.app")
elseif(IDENTITY STREQUAL "-")
    set(notarised "signed ad hoc, not notarised")
else()
    set(notarised "signed with ${IDENTITY}, not notarised (no BG3MSE_NOTARY_PROFILE)")
endif()

# --- The zips -----------------------------------------------------------------------

run("${CMAKE_COMMAND}" "-DROOT=${installer_dir}" "-DENTRY=bg3mse.app" "-DOUTPUT=${out}/${name}.zip"
    -P "${SCRIPTS}/Zip.cmake")
run("${CMAKE_COMMAND}" "-DROOT=${out}" "-DENTRY=${name}" "-DOUTPUT=${out}/${name}-updater-payload.zip"
    -P "${SCRIPTS}/Zip.cmake")
run("${CMAKE_COMMAND}" "-DROOT=${out}" "-DENTRY=${name}-dSYM" "-DOUTPUT=${out}/${name}-dSYM.zip"
    -P "${SCRIPTS}/Zip.cmake")
# What a player gets from the zip is the app as signed.
file(REMOVE_RECURSE "${out}/unzipped")
run(ditto -x -k "${out}/${name}.zip" "${out}/unzipped")
run(codesign --verify --deep --strict "${out}/unzipped/bg3mse.app")
file(REMOVE_RECURSE "${out}/unzipped")
set(sums "")
foreach(zip "${name}.zip" "${name}-updater-payload.zip" "${name}-dSYM.zip")
    file(SHA256 "${out}/${zip}" digest)
    string(APPEND sums "${digest}  ${zip}\n")
endforeach()
file(WRITE "${out}/SHA256SUMS" "${sums}")
message(STATUS "release: ${out}/${name}.zip, the app, ${notarised}")
message(STATUS "release: ${out}/${name}-updater-payload.zip, the release folder, checked against game ${game_build}")
if(SELF_UPDATE)
    string(REGEX REPLACE "^https://([^/]+)/.*" "\\1" host "${UPDATER_URL}")
    message(STATUS "release: it updates itself from ${host} (${UPDATER_URL}${CHANNEL}/Manifest.json)")
else()
    message(STATUS "release: it does not update itself: its loader cannot reach the network")
endif()
if(DEBUG_ADAPTER)
    message(STATUS "release: the Lua debug adapter is in ${name}-updater-payload.zip, in ${name}/lua-debugger")
else()
    message(STATUS "release: no Lua debug adapter; configure with -DBG3MSE_DOTNET=<the dotnet of a .NET 10 SDK> for one")
endif()
