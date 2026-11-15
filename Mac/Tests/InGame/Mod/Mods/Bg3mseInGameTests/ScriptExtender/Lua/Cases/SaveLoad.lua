-- What survives a save and its load (audit 2, WP-35): a persistent user
-- variable on the host, a persistent mod variable, the mod's PersistentVars
-- and a persistent timer, written into a save the owner makes and loaded
-- back. Opt-in ("SaveLoad" in the trigger's Extras) and never part of a
-- normal run: the extender has no function that saves (Norbyte's Lua API
-- has none, and Osiris's AutoSave would rotate the owner's own autosaves),
-- so the client's window asks the owner to save under a name it gives and
-- to load that save at once.
--
-- What is written to disk: one save folder in the profile's
-- Savegames/Story, named by the game after the name typed (it ends in
-- "__bg3mse-igt-throwaway"), holding the session as it was plus this mod's
-- variables (Norbyte's savegame visit: ScriptExtenderServer.cpp,
-- esv::OsirisVariableHelper SavegameVisit). It lists this mod as used.
-- The owner deletes it from the game's Load screen afterwards, or the
-- coordinator removes the folder.
--
-- The run: the server's SaveLoad.Before sets the values and waits; the
-- client's SaveLoad.Prompt shows the window. Neither ends by itself: the
-- load ends both Lua states (a new session), the runner reports the two
-- cases as passed (endsLaunch = "load", Runner.lua) and goes on with the
-- After cases in the loaded session. Skip on the window ends both as skipped
-- (the client writes a file the server's case looks for).

local library = { name = "SaveLoad" }

local SAVE_NAME = "bg3mse-igt-throwaway"
local USER_VAR = "Bg3mseIgtSaved"
local MOD_VAR = "Bg3mseIgtSavedMod"
local TIMER_HANDLER = "Bg3mseIgtSavedTimer"
-- The persistent timer's delay in game time: long enough for the owner to
-- save and load, short enough for the After case to wait for it.
local TIMER_MS = 120000
local WAIT_MS = 20 * 60 * 1000
local timerFired = nil

function library.bootstrap(C, E, M)
    if not C.optedIn("SaveLoad") then return end
    -- Registered before the session loads, so that the load's savegame
    -- visit finds them (Vars.inl).
    local flags = { Server = true, Client = true, SyncToClient = true, Persistent = true }
    E.Vars.RegisterUserVariable(USER_VAR, flags)
    E.Vars.RegisterModVariable(C.ModuleUUID, MOD_VAR, flags)
    if E.IsServer() then
        -- Called with the timer's arguments and then its handle (Lua/Libs/Timer.inl, FireTimer).
        E.Timer.RegisterPersistentHandler(TIMER_HANDLER, function (args, handle)
            timerFired = { handle = handle, value = type(args) == "table" and args.Value or nil }
        end)
    end
end

function library.define(C)
    local SKIP_FILE = C.ScratchDir .. "saveload-skip.json"
    local EXPECT_FILE = C.ScratchDir .. "saveload-expect.json"
    local function Rows(detail, rows) return detail .. " [rows: " .. rows .. "]" end

    local function Read(E, path)
        local ok, text = pcall(E.IO.LoadFile, path)
        if not ok or type(text) ~= "string" or text == "" then return nil end
        local okJ, value = pcall(E.Json.Parse, text)
        return okJ and value or nil
    end

    local function Token()
        return "igt-" .. tostring(C.trigger and C.trigger.RunId or "?")
    end

    return {
        { "SaveLoad.Before", id = "LUA-080", ctx = "server", risk = "unproved", async = true, exclusive = true,
          optIn = "SaveLoad", endsLaunch = "load", rows = "FEAT-35", timeout = WAIT_MS, run = function (E, M, done)
            local character = C.character()
            pcall(E.IO.SaveFile, SKIP_FILE, "")
            local token = Token()
            character.Vars[USER_VAR] = token
            E.Vars.GetModVariables(C.ModuleUUID)[MOD_VAR] = 271828
            Mods[C.ModTable].PersistentVars = Mods[C.ModTable].PersistentVars or {}
            Mods[C.ModTable].PersistentVars.SaveLoad = token
            E.Vars.SyncUserVariables()
            E.Vars.SyncModVariables()
            E.Timer.WaitForPersistent(TIMER_MS, TIMER_HANDLER, { Value = token })
            E.IO.SaveFile(EXPECT_FILE, E.Json.Stringify({ Token = token, Mod = 271828 }))
            -- Ends only by the load, or by the owner's Skip.
            local sub
            sub = E.Events.Tick:Subscribe(function ()
                local skip = Read(E, SKIP_FILE)
                if type(skip) == "table" and skip.Skip == true then
                    E.Events.Tick:Unsubscribe(sub)
                    done("skip", "the owner pressed Skip on the save-and-load window")
                end
            end)
        end },
        { "SaveLoad.Prompt", id = "LUA-080", ctx = "client", risk = "unproved", async = true, exclusive = true,
          optIn = "SaveLoad", endsLaunch = "load", timeout = WAIT_MS, run = function (E, M, done)
            local window = E.IMGUI.NewWindow("bg3mse in-game test: save and load")
            for _, line in ipairs({
                "This test needs one save, made now and loaded at once:",
                "  1. Open the game's menu (Esc) and choose Save Game.",
                "  2. Make a NEW save named exactly:  " .. SAVE_NAME,
                "  3. Then, at once, Load Game and load that save " .. SAVE_NAME .. ".",
                "Do it within two minutes. The test goes on by itself after the load.",
                "Afterwards delete the save " .. SAVE_NAME .. " from the Load screen.",
            }) do window:AddText(line) end
            local skip = window:AddButton("Skip this test")
            skip.OnClick = function ()
                pcall(E.IO.SaveFile, SKIP_FILE, E.Json.Stringify({ Skip = true }))
                pcall(function () window:Destroy() end)
                done("skip", "the owner pressed Skip")
            end
            pcall(function () window:SetFocus() end)
        end },
        { "SaveLoad.After", id = "LUA-080", ctx = "server", risk = "unproved", async = true, optIn = "SaveLoad",
          timeout = TIMER_MS + 60000, run = function (E, M, done)
            local expect = Read(E, EXPECT_FILE)
            if type(expect) ~= "table" then C.skip("SaveLoad.Before set nothing in this run") end
            local skip = Read(E, SKIP_FILE)
            if type(skip) == "table" and skip.Skip == true then C.skip("the owner skipped the save and load") end
            local character = C.character()
            C.equal(character.Vars[USER_VAR], expect.Token, "the persistent user variable after the load")
            C.equal(E.Vars.GetModVariables(C.ModuleUUID)[MOD_VAR], expect.Mod, "the persistent mod variable after the load")
            local pv = Mods[C.ModTable].PersistentVars
            C.equal(type(pv) == "table" and pv.SaveLoad or nil, expect.Token, "PersistentVars.SaveLoad after the load")
            -- The persistent timer, saved with time left, fires in the loaded session.
            local started = E.Timer.MonotonicTime()
            local sub
            sub = E.Events.Tick:Subscribe(function ()
                if timerFired ~= nil then
                    E.Events.Tick:Unsubscribe(sub)
                    if timerFired.value ~= expect.Token then
                        return done(false, "the persistent timer fired with " .. tostring(timerFired.value))
                    end
                    done(true, Rows(string.format("user variable, mod variable and PersistentVars read back after the load; the persistent timer fired %d ms into the loaded session",
                        E.Timer.MonotonicTime() - started), "LUA-081, LUA-082, LUA-079, EVT-09, G-046"))
                end
            end)
        end },
        { "SaveLoad.After#client", id = "LUA-080", ctx = "client", risk = "unproved", async = true, optIn = "SaveLoad",
          run = function (E, M, done)
            local expect = Read(E, EXPECT_FILE)
            if type(expect) ~= "table" then C.skip("SaveLoad.Before set nothing in this run") end
            local skip = Read(E, SKIP_FILE)
            if type(skip) == "table" and skip.Skip == true then C.skip("the owner skipped the save and load") end
            local character = C.character()
            local n, sub = 0, nil
            sub = E.Events.Tick:Subscribe(function ()
                n = n + 1
                if character.Vars[USER_VAR] == expect.Token then
                    E.Events.Tick:Unsubscribe(sub)
                    done(true, Rows("the persistent user variable synced to the client after the load", "NET-13"))
                elseif n > 300 then
                    E.Events.Tick:Unsubscribe(sub)
                    done(false, "the client reads " .. tostring(character.Vars[USER_VAR]))
                end
            end)
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
