# The version stamp of an image of the build (Release/Version.h), read back
# from the file as a tool reads it:
#
#   what -s <image>     the stamp, "bg3mse <version> commit=<commit> extender=<version> channel=<channel>"
#   otool -l <image>    LC_ID_DYLIB's current version, for a dylib
#
# Included, it defines bg3mse_read_stamp(<image> <prefix>), which sets
# <prefix>_VERSION, _COMMIT, _EXTENDER, _CHANNEL and, for a dylib,
# _CURRENT_VERSION. Run as a script it checks an image against the build:
#
#   cmake -DIMAGE=<image> -DVERSION=<a.b.c> -DCOMMIT=<commit> -DEXTENDER=<version> -DCHANNEL=<channel>
#         [-DDYLIB=ON] -P Stamp.cmake

function(bg3mse_read_stamp image prefix)
    find_program(what_PATH what REQUIRED)
    find_program(otool_PATH otool REQUIRED)
    execute_process(COMMAND "${what_PATH}" -s "${image}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "\n\tbg3mse ([0-9]+\\.[0-9]+\\.[0-9]+) commit=([0-9a-f]+|unknown) extender=([0-9.]+) channel=([A-Za-z]+)\n")
        message(FATAL_ERROR "${image} carries no bg3mse stamp (what -s says: ${out})")
    endif()
    set(${prefix}_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${prefix}_COMMIT "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(${prefix}_EXTENDER "${CMAKE_MATCH_3}" PARENT_SCOPE)
    set(${prefix}_CHANNEL "${CMAKE_MATCH_4}" PARENT_SCOPE)
    execute_process(COMMAND "${otool_PATH}" -l "${image}" OUTPUT_VARIABLE commands RESULT_VARIABLE rc)
    if(rc EQUAL 0 AND commands MATCHES "cmd LC_ID_DYLIB\n[^\n]*\n[^\n]*\n[^\n]*\n *current version ([0-9.]+)\n")
        set(${prefix}_CURRENT_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${prefix}_CURRENT_VERSION "" PARENT_SCOPE)
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    bg3mse_read_stamp("${IMAGE}" found)
    foreach(field VERSION COMMIT EXTENDER CHANNEL)
        if(NOT found_${field} STREQUAL "${${field}}")
            message(FATAL_ERROR "${IMAGE}: the stamp's ${field} is '${found_${field}}', the build's '${${field}}'")
        endif()
    endforeach()
    if(DYLIB AND NOT found_CURRENT_VERSION STREQUAL VERSION)
        message(FATAL_ERROR "${IMAGE}: LC_ID_DYLIB's current version is '${found_CURRENT_VERSION}', not ${VERSION}")
    endif()
    message(STATUS "${IMAGE}: bg3mse ${found_VERSION}, commit ${found_COMMIT}, Script Extender ${found_EXTENDER}, ${found_CHANNEL}")
endif()
