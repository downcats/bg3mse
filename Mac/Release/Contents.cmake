# What a release folder may hold, and nothing else: Mac/Release/Assemble.cmake
# checks the folder with it before the manifest is written, so that nothing
# the build makes for its tests (the installer's fixtures, say) is shipped.
#
#   cmake -DDIR=<release folder> "-DTHIRD_PARTY=<name>|..." "-DADAPTER_FILES=<file>|..." -P Contents.cmake
#
# THIRD_PARTY names the licences in licenses/third-party (<name>.txt), and
# ADAPTER_FILES the Lua debug adapter's files in lua-debugger/, as the
# release ships them. Any other file is refused, each named on a line of its
# own: "contents: not shipped: <path>".

if(NOT DEFINED DIR OR NOT IS_DIRECTORY "${DIR}")
    message(FATAL_ERROR "contents: DIR is not a folder")
endif()

set(expected
    bg3mse-install
    bg3mse-launch
    libbg3mse.dylib
    libbg3mse-loader.dylib
    camera/libbg3mse-camera.dylib
    camera/LICENSE
    camera/COPYING
    camera/EXCEPTIONS
    camera/NativeCameraTweaks.default.toml
    licenses/bg3se.txt
    licenses/bg3mse.txt)
string(REPLACE "|" ";" third_party "${THIRD_PARTY}")
foreach(name IN LISTS third_party)
    list(APPEND expected "licenses/third-party/${name}.txt")
endforeach()
string(REPLACE "|" ";" adapter "${ADAPTER_FILES}")
foreach(file IN LISTS adapter)
    list(APPEND expected "lua-debugger/${file}")
endforeach()

file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${DIR}" "${DIR}/*")
list(SORT files COMPARE STRING)
set(unexpected 0)
foreach(file IN LISTS files)
    list(FIND expected "${file}" at)
    if(at EQUAL -1)
        message("contents: not shipped: ${file}")
        math(EXPR unexpected "${unexpected} + 1")
    endif()
endforeach()
if(unexpected GREATER 0)
    message(FATAL_ERROR "contents: ${DIR} holds ${unexpected} file(s) a release does not ship, named above")
endif()
