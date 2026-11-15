# Run by ctest, and for any image that links bg3mse's Lua
# (cmake -DIMAGE=<linked image> -DLUA_ARCHIVE=<libbg3se_lua.a> -P CheckLuaIsolation.cmake).
#
# The game exports its own Lua 5.3 C API (_lua_gettop and about 300 more). The
# extender brings Norbyte's Lua, a different one. The two must never meet:
# bg3mse's code must call bg3mse's Lua only, and the game's code the game's.
#
# bg3mse's Lua is compiled as C++, so its functions have mangled names the
# game does not have. Its global variables are not mangled (_luai_ctype_,
# _luaO_nilobject_, _luaT_typenames_, _luaP_opmodes are names the game exports
# too), so the names alone do not keep the two apart. These do, and are
# checked here:
# - every symbol the Lua archive defines is hidden, so the static linker binds
#   each use inside the image and exports none;
# - the image exports no symbol the archive defines, and nothing named like
#   the Lua C API: no other image can be bound to bg3mse's Lua;
# - the image imports no such symbol either, bound or left to be looked up at
#   run time: nothing of bg3mse's can be bound to the game's Lua. A Lua header
#   included inside extern "C" would produce exactly such an import.
# - the image does contain the archive's Lua (EXPECT_LUA), so the checks above
#   were made on something.
#
# And one thing of the archive alone: nothing in it calls the dynamic loader.
# Lua's package library can load a C module from any file a script names; the
# archive is compiled without that (no LUA_USE_DLOPEN), so that no way of
# opening the library brings it into the image.

find_program(nm_PATH nm REQUIRED)
find_program(grep_PATH grep REQUIRED)

function(run_nm out)
    execute_process(COMMAND ${nm_PATH} ${ARGN} OUTPUT_VARIABLE text RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "nm ${ARGN} failed")
    endif()
    string(REPLACE "\n" ";" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# What the archive defines, and that all of it is hidden.
run_nm(lines -m "${LUA_ARCHIVE}")
set(lua_names "")
set(visible "")
set(loader "")
foreach(line IN LISTS lines)
    if(line MATCHES "\\(undefined\\).* _(dlopen|dlsym|dlclose|dlerror)$")
        string(APPEND loader "  ${line}\n")
    endif()
    if(line STREQUAL "" OR line MATCHES ":$" OR line MATCHES "\\(undefined\\)")
        continue()
    endif()
    if(line MATCHES "non-external ")
        continue()          # local to its object
    endif()
    if(NOT line MATCHES "external ([^ ]+)$")
        message(FATAL_ERROR "unexpected nm output: ${line}")
    endif()
    list(APPEND lua_names "${CMAKE_MATCH_1}")
    if(NOT line MATCHES "private external ")
        string(APPEND visible "  ${line}\n")
    endif()
endforeach()
list(REMOVE_DUPLICATES lua_names)
list(LENGTH lua_names count)
if(count LESS 100)
    message(FATAL_ERROR "${LUA_ARCHIVE} defines ${count} symbols: not a Lua library?")
endif()
if(NOT visible STREQUAL "")
    message(FATAL_ERROR "${LUA_ARCHIVE} defines symbols that are not hidden:\n${visible}")
endif()
if(NOT loader STREQUAL "")
    message(FATAL_ERROR "${LUA_ARCHIVE} calls the dynamic loader: it can load C modules:\n${loader}")
endif()

# A name the game's Lua could have: the C API and Lua's C globals.
function(check_names what)
    set(bad "")
    foreach(line IN LISTS ARGN)
        if(line STREQUAL "")
            continue()
        endif()
        if(NOT line MATCHES "(_[^ ]+)( \\(.*\\))?$")
            message(FATAL_ERROR "unexpected nm output: ${line}")
        endif()
        set(name "${CMAKE_MATCH_1}")
        if(name IN_LIST lua_names OR name MATCHES "^_lua")
            string(APPEND bad "  ${line}\n")
        endif()
    endforeach()
    if(NOT bad STREQUAL "")
        message(FATAL_ERROR "${IMAGE} ${what} Lua symbols:\n${bad}")
    endif()
endfunction()

run_nm(exported -gU "${IMAGE}")
check_names("exports" ${exported})
run_nm(imported -m -u "${IMAGE}")
check_names("imports" ${imported})

if(EXPECT_LUA)
    # lua_newstate, as the archive defines it, is in the image as a local symbol.
    set(probe "")
    foreach(name IN LISTS lua_names)
        if(name MATCHES "lua_newstate")
            set(probe "${name}")
        endif()
    endforeach()
    if(probe STREQUAL "")
        message(FATAL_ERROR "${LUA_ARCHIVE} does not define lua_newstate")
    endif()
    execute_process(COMMAND ${nm_PATH} -m "${IMAGE}" COMMAND ${grep_PATH} -F " ${probe}"
        OUTPUT_VARIABLE found ERROR_QUIET)
    if(NOT found MATCHES "non-external")
        message(FATAL_ERROR "${IMAGE} does not contain ${probe} as a local symbol:\n${found}")
    endif()
endif()

message(STATUS "${count} Lua symbols, all hidden, and no use of the dynamic loader; ${IMAGE} exports and imports none")
