# bg3mse-install (Mac/Installer/Installer.h): puts bg3mse in the game and
# takes it out again. Included from the root CMakeLists.txt.

add_library(bg3mse_installer STATIC
    Mac/Installer/MachO.cpp
    Mac/Installer/Extras.cpp
    Mac/Installer/Installer.cpp
    Mac/Installer/Launch.cpp
    Mac/Installer/Setup.cpp
    Mac/Installer/Support.cpp)
target_include_directories(bg3mse_installer PUBLIC Mac)
target_compile_options(bg3mse_installer PRIVATE ${BG3MSE_WARNINGS})
# CoreFoundation: the bundle's Info.plist. The updater's stamps: the versions of the files installed.
# The platform layer: bg3mse.conf's keys.
target_link_libraries(bg3mse_installer PUBLIC bg3mse_updater_stamp bg3mse_platform "-framework CoreFoundation")

# What bg3mse-install and the installer app give the verbs (Mac/Installer/ToolHost.h).
# The engine table decides whether bg3mse supports the game, as bg3mse-engine check does.
# The shader library: the game's own Metal functions, read for mods' shaders
# (Mac/Shaders/GameInterface.h); what of it that read does not reach is left out.
add_library(bg3mse_installer_host STATIC Mac/Installer/ToolHost.cpp)
target_link_libraries(bg3mse_installer_host PUBLIC bg3mse_installer bg3mse_engine bg3mse_version bg3mse_core)
target_compile_options(bg3mse_installer_host PRIVATE ${BG3MSE_WARNINGS})

add_executable(bg3mse-install Mac/Installer/InstallTool.cpp)
target_link_libraries(bg3mse-install PRIVATE bg3mse_installer_host)
target_link_options(bg3mse-install PRIVATE -Wl,-dead_strip)
target_compile_options(bg3mse-install PRIVATE ${BG3MSE_WARNINGS})

# Starts the game with the dylib named in DYLD_INSERT_LIBRARIES (Mac/Installer/Launch.h).
add_executable(bg3mse-launch Mac/Installer/LaunchTool.cpp)
# The platform layer: where bg3mse's data directory is, for the launcher's log.
target_link_libraries(bg3mse-launch PRIVATE bg3mse_installer bg3mse_platform bg3mse_version)
target_compile_options(bg3mse-launch PRIVATE ${BG3MSE_WARNINGS})

# bg3mse.app, the installer a player double-clicks (Mac/Installer/App): the
# program; the release target makes the bundle around it, with the release in it.
add_executable(bg3mse_installer_app Mac/Installer/App/InstallerApp.mm)
target_link_libraries(bg3mse_installer_app PRIVATE bg3mse_installer_host "-framework AppKit")
target_link_options(bg3mse_installer_app PRIVATE -Wl,-dead_strip)
target_compile_options(bg3mse_installer_app PRIVATE ${BG3MSE_WARNINGS} -fobjc-arc)

include(Mac/Installer/Tests/Tests.cmake)

if(BG3MSE_GAME_APP)
    # bg3mse-install shader-interface on the game: what it leaves is the
    # game's Metal interface. The game is only read.
    add_test(NAME bg3mse_game_shader_interface
        COMMAND bg3mse_shader_startup_tests game "${BG3MSE_GAME_APP}" $<TARGET_FILE:bg3mse-install>)
    set_tests_properties(bg3mse_game_shader_interface PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
endif()
