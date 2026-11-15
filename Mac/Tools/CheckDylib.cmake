# Run after linking the dylib
# (cmake -DDYLIB=<path> [-DLAST_INITIALIZER=<function>] [-DMIN_HEADER_PADDING=<bytes>] [-DEXPORTS=<regex>] [-DEXPORTED=<names>]
#        [-DMACOS_MINIMUM=<version> [-DARCHIVE_LIST=<file>] [-DGAME_APP=<Game.app>]] -P CheckDylib.cmake).
#
# The dylib is loaded into the game for its initializer, so:
# - it exports nothing (with EXPORTS: nothing but symbols whose names match
#   it; with EXPORTED, a list of names separated by commas: each of them, and
#   nothing else);
# - every symbol it imports is bound, at link time, to a named library or
#   framework of the OS (two-level namespace). A symbol left to be looked up at
#   run time binds to whatever image defines that name first, which can be
#   nothing (a call through it jumps to address zero) or the game's own symbol
#   of the same name. C headers included from C++ or Objective-C++ without
#   extern "C" produce such names: mangled ones that nothing defines;
# - every library it loads is the OS's;
# - with LAST_INITIALIZER: the function of that name is the last of the
#   image's initializers. They run in the order the linker laid them out,
#   file by file; the one that starts everything uses objects the others
#   construct, and must not run before them;
# - with MIN_HEADER_PADDING: at least that many bytes lie between the load
#   commands and the first section. A hook on a function of the image itself
#   takes its island from there (Mac/Platform/Islands.h).
# - with MACOS_MINIMUM: the dylib says it runs on that macOS and later, which
#   is what dyld compares with the running system before it loads it;
# - with ARCHIVE_LIST as well (a file naming, one a line, the static libraries
#   from outside the build that are linked into the dylib): no object of
#   theirs was compiled for a later macOS. The linker takes such an object
#   with no more than a warning, and its code may use what the older system
#   does not have;
# - with GAME_APP as well: the game's arm64 image runs on nothing older than
#   that minimum. The dylib is loaded into the game, wherever the game starts.

foreach(tool nm otool)
    find_program(${tool}_PATH ${tool} REQUIRED)
endforeach()

execute_process(COMMAND ${nm_PATH} -gU "${DYLIB}" OUTPUT_VARIABLE exported RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm failed on ${DYLIB}")
endif()
string(STRIP "${exported}" exported)
if(EXPORTED)
    string(REPLACE "," ";" names "${EXPORTED}")
    string(REPLACE "\n" ";" lines "${exported}")
    set(exported "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^.* " "" symbol "${line}")
        if(symbol IN_LIST names)
            list(REMOVE_ITEM names "${symbol}")
        else()
            string(APPEND exported "${line}\n")
        endif()
    endforeach()
    if(names)
        message(FATAL_ERROR "${DYLIB} does not export ${names}")
    endif()
    string(STRIP "${exported}" exported)
endif()
if(EXPORTS)
    string(REGEX REPLACE "[^\n]* (${EXPORTS})(\n|$)" "" exported "${exported}")
    string(STRIP "${exported}" exported)
endif()
if(NOT exported STREQUAL "")
    message(FATAL_ERROR "${DYLIB} exports symbols:\n${exported}")
endif()

execute_process(COMMAND ${nm_PATH} -m -u "${DYLIB}" OUTPUT_VARIABLE imports RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm failed on ${DYLIB}")
endif()
string(REPLACE "\n" ";" imports "${imports}")
set(unbound "")
foreach(line IN LISTS imports)
    if(line STREQUAL "")
        continue()
    endif()
    if(NOT line MATCHES "\\(from [^)]+\\)" OR line MATCHES "dynamically looked up")
        string(APPEND unbound "  ${line}\n")
    endif()
endforeach()
if(NOT unbound STREQUAL "")
    message(FATAL_ERROR "${DYLIB} imports symbols bound to no library:\n${unbound}")
endif()

execute_process(COMMAND ${otool_PATH} -L "${DYLIB}" OUTPUT_VARIABLE libraries RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "otool failed on ${DYLIB}")
endif()
string(REPLACE "\n" ";" libraries "${libraries}")
list(REMOVE_AT libraries 0)     # "<file>:"
get_filename_component(own_name "${DYLIB}" NAME)
set(foreign "")
foreach(line IN LISTS libraries)
    string(STRIP "${line}" line)
    string(FIND "${line}" "@rpath/${own_name} " at)
    if(line STREQUAL "" OR at EQUAL 0)
        continue()          # its own install name
    endif()
    if(NOT line MATCHES "^/usr/lib/" AND NOT line MATCHES "^/System/Library/")
        string(APPEND foreign "  ${line}\n")
    endif()
endforeach()
if(NOT foreign STREQUAL "")
    message(FATAL_ERROR "${DYLIB} loads libraries that are not the OS's:\n${foreign}")
endif()

if(LAST_INITIALIZER)
    # dyld_info lists the initializers in the order they run, each with its symbol.
    execute_process(COMMAND xcrun dyld_info -inits "${DYLIB}" OUTPUT_VARIABLE inits RESULT_VARIABLE rc ERROR_VARIABLE problem)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "dyld_info failed on ${DYLIB}: ${problem}")
    endif()
    string(REGEX MATCHALL "0x[0-9A-Fa-f]+ +[^\n]+" entries "${inits}")
    list(LENGTH entries count)
    if(count EQUAL 0)
        message(FATAL_ERROR "${DYLIB} has no initializer:\n${inits}")
    endif()
    list(GET entries -1 last)
    set(named 0)
    foreach(entry IN LISTS entries)
        if(entry MATCHES "${LAST_INITIALIZER}")
            math(EXPR named "${named} + 1")
        endif()
    endforeach()
    if(NOT named EQUAL 1 OR NOT last MATCHES "${LAST_INITIALIZER}")
        message(FATAL_ERROR "${DYLIB}: ${LAST_INITIALIZER} must be the image's one and last initializer of ${count}; "
                            "it is there ${named} time(s), and the last is:\n  ${last}")
    endif()
endif()

if(MIN_HEADER_PADDING)
    execute_process(COMMAND ${otool_PATH} -l "${DYLIB}" OUTPUT_VARIABLE commands RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "otool failed on ${DYLIB}")
    endif()
    # The header is 32 bytes; the load commands follow it; __text is the first section.
    if(NOT commands MATCHES "sizeofcmds ([0-9]+)")
        execute_process(COMMAND ${otool_PATH} -h "${DYLIB}" OUTPUT_VARIABLE header)
        string(REGEX MATCH "\n *0x[0-9a-fA-F]+ +[0-9]+ +[0-9]+ +0x[0-9a-fA-F]+ +[0-9]+ +[0-9]+ +([0-9]+)" found "${header}")
    endif()
    set(sizeofcmds "${CMAKE_MATCH_1}")
    if(NOT commands MATCHES "sectname __text[^\n]*\n[^\n]*segname __TEXT[^\n]*\n[^\n]*\n[^\n]*\n *offset ([0-9]+)")
        message(FATAL_ERROR "${DYLIB}: no __TEXT,__text section found")
    endif()
    set(text_offset "${CMAKE_MATCH_1}")
    if(NOT sizeofcmds MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${DYLIB}: the size of the load commands was not found")
    endif()
    math(EXPR padding "${text_offset} - 32 - ${sizeofcmds}")
    if(padding LESS MIN_HEADER_PADDING)
        message(FATAL_ERROR "${DYLIB} has ${padding} bytes of header padding, less than the ${MIN_HEADER_PADDING} asked for")
    endif()
endif()

# The "minos" of every LC_BUILD_VERSION in `files` (Mach-O files or archives of them).
function(minimum_systems out)
    execute_process(COMMAND ${otool_PATH} ${ARGN} OUTPUT_VARIABLE commands RESULT_VARIABLE rc ERROR_VARIABLE problem)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "otool ${ARGN} failed: ${problem}")
    endif()
    string(REGEX MATCHALL "cmd LC_BUILD_VERSION[^\n]*\n[^\n]*\n[^\n]*\n *minos [0-9.]+" found "${commands}")
    set(versions "")
    foreach(command IN LISTS found)
        string(REGEX REPLACE ".*minos " "" version "${command}")
        list(APPEND versions "${version}")
    endforeach()
    list(REMOVE_DUPLICATES versions)
    set(${out} "${versions}" PARENT_SCOPE)
endfunction()

if(MACOS_MINIMUM)
    minimum_systems(built_for -l "${DYLIB}")
    if(NOT built_for MATCHES "^[0-9.]+$" OR NOT built_for VERSION_EQUAL MACOS_MINIMUM)
        message(FATAL_ERROR "${DYLIB} is built for macOS '${built_for}', not for ${MACOS_MINIMUM}")
    endif()

    if(ARCHIVE_LIST)
        file(STRINGS "${ARCHIVE_LIST}" archives)
        set(later "")
        foreach(archive IN LISTS archives)
            minimum_systems(versions -l "${archive}")
            if(versions STREQUAL "")
                message(FATAL_ERROR "${archive}: no object says which macOS it was compiled for")
            endif()
            foreach(version IN LISTS versions)
                if(version VERSION_GREATER MACOS_MINIMUM)
                    string(APPEND later "  ${archive}: macOS ${version}\n")
                endif()
            endforeach()
        endforeach()
        if(NOT later STREQUAL "")
            message(FATAL_ERROR "${DYLIB} is built for macOS ${MACOS_MINIMUM} and links libraries compiled for a later one "
                                "(Mac/setup.sh builds them for it):\n${later}")
        endif()
    endif()

    if(GAME_APP)
        execute_process(COMMAND plutil -extract CFBundleExecutable raw "${GAME_APP}/Contents/Info.plist"
            OUTPUT_VARIABLE executable RESULT_VARIABLE rc ERROR_VARIABLE problem OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${GAME_APP}: its executable's name could not be read: ${problem}${executable}")
        endif()
        minimum_systems(game -arch arm64 -l "${GAME_APP}/Contents/MacOS/${executable}")
        if(NOT game MATCHES "^[0-9.]+$")
            message(FATAL_ERROR "${GAME_APP}: the arm64 image of ${executable} does not say which macOS it runs on ('${game}')")
        endif()
        if(MACOS_MINIMUM VERSION_GREATER game)
            message(FATAL_ERROR "${DYLIB} is built for macOS ${MACOS_MINIMUM}; the game runs on ${game} and later")
        endif()
        message(STATUS "${DYLIB}: macOS ${MACOS_MINIMUM}; the game: macOS ${game}")
    endif()
endif()
