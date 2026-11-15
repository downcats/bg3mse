# The tests of bg3mse-install (Mac/Installer/Tests/InstallerTests.cpp).

# What the tests start: a stand-in for the game's executable, universal and
# signed ad hoc as the game's is. And stand-ins for the libraries, which say
# when they load.
set(BG3MSE_INSTALLER_FIXTURES "${CMAKE_BINARY_DIR}/installer-fixtures")
add_executable(bg3mse_installer_fixture_game Mac/Installer/Tests/Fixture.c)
set_target_properties(bg3mse_installer_fixture_game PROPERTIES OUTPUT_NAME game OSX_ARCHITECTURES "arm64;x86_64"
    RUNTIME_OUTPUT_DIRECTORY "${BG3MSE_INSTALLER_FIXTURES}")
add_custom_command(TARGET bg3mse_installer_fixture_game POST_BUILD
    COMMAND codesign --sign - --force "$<TARGET_FILE:bg3mse_installer_fixture_game>" VERBATIM)
list(APPEND BG3MSE_INSTALLER_FIXTURE_TARGETS bg3mse_installer_fixture_game)
foreach(library IN ITEMS bg3mse bg3mse-camera bg3mse-loader)
    string(REPLACE "-" "_" target "bg3mse_installer_standin_${library}")
    add_library(${target} SHARED Mac/Installer/Tests/StandIn.c)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${library} LIBRARY_OUTPUT_DIRECTORY "${BG3MSE_INSTALLER_FIXTURES}/standin")
    target_compile_definitions(${target} PRIVATE "STAND_IN_NAME=\"lib${library}.dylib\"")
    list(APPEND BG3MSE_INSTALLER_FIXTURE_TARGETS ${target})
endforeach()
target_compile_definitions(bg3mse_installer_standin_bg3mse_loader PRIVATE "STAND_IN_LOADS=\"libbg3mse.dylib\"")
# libbg3mse.dylib with the graphics module's mark (Release/GfxMark.h), and
# one without it in standin-plain/: a build without the module.
target_compile_definitions(bg3mse_installer_standin_bg3mse PRIVATE STAND_IN_GFX)
target_include_directories(bg3mse_installer_standin_bg3mse PRIVATE Mac)
add_library(bg3mse_installer_standin_plain SHARED Mac/Installer/Tests/StandIn.c)
set_target_properties(bg3mse_installer_standin_plain PROPERTIES OUTPUT_NAME bg3mse LIBRARY_OUTPUT_DIRECTORY "${BG3MSE_INSTALLER_FIXTURES}/standin-plain")
target_compile_definitions(bg3mse_installer_standin_plain PRIVATE "STAND_IN_NAME=\"libbg3mse.dylib\"")
list(APPEND BG3MSE_INSTALLER_FIXTURE_TARGETS bg3mse_installer_standin_plain)
# The camera module's licence texts beside the stand-ins, in camera/, as a
# build directory has them: the module is installed only with them.
set(standin_texts "")
foreach(licence IN ITEMS LICENSE COPYING EXCEPTIONS)
    set(text "${BG3MSE_INSTALLER_FIXTURES}/standin/camera/${licence}")
    add_custom_command(OUTPUT "${text}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${BG3MSE_INSTALLER_FIXTURES}/standin/camera"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Mac/Camera/${licence}" "${text}"
        DEPENDS "Mac/Camera/${licence}"
        VERBATIM)
    list(APPEND standin_texts "${text}")
endforeach()
add_custom_target(bg3mse_installer_standin_camera_texts DEPENDS ${standin_texts})
list(APPEND BG3MSE_INSTALLER_FIXTURE_TARGETS bg3mse_installer_standin_camera_texts)

add_executable(bg3mse_installer_tests Mac/Installer/Tests/InstallerTests.cpp)
target_link_libraries(bg3mse_installer_tests PRIVATE bg3mse_installer)
target_compile_options(bg3mse_installer_tests PRIVATE ${BG3MSE_WARNINGS})
target_compile_definitions(bg3mse_installer_tests PRIVATE
    BG3MSE_INSTALLER_FIXTURES="${BG3MSE_INSTALLER_FIXTURES}"
    BG3MSE_INSTALLER_WORK="${CMAKE_BINARY_DIR}/installer-work"
    BG3MSE_INSTALL_TOOL="$<TARGET_FILE:bg3mse-install>"
    BG3MSE_LAUNCHER="$<TARGET_FILE:bg3mse-launch>")
add_dependencies(bg3mse_installer_tests bg3mse-install bg3mse-launch ${BG3MSE_INSTALLER_FIXTURE_TARGETS})
# The camera module's own reader and default settings file, when the build has
# them: what the camera's settings are written from, and read back with.
if(TARGET bg3mse-camera)
    target_compile_definitions(bg3mse_installer_tests PRIVATE BG3MSE_CAMERA_TOOL="$<TARGET_FILE:bg3mse-camera>"
        BG3MSE_CAMERA_DEFAULTS="${BG3MSE_CAMERA_DEFAULTS}")
    add_dependencies(bg3mse_installer_tests bg3mse-camera bg3mse_camera_texts)
endif()
# This build's libbg3mse.dylib, when it has the graphics module: the mark it
# carries (Release/GfxMark.h), as bg3mse-install reads it.
if(TARGET bg3mse_gfx)
    target_compile_definitions(bg3mse_installer_tests PRIVATE BG3MSE_GFX_DYLIB="$<TARGET_FILE:bg3mse>")
    add_dependencies(bg3mse_installer_tests bg3mse)
endif()
# HOME is the build directory's, so that nothing reaches the user's settings.
add_test(NAME bg3mse_installer_tests COMMAND bg3mse_installer_tests)
set_tests_properties(bg3mse_installer_tests PROPERTIES ENVIRONMENT "HOME=${CMAKE_BINARY_DIR}/installer-home")
