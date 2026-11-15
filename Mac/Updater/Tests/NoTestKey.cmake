# The shipped loader takes its key from the build only: the tests' loader
# reads BG3MSE_UPDATER_TEST_KEY from its environment, and the shipped one
# must not have that name in it at all.
#
#   cmake -DLOADER=<libbg3mse-loader.dylib> -DTEST_LOADER=<libbg3mse-loader-test.dylib> -P NoTestKey.cmake

file(STRINGS "${TEST_LOADER}" test_names REGEX "BG3MSE_UPDATER_TEST_KEY")
if(NOT test_names)
    message(FATAL_ERROR "${TEST_LOADER} does not name BG3MSE_UPDATER_TEST_KEY: this check finds nothing")
endif()
file(STRINGS "${LOADER}" names REGEX "BG3MSE_UPDATER_TEST_KEY")
if(names)
    message(FATAL_ERROR "${LOADER} reads a key from its environment")
endif()
# It exports Norbyte's updater API and nothing else.
execute_process(COMMAND nm -gU "${LOADER}" OUTPUT_VARIABLE exported RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm failed on ${LOADER}")
endif()
string(REGEX MATCHALL "[^\n]+" lines "${exported}")
# All fourteen of BG3Updater/Exports.def but DWriteCreateFactory.
foreach(name SEUpdaterInitialize SESetGameVersion SEUpdate SEUpdaterGetError SEUpdaterGetLog SEUpdaterGetResourceVersion
        SEUpdaterShowConsole SEUpdaterTest SEUpdaterShutdown SEUpdaterInitializeLibrary SEUpdaterSetLogCallback SEUpdaterGetVersion
        SEUpdaterGetErrorFlags SEUpdaterSetCapabilities)
    if(NOT exported MATCHES " _${name}(\n|$)")
        message(FATAL_ERROR "${LOADER} does not export ${name}")
    endif()
endforeach()
foreach(line IN LISTS lines)
    if(NOT line MATCHES " _SE(Updater[A-Za-z]+|SetGameVersion|Update)$")
        message(FATAL_ERROR "${LOADER} exports more than the updater API: ${line}")
    endif()
endforeach()
message(STATUS "${LOADER}: no test key, exports only the updater API")
