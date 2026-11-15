# Runs `bg3mse-ingame check` (or COMMAND: settings, crash-check) on LOGS and
# compares what it prints with EXPECTED (when given) and its exit status with EXIT.
#   cmake -DTOOL=... "-DLOGS=a.log;b.log" -DEXIT=1 [-DEXPECTED=out.txt] [-DCOMMAND=settings] -P CheckLog.cmake
if(NOT DEFINED COMMAND)
    set(COMMAND check)
endif()
execute_process(COMMAND "${TOOL}" ${COMMAND} ${LOGS}
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT "${status}" STREQUAL "${EXIT}")
    message(FATAL_ERROR "bg3mse-ingame ${COMMAND} exited with ${status}, not ${EXIT}:\n${output}${errors}")
endif()
if(DEFINED EXPECTED)
    file(READ "${EXPECTED}" expected)
    if(NOT output STREQUAL expected)
        message(FATAL_ERROR "bg3mse-ingame check printed:\n${output}\nnot what ${EXPECTED} holds:\n${expected}")
    endif()
endif()
