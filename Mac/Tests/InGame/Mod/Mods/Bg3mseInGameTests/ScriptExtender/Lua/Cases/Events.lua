-- The engine events (BG3Extender/GameDefinitions/Generated/Events.inl, from
-- make_events.py) and the Lua-side Log event (BuiltinLibrary.lua). The runner
-- subscribes to each at bootstrap and reports at the end of the run: pass when
-- the event reached the handler, otherwise a skip saying what would provoke it.
-- Events the run provokes itself: Tick, NetMessage and NetModMessage (the Net
-- cases), Log (below), the functor and damage events (the Hooks cases), and
-- those of a Lua reset (phase = "reset": reported by the state the run's
-- reset makes; Shutdown, beforeReset, fires in the state that ends and is
-- written down by it). FindPath is the game's own path search
-- (eoc::AiGrid::FindPath, which Ext.Level's requests do not go through):
-- a character's movement calls it, the client's only when the player moves one.

local library = { name = "Events" }

function library.define(C)
    local function ev(name, id, extra)
        local c = { "Ext.Events." .. name, id = id }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    local function Fields(...)
        local names = { ... }
        return function (e)
            local parts = {}
            for _, n in ipairs(names) do parts[#parts + 1] = n .. "=" .. tostring(e[n]) end
            return table.concat(parts, " ")
        end
    end
    return {
        ev("SessionLoading", "EV-01", { needs = "a session load after the bootstrap" }),
        ev("SessionLoaded", "EV-02", { needs = "a session load after the bootstrap" }),
        -- Thrown only in the client's states, around the stats load
        -- (ScriptExtender::OnStatsLoadGuarded): the first two in the module
        -- load's own state, which BootstrapModule.lua records them from.
        ev("ModuleLoadStarted", "EV-03", { ctx = "client", needs = "the game's stats load (at start-up, or a save with another mod list)" }),
        ev("StatsLoaded", "EV-04", { ctx = "client", needs = "the game's stats load (at start-up, or a save with another mod list)" }),
        ev("StatsStructureLoaded", "EV-05", { ctx = "client", needs = "the game's stats load (at start-up, or a save with another mod list)" }),
        ev("ModuleLoadStarted", "EV-03", { ctx = "server", skip = "thrown only in the client's states (ScriptExtender::OnStatsLoadGuarded)" }),
        ev("StatsLoaded", "EV-04", { ctx = "server", skip = "thrown only in the client's states (ScriptExtender::OnStatsLoadGuarded)" }),
        ev("StatsStructureLoaded", "EV-05", { ctx = "server", skip = "thrown in the state current while the stats are parsed, the client's (Hooks::OnParseDataBuffers)" }),
        ev("ModuleResume", "EV-06", { phase = "reset", needs = "the run's Lua reset" }),
        ev("ResetCompleted", "EV-07", { phase = "reset", needs = "the run's Lua reset" }),
        ev("Shutdown", "EV-08", { phase = "reset", beforeReset = true, needs = "the end of the Lua state the run resets" }),
        ev("Tick", "EV-09", { check = function (e) return "Time " .. tostring(e.Time and e.Time.DeltaTime) end }),
        ev("GameStateChanged", "EV-10", { ctx = "server", check = Fields("FromState", "ToState") }),
        ev("GameStateChanged", "EV-11", { ctx = "client", check = Fields("FromState", "ToState") }),
        ev("DoConsoleCommand", "EV-12", { needs = "a command typed in the extender console (the overlay's console window: Ext.Events.DoConsoleCommand#typed)" }),
        ev("KeyInput", "EV-13", { ctx = "client", needs = "a key pressed while the run goes", check = Fields("Key", "Event", "Modifiers") }),
        ev("MouseButtonInput", "EV-14", { ctx = "client", needs = "a mouse click while the run goes", check = Fields("Button", "Pressed", "Clicks") }),
        ev("MouseWheelInput", "EV-15", { ctx = "client", needs = "the scroll wheel turned while the run goes", check = Fields("ScrollX", "ScrollY") }),
        ev("ControllerAxisInput", "EV-16", { ctx = "client", needs = "a game controller: a stick or trigger moved while the run goes", check = Fields("Axis", "Value", "DeviceId") }),
        ev("ControllerButtonInput", "EV-17", { ctx = "client", needs = "a game controller: a button pressed while the run goes", check = Fields("Button", "Pressed", "DeviceId") }),
        ev("ViewportResized", "EV-18", { ctx = "client", needs = "the game window resized while the run goes", check = Fields("Width", "Height") }),
        ev("NetMessage", "EV-19", { needs = "a legacy net message (the Net cases send them)", check = Fields("Channel", "UserID") }),
        ev("NetModMessage", "EV-20", { needs = "a channel message (the Net cases send them)", check = Fields("Channel", "Module") }),
        ev("FindPath", "EV-21", { needs = "a character moved by the game's own path search (on the client, by the player)",
            -- The path searched (FindPathEventParams, SharedEvents.h:29-32;
            -- AiPath, GameDefinitions/Ai.h:404-500), reported: its moving
            -- bound, where it starts and how far that is from its source
            -- entity. The row is named only for a bound a character could have
            -- and a start within 10 m of the source (the game's values).
            check = function (e)
                local path = C.notNil(e.Path, "Path")
                local function Finite(v, what)
                    C.check(type(v) == "number" and v == v and math.abs(v) < 1e7, "%s is %s", what, tostring(v))
                    return v
                end
                local bound = Finite(path.MovingBound, "MovingBound")
                local from = path.SourceOriginal
                for i = 1, 3 do Finite(from[i], "SourceOriginal[" .. i .. "]") end
                local d = nil
                local ok, transform = pcall(function () return Ext.Entity.Get(path.Source).Transform.Transform.Translate end)
                if ok and transform ~= nil then
                    d = math.sqrt((from[1] - transform[1]) ^ 2 + (from[3] - transform[3]) ^ 2)
                end
                local plausible = bound > 0 and bound < 50 and (d == nil or d < 10)
                return string.format("MovingBound %.2f, from %.1f,%.1f,%.1f%s, GoalFound %s%s", bound, from[1], from[2], from[3],
                    d and string.format(", %.2f m from its source entity", d) or ", no source entity",
                    tostring(path.GoalFound), plausible and " [rows: EV-21]" or "")
            end }),
        ev("ExecuteFunctor", "EV-22", { ctx = "server", needs = "stats functors run (the Ext.Stats.ExecuteFunctor cases in Hooks)", check = Fields("Params") }),
        ev("AfterExecuteFunctor", "EV-23", { ctx = "server", needs = "stats functors run (the Ext.Stats.ExecuteFunctor cases in Hooks)", check = Fields("Params") }),
        ev("BeforeDealDamage", "EV-24", { ctx = "server", needs = "damage dealt (the damage cases in Hooks)" }),
        ev("DealDamage", "EV-25", { ctx = "server", needs = "damage dealt (the damage cases in Hooks)" }),
        ev("DealtDamage", "EV-26", { ctx = "server", needs = "damage dealt (the damage cases in Hooks)" }),
        ev("Log", "EV-27", {
            needs = "a mod's Ext.Log call",
            provoke = function (E, M)
                -- The mod's own Ext.Log goes through Logger:Log, which throws the event.
                M.Log.Print("bg3mse in-game test: the Log event")
            end,
            check = Fields("Level", "Module") }),
    }
end

if Bg3mseIgtOfflineEvents then Bg3mseIgtOfflineEvents(library) end
return library
