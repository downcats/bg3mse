-- The settings of ScriptExtenderSettings.json that show in Lua (audit 2,
-- WP-41), each read back against the value the launch's settings files set.
-- The settings launches (Mac/Tests/InGame/Settings/<n>-<name>) put the same
-- keys and values in the trigger file's Settings.Values; a run without them
-- skips these cases. What no Lua call shows is read from the logs by
-- `bg3mse-ingame settings`.
--
-- Ext.Config is Norbyte's (Lua/Libs/LuaSharedLibs.cpp, RegisterConfig):
-- ProfilerEnabled is Optick, PerfMessagesEnabled EnablePerfMessages, the
-- thresholds the Error of each ProfilerThreshold (ExtenderConfig.inl reads
-- "<name>Error"), the server's ProfilerCallbackThreshold and the client's
-- ProfilerClientCallbackThreshold under one name.

local library = { name = "Settings" }

function library.define(C)
    -- The launch's value of `key`, or a skip when this run sets none.
    local function Value(key)
        local settings = C.trigger and C.trigger.Settings
        if type(settings) ~= "table" or type(settings.Values) ~= "table" then
            C.skip("not a settings launch (the trigger names no Settings)")
        end
        local v = settings.Values[key]
        if v == nil then C.skip("this settings launch (" .. tostring(settings.Name) .. ") does not set " .. key) end
        return v
    end

    local function Config(E, field)
        local config = C.notNil(E.Config, "Ext.Config")
        return config[field]
    end

    return {
        { "Settings.DeveloperMode", id = "CFG-24", risk = "pure", run = function (E)
            local want = Value("DeveloperMode")
            C.equal(E.Debug.IsDeveloperMode(), want, "Ext.Debug.IsDeveloperMode()")
            return "Ext.Debug.IsDeveloperMode() is " .. tostring(want) .. ", as the settings file sets DeveloperMode [rows: CFG-24, FEAT-08]"
        end },
        { "Settings.DisableStoryPatching", id = "CFG-18", ctx = "server", risk = "pure", run = function (E)
            local want = Value("DisableStoryPatching")
            if want ~= true then C.skip("only true is the extender's doing (DataLibraries.cpp sets the game's switch)") end
            local switches = C.notNil(E.Utils.GetGlobalSwitches(), "Ext.Utils.GetGlobalSwitches()")
            C.equal(switches.DisableStoryPatching, true, "GlobalSwitches.DisableStoryPatching")
            return "the game's GlobalSwitches.DisableStoryPatching is true, set at start-up (DataLibraries.cpp) [rows: CFG-18]"
        end },
        { "Settings.Optick", id = "CFG-22", risk = "pure", run = function (E)
            local want = Value("Optick")
            C.equal(Config(E, "ProfilerEnabled"), want, "Ext.Config.ProfilerEnabled")
            return "Ext.Config.ProfilerEnabled is " .. tostring(want) .. " (the Optick server itself is compiled out of Release) [rows: CFG-22]"
        end },
        { "Settings.LuaBuiltinResourceDirectory", id = "CFG-27", risk = "pure", run = function (E)
            Value("LuaBuiltinResourceDirectory")
            -- The launch's copy of ServerStartup.lua / ClientStartup.lua sets it
            -- (Settings/<launch>/LuaScripts); Norbyte's own files do not.
            local want = E.IsServer() and "server" or "client"
            C.equal(Bg3mseIgtBuiltinDirectory, want, "Bg3mseIgtBuiltinDirectory, set by the directory's startup script")
            return "the " .. want .. " ran its startup script from the directory, not from the bundle in the dylib [rows: CFG-27, G-142]"
        end },
        { "Settings.EnablePerfMessages", id = "CFG-29", risk = "pure", run = function (E)
            local want = Value("EnablePerfMessages")
            C.equal(Config(E, "PerfMessagesEnabled"), want, "Ext.Config.PerfMessagesEnabled")
            return "Ext.Config.PerfMessagesEnabled is " .. tostring(want) .. " [rows: CFG-29]"
        end },
        { "Settings.ProfilerLoadCallbackThresholdError", id = "CFG-31", risk = "pure", run = function (E)
            local want = Value("ProfilerLoadCallbackThresholdError")
            C.equal(Config(E, "ProfilerLoadCallbackErrorThreshold"), want, "Ext.Config.ProfilerLoadCallbackErrorThreshold")
            return "Ext.Config.ProfilerLoadCallbackErrorThreshold is " .. tostring(want) .. " [rows: CFG-31]"
        end },
        { "Settings.ProfilerCallbackThresholdError", id = "CFG-32", ctx = "server", risk = "pure", run = function (E)
            local want = Value("ProfilerCallbackThresholdError")
            C.equal(Config(E, "ProfilerCallbackErrorThreshold"), want, "the server's Ext.Config.ProfilerCallbackErrorThreshold")
            return "the server's Ext.Config.ProfilerCallbackErrorThreshold is " .. tostring(want) .. " [rows: CFG-32]"
        end },
        { "Settings.ProfilerClientCallbackThresholdError", id = "CFG-33", ctx = "client", risk = "pure", run = function (E)
            local want = Value("ProfilerClientCallbackThresholdError")
            C.equal(Config(E, "ProfilerCallbackErrorThreshold"), want, "the client's Ext.Config.ProfilerCallbackErrorThreshold")
            return "the client's Ext.Config.ProfilerCallbackErrorThreshold is " .. tostring(want) .. " [rows: CFG-33]"
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
