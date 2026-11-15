-- The story tests (Cases/Story.lua) with the in-game tests' runner, which
-- the build copies in beside this file (CMakeLists.txt, bg3mse_story_pak).
local Runner = Ext.Require("Runner.lua")
Runner.Configure({
    ModuleUUID = "5a2aab3d-7758-42d4-8f95-0dc5181e75f8",
    ModuleFolder = "Bg3mseStoryTests",
    ModTable = "Bg3mseStoryTests",
    Trigger = "bg3mse-story-tests.json",
    ProgressDir = "bg3mse-story-tests/",
    Command = "bg3mse_story_tests",
    Norbyte = false,
    Coverage = false,
})
Runner.AddLibrary(Ext.Require("Cases/Story.lua"))
Runner.Bootstrap(Ext)
