-- The functions of Ext itself, from the built-in scripts
-- (BG3Extender/LuaScripts/BuiltinLibrary*.lua, Startup.lua, Libs/Event.lua,
-- Libs/Events/EventManager.lua), and Ext.Internal (Lua/Libs/Internal.inl),
-- which Startup.lua moves to Ext._Internal.EngineInternals.

local library = { name = "Root" }

function library.define(C)
    return {
        { "Ext.IsClient", id = "LUA-007", risk = "pure", offline = true, run = function (E)
            C.type(E.IsClient(), "boolean")
            C.equal(E.IsClient(), not E.IsServer(), "IsClient")
        end },
        { "Ext.IsServer", id = "LUA-007", risk = "pure", offline = true, run = function (E)
            C.type(E.IsServer(), "boolean")
        end },
        { "Ext.DumpExport", id = "LUA-007", risk = "pure", offline = true, run = function (E)
            local text = E.DumpExport({ a = 1 })
            C.check(type(text) == "string" and text:find('"a"') ~= nil, "DumpExport gave %s", tostring(text))
        end },
        { "Ext.Dump", id = "LUA-007", risk = "pure", offline = true, run = function (E)
            E.Dump({ bg3mse = "Ext.Dump probe" })
        end },
        { "Ext.DumpShallow", id = "LUA-007", risk = "pure", offline = true, run = function (E)
            E.DumpShallow({ bg3mse = "Ext.DumpShallow probe", nested = { 1, 2, 3, 4, 5 } })
        end },
        { "Ext.OnNextTick", id = "LUA-007", risk = "pure", async = true, run = function (E, M, done)
            E.OnNextTick(function () done(true) end)
        end },
        { "Ext.Require", id = "LUA-005", run = function (E)
            C.equal(E.Require(C.ModuleUUID, "Probe.lua"), "bg3mse-ingame-probe", "Ext.Require")
        end },
        { "Ext.CoreLib", id = "LUA-007", risk = "pure", offline = true, run = function (E)
            C.type(E.CoreLib("Class"), "table")
        end },
        { "Ext.RegisterModEvent", id = "EV-29", run = function (E)
            -- Registration is allowed only while the mod's bootstrap runs; the
            -- bootstrap registered "Probe" when the trigger asked for a run.
            -- Mod events are keyed by the mod table (ModEventManager.lua).
            if not C.modEventRegistered then C.skip("the bootstrap did not register the mod event (no trigger file)") end
            local events = E.ModEvents[C.ModTable]
            local seen = nil
            local event = events.Probe
            local id = event:Subscribe(function (e) seen = e.Value end)
            event:Throw({ Value = 42 })
            event:Unsubscribe(id)
            C.equal(seen, 42, "the mod event's payload")
        end },
        { "Ext.RegisterConsoleCommand", id = "EV-12", run = function (E)
            local got = nil
            E.RegisterConsoleCommand("bg3mse_ingame_probe", function (cmd, arg) got = arg end)
            -- What the console hands a typed line to (EventManager:DoConsoleCommand).
            E._Internal.DoConsoleCommand("bg3mse_ingame_probe 7")
            C.equal(got, "7", "the command's argument")
        end },
        { "Ext.Events.DoConsoleCommand#typed", id = "EV-12", async = true, timeout = 180000, run = function (E, M, done)
            -- The owner types a command in the extender's console window
            -- (CreateConsole; the overlay shows it, F6 hides and shows it):
            -- "!bg3mse_igt" reaches DoConsoleCommand in the context the
            -- console is in, the server's first ("client" switches to the
            -- client's). Only when the trigger file asks for it (Console: true).
            if not (C.trigger and C.trigger.Console == true) then
                C.skip("the trigger file does not ask for the console case (\"Console\": true)")
            end
            local id
            id = E.Events.DoConsoleCommand:Subscribe(function (e)
                local command = tostring(e.Command or "")
                if command:match("^bg3mse_igt") then
                    E.Events.DoConsoleCommand:Unsubscribe(id)
                    done(true, "typed: !" .. command)
                end
            end)
            local context = E.IsServer() and "server" or "client"
            local how = E.IsServer() and "press Enter, type  !bg3mse_igt  and press Enter"
                or "type  client  and Enter, then  !bg3mse_igt  and Enter"
            E.Utils.Print("bg3mse in-game test (" .. context .. "): in the console window, " .. how .. " (within 3 minutes)")
            E.Timer.WaitForRealtime(170000, function ()
                E.Events.DoConsoleCommand:Unsubscribe(id)
                done("skip", "nobody typed !bg3mse_igt in the " .. context .. " console within 3 minutes")
            end)
        end },
        { "Ext.RegisterNetListener", id = "NET-18", run = function (E)
            -- Registration only; delivery is the Net cases' round trip.
            E.RegisterNetListener("Bg3mseInGameTestsLegacy", function () end)
        end },
        { "Ext.Internal.GetPublishedEvents", id = "LUA-006", risk = "pure", offline = true, run = function (E)
            local events = E._Internal.EngineInternals.GetPublishedEvents()
            C.check(events.Tick ~= nil, "Tick is not among the published events")
        end },
        { "Ext.Internal.ActivateEvent", id = "LUA-006", risk = "pure", offline = true, run = function (E)
            -- Tick is active already (the runner subscribes to it): no change.
            local id = E._Internal.EngineInternals.GetPublishedEvents().Tick
            E._Internal.EngineInternals.ActivateEvent(id)
        end },
        { "Ext.Internal.DeactivateEvent", id = "LUA-006", run = function (E)
            -- On an engine event whose every subscriber in this Lua state is
            -- the runner's (no other mod would miss it), and only between two
            -- statements: it is activated again at once, on the same thread,
            -- so the engine cannot throw it in between (the hints are this
            -- context's own, LuaBinding.h ShouldThrowEvent). An out-of-range
            -- id raises (Internal.inl).
            local internals = E._Internal.EngineInternals
            local published = internals.GetPublishedEvents()
            local names = {}
            for name in pairs(published) do names[#names + 1] = name end
            table.sort(names)
            local chosen = nil
            for _, name in ipairs(names) do
                local event = rawget(E.Events, name)
                local node = event ~= nil and rawget(event, "First") or nil
                local count, own = 0, true
                while node ~= nil do
                    count = count + 1
                    if not C.ownHandlers[node.Handler] then own = false end
                    node = node.Next
                end
                if count > 0 and own and name ~= "Tick" then chosen = name break end
            end
            if chosen == nil then C.skip("no engine event has the runner's subscribers alone in this context") end
            internals.DeactivateEvent(published[chosen])
            internals.ActivateEvent(published[chosen])
            C.raises(internals.DeactivateEvent, 0x7fffffff)
            return chosen .. " (id " .. tostring(published[chosen]) .. "), deactivated and activated again"
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
