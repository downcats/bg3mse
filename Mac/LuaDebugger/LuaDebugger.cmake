# Norbyte's Lua debug adapter (LuaDebugger/), built for macOS from his
# sources by Mac/LuaDebugger/LuaDebugger.csproj: the program an editor runs
# and speaks the Debug Adapter Protocol to, which connects to the Lua
# debugger's server in the game (EnableLuaDebugger). On Windows his own
# project builds it for .NET Framework and publish.ps1 ships it as
# DebugAdapter.zip.
#
# Built when BG3MSE_DOTNET names the dotnet of a .NET 10 SDK (the project
# targets .NET 10, whose reference assemblies that SDK carries). The build
# fetches nothing: the two packages come from External/nuget, where
# Mac/setup.sh puts them, and the protocol source from the protoc of
# External/protobuf-build. Everything it writes is in the build directory;
# the adapter is in DebugAdapter/ there: his assembly, the two packages', and
# the launcher an editor runs (Mac/LuaDebugger/LuaDebugger), where Windows
# has LuaDebugger.exe. It runs on the .NET runtime the machine has (10 or
# later), as it runs on the .NET Framework of Windows.
set(BG3MSE_DOTNET "" CACHE FILEPATH "The dotnet of a .NET 10 SDK; when set, Norbyte's Lua debug adapter is built for macOS (Mac/LuaDebugger)")

if(BG3MSE_DOTNET)
    set(BG3MSE_NUGET "${EXT}/nuget")
    foreach(package google.protobuf.3.36.1 newtonsoft.json.13.0.4)
        if(NOT EXISTS "${BG3MSE_NUGET}/${package}.nupkg")
            message(FATAL_ERROR "External/nuget/${package}.nupkg is missing: run Mac/setup.sh, which fetches it.")
        endif()
    endforeach()

    set(BG3MSE_DEBUG_ADAPTER_DIR "${CMAKE_BINARY_DIR}/DebugAdapter")
    set(work "${CMAKE_BINARY_DIR}/lua-debugger")
    set(project "${CMAKE_SOURCE_DIR}/Mac/LuaDebugger/LuaDebugger.csproj")
    file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/LuaDebugger/*.cs" "${CMAKE_SOURCE_DIR}/LuaDebugger/Properties/*.cs")

    # His project's pre-build step, with the output in the build directory.
    add_custom_command(OUTPUT "${work}/DbgProtocol.cs"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${work}"
        COMMAND "${PROTOC}" "--proto_path=${CMAKE_SOURCE_DIR}/LuaDebugger" "--csharp_out=${work}" DbgProtocol.proto
        DEPENDS "${CMAKE_SOURCE_DIR}/LuaDebugger/DbgProtocol.proto" "${PROTOC}"
        VERBATIM)

    # The SDK is kept to this directory and to itself: its home, its package
    # cache, no telemetry, no build server left behind, two jobs.
    add_custom_command(OUTPUT "${BG3MSE_DEBUG_ADAPTER_DIR}/LuaDebugger" "${BG3MSE_DEBUG_ADAPTER_DIR}/LuaDebugger.dll"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${BG3MSE_DEBUG_ADAPTER_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E env
            DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_GENERATE_ASPNET_CERTIFICATE=0 DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0
            "DOTNET_CLI_HOME=${work}/home" "NUGET_PACKAGES=${work}/packages"
            "${BG3MSE_DOTNET}" publish "${project}" --configuration Release --source "${BG3MSE_NUGET}"
            --output "${BG3MSE_DEBUG_ADAPTER_DIR}" --disable-build-servers -nodeReuse:false -m:2 --verbosity quiet
            "-p:DbgProtocolSource=${work}/DbgProtocol.cs"
            "-p:BaseIntermediateOutputPath=${work}/obj/" "-p:MSBuildProjectExtensionsPath=${work}/obj/" "-p:BaseOutputPath=${work}/bin/"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/Mac/LuaDebugger/LuaDebugger" "${BG3MSE_DEBUG_ADAPTER_DIR}/LuaDebugger"
        DEPENDS "${project}" ${sources} "${work}/DbgProtocol.cs" "${CMAKE_SOURCE_DIR}/Mac/LuaDebugger/LuaDebugger"
        VERBATIM)
    # What the adapter is, for the release (Mac/Release/Assemble.cmake).
    set(BG3MSE_DEBUG_ADAPTER_FILES LuaDebugger LuaDebugger.dll LuaDebugger.deps.json LuaDebugger.runtimeconfig.json
        Google.Protobuf.dll Newtonsoft.Json.dll)
    add_custom_target(bg3mse_lua_debug_adapter ALL DEPENDS "${BG3MSE_DEBUG_ADAPTER_DIR}/LuaDebugger")

    # The adapter and the Lua debugger together (Mac/Tests/StartupLuaDebugAdapter.cpp):
    # the start-up harness is the game, the test the editor, and the adapter
    # is run through its launcher, on the runtime beside the SDK's dotnet.
    if(TARGET bg3mse_startup_tests)
        get_filename_component(dotnet_root "${BG3MSE_DOTNET}" REALPATH)
        get_filename_component(dotnet_root "${dotnet_root}" DIRECTORY)
        add_test(NAME bg3mse_startup_lua_debug_adapter
            COMMAND bg3mse_startup_tests --lua-debug-adapter "${BG3MSE_DEBUG_ADAPTER_DIR}/LuaDebugger")
        set_tests_properties(bg3mse_startup_lua_debug_adapter PROPERTIES TIMEOUT 300
            ENVIRONMENT "DOTNET_ROOT=${dotnet_root};DOTNET_CLI_TELEMETRY_OPTOUT=1")
    endif()
endif()
