-- A real crash report from the build under test (audit 2, WP-40): the
-- client's Ext.Debug.Crash(2) writes through a null pointer inside Norbyte's
-- bg3se::lua::debug::Crash (Lua/Libs/Debug.inl), so the fault is in the
-- extender's own code with this file on the Lua stack. Opt-in ("Crash" in
-- the trigger's Extras) and only in the crash launch (Mac/Tests/InGame/
-- Settings/6-crash): in a release build Ext.Debug.Crash does nothing unless
-- developer mode is on and bg3mse.conf has debug_crash = 1
-- (Mac/Crash/ExtenderCrashReporting.cpp, DebugCrashAllowed).
--
-- THE GAME CLOSES. A window says so first and counts down; Skip on it ends
-- the case without a crash. The runner reports the case as passed at the
-- next launch (endsLaunch = "crash", Runner.lua); what the crash left, the
-- report, its backtrace and stack files, the log line naming it and the
-- next start's notice, is checked by `bg3mse-ingame crash-check` against
-- ~/Library/Application Support/bg3mse/Crashes and the logs.

local library = { name = "CrashReport" }

-- How long the owner has to read the window (or press Skip).
local COUNTDOWN_S = 15

function library.define(C)
    return {
        { "Ext.Debug.Crash#report", id = "SHIM-114", ctx = "client", risk = "unproved", async = true, exclusive = true,
          optIn = "Crash", endsLaunch = "crash",
          rows = "SHIM-114, SHIM-115, SHIM-118, SHIM-119, SHIM-142, SHIM-144, HOOK-76, FEAT-20, FEAT-21, G-041, BUILD-11, CFG-14",
          timeout = (COUNTDOWN_S + 30) * 1000, run = function (E, M, done)
            if not E.Debug.IsDeveloperMode() then
                return done(false, "developer mode is off: Ext.Debug.Crash would do nothing (DeveloperMode in ScriptExtenderSettings.json)")
            end
            local window = E.IMGUI.NewWindow("bg3mse in-game test: the game will close")
            window:AddText("This test crashes the game on purpose, to check the crash report.")
            local count = window:AddText("The game closes in " .. COUNTDOWN_S .. " seconds.")
            window:AddText("Nothing is saved. Start the game again afterwards: the next start tells of the crash.")
            local skipped = false
            local skip = window:AddButton("Skip this test")
            skip.OnClick = function ()
                skipped = true
                pcall(function () window:Destroy() end)
                done("skip", "the owner pressed Skip")
            end
            pcall(function () window:SetFocus() end)
            local left = COUNTDOWN_S
            local function Second()
                if skipped then return end
                left = left - 1
                if left > 0 then
                    count.Label = "The game closes in " .. left .. " seconds."
                    E.Timer.WaitForRealtime(1000, Second)
                    return
                end
                E.Utils.Print("bg3mse in-game tests: Ext.Debug.Crash(2), a write through a null pointer in bg3se::lua::debug::Crash")
                E.Debug.Crash(2)
                -- Still here: the gate said no.
                pcall(function () window:Destroy() end)
                done(false, "Ext.Debug.Crash(2) returned: bg3mse.conf has no debug_crash = 1, so nothing crashed")
            end
            E.Timer.WaitForRealtime(1000, Second)
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
