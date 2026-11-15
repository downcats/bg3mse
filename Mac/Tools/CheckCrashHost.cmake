# Run by ctest
# (cmake -DARCHIVE=<libbg3mse_crash_extender.a> -P CheckCrashHost.cmake).
#
# What the crash reporter takes from Norbyte's extender
# (Mac/Crash/ExtenderCrashReporting.cpp) is called at the time of a crash, on
# one of bg3mse's exception threads, while the faulting thread is stopped
# holding whatever it held. It must not call the game: the game's functions
# are written for the game's threads.
#
# The call that is easiest to make without noticing is reading a FixedString:
# its characters, length and hash, and its reference count when one is copied,
# all go through functions of the game's string table. ls::gst::Get, on a
# thread's first call, sets up a thread-local variable of the game's (an
# allocation, while the stopped thread may hold the allocator's lock) and
# takes one of the game's thread slots. So the file must not refer to any
# function of FixedStringBase; what it needs of a fixed string it takes
# another way (a module's UUID from the Guid beside it).

find_program(nm_PATH nm REQUIRED)

execute_process(COMMAND ${nm_PATH} -u "${ARCHIVE}" OUTPUT_VARIABLE text RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm -u ${ARCHIVE} failed")
endif()
string(REPLACE "\n" ";" lines "${text}")

set(undefined 0)
set(found "")
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^_")
        continue()
    endif()
    math(EXPR undefined "${undefined} + 1")
    # Itanium names: bg3se::FixedStringBase::... is N[K]5bg3se15FixedStringBase...
    if(line MATCHES "5bg3se15FixedStringBase")
        string(APPEND found "  ${line}\n")
    endif()
endforeach()

# The host refers to the extender, the static symbols and the Lua API: an
# archive with next to nothing undefined is not the one this check is for.
if(undefined LESS 10)
    message(FATAL_ERROR "${ARCHIVE} has ${undefined} undefined symbols: not the crash reporter's host?")
endif()
if(NOT found STREQUAL "")
    message(FATAL_ERROR "${ARCHIVE} calls the game's string table at the time of a crash:\n${found}")
endif()
message(STATUS "${ARCHIVE}: ${undefined} undefined symbols, none of FixedStringBase")
