# The release holds what it ships and nothing else (Mac/Release/Contents.cmake):
# a folder of exactly that passes, and one with anything more is refused,
# the installer's test fixtures among them.
#
#   cmake -DCONTENTS_SCRIPT=<Mac/Release/Contents.cmake> -DSCRATCH=<directory> -P ReleaseContents.cmake

foreach(name CONTENTS_SCRIPT SCRATCH)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "${name} is not set")
    endif()
endforeach()
file(REMOVE_RECURSE "${SCRATCH}")
set(dir "${SCRATCH}/bg3mse-test")
set(shipped
    bg3mse-install bg3mse-launch libbg3mse.dylib libbg3mse-loader.dylib
    camera/libbg3mse-camera.dylib camera/LICENSE camera/COPYING camera/EXCEPTIONS camera/NativeCameraTweaks.default.toml
    licenses/bg3se.txt licenses/bg3mse.txt licenses/third-party/lua.txt licenses/third-party/abseil.txt
    lua-debugger/LuaDebugger lua-debugger/LuaDebugger.dll)
foreach(file IN LISTS shipped)
    file(WRITE "${dir}/${file}" "${file}\n")
endforeach()

function(check expect_pass what)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DDIR=${dir}" "-DTHIRD_PARTY=lua|abseil"
        "-DADAPTER_FILES=LuaDebugger|LuaDebugger.dll" -P "${CONTENTS_SCRIPT}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expect_pass AND NOT rc EQUAL 0)
        message(FATAL_ERROR "${what}: refused:\n${out}${err}")
    endif()
    if(NOT expect_pass AND rc EQUAL 0)
        message(FATAL_ERROR "${what}: not refused")
    endif()
    set(check_output "${out}${err}" PARENT_SCOPE)
endfunction()

check(ON "what a release ships")
# Each of these, added alone, is refused, and named.
foreach(extra game game-updated "Install bg3mse.command" camera/game libbg3mse.dylib.dSYM/x licenses/third-party/other.txt lua-debugger/extra.dll notes.md)
    file(WRITE "${dir}/${extra}" "x\n")
    check(OFF "${extra}")
    string(FIND "${check_output}" "contents: not shipped: ${extra}\n" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${extra}: refused without being named:\n${check_output}")
    endif()
    file(REMOVE_RECURSE "${dir}/${extra}")
    if(extra MATCHES "^(libbg3mse.dylib.dSYM)/")
        file(REMOVE_RECURSE "${dir}/${CMAKE_MATCH_1}")
    endif()
endforeach()
check(ON "what a release ships, again")
