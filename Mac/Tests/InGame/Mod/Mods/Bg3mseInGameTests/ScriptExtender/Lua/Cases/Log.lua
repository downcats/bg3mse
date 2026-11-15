-- Ext.Log (BG3Extender/Lua/Libs/Log.inl).

local library = { name = "Log" }

function library.define(C)
    return {
        { "Ext.Log.Print", id = "LUA-030", risk = "pure", offline = true, run = function (E)
            E.Log.Print("bg3mse in-game test: Ext.Log.Print", 1, true, nil)
        end },
        { "Ext.Log.PrintWarning", id = "LUA-030", risk = "pure", offline = true, run = function (E)
            E.Log.PrintWarning("bg3mse in-game test: Ext.Log.PrintWarning (expected)")
        end },
        -- An error line fails the offline harness, so this one runs in the game only.
        { "Ext.Log.PrintError", id = "LUA-031", risk = "pure", run = function (E)
            E.Log.PrintError("bg3mse in-game test: Ext.Log.PrintError (expected)")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
