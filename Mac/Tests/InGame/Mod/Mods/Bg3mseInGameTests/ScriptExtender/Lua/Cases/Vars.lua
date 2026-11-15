-- Ext.Vars (BG3Extender/Lua/Libs/Vars.inl): variables registered here are
-- not persistent (Persistent = false), so nothing reaches the save, and each
-- value set is cleared again. Saving and loading them is WP-17.

local library = { name = "Vars" }

function library.define(C)
    local USER = "Bg3mseInGameTestsUser"
    local MOD = "Bg3mseInGameTestsMod"
    local function UserFlags(E)
        if E.IsServer() then
            return { Server = true, Client = false, Persistent = false, SyncOnTick = false }
        end
        return { Server = false, Client = true, WriteableOnServer = false, WriteableOnClient = true, Persistent = false, SyncOnTick = false }
    end
    local function Register(E)
        E.Vars.RegisterUserVariable(USER, UserFlags(E))
    end
    local function RegisterMod(E)
        E.Vars.RegisterModVariable(C.ModuleUUID, MOD, UserFlags(E))
    end
    return {
        { "Ext.Vars.RegisterUserVariable", id = "LUA-080", run = function (E)
            Register(E)
            local character = C.character()
            character.Vars[USER] = 11
            local back = character.Vars[USER]
            character.Vars[USER] = nil
            C.equal(back, 11, "the variable read back")
        end },
        { "Ext.Vars.GetEntitiesWithVariable", id = "LUA-080", run = function (E)
            Register(E)
            local character, guid = C.character()
            character.Vars[USER] = 12
            -- A write stays in the Lua state's cache until a flush (end of
            -- tick, Sync, Dirty); GetEntitiesWithVariable reads only what
            -- was flushed (Vars.inl, UserVariables.inl), on Windows too.
            E.Vars.SyncUserVariables()
            local list = E.Vars.GetEntitiesWithVariable(USER)
            character.Vars[USER] = nil
            E.Vars.SyncUserVariables()
            local found = false
            for _, g in ipairs(list) do if g == guid then found = true end end
            C.check(found, "the character is not listed: %s", E.Json.Stringify(list, { Beautify = false }))
        end },
        { "Ext.Vars.SyncUserVariables", id = "LUA-080", run = function (E)
            E.Vars.SyncUserVariables()
        end },
        { "Ext.Vars.DirtyUserVariables", id = "LUA-080", run = function (E)
            local _, guid = C.character()
            E.Vars.DirtyUserVariables(guid, USER)
            E.Vars.DirtyUserVariables(guid)
        end },
        { "Ext.Vars.RegisterModVariable", id = "LUA-081", run = function (E)
            RegisterMod(E)
        end },
        { "Ext.Vars.GetModVariables", id = "LUA-081", run = function (E)
            RegisterMod(E)
            local vars = C.notNil(E.Vars.GetModVariables(C.ModuleUUID), "GetModVariables")
            vars[MOD] = 13
            local back = vars[MOD]
            vars[MOD] = nil
            C.equal(back, 13, "the mod variable read back")
        end },
        { "Ext.Vars.SyncModVariables", id = "LUA-081", run = function (E)
            E.Vars.SyncModVariables()
        end },
        { "Ext.Vars.DirtyModVariables", id = "LUA-081", run = function (E)
            E.Vars.DirtyModVariables(C.ModuleUUID, MOD)
            E.Vars.DirtyModVariables(C.ModuleUUID)
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
