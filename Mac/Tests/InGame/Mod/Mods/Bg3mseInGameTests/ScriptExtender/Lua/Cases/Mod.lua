-- Ext.Mod (BG3Extender/Lua/Libs/Mod.inl).

local library = { name = "Mod" }

function library.define(C)
    return {
        { "Ext.Mod.IsModLoaded", id = "LUA-033", run = function (E)
            C.equal(E.Mod.IsModLoaded(C.GustavUUID), true, "the base module")
            C.equal(E.Mod.IsModLoaded(C.ModuleUUID), true, "this mod")
            C.equal(E.Mod.IsModLoaded("01010101-0202-0303-0404-050505050505"), false, "a module that does not exist")
        end },
        { "Ext.Mod.GetLoadOrder", id = "LUA-033", run = function (E)
            local order = E.Mod.GetLoadOrder()
            local found = false
            for _, uuid in ipairs(order) do
                if uuid == C.ModuleUUID then found = true end
            end
            C.check(found, "this mod is not in the load order")
            return #order .. " modules"
        end },
        { "Ext.Mod.GetMod", id = "LUA-032", run = function (E, M)
            local mod = C.notNil(E.Mod.GetMod(C.ModuleUUID), "GetMod(this mod)")
            C.equal(mod.Info.Directory, C.ModuleFolder, "this mod's folder")
            C.equal(mod.Info.Name, "bg3mse in-game tests", "this mod's name")
            -- The mod's environment (ModLoader.lua:34-67, LoadBootstrap
            -- :70-79): Mods[<ModTable>] is it, sandboxed, with this mod's
            -- UUID and its own Ext, which takes no new key (:24-30: an error
            -- line, "Couldn't set Ext.<key>", and the key stays unset).
            local env = C.notNil(Mods[C.ModTable], "Mods." .. C.ModTable)
            C.equal(rawget(env, "Sandboxed"), true, "Sandboxed")
            C.equal(rawget(env, "ModuleUUID"), C.ModuleUUID, "ModuleUUID")
            C.check(rawequal(rawget(env, "_G"), env), "_G is not the environment itself")
            C.check(rawequal(rawget(env, "Ext"), M), "the environment's Ext is not the mod's own")
            C.check(not rawequal(M, E), "the mod's Ext is the real one")
            local KEY = "Bg3mseIgtNewKey"
            M[KEY] = 1
            C.equal(rawget(M, KEY), nil, "the mod's Ext." .. KEY .. " after setting it")
            C.equal(M[KEY], nil, "Ext." .. KEY .. " read through the mod's Ext")
            return "sandboxed, own Ext; Ext." .. KEY .. " refused (the error line before this one is expected) [rows: W1-06]"
        end },
        { "Ext.Mod.GetBaseMod", id = "LUA-033", run = function (E)
            C.equal(E.Mod.GetBaseMod().Info.ModuleUUIDString, C.GustavUUID, "the base module's UUID")
        end },
        { "Ext.Mod.GetModManager", id = "LUA-033", run = function (E)
            local manager = E.Mod.GetModManager()
            C.equal(manager.BaseModule.Info.ModuleUUIDString, C.GustavUUID, "the manager's base module")
            C.check(#manager.LoadOrderedModules > 0, "no modules in the load order")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
