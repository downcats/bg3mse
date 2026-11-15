# A zip of a directory, as the updater reads one (Mac/Updater/Zip.h):
#
#   cmake -DROOT=<parent directory> -DENTRY=<directory under it> -DOUTPUT=<file.zip> -P Zip.cmake
#
# The archive holds the files of ROOT/ENTRY under ENTRY/, with their modes,
# and no entries for directories nor the extra fields of uid, gid and times
# (zip -D -X).

foreach(name ROOT ENTRY OUTPUT)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "Zip.cmake: ${name} is not set")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${ROOT}/${ENTRY}")
    message(FATAL_ERROR "Zip.cmake: ${ROOT}/${ENTRY} is not a directory")
endif()
find_program(zip_PATH zip REQUIRED)

get_filename_component(output "${OUTPUT}" ABSOLUTE)
get_filename_component(output_dir "${output}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
# zip adds to an archive that is there.
file(REMOVE "${output}")
execute_process(COMMAND "${zip_PATH}" -r -X -D -q -9 "${output}" "${ENTRY}"
    WORKING_DIRECTORY "${ROOT}" RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Zip.cmake: zip failed (${rc}): ${err}")
endif()
