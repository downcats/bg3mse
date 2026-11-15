-- What Norbyte's builtin Lua sets up in every state (audit 2, WP-35 rows
-- W1-02, W1-03, W1-07, W1-09): the global helpers (BuiltinLibrary.lua), the
-- sandbox's replacements (SandboxStartup.lua), the per-mod Ext.Log
-- (Libs/Logger.lua, installed per mod by Libs/ModLoader.lua), and the
-- subscription options and event-object methods of Ext.Events
-- (Libs/Events/SubscribableEvent.lua, Lua/Shared/Proxies/LuaEvent.inl).
-- All but the last run in the start-up harness's real Lua states too.

local library = { name = "LuaEnv" }

-- The state's own globals: a case file runs in its mod's environment, whose
-- __index is _G (ModLoader:CreateModEnv); elsewhere the environment is _G.
local MOD_ENV = _ENV
local function Globals()
    local mt = getmetatable(MOD_ENV)
    local g = type(mt) == "table" and type(mt.__index) == "table" and mt.__index or MOD_ENV
    return g
end

function library.define(C)
    local function Rows(detail, rows) return detail .. " [rows: " .. rows .. "]" end

    return {
        { "Globals#helpers", id = "W1-07", risk = "pure", offline = true, run = function (E)
            local G = Globals()
            local same = {
                { "_D", E.Dump }, { "_DS", E.DumpShallow }, { "_P", E.Log.Print }, { "_PW", E.Log.PrintWarning },
                { "_PE", E.Log.PrintError }, { "Print", E.Log.Print }, { "print", E.Log.Print },
            }
            for _, pair in ipairs(same) do
                C.check(rawget(G, pair[1]) == pair[2], "the global %s is not Ext's own function", pair[1])
            end
            C.check(G.table.find == E.Table.Find, "table.find is not Ext.Table.Find")
            C.equal(G.table.find({ "a", "b", "c" }, "b"), 2, "table.find({'a','b','c'}, 'b')")
            C.equal(G.table.find({ "a" }, "z"), nil, "table.find of a value not in the table")
            C.type(rawget(G, "require"), "function", "require")
            -- require keeps a name that ends in .lua and adds .lua to one that
            -- does not (BuiltinLibrary.lua); a builtin path needs no mod
            -- (FileLoader.lua), and the loader caches what it loaded.
            local class = E.CoreLib("Class")
            C.check(G.require("builtin://Libs/Class.lua") == class, "require of builtin://Libs/Class.lua is not Ext.CoreLib('Class')")
            C.check(G.require("builtin://Libs/Class") == class, "require of builtin://Libs/Class is not Ext.CoreLib('Class')")
            local helpers = "_D, _DS, _P, _PW, _PE, Print, print, table.find, require"
            -- A mod's environment carries its own printers (ModLoader:CreateModEnv).
            if rawget(MOD_ENV, "Sandboxed") == true then
                C.check(rawget(MOD_ENV, "_P") == MOD_ENV.Ext.Log.Print, "the mod's _P is not its own Ext.Log.Print")
                C.check(rawget(MOD_ENV, "print") == MOD_ENV.Ext.Log.Print, "the mod's print is not its own Ext.Log.Print")
                helpers = helpers .. "; the mod's own _P/print"
            end
            return Rows(helpers, "W1-07")
        end },
        { "Globals#_C", id = "W1-07", risk = "pure", run = function (E)
            local G = Globals()
            local host = C.notNil(G._C(), "_C()")
            local _, guid = C.character()
            C.equal(host.Uuid.EntityUuid, guid, "the UUID of the entity _C() gives")
            return Rows("_C() is the host character " .. guid .. " in the " .. (E.IsServer() and "server" or "client"), "W1-07")
        end },
        { "Sandbox#overrides", id = "W1-09", risk = "pure", offline = true, run = function (E)
            local G = Globals()
            C.check(G.math.random == E.Math.Random, "math.random is not Ext.Math.Random")
            local r = G.math.random(1, 6)
            C.check(math.type(r) == "integer" and r >= 1 and r <= 6, "math.random(1, 6) gave %s", tostring(r))
            local ok, err = pcall(G.math.randomseed, 1)
            C.check(not ok and tostring(err):find("not implemented", 1, true), "math.randomseed: %s", tostring(err))
            for _, name in ipairs({ "dofile", "loadfile" }) do
                ok, err = pcall(G[name], "x.lua")
                C.check(not ok and tostring(err):find("disabled for security reasons", 1, true), "%s: %s", name, tostring(err))
            end
            C.check(G.load == E.Utils.LoadString and G.loadstring == E.Utils.LoadString, "load/loadstring are not Ext.Utils.LoadString")
            local chunk = C.notNil(G.load("return 40 + 2"), "load('return 40 + 2')")
            C.equal(chunk(), 42, "the loaded chunk's result")
            local fields = {}
            for k in pairs(G.debug) do fields[#fields + 1] = k end
            table.sort(fields)
            C.equal(table.concat(fields, ","), "getinfo,traceback", "what is left of debug")
            return Rows("math.random, math.randomseed, dofile, loadfile, load, loadstring, debug as SandboxStartup.lua sets them", "W1-09")
        end },
        { "Ext.Log#perMod", id = "W1-02", risk = "pure", offline = true, run = function (E, M)
            -- The mod's own Ext.Log, or one made the same way where this state
            -- has no mod loaded (the start-up harness).
            local log = type(M.Log) == "table" and M.Log or nil
            if log == nil or rawget(log, "MakePrinter") == nil then log = E.CoreLib("Logger"):CreateLogModule(C.ModuleUUID) end
            local logger = E.CoreLib("Logger")
            local MARK = "bg3mse-igt-log:"
            local seen = {}
            local sub = E.Events.Log:Subscribe(function (e)
                local first = e.Message and e.Message[1]
                if type(first) == "string" and first:sub(1, #MARK) == MARK then
                    seen[#seen + 1] = string.format("%s/%s/%s/%s", first:sub(#MARK + 1), e.Level, e.Topic,
                        e.Module == C.ModuleUUID and "mod" or tostring(e.Module))
                    -- Not printed (Logger:Log reads it back).
                    e.ActionPrevented = true
                end
            end, { Priority = 1000 })
            local ok, err = pcall(function ()
                log.Debug(MARK .. "debug")
                log.Log("Warning", "Bg3mseIgtTopic", MARK .. "log")
                log.MakePrinter("Info", "Bg3mseIgtTopic")(MARK .. "printer")
                log.EnableTopic("Bg3mseIgtTopic", false)
                log.Log("Info", "Bg3mseIgtTopic", MARK .. "topic-off")
                log.EnableTopic("Bg3mseIgtTopic", nil)
                log.EnableModLogging(C.ModuleUUID, false)
                log.Print(MARK .. "mod-off")
                log.EnableModLogging(C.ModuleUUID, nil)
                log.SetLogLevel("Warning")
                log.Print(MARK .. "below-level")
                log.PrintWarning(MARK .. "at-level")
            end)
            -- Every level on again, as before the case (Logger:SetLogLevel from
            -- the first level enables all), and nothing of the case left.
            logger:SetLogLevel("Debug")
            logger.EnabledTopics.Bg3mseIgtTopic = nil
            logger.EnabledMods[C.ModuleUUID] = nil
            E.Events.Log:Unsubscribe(sub)
            if not ok then error(err, 0) end
            C.equal(table.concat(seen, ", "),
                "debug/Debug//mod, log/Warning/Bg3mseIgtTopic/mod, printer/Info/Bg3mseIgtTopic/mod, at-level/Warning//mod",
                "the Log events of the mod's logger")
            return Rows("Debug, Log, MakePrinter, EnableTopic, EnableModLogging and SetLogLevel each as Logger.lua has them", "W1-02, EV-27")
        end },
        { "Ext.Events#subscribeOptions", id = "W1-03", risk = "pure", offline = true, run = function (E)
            -- An event of the case's own, made as Ext.Events makes each one.
            local Event = E.CoreLib("Events/SubscribableEvent")
            local event = Event:New("Bg3mseIgtOptions")
            local order = {}
            event:Subscribe(function () order[#order + 1] = "p50" end, { Priority = 50 })
            event:Subscribe(function () order[#order + 1] = "p200" end, { Priority = 200 })
            event:Subscribe(function () order[#order + 1] = "once" end, { Priority = 150, Once = true })
            event:Subscribe(function (e) order[#order + 1] = "p100"; if e.StopHere then e.Stopped = true end end)
            event:Throw({ Stopped = false })
            event:Throw({ Stopped = false, StopHere = true })
            C.equal(table.concat(order, ","), "p200,once,p100,p50,p200,p100", "the handlers' calls over two throws")
            return Rows("Priority orders the handlers, Once runs one throw, a stopped event reaches no later handler", "W1-03")
        end },
        { "Ext.Events#eventMethods", id = "W1-03", risk = "pure", async = true, run = function (E, M, done)
            -- The engine's own event object (lua::EventBase) of a Tick, used last
            -- (the lowest priority) so that stopping it holds back no other
            -- subscriber of the Tick.
            local Event = E.CoreLib("Events/SubscribableEvent")
            local mine = Event:New("Bg3mseIgtNative")
            local calls = {}
            mine:Subscribe(function (e) calls[#calls + 1] = "first"; e:StopPropagation() end, { Priority = 200 })
            mine:Subscribe(function () calls[#calls + 1] = "second" end, { Priority = 100 })
            local sub
            sub = E.Events.Tick:Subscribe(function (e)
                E.Events.Tick:Unsubscribe(sub)
                local ok, err = pcall(function ()
                    C.equal(e.Stopped, false, "Stopped of a Tick before")
                    C.equal(e.CanPreventAction, false, "CanPreventAction of a Tick")
                    mine:Throw(e)
                    C.equal(table.concat(calls, ","), "first", "the handlers after StopPropagation")
                    C.equal(e.Stopped, true, "Stopped after StopPropagation")
                    -- A Tick cannot be prevented: PreventAction says so and changes nothing.
                    e:PreventAction()
                    C.equal(e.ActionPrevented, false, "ActionPrevented after PreventAction on a Tick")
                end)
                done(ok, ok and Rows("StopPropagation held back the second handler; PreventAction refused on a Tick (CanPreventAction false)", "W1-03") or err)
            end, { Priority = -1000000 })
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
