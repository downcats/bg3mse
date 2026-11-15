-- Ext.Debug (BG3Extender/Lua/Libs/Debug.inl).

local library = { name = "Debug" }

function library.define(C)
    return {
        { "Ext.Debug.IsDeveloperMode", id = "LUA-036", risk = "pure", offline = true, run = function (E)
            return tostring(C.type(E.Debug.IsDeveloperMode(), "boolean"))
        end },
        { "Ext.Debug.DumpStack", id = "LUA-037", risk = "pure", offline = true, run = function (E)
            E.Debug.DumpStack()
        end },
        { "Ext.Debug.DebugDumpLifetimes", id = "LUA-037", risk = "pure", offline = true, run = function (E)
            E.Debug.DebugDumpLifetimes()
        end },
        { "Ext.Debug.SetEntityRuntimeCheckLevel", id = "LUA-037", risk = "pure", offline = true, run = function (E)
            -- 1 = RuntimeCheckLevel::Once, the release build's default
            -- (GameDefinitions/EntitySystem.cpp): the level is left as it was.
            E.Debug.SetEntityRuntimeCheckLevel(1)
        end },
        { "Ext.Debug.DebugBreak", id = "LUA-038", run = function (E)
            -- Returns at once when no Lua debugger is attached.
            E.Debug.DebugBreak()
        end },
        { "Ext.Debug.Crash", id = "LUA-039", risk = "pure", offline = true, run = function (E)
            -- A type outside 0..2 does nothing in any build (Debug.inl).
            E.Debug.Crash(99)
        end },
        { "Ext.Debug.GenerateIdeHelpers", id = "LUA-013", ctx = "server", risk = "unproved", optIn = "IdeHelpers",
          run = function (E)
            -- Norbyte's writes Mods/<base module>/Story/RawFiles/Lua/
            -- OsiIdeHelpers.lua under the game's Data folder (Debug.inl),
            -- which on macOS is inside the app bundle; the macOS build
            -- writes the same path under the Script Extender folder, the
            -- root of Ext.IO's files, and returns nothing, as Norbyte's.
            -- Opt-in ("Extras": ["IdeHelpers"]): a build without that change
            -- writes into the bundle. It needs developer mode and the
            -- server's Osiris. The file is emptied first, so that what is
            -- read back is this call's.
            C.check(E.Debug.IsDeveloperMode(), "needs developer mode")
            local path = "Mods/" .. E.Mod.GetBaseMod().Info.Directory .. "/Story/RawFiles/Lua/OsiIdeHelpers.lua"
            pcall(E.IO.SaveFile, path, "")
            E.Debug.GenerateIdeHelpers(true)
            local text = E.IO.LoadFile(path)
            C.check(type(text) == "string" and #text > 0,
                "nothing at %s in the Script Extender folder (a build that writes into the game's Data folder?)", path)
            -- Compiled, not run (Utils.inl, LoadString: luaL_loadbufferx).
            local okC, chunk = pcall(E.Utils.LoadString, text)
            C.check(okC and type(chunk) == "function", "the file at %s does not compile as Lua: %s", path, tostring(chunk))
            C.check(text:find("Osi.", 1, true) ~= nil, "the file at %s has no Osi. helpers", path)
            return path .. " in the Script Extender folder, " .. #text .. " bytes"
        end },
        { "Ext.Debug.GenerateIdeHelpers#client", id = "LUA-013", ctx = "client", risk = "pure", offline = true, run = function (E)
            -- Registered on the client too; without the server's Osiris it
            -- raises before it writes anything, and outside developer mode
            -- it only logs (Debug.inl).
            if E.Debug.IsDeveloperMode() then
                C.raises(E.Debug.GenerateIdeHelpers, true)
                return "raised: no Osiris on the client"
            end
            E.Debug.GenerateIdeHelpers(true)
            return "not developer mode: logged and returned"
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
