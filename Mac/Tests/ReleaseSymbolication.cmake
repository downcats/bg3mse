# A crash report from a stripped dylib is symbolicated with its dSYM.
#
#   cmake -DFIXTURE=<libbg3mse_release_crash_fixture.dylib> -DDRIVER=<bg3mse_release_crash_driver>
#         -DSYMBOLICATE=<bg3mse-symbolicate> -DSOURCE=<Mac/Tests/ReleaseCrashFixture.cpp>
#         -DSYMBOLS_SCRIPT=<Mac/Release/Symbols.cmake> -DSCRATCH=<directory> -P ReleaseSymbolication.cmake
#
# The fixture carries the crash reporter as libbg3mse.dylib does. It is
# stripped and signed as the release does it, loaded, and crashes in a
# function of its own; the report it leaves names no function of the fixture,
# and bg3mse-symbolicate names it, with its file and line, from the dSYM, and
# the function the fixture's other thread waits in.

foreach(name FIXTURE DRIVER SYMBOLICATE SOURCE SYMBOLS_SCRIPT SCRATCH)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "${name} is not set")
    endif()
endforeach()
file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}/reports")
get_filename_component(name "${FIXTURE}" NAME)
set(shipped "${SCRATCH}/shipped/${name}")
set(dsym "${SCRATCH}/symbols/${name}.dSYM")

execute_process(COMMAND "${CMAKE_COMMAND}" "-DIMAGE=${FIXTURE}" "-DOUTPUT=${shipped}" "-DDSYM=${dsym}"
    -P "${SYMBOLS_SCRIPT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Symbols.cmake failed on the fixture")
endif()

execute_process(COMMAND "${DRIVER}" "${shipped}" "${SCRATCH}/reports" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc MATCHES "^[0-9]+$")
    message(FATAL_ERROR "the stripped fixture did not crash (exit ${rc}): ${out}${err}")
endif()
file(GLOB reports "${SCRATCH}/reports/crash_*.crpt")
list(LENGTH reports count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "the crash left ${count} reports, not one (${rc}): ${err}")
endif()

# The report of the stripped image: the faulting frame is the fixture's, and
# without the dSYM it has no name but its address in the file, in the
# backtrace (MBTR) and in the text shown to the user (TRAC).
file(MAKE_DIRECTORY "${SCRATCH}/none")
execute_process(COMMAND "${SYMBOLICATE}" "${reports}" "${SCRATCH}/none" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT "\n${out}" MATCHES "\n  0  0x[0-9a-f]+  ${name} +${name}!0x[0-9a-f]+\n")
    message(FATAL_ERROR "without its dSYM, frame 0 is not an unnamed frame of the fixture (${rc}):\n${out}${err}")
endif()
file(STRINGS "${reports}" trace REGEX "${name}!0x[0-9a-f]+")
if(NOT trace)
    message(FATAL_ERROR "the report's backtrace text has no line ${name}!0x...")
endif()

# The line of the fault, in the fixture's source.
file(STRINGS "${SOURCE}" lines)
set(number 0)
set(fault_line 0)
foreach(line IN LISTS lines)
    math(EXPR number "${number} + 1")
    if(line MATCHES "// The fault\\.$")
        set(fault_line ${number})
    endif()
endforeach()
if(fault_line EQUAL 0)
    message(FATAL_ERROR "${SOURCE} has no line marked as the fault")
endif()
get_filename_component(source_name "${SOURCE}" NAME)

execute_process(COMMAND "${SYMBOLICATE}" "${reports}" "${SCRATCH}/symbols" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bg3mse-symbolicate failed (${rc}): ${out}${err}")
endif()
string(REGEX MATCH "\n  0  0x[0-9a-f]+  ${name} +[^\n]*" first "\n${out}")
if(NOT first MATCHES "FixtureFaultingFunction" OR NOT first MATCHES "\\(${source_name}:${fault_line}\\)")
    message(FATAL_ERROR "frame 0 is not FixtureFaultingFunction at ${source_name}:${fault_line}:\n${out}")
endif()
# The frame that called it, the fixture's exported function, too.
if(NOT out MATCHES "\n  1  0x[0-9a-f]+  ${name} +bg3mse_release_fixture_crash")
    message(FATAL_ERROR "frame 1 is not bg3mse_release_fixture_crash:\n${out}")
endif()

# The fixture's other thread, by its name, waiting in a function of the
# fixture that only the dSYM names, at the line marked as where it waits.
set(wait_line 0)
set(number 0)
foreach(line IN LISTS lines)
    math(EXPR number "${number} + 1")
    if(line MATCHES "// The worker waits\\.$")
        set(wait_line ${number})
    endif()
endforeach()
string(FIND "${out}" "\nThread " threads_at)
string(REGEX MATCH "\nThread [0-9]+ \"bg3mse fixture worker\", id [0-9]+\n(  *[0-9]+  0x[^\n]*\n)+" worker "${out}")
if(threads_at EQUAL -1 OR NOT worker)
    message(FATAL_ERROR "the fixture's worker thread is not in the output:\n${out}")
endif()
if(NOT worker MATCHES "  ${name} +[^\n]*FixtureWaitingFunction[^\n]*\\(${source_name}:${wait_line}\\)")
    message(FATAL_ERROR "the worker thread has no frame FixtureWaitingFunction at ${source_name}:${wait_line}:\n${worker}")
endif()
file(REMOVE_RECURSE "${SCRATCH}")
message(STATUS "symbolicated:\n${out}")
