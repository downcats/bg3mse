# Run by ctest (cmake -DIMAGE=<image> "-DRESOURCES=<type>,<name>,<file>;..."
# -DSCRATCH=<directory> -P CheckResources.cmake).
#
# An image that links the extender carries Norbyte's resources as sections
# (Mac/Shim/Win32Resources.cpp: segment <type>, section <name>). Each section
# is taken out of the image file and compared with the file it was made from.

find_program(segedit_PATH segedit REQUIRED)
file(MAKE_DIRECTORY "${SCRATCH}")
set(count 0)
foreach(resource IN LISTS RESOURCES)
    if(NOT resource MATCHES "^([^,]+),([^,]+),(.+)$")
        message(FATAL_ERROR "not <type>,<name>,<file>: ${resource}")
    endif()
    set(type "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    set(source "${CMAKE_MATCH_3}")
    set(extracted "${SCRATCH}/${type}.${name}")
    file(REMOVE "${extracted}")
    execute_process(COMMAND ${segedit_PATH} "${IMAGE}" -extract "${type}" "${name}" "${extracted}"
        RESULT_VARIABLE rc ERROR_VARIABLE error)
    if(NOT rc EQUAL 0 OR NOT EXISTS "${extracted}")
        message(FATAL_ERROR "${IMAGE} has no resource ${name} of type ${type}:\n${error}")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${extracted}" "${source}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "resource ${name} of type ${type} in ${IMAGE} differs from ${source}")
    endif()
    file(SIZE "${source}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "${source} is empty")
    endif()
    math(EXPR count "${count} + 1")
endforeach()
if(count EQUAL 0)
    message(FATAL_ERROR "no resources to check")
endif()
message(STATUS "${count} resources match their files")
