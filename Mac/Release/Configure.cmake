# What every target of the build gets from Mac/Release, included before the
# first target is defined: debug information, and bg3mse's version stamp.

# --- Debug information ----------------------------------------------------------
#
# Line tables only: function names, files, lines and inlined frames, which is
# what a crash report is symbolicated with (the release's .dSYM files,
# Mac/Release/Symbols.cmake). They do not change the code. Full -g costs
# Norbyte's largest sources much more compiler memory.
option(BG3MSE_DEBUG_INFO "Compile with line tables (-gline-tables-only), for the release's .dSYM files" ON)
if(BG3MSE_DEBUG_INFO)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-gline-tables-only>")
endif()

# --- The version stamp ------------------------------------------------------------
#
# bg3mse's own version, raised by hand for a release. It is the dylibs'
# LC_ID_DYLIB current version, so it is three numbers, the first at most
# 65535 and the others at most 255.
set(BG3MSE_VERSION "1.0.0")
if(NOT BG3MSE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$"
        OR CMAKE_MATCH_1 GREATER 65535 OR CMAKE_MATCH_2 GREATER 255 OR CMAKE_MATCH_3 GREATER 255)
    message(FATAL_ERROR "BG3MSE_VERSION '${BG3MSE_VERSION}' is not a Mach-O version (a.b.c, a <= 65535, b and c <= 255)")
endif()

# Norbyte's version of the extender, as BG3Extender.rc takes it.
set(version_header "${CMAKE_SOURCE_DIR}/BG3Extender/resource.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${version_header}")
file(STRINGS "${version_header}" version_line REGEX "^#define[ \t]+RES_DLL_VERSION_STRING[ \t]+\"[0-9.]+\"")
if(NOT version_line MATCHES "\"([0-9.]+)\"")
    message(FATAL_ERROR "BG3Extender/resource.h defines no RES_DLL_VERSION_STRING")
endif()
set(BG3MSE_EXTENDER_VERSION "${CMAKE_MATCH_1}")

# The commit the tree is at, read when the build is configured. Configuring
# again is what a moved HEAD brings about: the files git changes when it moves
# (HEAD itself, its log, the branch it names) are inputs of the configuration,
# and nothing is read from git while building, so a build is not out of date
# because of git alone. Whether the tree has changes since that commit is not
# stamped: what was true when the build was configured need not be true when
# it is built. The release target checks it at its own time.
find_program(BG3MSE_GIT git)
set(BG3MSE_COMMIT "unknown")
if(BG3MSE_GIT)
    execute_process(COMMAND "${BG3MSE_GIT}" rev-parse --verify --quiet HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE head RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(rc EQUAL 0 AND head MATCHES "^[0-9a-f]+$")
        set(BG3MSE_COMMIT "${head}")
        execute_process(COMMAND "${BG3MSE_GIT}" rev-parse --symbolic-full-name HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE ref OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        set(watched HEAD logs/HEAD packed-refs)
        if(ref MATCHES "^refs/")
            list(APPEND watched "${ref}" "logs/${ref}")
        endif()
        foreach(name IN LISTS watched)
            execute_process(COMMAND "${BG3MSE_GIT}" rev-parse --git-path "${name}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(path)
                get_filename_component(path "${path}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
                if(EXISTS "${path}")
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
                endif()
            endif()
        endforeach()
    endif()
endif()

# One translation unit, linked into both dylibs: the stamp, a line what(1)
# finds ("what -s libbg3mse.dylib"), in a section of its own, and the values
# for the code (Release/Version.h). Written only when it changes.
set(BG3MSE_STAMP "bg3mse ${BG3MSE_VERSION} commit=${BG3MSE_COMMIT} extender=${BG3MSE_EXTENDER_VERSION} channel=${BG3MSE_CHANNEL}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/version/Version.cpp" @ONLY CONTENT [=[
// Written by Mac/Release/Configure.cmake.
#include <Release/Version.h>

namespace bg3se::mac::version {

char const kVersion[] = "@BG3MSE_VERSION@";
char const kCommit[] = "@BG3MSE_COMMIT@";
char const kExtenderVersion[] = "@BG3MSE_EXTENDER_VERSION@";
char const kChannel[] = "@BG3MSE_CHANNEL@";
__attribute__((used, section("__TEXT,__bg3mse_version")))
char const kStamp[] = "@(#)@BG3MSE_STAMP@";

}
]=])
add_library(bg3mse_version STATIC "${CMAKE_BINARY_DIR}/version/Version.cpp")
target_include_directories(bg3mse_version PUBLIC Mac)
message(STATUS "bg3mse ${BG3MSE_VERSION}, commit ${BG3MSE_COMMIT}, Script Extender ${BG3MSE_EXTENDER_VERSION}")
