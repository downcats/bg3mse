# The shipped copy of an image of the build, and its debug symbols:
#
#   cmake -DIMAGE=<image as linked> -DOUTPUT=<shipped copy> -DDSYM=<name.dSYM>
#         [-DIDENTITY=<codesign identity, "-" by default>] [-DEXECUTABLE=ON]
#         [-DENTITLEMENTS=<plist>] -P Symbols.cmake
#
# 1. dsymutil gathers the image's debug information (the line tables of the
#    objects its debug map names) into DSYM, which carries the image's UUID.
# 2. The copy is stripped of its debug map (it names the build's object
#    files) and of its local symbols. What is left is what the image needs
#    at run time and what the crash reporter reads of it: the exported and
#    imported symbols, LC_UUID, which ties a report to its dSYM (the report's
#    MIMG), and LC_FUNCTION_STARTS, which bg3mse's function extents are
#    taken from (Platform/FunctionExtents.h: the crash reporter's rule and
#    the hooks on the extender's own functions). A frame in a stripped image
#    is reported by its address in the file; the dSYM names it
#    (bg3mse-symbolicate).
# 3. Stripping invalidates the linker's signature, and an arm64 image whose
#    signature does not match does not load: the copy is signed again. "-"
#    signs ad hoc, as the linker did; another identity is a certificate in
#    the keychain ("Developer ID Application: ..."), and gets a secure
#    timestamp, and an executable the hardened runtime, with ENTITLEMENTS
#    when they are given.
#
# Each step is checked, and the script fails when one did not do what it says.

foreach(name IMAGE OUTPUT DSYM)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "Symbols.cmake: ${name} is not set")
    endif()
endforeach()
if(NOT EXISTS "${IMAGE}")
    message(FATAL_ERROR "Symbols.cmake: ${IMAGE} does not exist")
endif()
if(NOT DEFINED IDENTITY OR IDENTITY STREQUAL "")
    set(IDENTITY "-")
endif()

foreach(tool dsymutil strip codesign dwarfdump nm otool)
    find_program(${tool}_PATH ${tool} REQUIRED)
endforeach()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        list(JOIN ARGN " " command)
        message(FATAL_ERROR "Symbols.cmake: ${command} failed (${rc}):\n${out}${err}")
    endif()
    set(run_output "${out}" PARENT_SCOPE)
endfunction()

# The UUID of the arm64 image in `file`.
function(uuid_of file out)
    run("${dwarfdump_PATH}" --uuid --arch arm64 "${file}")
    if(NOT run_output MATCHES "UUID: ([0-9A-F-]+) \\(arm64\\)")
        message(FATAL_ERROR "Symbols.cmake: ${file} has no arm64 UUID:\n${run_output}")
    endif()
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

get_filename_component(image_name "${IMAGE}" NAME)
get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(REMOVE_RECURSE "${DSYM}" "${OUTPUT}")

# 1. The debug information.
run("${dsymutil_PATH}" --arch arm64 "${IMAGE}" -o "${DSYM}")
set(dwarf "${DSYM}/Contents/Resources/DWARF/${image_name}")
if(NOT EXISTS "${dwarf}")
    message(FATAL_ERROR "Symbols.cmake: dsymutil wrote no ${dwarf}")
endif()
run("${otool_PATH}" -l "${dwarf}")
if(NOT run_output MATCHES "sectname __debug_line\n *segname __DWARF\n *addr 0x[0-9a-f]+\n *size 0x0*[1-9a-f]")
    message(FATAL_ERROR "Symbols.cmake: ${DSYM} has no line tables: ${image_name} was linked from objects "
                        "compiled without debug information (BG3MSE_DEBUG_INFO)")
endif()

# 2. The stripped copy.
run("${nm_PATH}" -gU "${IMAGE}")
set(exported_before "${run_output}")
file(COPY_FILE "${IMAGE}" "${OUTPUT}")
run("${strip_PATH}" -S -x "${OUTPUT}")
run("${nm_PATH}" -gU "${OUTPUT}")
if(NOT run_output STREQUAL exported_before)
    message(FATAL_ERROR "Symbols.cmake: stripping ${image_name} changed what it exports")
endif()
# strip leaves one entry of its own, "OPT radr://5614542", which says the
# image was stripped.
run("${nm_PATH}" -a "${OUTPUT}")
string(REGEX REPLACE "[0-9a-f]+ - 00 0000 +OPT radr://5614542\n" "" all_symbols "${run_output}")
run("${nm_PATH}" "${OUTPUT}")
if(NOT run_output STREQUAL all_symbols)
    message(FATAL_ERROR "Symbols.cmake: ${OUTPUT} still has debug map entries")
endif()
run("${nm_PATH}" -m "${OUTPUT}")
string(REGEX MATCHALL "[^\n]*\\) non-external [^\n]*" locals "${run_output}")
if(locals)
    list(GET locals 0 first)
    message(FATAL_ERROR "Symbols.cmake: ${OUTPUT} still has local symbols: ${first}")
endif()
run("${otool_PATH}" -l "${OUTPUT}")
foreach(command LC_UUID LC_FUNCTION_STARTS)
    if(NOT run_output MATCHES "cmd ${command}\n")
        message(FATAL_ERROR "Symbols.cmake: ${OUTPUT} has no ${command}")
    endif()
endforeach()

# 3. The signature.
set(sign "${codesign_PATH}" --force --sign "${IDENTITY}")
if(NOT IDENTITY STREQUAL "-")
    list(APPEND sign --timestamp)
    if(EXECUTABLE)
        list(APPEND sign --options runtime)
        if(ENTITLEMENTS)
            list(APPEND sign --entitlements "${ENTITLEMENTS}")
        endif()
    endif()
endif()
run(${sign} "${OUTPUT}")
run("${codesign_PATH}" --verify --strict "${OUTPUT}")

# The dSYM is the shipped image's.
uuid_of("${OUTPUT}" shipped)
uuid_of("${dwarf}" symbols)
if(NOT shipped STREQUAL symbols)
    message(FATAL_ERROR "Symbols.cmake: ${OUTPUT} is ${shipped}, its dSYM ${symbols}")
endif()
message(STATUS "${image_name}: stripped and signed (${IDENTITY}), dSYM ${symbols}")
