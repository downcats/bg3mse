-- The runner of the in-game test pass: one line per case, in a format the
-- checker (Mac/Tests/InGame/InGameTool.cpp) reads back from the log:
--
--   @BG3MSE-IGT|<context>|<id>|<name>|<result>|<detail>
--
-- <context> is server or client, <id> the audit row (or "-"), <name> the
-- function, event or test, <result> one of pass, error, skip, crashed,
-- uncovered, or planned (written at the start for each case the run will
-- report; its detail is the case's risk, "event", or "reset" for a case of
-- the reset phase). Control lines use the same prefix: begin, plan, end,
-- version, machine (host or remote: which side of a two-machine run this
-- Lua state is on, then the machine's own id, kept in the progress folder),
-- reset (the run asks for a Lua reset), reset-begin and reset-end (the reset
-- phase, in the Lua state the reset made), notice, settings (one of the
-- settings the trigger says this launch started with: the settings files'
-- name, the key, the value; the checker's settings command reads them).
--
-- A context's run has two phases. The main phase runs every case; then, in
-- developer mode, the run resets its own Lua state (Ext.Debug.Reset) and the
-- new state, bootstrapped again from the same files, runs the reset phase:
-- the cases marked phase = "reset" and the events a reset throws. The
-- progress file carries the run across the reset as across a crash.
--
-- Nothing here runs unless the owner asks for it: the trigger file below, or
-- the console command. A case that would change the save or the settings for
-- good is a skip with its reason and is never called.

local Runner = {}

local MARK = "@BG3MSE-IGT"
local TRIGGER = "bg3mse-ingame-tests.json"
local PROGRESS_DIR = "bg3mse-ingame-tests/"
local COMMAND = "bg3mse_ingame_tests"
-- Whether a run includes Norbyte's test suite and the list of registered
-- functions no case names (Runner.Configure).
local WITH_NORBYTE, WITH_COVERAGE = true, true
local CASES_PER_TICK = 8
local ASYNC_TIMEOUT_MS = 15000
local SERVER_WAIT_MS = 20 * 60 * 1000
-- How long the state that asked for a reset waits for it before it says the
-- reset did not come.
local RESET_TIMEOUT_MS = 60 * 1000
-- How long the client's closing notice stays on screen.
local NOTICE_MS = 20 * 1000
-- A two-machine run starts in a level the players play in (Runner.World);
-- when no level can be read for this long, it starts all the same.
local WORLD_WAIT_MS = 60 * 1000
-- How far apart two readings of this process's start may be (Runner.Process):
-- the wall clock counts whole seconds.
local PROCESS_TOLERANCE_S = 10

-- Inside a mod, Ext is the mod's own table over the real one (ModLoader.lua);
-- the cases test the real functions, as the extender registers them.
local function RealOf(ext)
    local mt = getmetatable(ext)
    if mt ~= nil and type(mt.__index) == "table" and rawget(ext, "Log") ~= nil
        and rawget(mt.__index, "Log") ~= nil and rawget(ext, "Log") ~= rawget(mt.__index, "Log") then
        return mt.__index
    end
    return ext
end

-- Files loaded with Ext.Require run in the global environment, where Ext is
-- the real table; the bootstrap hands over the mod's own (Runner.Bootstrap).
local ModExt = Ext
local RealExt = RealOf(Ext)

local Context = RealExt.IsServer() and "server" or "client"
Runner.Context = Context
Runner.RealExt = RealExt
Runner.ModExt = ModExt

local RISK_ORDER = { pure = 1, proved = 2, unproved = 3 }

local libraries = {}
local eventLibrary = nil -- Cases/Events.lua (Runner.SetEvents)
Runner.Libraries = libraries

-- --- Output -------------------------------------------------------------------

local function Clean(text)
    text = tostring(text == nil and "" or text)
    text = text:gsub("[\r\n\t]+", " \\n "):gsub("|", "/")
    if #text > 600 then
        text = text:sub(1, 600) .. "..."
    end
    return text
end
Runner.Clean = Clean

-- This launch's results (the end lines), and the run's across launches and
-- the reset (the progress file keeps them; the client's notice shows them).
local counts = { pass = 0, error = 0, skip = 0, crashed = 0, uncovered = 0 }
local totals = { pass = 0, error = 0, skip = 0, crashed = 0, uncovered = 0 }
-- The opt-in cases (optIn = a name of the trigger's Extras) that ended
-- without being exercised: the client's notice lists them, both contexts'.
local missed = {}

local machineSaid = false
local function Emit(...)
    -- Each Lua state's first line says which side of a two-machine run it is
    -- on and which machine it is (the checker keys its runs by both: a
    -- machine may host one game and join the next in one launch).
    if not machineSaid then
        machineSaid = true
        Emit("machine", Runner.IsRemote() and "remote" or "host", Runner.MachineId())
    end
    local parts = { MARK, Context }
    for i = 1, select("#", ...) do
        parts[#parts + 1] = Clean(select(i, ...))
    end
    RealExt.Log.Print(table.concat(parts, "|"))
end

local function Result(case, result, detail, elsewhere)
    counts[result] = (counts[result] or 0) + 1
    totals[result] = (totals[result] or 0) + 1
    if case.optIn ~= nil and result ~= "pass" and not elsewhere and #missed < 100 then
        missed[#missed + 1] = Context .. " " .. case.name .. " (" .. case.optIn .. "): " .. result
    end
    Emit(case.id or "-", case.name, result, detail or "")
end
Runner.Result = Result

-- --- Helpers handed to the case files -----------------------------------------

local SkipMarker = {}

local C = {}
Runner.C = C

-- This mod (meta.lsx).
C.ModuleUUID = "da54e292-068f-4575-8df1-87adf68384c1"
C.ModuleFolder = "Bg3mseInGameTests"
C.ModTable = "Bg3mseInGameTests"
-- The game's base module (GustavX, first in the modsettings.lsx this game
-- version writes; Norbyte's ModTests.lua still names GustavDev, 28ac9ce2-...)
-- and its dependency.
C.GustavUUID = "cb555efe-2d9e-131f-8195-a89329d218ea"
C.SharedUUID = "ed539163-bb70-431b-96a7-f5b2eda5376b"
-- Where the cases may write: under the profile's Script Extender folder.
C.ScratchDir = "bg3mse-ingame-tests/"

-- Ends the case as skipped: what it needs is not there in this session.
function C.skip(reason)
    error(setmetatable({ reason = reason }, SkipMarker), 0)
end

-- Ends the case as skipped because, in a two-machine run, it is the other
-- machine's to run: not listed among the opt-in cases not exercised.
function C.elsewhere(reason)
    error(setmetatable({ reason = reason, elsewhere = true }, SkipMarker), 0)
end

-- Whether the trigger file asks for the opt-in cases of that name
-- ("Extras": ["Controller", ...]): those that wait for the owner.
function C.optedIn(name)
    local extras = C.trigger and C.trigger.Extras
    if type(extras) ~= "table" then return false end
    for _, e in ipairs(extras) do
        if e == name then return true end
    end
    return false
end

function C.check(cond, message, ...)
    if not cond then
        error(string.format(message or "check failed", ...), 2)
    end
    return cond
end

function C.type(value, expected, what)
    if type(value) ~= expected then
        error((what or "value") .. " is a " .. type(value) .. ", expected " .. expected, 2)
    end
    return value
end

-- Norbyte's Lua calls a C++ object's __eq even against a value of another
-- type (lvm.c, luaV_equalobj): an enumeration equals its label that way, but
-- against nil the __eq raises or, where there is none, nil is called. So nil
-- is compared raw.
local function Equal(a, b)
    if rawequal(a, nil) or rawequal(b, nil) then return rawequal(a, b) end
    return a == b
end
C.same = Equal

function C.equal(value, expected, what)
    if not Equal(value, expected) then
        error((what or "value") .. " is " .. tostring(value) .. ", expected " .. tostring(expected), 2)
    end
    return value
end

function C.near(value, expected, what, eps)
    if type(value) ~= "number" or math.abs(value - expected) > (eps or 1e-4) then
        error((what or "value") .. " is " .. tostring(value) .. ", expected " .. tostring(expected), 2)
    end
    return value
end

function C.vecNear(value, expected, what, eps)
    for i = 1, #expected do
        if type(value) ~= "table" and type(value) ~= "userdata" then
            error((what or "vector") .. " is a " .. type(value), 2)
        end
        if math.abs(value[i] - expected[i]) > (eps or 1e-4) then
            error((what or "vector") .. "[" .. i .. "] is " .. tostring(value[i]) .. ", expected " .. tostring(expected[i]), 2)
        end
    end
    return value
end

function C.notNil(value, what)
    if rawequal(value, nil) then
        error((what or "value") .. " is nil", 2)
    end
    return value
end

-- A function that must raise a Lua error for the arguments given.
function C.raises(fn, ...)
    local ok = pcall(fn, ...)
    if ok then
        error("expected an error, the call returned", 2)
    end
end

local function Count(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n
end
C.count = Count

-- What the session gives the cases: the host character's GUID and entity on
-- the server, the client's controlled character on the client. Each is nil
-- when the session has none (the offline harness, the main menu).
local sessionCache = nil
function C.session()
    if sessionCache ~= nil then return sessionCache end
    local s = {}
    if Context == "server" then
        local ok, host = pcall(function () return Osi.GetHostCharacter() end)
        if ok and type(host) == "string" and host ~= "" then
            s.characterGuid = host
            s.character = RealExt.Entity.Get(host)
        end
    else
        local ok, list = pcall(RealExt.Entity.GetAllEntitiesWithComponent, "ClientControl")
        if ok and list ~= nil then
            for _, entity in pairs(list) do
                if entity.ClientCharacter ~= nil and entity.ClientCharacter.ReservedUserID == 1 then
                    s.character = entity
                    break
                end
            end
            if s.character == nil then s.character = list[1] end
        end
        if s.character ~= nil and s.character.Uuid ~= nil then
            s.characterGuid = s.character.Uuid.EntityUuid
        end
    end
    if s.character ~= nil then
        local ok, transform = pcall(function () return s.character.Transform.Transform end)
        if ok and transform ~= nil then
            s.position = { transform.Translate[1], transform.Translate[2], transform.Translate[3] }
        end
    end
    sessionCache = s
    return s
end

function C.character()
    local s = C.session()
    if s.character == nil then C.skip("no character in this session") end
    return s.character, s.characterGuid
end

function C.position()
    local s = C.session()
    if s.position == nil then C.skip("no character position in this session") end
    return s.position
end

-- Another mod that runs its own cases with this runner (the story tests,
-- Mac/Tests/InGame/StoryMod) says which it is before its bootstrap: its
-- module, folder and mod table, its trigger file and progress folder, its
-- console command, and whether Norbyte's suite and the coverage list belong
-- to its runs.
function Runner.Configure(o)
    C.ModuleUUID = o.ModuleUUID or C.ModuleUUID
    C.ModuleFolder = o.ModuleFolder or C.ModuleFolder
    C.ModTable = o.ModTable or C.ModTable
    TRIGGER = o.Trigger or TRIGGER
    PROGRESS_DIR = o.ProgressDir or PROGRESS_DIR
    C.ScratchDir = PROGRESS_DIR
    COMMAND = o.Command or COMMAND
    if o.Norbyte ~= nil then WITH_NORBYTE = o.Norbyte end
    if o.Coverage ~= nil then WITH_COVERAGE = o.Coverage end
end

-- --- Libraries and the plan ---------------------------------------------------

-- A library file returns { name = ..., define = function (C) return { cases } end }.
function Runner.AddLibrary(library)
    libraries[#libraries + 1] = library
end

local function CaseContexts(case)
    local ctx = case.ctx or "both"
    if ctx == "both" then return { server = true, client = true } end
    return { [ctx] = true }
end
Runner.CaseContexts = CaseContexts

-- Every case of every library, in a fixed order: risk first (pure, proved,
-- unproved), then library order, then the order in its file.
function Runner.Plan(filter)
    local plan = {}
    for li, library in ipairs(libraries) do
        local defined = library.define(C)
        for ci, def in ipairs(defined) do
            local case = {
                name = def[1],
                id = def.id,
                ctx = def.ctx,
                risk = def.risk or "proved",
                offline = def.offline,
                skip = def.skip,
                async = def.async,
                timeout = def.timeout,
                -- Waits for the owner: started only when no other case is
                -- waiting, and no case starts while it waits.
                exclusive = def.exclusive,
                -- Run only when the trigger's Extras names it (C.optedIn).
                optIn = def.optIn,
                -- The case ends the launch or the session on purpose:
                -- "crash" (the game closes in it) or "load" (the owner
                -- loads a save while it waits). The run that resumes
                -- after it reports it as passed, not crashed, and goes on
                -- with the next case, which checks what it was for.
                endsLaunch = def.endsLaunch,
                -- The audit rows a case that ends its launch confirms, for
                -- the pass the next launch reports.
                rows = def.rows,
                run = def.run,
                -- The reset phase (Runner.Run): before runs in the state that
                -- asks for the reset, its result handed to run in the new one.
                phase = def.phase,
                before = def.before,
                resetFirst = def.resetFirst,
                library = library.name,
                order = li * 10000 + ci
            }
            if CaseContexts(case)[Context] and (filter == nil or filter(case)) then
                plan[#plan + 1] = case
            end
        end
    end
    table.sort(plan, function (a, b)
        local ra, rb = RISK_ORDER[a.risk] or 2, RISK_ORDER[b.risk] or 2
        if ra ~= rb then return ra < rb end
        return a.order < b.order
    end)
    return plan
end

-- --- Coverage: every function the extender registered, against the cases -------

-- Tables of Ext that are not libraries of functions.
local NOT_LIBRARIES = {
    Enums = true, Events = true, ModEvents = true, Test = true, _Internal = true,
    Config = true, System = true
}

local function Resolve(root, dotted)
    local value = root
    for part in dotted:gmatch("[^.]+") do
        if part ~= "Ext" then
            if type(value) ~= "table" then return nil end
            value = rawget(value, part)
        end
        -- type(), not ==: in Norbyte's Lua a C++ object without __eq (Ext.System)
        -- compared with a value of another type raises "attempt to call a nil
        -- value" (lvm.c, luaV_equalobj, takes ltm.c's luaO_nilobject for a
        -- metamethod).
        if type(value) == "nil" then return nil end
    end
    return value
end

-- The registered functions no case of this context names (by value, so that
-- an alias such as Ext.Utils.Round = Ext.Math.Round counts as covered).
function Runner.Uncovered(plan)
    local covered = {}
    for _, case in ipairs(plan) do
        local base = case.name:gsub("#.*$", "")
        if base:sub(1, 4) == "Ext." then
            local value = Resolve(RealExt, base)
            if type(value) == "function" then covered[value] = true end
        end
    end

    local missing = {}
    local function Walk(tab, prefix, depth)
        local keys = {}
        for k in pairs(tab) do
            if type(k) == "string" then keys[#keys + 1] = k end
        end
        table.sort(keys)
        for _, k in ipairs(keys) do
            local v = rawget(tab, k)
            local name = prefix .. "." .. k
            if type(v) == "function" then
                if not covered[v] then missing[#missing + 1] = name end
            elseif type(v) == "table" and depth < 2 and not (depth == 0 and NOT_LIBRARIES[k]) then
                Walk(v, name, depth + 1)
            end
        end
    end
    Walk(RealExt, "Ext", 0)

    -- The events the event manager registered (engine events and the local
    -- ones, Libs/Events/EventManager.lua), against Cases/Events.lua.
    local named = {}
    if eventLibrary ~= nil then
        for _, def in ipairs(eventLibrary.define(C)) do
            if CaseContexts({ ctx = def.ctx })[Context] then named[def[1]] = true end
        end
    end
    local manager = RealExt._Internal and RealExt._Internal.EventManager
    local events = manager ~= nil and rawget(manager, "Events") or nil
    if type(events) ~= "table" then
        missing[#missing + 1] = "Ext.Events (the event manager's list is not there)"
    else
        local names = {}
        for k in pairs(events) do
            if type(k) == "string" then names[#names + 1] = k end
        end
        table.sort(names)
        for _, k in ipairs(names) do
            if not named["Ext.Events." .. k] then missing[#missing + 1] = "Ext.Events." .. k end
        end
    end
    return missing
end

-- --- Running one case ---------------------------------------------------------

local pending = {}
local pendingCount = 0
-- A waiting case got its result since the progress was last written.
local pendingChanged = false

local function Now()
    local ok, t = pcall(RealExt.Timer.MonotonicTime)
    if ok and type(t) == "number" then return t end
    return nil
end

local function Finish(case, ok, err)
    if ok then
        Result(case, "pass", err)
    elseif type(err) == "table" and getmetatable(err) == SkipMarker then
        Result(case, "skip", err.reason, err.elsewhere)
    else
        Result(case, "error", err)
    end
end

local function Traceback(e)
    if type(e) == "table" and getmetatable(e) == SkipMarker then return e end
    return debug.traceback(e, 2)
end

-- extra is what a reset-phase case's before gave (Runner.Run).
local function RunCase(case, extra)
    if case.skip ~= nil then
        Result(case, "skip", case.skip)
        return
    end
    if case.optIn ~= nil and not C.optedIn(case.optIn) then
        Result(case, "skip", "opt-in: the trigger file's Extras does not name \"" .. case.optIn .. "\"")
        return
    end
    if case.async then
        local entry = { case = case, started = Now(), ticks = 0, done = false }
        -- done(true|false, detail), or done("skip", reason).
        local function done(ok, detail)
            if entry.done then return end
            entry.done = true
            pending[case] = nil
            pendingCount = pendingCount - 1
            pendingChanged = true
            if ok == "skip" then
                Result(case, "skip", detail)
            else
                Finish(case, ok ~= false, detail)
            end
        end
        pending[case] = entry
        pendingCount = pendingCount + 1
        local ok, err = xpcall(case.run, Traceback, RealExt, ModExt, done, extra)
        if not ok then done(false, err) end
        return
    end
    local ok, detail = xpcall(case.run, Traceback, RealExt, ModExt, extra)
    Finish(case, ok, detail)
end
Runner.RunCase = RunCase

local function ExclusiveWaiting()
    for case in pairs(pending) do
        if case.exclusive then return true end
    end
    return false
end

local function ExpirePending(force)
    local now = Now()
    for case, entry in pairs(pending) do
        entry.ticks = entry.ticks + 1
        local elapsed = (now ~= nil and entry.started ~= nil) and (now - entry.started) or (entry.ticks * 33)
        if force or elapsed > (case.timeout or ASYNC_TIMEOUT_MS) then
            entry.done = true
            pending[case] = nil
            pendingCount = pendingCount - 1
            pendingChanged = true
            Result(case, "error", "no result within " .. tostring(case.timeout or ASYNC_TIMEOUT_MS) .. " ms")
        end
    end
end

-- --- Norbyte's own test suite (BG3Extender/LuaScripts/Tests) --------------------

-- As his runners register them (Ext.Utils.LoadTestLibrary loads
-- ServerTestRunner.lua or ClientTestRunner.lua), each test run under this
-- runner's wrapper instead of his RunTests, which prints its own format.
-- His tests whose expectation this game version or his own later code no
-- longer meets, on Windows too: each skips with the reason while it holds.
local function NotTheBaseModule()
    local base = RealExt.Mod.GetBaseMod().Info.ModuleUUIDString
    if base ~= "28ac9ce2-2aba-8cda-b3b5-6e922f71b6b8" then
        return "expects GustavDev as the base module; this game's is " .. base
    end
end
local kStaleNorbyteTests = {
    ["Mod/TestBaseMod"] = NotTheBaseModule,
    ["Mod/TestModManager"] = NotTheBaseModule,
    ["ECS/TestECSComponents"] = function ()
        return "compares DisplayName.Name, a TranslatedString object, with a string"
    end,
    ["ECS/TestECSFunctions"] = function ()
        return "calls GetEntityType, which his \"Add Ext.Entity.Create\" took out of the entity proxy"
    end,
    ["Stats/TestStatAttributes"] = function ()
        -- Only when a later module redefines the stat: a wrong read is not
        -- 71 either.
        local stat = RealExt.Stats.Get("MindFlayer")
        if stat and stat.ModId ~= stat.OriginalModId then
            return "expects MindFlayer's Vitality of the game's data, 71; module " .. tostring(stat.ModId)
                .. " redefines the stat (Vitality " .. tostring(stat.Vitality) .. ")"
        end
    end,
}

function Runner.NorbyteCases()
    local env = RealExt.Test
    if env == nil then
        local ok, err = pcall(RealExt.Utils.LoadTestLibrary)
        env = RealExt.Test
        if env == nil then
            return { { name = "Norbyte:LoadTestLibrary", id = "LUA-009", risk = "proved",
                run = function () error("Ext.Utils.LoadTestLibrary did not make Ext.Test: " .. tostring(err)) end } }
        end
    end
    local registered = rawget(env, "RegisteredTests") or {}
    local categories = {}
    for category in pairs(registered) do categories[#categories + 1] = category end
    table.sort(categories)
    local cases = {}
    for _, category in ipairs(categories) do
        for _, test in ipairs(registered[category]) do
            cases[#cases + 1] = {
                name = "Norbyte:" .. category .. "/" .. test,
                id = "LUA-009",
                risk = "proved",
                run = function ()
                    local fn = rawget(env, test)
                    if type(fn) ~= "function" then error("the test function is not defined") end
                    local stale = kStaleNorbyteTests[category .. "/" .. test]
                    local reason = stale and stale()
                    if reason ~= nil then C.skip("stale upstream expectation: " .. reason) end
                    fn()
                end
            }
        end
    end
    return cases
end

-- --- Progress across a crash and a reset -------------------------------------------

-- One file per context, and one more for the client of a machine that joined
-- another's game, so that a machine can host one game of a run and join the
-- next. Every state written carries the run's id, the plan's fingerprint,
-- the totals so far, the process that wrote it (Process: when it started),
-- and one of:
--   main phase:  Next (the last case started), NextReported (it has its
--                result, or waits and is in Waiting), Waiting, Restarted,
--                Since (when each waiting case was first started)
--   reset phase: Phase = "reset", Stage = "requested" (Ext.Debug.Reset was
--                called), "running" (ResetNext is the case being run) or
--                "events"; Carry (what each case's before gave), Seen (events
--                the old state saw as it ended), Crashed, Attempt
--   the end:     Finished = true
local function ProgressPath(context)
    if context == "client" and Runner.IsRemote() then
        return PROGRESS_DIR .. "progress-client-remote.json"
    end
    return PROGRESS_DIR .. "progress-" .. context .. ".json"
end

local function ReadJson(path, context)
    local ok, text = pcall(RealExt.IO.LoadFile, path, context)
    if not ok or text == nil or text == "" then return nil end
    local ok2, value = pcall(RealExt.Json.Parse, text)
    if ok2 then return value end
    return nil
end

-- The wall clock, in seconds.
local function Epoch()
    local ok, t = pcall(RealExt.Timer.ClockEpoch)
    if ok and type(t) == "number" then return t end
    return nil
end

-- When this process started, in seconds of the wall clock: MicrosecTime
-- counts from the registration of the extender's libraries, which happens
-- once per process (Lua/Libs/Timer.inl, LuaSharedLibs.cpp), so every Lua
-- state of one launch reads the same start, to within a second, and the
-- states of another launch read another.
function Runner.Process()
    local ok, at = pcall(function () return RealExt.Timer.ClockEpoch() - RealExt.Timer.MicrosecTime() / 1000000 end)
    if ok and type(at) == "number" then return at end
    return nil
end

-- Whether the progress was written by this process: the session was loaded
-- again (a new game goes from character creation into the world, a save is
-- loaded), and the Lua states with it; nothing crashed.
function Runner.SameProcess(progress, now)
    local was = type(progress) == "table" and tonumber(progress.Process) or nil
    if now == nil then now = Runner.Process() end
    return was ~= nil and now ~= nil and math.abs(now - was) <= PROCESS_TOLERANCE_S
end
C.sameProcess = Runner.SameProcess

local runKey = { RunId = "", Plan = "" }

local function SaveProgress(state)
    state.RunId = runKey.RunId
    state.Plan = runKey.Plan
    state.Process = Runner.Process()
    state.Totals = totals
    state.Missed = missed
    pcall(RealExt.IO.SaveFile, ProgressPath(Context), RealExt.Json.Stringify(state))
end

local function AdoptTotals(progress)
    if type(progress) ~= "table" or type(progress.Totals) ~= "table" then return end
    for k in pairs(totals) do
        totals[k] = math.tointeger(progress.Totals[k]) or 0
    end
    if type(progress.Missed) == "table" then
        for _, m in ipairs(progress.Missed) do
            if type(m) == "string" and #missed < 100 then missed[#missed + 1] = m end
        end
    end
end

-- What a run does, from the progress file and how this Lua state started:
-- how = "running" (the session reached Running in this launch, or the
-- console command) or "reset" (this state was made by a Lua reset).
-- A pure function, so that the offline harness can check it (Cases/Reset.lua).
--   fresh          start the main phase
--   resume-main    go on with the main phase after the case that was running
--   finished       the run is over (how = "running": say so)
--   none           nothing to do
--   run-reset      the reset phase, from case first; crashed is a case to
--                  report crashed first (why: "reset" if the state was reset
--                  inside it)
--   request-reset  a launch ended inside a reset-phase case: reset again in
--                  this launch and go on after it
--   reset-crashed  a launch ended during the reset itself: report it and end
--   events         a launch ended while the reset phase reported its events
function Runner.Decide(progress, runId, fingerprint, how)
    if type(progress) ~= "table" or progress.RunId ~= runId or progress.Plan ~= fingerprint then
        return { action = how == "reset" and "none" or "fresh" }
    end
    if progress.Finished then
        return { action = how == "reset" and "none" or "finished" }
    end
    if progress.Phase ~= "reset" then
        if type(progress.Next) == "number" then return { action = "resume-main" } end
        return { action = how == "reset" and "none" or "fresh" }
    end
    local next = math.tointeger(progress.ResetNext) or 0
    local crashed = math.tointeger(progress.Crashed)
    if how == "reset" then
        if progress.Stage == "requested" then
            return { action = "run-reset", first = next + 1, crashed = crashed }
        elseif progress.Stage == "running" then
            return { action = "run-reset", first = next + 1, crashed = next, why = "reset" }
        end
        return { action = "events", lost = "the Lua state was reset while the reset phase reported its events" }
    end
    if progress.Stage == "requested" then
        return { action = "reset-crashed", first = crashed or 1, again = crashed ~= nil }
    elseif progress.Stage == "running" then
        return { action = "request-reset", first = next + 1, crashed = next }
    end
    return { action = "events", lost = "the previous launch ended while the reset phase reported its events" }
end
C.decide = Runner.Decide

-- This machine, for the checker: an id made once and kept beside the
-- progress files.
local machineId = nil
function Runner.MachineId()
    if machineId ~= nil then return machineId end
    -- Outside a mod (the offline harness) there is no profile to keep it in.
    if rawget(_G, "ModuleUUID") == nil then
        machineId = "-"
        return machineId
    end
    local path = PROGRESS_DIR .. "machine.json"
    local kept = ReadJson(path)
    if type(kept) == "table" and type(kept.Id) == "string" and kept.Id ~= "" then
        machineId = kept.Id
        return machineId
    end
    local ok, id = pcall(function ()
        return string.format("%x-%x", RealExt.Timer.ClockEpoch(), math.floor(RealExt.Timer.MicrosecTime()))
    end)
    machineId = ok and id or "-"
    if ok then pcall(function () RealExt.IO.SaveFile(path, RealExt.Json.Stringify({ Id = id })) end) end
    return machineId
end

-- --- The trigger -----------------------------------------------------------------

-- The owner's switch: a file in the profile's Script Extender folder.
--   { "Run": true, "RunId": "1", "Only": ["Ext.Math"], "Exclude": ["Ext.Audio"],
--     "Reset": false, "Console": true }
-- Only/Exclude match the start of a case name. Changing RunId starts again.
-- Reset false leaves out the reset phase; Console true runs the cases that
-- wait for the owner to type in the extender's console window; Extras names
-- the opt-in cases to run ("Extras": ["IdeHelpers"]: C.optedIn).
function Runner.ReadTrigger()
    local trigger = ReadJson(TRIGGER)
    if type(trigger) ~= "table" or trigger.Run ~= true then return nil end
    trigger.RunId = tostring(trigger.RunId or "1")
    return trigger
end

local function MakeFilter(trigger)
    local function Matches(list, name)
        if type(list) ~= "table" then return false end
        for _, prefix in ipairs(list) do
            if name:sub(1, #prefix) == prefix then return true end
        end
        return false
    end
    return function (case)
        if type(trigger.Only) == "table" and #trigger.Only > 0 and not Matches(trigger.Only, case.name) then
            return false
        end
        return not Matches(trigger.Exclude, case.name)
    end
end

-- --- Events --------------------------------------------------------------------

-- The event cases (Cases/Events.lua) subscribe at bootstrap, so that the events
-- of the session's load are seen too; the run reports them at the end of the
-- phase they belong to. An event of the reset phase marked beforeReset fires
-- in the state that ends (Shutdown): that state writes it into the progress
-- file, and the new one reports it.
function Runner.SetEvents(library)
    eventLibrary = library
end

local seenEvents = {}
Runner.SeenEvents = seenEvents
local eventCases = {}
-- The events in the order they fired (the reset cases check it), but those
-- that come all the time: every tick, every mod's log line, the input.
local UNORDERED = { Tick = true, Log = true, KeyInput = true, MouseButtonInput = true, MouseWheelInput = true,
    ControllerAxisInput = true, ControllerButtonInput = true, NetMessage = true, NetModMessage = true }
local eventOrder = {}
C.eventOrder = eventOrder
-- The runner's own event handlers: an event whose every subscriber is one of
-- them is this mod's alone (the DeactivateEvent case).
C.ownHandlers = {}

local function EventCase(def)
    return { name = def[1], id = def.id, ctx = def.ctx, skip = def.skip, provoke = def.provoke,
        needs = def.needs, check = def.check, phase = def.phase, beforeReset = def.beforeReset, risk = "proved" }
end

local function CheckEvent(case, e)
    if case.check == nil then return "" end
    local ok, res = pcall(case.check, e)
    return ok and tostring(res or "") or ("handler check failed: " .. tostring(res))
end

function Runner.SubscribeEvents()
    if eventLibrary == nil then return end
    for _, def in ipairs(eventLibrary.define(C)) do
        local case = EventCase(def)
        if CaseContexts(case)[Context] then
            eventCases[#eventCases + 1] = case
            if case.skip == nil then
                local eventName = case.name:match("^Ext%.Events%.(.+)$")
                local function Handler(e)
                    if not UNORDERED[eventName] and #eventOrder < 500 then eventOrder[#eventOrder + 1] = eventName end
                    if seenEvents[case.name] == nil then
                        seenEvents[case.name] = CheckEvent(case, e)
                    end
                end
                C.ownHandlers[Handler] = true
                local ok, err = pcall(function ()
                    RealExt.Events[eventName]:Subscribe(Handler)
                end)
                if not ok then case.subscribeError = err end
            end
        end
    end
end

-- seen: events the state before the reset saw (progress Seen); lost: why the
-- others were not seen, when it is not what they need.
local function ReportEvents(list, seen, lost)
    -- What the module load's own Lua state saw (BootstrapModule.lua).
    local recorded = ReadJson(PROGRESS_DIR .. "load-events-" .. Context .. ".json")
    if type(recorded) ~= "table" or recorded.RunId ~= runKey.RunId or type(recorded.Seen) ~= "table" then
        recorded = nil
    end
    for _, case in ipairs(list) do
        if case.skip ~= nil then
            Result(case, "skip", case.skip)
        elseif case.subscribeError ~= nil then
            Result(case, "error", "Subscribe failed: " .. tostring(case.subscribeError))
        elseif seenEvents[case.name] ~= nil then
            Result(case, "pass", seenEvents[case.name])
        elseif seen ~= nil and seen[case.name] ~= nil then
            Result(case, "pass", seen[case.name])
        elseif recorded ~= nil and recorded.Seen[case.name] ~= nil then
            Result(case, "pass", "in the module load's Lua state: " .. tostring(recorded.Seen[case.name]))
        else
            Result(case, "skip", lost or ("not seen in this run: " .. tostring(case.needs or "nothing provoked it")))
        end
    end
end

local function ProvokeEvents(list)
    for _, case in ipairs(list) do
        if case.provoke ~= nil and case.skip == nil then
            local ok, err = pcall(case.provoke, RealExt, ModExt)
            if not ok then case.subscribeError = "provoking it failed: " .. tostring(err) end
        end
    end
end

-- --- What a run is made of ---------------------------------------------------------

-- The cases of the main phase (pure, Norbyte's suite, proved, unproved), the
-- cases and events of the reset phase, and why the reset phase is off, if it is.
function Runner.Prepare(trigger)
    local filter = MakeFilter(trigger)
    -- Every case of this context, and the ones the trigger lets run.
    local all = Runner.Plan(nil)
    local plan = {}
    for _, case in ipairs(all) do
        if filter(case) then plan[#plan + 1] = case end
    end
    local run = { trigger = trigger, filter = filter, all = all, plan = plan, ordered = {}, resetList = {},
        mainEvents = {}, resetEvents = {} }

    local norbyte = {}
    for _, case in ipairs(WITH_NORBYTE and Runner.NorbyteCases() or {}) do
        if filter(case) then norbyte[#norbyte + 1] = case end
    end
    for _, case in ipairs(plan) do
        if case.phase ~= "reset" and case.risk == "pure" then run.ordered[#run.ordered + 1] = case end
    end
    for _, case in ipairs(norbyte) do run.ordered[#run.ordered + 1] = case end
    for _, case in ipairs(plan) do
        if case.phase == "reset" then
            run.resetList[#run.resetList + 1] = case
        elseif case.risk ~= "pure" then
            run.ordered[#run.ordered + 1] = case
        end
    end
    -- The reset phase in file order, its resetFirst case (Ext.Debug.Reset) first.
    table.sort(run.resetList, function (a, b)
        local fa, fb = a.resetFirst and 0 or 1, b.resetFirst and 0 or 1
        if fa ~= fb then return fa < fb end
        return a.order < b.order
    end)
    for _, case in ipairs(eventCases) do
        if filter(case) then
            if case.phase == "reset" then
                run.resetEvents[#run.resetEvents + 1] = case
            else
                run.mainEvents[#run.mainEvents + 1] = case
            end
        end
    end

    if #run.resetList + #run.resetEvents > 0 then
        local okDev, developer = pcall(RealExt.Debug.IsDeveloperMode)
        if trigger.Reset == false then
            run.resetOff = "the trigger file leaves out the reset phase (Reset: false)"
        elseif not (okDev and developer == true) then
            run.resetOff = "Ext.Debug.Reset works only in developer mode (DeveloperMode in ScriptExtenderSettings.json)"
        end
    end

    run.maxTimeout = ASYNC_TIMEOUT_MS
    for _, case in ipairs(run.ordered) do
        if case.timeout ~= nil and case.timeout > run.maxTimeout then run.maxTimeout = case.timeout end
    end

    -- The plan's fingerprint: how many cases each part has and a hash of
    -- every name in the order they run, so that a progress file is taken up
    -- only by the plan that wrote it. A trigger file changed between two
    -- launches of one run id (a case left out after it ended a launch) gives
    -- another plan, and the run starts again from its first case.
    local hash = 2166136261
    local function Mix(text)
        for i = 1, #text do
            hash = ((hash ~ text:byte(i)) * 16777619) & 0xffffffff
        end
        hash = ((hash ~ 10) * 16777619) & 0xffffffff
    end
    for _, list in ipairs({ run.ordered, run.resetList, run.mainEvents, run.resetEvents }) do
        for _, case in ipairs(list) do Mix(case.name) end
        Mix("")
    end
    run.fingerprint = string.format("%d:%d:%d:%d:%s:%08x", #run.ordered, #run.resetList, #run.mainEvents, #run.resetEvents,
        run.resetOff and "off" or "on", hash)
    return run
end

-- --- The end of a run ------------------------------------------------------------

local running = false

local function Totals(t)
    if type(t) ~= "table" then return "no result" end
    return string.format("%d pass, %d error, %d skip, %d crashed",
        math.tointeger(t.pass) or 0, math.tointeger(t.error) or 0, math.tointeger(t.skip) or 0, math.tointeger(t.crashed) or 0)
end

-- The client's last act: what both contexts gave, on screen for a while (an
-- ImGui window that closes itself) and in the log.
local function ShowNotice()
    local server = ReadJson(ProgressPath("server"))
    local serverRun = type(server) == "table" and server.RunId == runKey.RunId
    local lines = {
        "bg3mse in-game tests finished.",
        "Server: " .. (Runner.IsRemote() and "none on this machine (it is not the host)"
            or Totals(serverRun and server.Totals or nil)),
        "Client: " .. Totals(totals),
    }
    Emit("notice", lines[2], lines[3])
    -- The opt-in cases that were not exercised, the server's first.
    local notExercised = {}
    if serverRun and type(server.Missed) == "table" then
        for _, m in ipairs(server.Missed) do notExercised[#notExercised + 1] = tostring(m) end
    end
    for _, m in ipairs(missed) do notExercised[#notExercised + 1] = m end
    if #notExercised > 0 then
        lines[#lines + 1] = "Opt-in cases not exercised:"
        for _, m in ipairs(notExercised) do
            lines[#lines + 1] = "  " .. m
            Emit("notice", "not exercised", m)
        end
    end
    lines[#lines + 1] = "This window closes by itself in " .. (NOTICE_MS // 1000) .. " s."
    local ok, err = pcall(function ()
        local window = RealExt.IMGUI.NewWindow("bg3mse in-game tests")
        for _, line in ipairs(lines) do window:AddText(line) end
        RealExt.Timer.WaitForRealtime(NOTICE_MS, function () pcall(function () window:Destroy() end) end)
    end)
    if not ok then
        RealExt.Log.Print("bg3mse in-game tests: no notice window: " .. tostring(err))
    end
end

local function FinishRun()
    SaveProgress({ Finished = true })
    running = false
    if Context ~= "client" then return end
    -- A client that started without waiting for the server's run (the
    -- save-and-load phase) waits for it here, so that the notice counts
    -- both, for as long as the client waits for a server run at its start
    -- (the server's last case may wait on a persistent timer).
    if not C.optedIn("SaveLoad") or Runner.IsRemote() then return ShowNotice() end
    local startedAt, waited, sub = Now(), 0, nil
    sub = RealExt.Events.Tick:Subscribe(function ()
        waited = waited + 1
        if waited % 30 ~= 0 then return end
        local server = ReadJson(ProgressPath("server"))
        local now = Now()
        if (type(server) == "table" and server.RunId == runKey.RunId and server.Finished and Runner.SameProcess(server))
            or (startedAt ~= nil and now ~= nil and now - startedAt > SERVER_WAIT_MS) or waited > 60 * 60 * 30 then
            RealExt.Events.Tick:Unsubscribe(sub)
            ShowNotice()
        end
    end)
end

local function EndReset()
    Emit("reset-end", counts.pass, counts.error, counts.skip, counts.crashed, counts.uncovered)
    FinishRun()
end

-- The reset phase when no new state runs it: begin line, the case at first
-- with head's result, the rest skipped, the events from what was seen.
local function ReportResetLost(run, seen, first, head, headDetail, rest, beginDetail)
    Emit("reset-begin", runKey.RunId, #run.resetList, beginDetail)
    for i = first, #run.resetList do
        local case = run.resetList[i]
        if i == first and head ~= nil then
            Result(case, head, headDetail)
        else
            Result(case, "skip", rest)
        end
    end
    ReportEvents(run.resetEvents, seen, rest)
    EndReset()
end

-- A step of the reset phase that fails in the runner itself still ends the
-- phase and the run, so that the client does not wait for the server in vain.
local function RunnerFailed(run, err)
    RealExt.Log.Print("bg3mse in-game tests: the runner failed in the reset phase: " .. tostring(err))
    pcall(ReportResetLost, run, nil, #run.resetList + 1, nil, nil, "the runner failed", "the runner failed: " .. Clean(err))
    running = false
end

local function Guarded(run, step, ...)
    local ok, err = xpcall(step, debug.traceback, ...)
    if not ok then RunnerFailed(run, err) end
end

-- --- The reset -------------------------------------------------------------------

-- In the state that ends: each remaining case's before, the progress, then
-- Ext.Debug.Reset for this context alone. The new state takes over in
-- Runner.Bootstrap (ResetCompleted).
local function RequestReset(run, first, crashed, attempt, reason)
    local carry = {}
    for i = first, #run.resetList do
        local case = run.resetList[i]
        if case.before ~= nil and case.skip == nil then
            local ok, value = xpcall(case.before, Traceback, RealExt, ModExt)
            local entry
            if ok then
                entry = { value = value }
            elseif type(value) == "table" and getmetatable(value) == SkipMarker then
                entry = { skip = value.reason }
            else
                entry = { error = tostring(value) }
            end
            if not pcall(RealExt.Json.Stringify, entry) then
                entry = { error = "what before gave cannot be written as JSON" }
            end
            carry[case.name] = entry
        end
    end
    SaveProgress({ Phase = "reset", Stage = "requested", ResetNext = first - 1, Crashed = crashed,
        Attempt = attempt, Carry = carry, Seen = {} })
    Emit("reset", runKey.RunId, reason)

    for _, case in ipairs(run.resetEvents) do
        if case.beforeReset and case.skip == nil then
            local eventName = case.name:match("^Ext%.Events%.(.+)$")
            pcall(function ()
                RealExt.Events[eventName]:Subscribe(function (e)
                    local p = ReadJson(ProgressPath(Context))
                    if type(p) == "table" and p.Phase == "reset" and p.Stage == "requested" then
                        if type(p.Seen) ~= "table" then p.Seen = {} end
                        p.Seen[case.name] = CheckEvent(case, e)
                        pcall(RealExt.IO.SaveFile, ProgressPath(Context), RealExt.Json.Stringify(p))
                    end
                end)
            end)
        end
    end

    local ok, err = pcall(RealExt.Debug.Reset, Context == "server", Context == "client")
    if not ok then
        ReportResetLost(run, nil, first, "error", "Ext.Debug.Reset raised: " .. tostring(err),
            "no Lua reset (Ext.Debug.Reset raised)", "no reset")
        return
    end
    -- Still running here after a while: the reset did not come.
    local started, ticks, sub = Now(), 0, nil
    sub = RealExt.Events.Tick:Subscribe(function ()
        ticks = ticks + 1
        local now = Now()
        local late = (started ~= nil and now ~= nil) and (now - started > RESET_TIMEOUT_MS) or (ticks * 33 > RESET_TIMEOUT_MS)
        if not late then return end
        RealExt.Events.Tick:Unsubscribe(sub)
        local p = ReadJson(ProgressPath(Context))
        ReportResetLost(run, type(p) == "table" and p.Seen or nil, first, "error",
            "Ext.Debug.Reset returned, but this Lua state was not reset within " .. (RESET_TIMEOUT_MS // 1000) .. " s",
            "no Lua reset came", "no reset")
    end)
end

-- In the state the reset made: the reset phase's cases, then its events.
local function RunResetPhase(run, progress, decision)
    local carry = type(progress.Carry) == "table" and progress.Carry or {}
    local seen = type(progress.Seen) == "table" and progress.Seen or {}
    local attempt = math.tointeger(progress.Attempt) or 1
    local crashed = decision.crashed ~= nil and run.resetList[decision.crashed] or nil
    Emit("reset-begin", runKey.RunId, #run.resetList,
        crashed and ("resume after " .. crashed.name) or "after the Lua reset")
    if crashed ~= nil then
        Result(crashed, "crashed", decision.why == "reset" and "the Lua state was reset inside this case"
            or "the previous launch ended inside this case")
    end

    local index = decision.first
    local tickSub
    local function Step()
        ExpirePending(false)
        local stop = math.min(index + CASES_PER_TICK - 1, #run.resetList)
        while index <= stop do
            local case = run.resetList[index]
            SaveProgress({ Phase = "reset", Stage = "running", ResetNext = index, Attempt = attempt, Carry = carry, Seen = seen })
            local entry = carry[case.name]
            if type(entry) == "table" and entry.skip ~= nil then
                Result(case, "skip", tostring(entry.skip))
            elseif type(entry) == "table" and entry.error ~= nil then
                Result(case, "error", "its part before the reset failed: " .. tostring(entry.error))
            else
                RunCase(case, type(entry) == "table" and entry.value or nil)
            end
            index = index + 1
        end
        if index > #run.resetList and pendingCount == 0 then
            RealExt.Events.Tick:Unsubscribe(tickSub)
            SaveProgress({ Phase = "reset", Stage = "events", Attempt = attempt, Seen = seen })
            ReportEvents(run.resetEvents, seen, nil)
            EndReset()
        end
    end
    tickSub = RealExt.Events.Tick:Subscribe(function ()
        local ok, err = xpcall(Step, debug.traceback)
        if not ok then
            RealExt.Events.Tick:Unsubscribe(tickSub)
            RunnerFailed(run, err)
        end
    end)
end

-- --- One run -------------------------------------------------------------------

-- how: "running" or "reset" (Runner.Decide).
function Runner.Run(trigger, reason, how)
    if running then return end
    running = true
    trigger = trigger or { RunId = "console" }
    C.trigger = trigger
    local run = Runner.Prepare(trigger)
    runKey = { RunId = trigger.RunId, Plan = run.fingerprint }
    local ordered = run.ordered
    local progress = ReadJson(ProgressPath(Context))
    local decision = Runner.Decide(progress, trigger.RunId, run.fingerprint, how or "running")
    if decision.action ~= "fresh" then AdoptTotals(progress) end

    if decision.action == "none" then
        running = false
        return
    elseif decision.action == "finished" then
        -- Written again, as this launch's word that the run is over: the
        -- host's client waits for it (Runner.Bootstrap).
        SaveProgress({ Finished = true })
        running = false
        Emit("-", "already-finished", "skip", "run " .. trigger.RunId .. " finished before; change RunId to run again")
        return
    elseif decision.action == "run-reset" then
        Guarded(run, RunResetPhase, run, progress, decision)
        return
    elseif decision.action == "request-reset" then
        local crashed = run.resetList[decision.crashed]
        Guarded(run, RequestReset, run, decision.first, decision.crashed, (math.tointeger(progress.Attempt) or 1) + 1,
            "again, after " .. (crashed and crashed.name or "?") .. " ended the previous launch")
        return
    elseif decision.action == "reset-crashed" then
        local head = run.resetList[decision.first]
        if decision.again then
            Guarded(run, ReportResetLost, run, progress.Seen, decision.first, "crashed", "the launch before the last ended inside this case",
                "the Lua reset made again after " .. (head and head.name or "?") .. " ended the launch too",
                "resume after the Lua reset")
        else
            Guarded(run, ReportResetLost, run, progress.Seen, decision.first, "crashed", "the previous launch ended during the Lua reset",
                "the Lua reset did not complete: the launch ended inside it", "resume after the Lua reset")
        end
        return
    elseif decision.action == "events" then
        Guarded(run, function ()
            Emit("reset-begin", trigger.RunId, #run.resetList, "resume after the reset phase's cases")
            ReportEvents(run.resetEvents, progress.Seen, decision.lost)
            EndReset()
        end)
        return
    end

    local indexOf = {}
    for i, case in ipairs(ordered) do indexOf[case] = i end
    -- The asynchronous cases still waiting: a launch that ends gives them no
    -- result, so the next one starts them again, once. Those started again
    -- and still waiting when that launch ends too are reported as crashed.
    local restarted = {}
    local function Waiting()
        local fresh, again = {}, {}
        for case in pairs(pending) do
            local i = indexOf[case]
            if i ~= nil then
                if restarted[i] then again[#again + 1] = i else fresh[#fresh + 1] = i end
            end
        end
        table.sort(fresh)
        table.sort(again)
        return fresh, again
    end
    local function Indices(list)
        local out = {}
        if type(list) ~= "table" then return out end
        for _, w in ipairs(list) do
            local i = math.tointeger(w)
            if i ~= nil and ordered[i] ~= nil then out[#out + 1] = i end
        end
        return out
    end
    -- The cases to start again: i, and whether that is the one time a case
    -- is started again after a launch that ended (counted).
    local rerun = {}
    -- When each waiting case was first started (the wall clock): a case
    -- started again goes on with the time it has left.
    local since = {}

    local first = 1
    if decision.action == "resume-main" and ordered[progress.Next] ~= nil then
        first = progress.Next + 1
        local at = ordered[progress.Next]
        local unreported = progress.NextReported ~= true
        local same = how ~= "reset" and Runner.SameProcess(progress)
        -- A case that ends its session or its launch on purpose
        -- (endsLaunch) has done so, and passes: a load leaves the process
        -- running or not, a crash does not leave it running. The case
        -- after it checks what it was for.
        local function Meant(case)
            if how == "reset" or case.endsLaunch == nil then return false end
            return case.endsLaunch ~= "crash" or not same
        end
        local function PassMeant(case)
            Result(case, "pass", (case.endsLaunch == "crash"
                and "the game closed inside this case, as the case meant it to"
                or "the session was left inside this case by a load, as the case asked")
                .. (case.rows and (" [rows: " .. case.rows .. "]") or ""))
        end
        if same then
            -- The same launch: the session was loaded again and the Lua
            -- states with it. Nothing crashed; the cases that waited are
            -- started again, with the time each has left.
            Emit("begin", trigger.RunId, #ordered, "resume after a session reload, at " .. at.name)
            if type(progress.Since) == "table" then
                for k, v in pairs(progress.Since) do
                    local i = math.tointeger(tonumber(k))
                    if i ~= nil and type(v) == "number" then since[i] = v end
                end
            end
            local listed = {}
            local function Again(i, was)
                if listed[i] then return end
                listed[i] = true
                if Meant(ordered[i]) then
                    PassMeant(ordered[i])
                else
                    if was then restarted[i] = true end
                    rerun[#rerun + 1] = { i = i }
                end
            end
            for _, i in ipairs(Indices(progress.Restarted)) do Again(i, true) end
            for _, i in ipairs(Indices(progress.Waiting)) do Again(i, false) end
            if unreported and not listed[progress.Next] then
                if at.async or Meant(at) then
                    Again(progress.Next, false)
                else
                    Result(at, "crashed", "the session ended inside this case")
                end
            end
            table.sort(rerun, function (a, b) return a.i < b.i end)
        else
            Emit("begin", trigger.RunId, #ordered, "resume after " .. at.name)
            if unreported then
                if Meant(at) then
                    PassMeant(at)
                else
                    Result(at, "crashed", how == "reset" and "the Lua state was reset inside this case"
                        or "the previous launch ended inside this case")
                end
            end
            for _, i in ipairs(Indices(progress.Restarted)) do
                if Meant(ordered[i]) then
                    PassMeant(ordered[i])
                else
                    Result(ordered[i], "crashed", "the launch ended while it waited, after it was started again")
                end
            end
            for _, i in ipairs(Indices(progress.Waiting)) do
                if i ~= progress.Next or not unreported then
                    if Meant(ordered[i]) then
                        PassMeant(ordered[i])
                    else
                        rerun[#rerun + 1] = { i = i, counted = true }
                    end
                end
            end
        end
    else
        Emit("begin", trigger.RunId, #ordered, reason or "")
        local okV, version = pcall(RealExt.Utils.Version)
        local okG, game = pcall(RealExt.Utils.GameVersion)
        Emit("version", okV and tostring(version) or "?", okG and tostring(game) or "?")
        -- The settings this launch started with, as the trigger says (the
        -- coordinator writes them with the settings files: Mac/Tests/InGame/
        -- Settings): one line each.
        local settings = type(trigger.Settings) == "table" and trigger.Settings or nil
        if settings ~= nil and type(settings.Values) == "table" then
            local keys = {}
            for k in pairs(settings.Values) do keys[#keys + 1] = tostring(k) end
            table.sort(keys)
            for _, k in ipairs(keys) do Emit("settings", tostring(settings.Name or "?"), k, tostring(settings.Values[k])) end
        end
        -- What the checker expects a result line for, should the game end
        -- before them.
        for _, case in ipairs(ordered) do Emit(case.id or "-", case.name, "planned", case.risk) end
        for _, case in ipairs(run.mainEvents) do Emit(case.id or "-", case.name, "planned", "event") end
        for _, case in ipairs(run.resetList) do Emit(case.id or "-", case.name, "planned", run.resetOff and case.risk or "reset") end
        for _, case in ipairs(run.resetEvents) do Emit(case.id or "-", case.name, "planned", run.resetOff and "event" or "reset") end
        -- The registered functions no case names: against every case of
        -- this context, since a case the trigger leaves out still names its
        -- function (and that function's other names, Ext.ServerStats for
        -- Ext.Stats); of those, the ones the trigger asks about.
        local uncovered = {}
        for _, name in ipairs(WITH_COVERAGE and Runner.Uncovered(run.all) or {}) do
            if run.filter({ name = name }) then uncovered[#uncovered + 1] = name end
        end
        for _, name in ipairs(uncovered) do
            Result({ name = name }, "uncovered", "registered, but no case of this context names it")
        end
        Emit("plan", #ordered, #uncovered)
    end

    local index = first
    -- The progress as it stands: next is the last case started; reported,
    -- that it has its result or waits (and then is in Waiting).
    local function Save(next, reported)
        local fresh, again = Waiting()
        local started = {}
        for case in pairs(pending) do
            local i = indexOf[case]
            if i ~= nil and since[i] ~= nil then started[tostring(i)] = since[i] end
        end
        pendingChanged = false
        SaveProgress({ Next = next, NextReported = reported, Waiting = fresh, Restarted = again, Since = started })
    end
    -- Starts a case; one that waits keeps when it was first started, and
    -- has that much less of its time left when it is started again.
    local function Start(i)
        local case = ordered[i]
        RunCase(case)
        local entry = pending[case]
        if entry == nil then
            since[i] = nil
            return
        end
        local now = Epoch()
        since[i] = since[i] or now
        if now ~= nil and since[i] ~= nil and entry.started ~= nil and now > since[i] then
            entry.started = entry.started - (now - since[i]) * 1000
        end
    end
    local tickSub
    local finishing = nil
    local finishTicks = (run.maxTimeout // 16) + 300
    tickSub = RealExt.Events.Tick:Subscribe(function ()
        if finishing ~= nil then
            ExpirePending(false)
            if pendingChanged then Save(#ordered, true) end
            finishing = finishing + 1
            if pendingCount == 0 or finishing > finishTicks then
                ExpirePending(true)
                ReportEvents(run.mainEvents, nil, nil)
                if run.resetOff ~= nil then
                    for _, case in ipairs(run.resetList) do Result(case, "skip", run.resetOff) end
                    ReportEvents(run.resetEvents, nil, run.resetOff)
                end
                Emit("end", counts.pass, counts.error, counts.skip, counts.crashed, counts.uncovered)
                RealExt.Events.Tick:Unsubscribe(tickSub)
                if run.resetOff == nil and #run.resetList + #run.resetEvents > 0 then
                    Guarded(run, RequestReset, run, 1, nil, 1, "after the main phase")
                else
                    FinishRun()
                end
            end
            return
        end
        ExpirePending(false)
        local ran = false
        if #rerun > 0 then
            local list = rerun
            rerun = {}
            -- Written before they start: a launch that ends inside one finds
            -- it among those already started again, and does not start it a
            -- third time.
            local fresh, again, started = {}, {}, {}
            for _, r in ipairs(list) do
                if r.counted then restarted[r.i] = true end
                local to = restarted[r.i] and again or fresh
                to[#to + 1] = r.i
                if since[r.i] ~= nil then started[tostring(r.i)] = since[r.i] end
            end
            SaveProgress({ Next = index - 1, NextReported = true, Waiting = fresh, Restarted = again, Since = started })
            for _, r in ipairs(list) do Start(r.i) end
            ran = true
        end
        local stop = math.min(index + CASES_PER_TICK - 1, #ordered)
        while index <= stop do
            local case = ordered[index]
            -- A case that waits for the owner runs alone: it starts once
            -- nothing waits, and nothing starts while it waits.
            if ExclusiveWaiting() or (case.exclusive and pendingCount > 0) then break end
            -- Written before the case: a launch that ends inside it leaves
            -- this, and the next one reports the case as crashed.
            Save(index, nil)
            Start(index)
            index = index + 1
            ran = true
        end
        if ran or pendingChanged then Save(index - 1, true) end
        if index > #ordered then
            ProvokeEvents(run.mainEvents)
            finishing = 0
        end
    end)
end

-- Whether this is the client of a machine that joined another's game: no
-- server runs here (Ext.Net.IsHost, ClientNet.inl: the server's state is
-- Uninitialized on such a machine).
function Runner.IsRemote()
    if Context ~= "client" then return false end
    local ok, host = pcall(function () return RealExt.Net.IsHost() end)
    return ok and host == false
end

-- The level this context's player is in, and whether it is one the players
-- play in: not a level a loaded module names for character creation, the
-- menu, the lobby or the photo booth (ModuleInfo, GameDefinitions/Module.h).
-- nil while no player's character and level can be read.
function Runner.World()
    local ok, level = pcall(function ()
        local entity = nil
        if Context == "server" then
            entity = RealExt.Entity.Get(Osi.GetHostCharacter())
        else
            local list = RealExt.Entity.GetAllEntitiesWithComponent("ClientControl")
            for _, e in pairs(list) do
                if e.ClientCharacter ~= nil and e.ClientCharacter.ReservedUserID == 1 then
                    entity = e
                    break
                end
            end
            if entity == nil then entity = list[1] end
        end
        local name = entity ~= nil and entity.Level ~= nil and entity.Level.LevelName or nil
        return name ~= nil and tostring(name) or nil
    end)
    if not ok or level == nil or level == "" then return nil, false end
    local other = {}
    pcall(function ()
        for _, uuid in ipairs(RealExt.Mod.GetLoadOrder()) do
            pcall(function ()
                local info = RealExt.Mod.GetMod(uuid).Info
                for _, field in ipairs({ "CharacterCreationLevelName", "MenuLevelName", "LobbyLevelName", "PhotoBoothLevelName" }) do
                    local name = info[field]
                    if name ~= nil and tostring(name) ~= "" then other[tostring(name)] = true end
                end
            end)
        end
    end)
    return level, not other[level]
end

-- --- Start-up --------------------------------------------------------------------

-- Called by the bootstraps. Registers the console command (it runs nothing by
-- itself) and, only when the trigger file asks for a run, subscribes to the
-- events and starts at the first Running state of the session, or, in a Lua
-- state that a reset made, once the reset has completed.
function Runner.Bootstrap(modExt)
    if modExt ~= nil then
        ModExt = modExt
        Runner.ModExt = modExt
    end
    pcall(function ()
        ModExt.RegisterConsoleCommand(COMMAND, function ()
            Runner.Run(Runner.ReadTrigger() or { RunId = "console" }, "console command", "running")
        end)
    end)

    local trigger = Runner.ReadTrigger()
    if trigger == nil then return end
    C.trigger = trigger

    -- What a library needs set up while the mod bootstraps (the Net round trip).
    for _, library in ipairs(libraries) do
        if library.bootstrap ~= nil then
            local ok, err = pcall(library.bootstrap, C, RealExt, ModExt)
            if not ok then
                C.bootstrapErrors = C.bootstrapErrors or {}
                C.bootstrapErrors[library.name] = tostring(err)
            end
        end
    end

    -- For the Ext.RegisterModEvent case: allowed only during the bootstrap.
    if pcall(ModExt.RegisterModEvent, C.ModTable, "Probe") then
        C.modEventRegistered = true
    end
    Runner.SubscribeEvents()
    local started = false

    -- A Lua reset in a running session throws ResetCompleted in the new state
    -- after its bootstrap (ScriptExtender::ResetLuaState); GameStateChanged
    -- does not come again. The run goes on at the next tick, out of the reset.
    RealExt.Events.ResetCompleted:Subscribe(function ()
        if started then return end
        started = true
        local sub
        sub = RealExt.Events.Tick:Subscribe(function ()
            RealExt.Events.Tick:Unsubscribe(sub)
            Runner.Run(trigger, "after the Lua reset", "reset")
        end)
    end)

    -- where: what a two-machine run adds about the level it starts in.
    local function Begin(where)
        if Context == "server" then
            Runner.Run(trigger, "server reached Running" .. where, "running")
            return
        end
        -- The client of a machine that only joined a game has no server of
        -- its own to wait for; in a two-machine run (the trigger's Extras
        -- names "Multiplayer") the host's client does not wait either: its
        -- cases and the server's talk to each other.
        -- A run with the save-and-load phase ("SaveLoad" in Extras) does not
        -- wait either: the server's part waits for the load that the
        -- client's window asks the owner for.
        if Runner.IsRemote() or C.optedIn("Multiplayer") or C.optedIn("SaveLoad") then
            Runner.Run(trigger, (Runner.IsRemote() and "client of a machine that is not the host"
                or C.optedIn("Multiplayer") and "client of the host, in a two-machine run"
                or "client of the host, in a run with the save-and-load phase") .. where, "running")
            return
        end
        -- The client starts once the server's run has finished, so that a fault
        -- is put down to the context that was running. The server's word
        -- must be this launch's: a progress file an earlier launch left
        -- finished says nothing of a server that starts again now, with
        -- another plan under the same run id.
        local waited = 0
        local sub
        local startedAt = Now()
        sub = RealExt.Events.Tick:Subscribe(function ()
            waited = waited + 1
            if waited % 60 ~= 0 then return end
            local server = ReadJson(ProgressPath("server"))
            local now = Now()
            local timedOut = (startedAt ~= nil and now ~= nil and now - startedAt > SERVER_WAIT_MS) or waited > 60 * 60 * 30
            if (type(server) == "table" and server.RunId == trigger.RunId and server.Finished and Runner.SameProcess(server))
                or timedOut then
                RealExt.Events.Tick:Unsubscribe(sub)
                Runner.Run(trigger, timedOut and "client: the server run did not finish in time" or "client after the server run", "running")
            end
        end)
    end

    RealExt.Events.GameStateChanged:Subscribe(function (e)
        -- As Norbyte's -ext:servertest waits for it (BuiltinLibraryServer.lua).
        if started or not (e.ToState == "Running" or tostring(e.ToState) == "Running") then return end
        started = true
        if not C.optedIn("Multiplayer") then
            Begin("")
            return
        end
        -- A two-machine run starts in a level the players play in: a new
        -- game reaches Running in character creation first, and the session
        -- is loaded again on the way into the world.
        -- unknownSince, unknownTicks: for how long no level could be read.
        local ticks, unknownSince, unknownTicks, sub = 0, nil, 0, nil
        sub = RealExt.Events.Tick:Subscribe(function ()
            ticks = ticks + 1
            if ticks % 30 ~= 1 then return end
            local level, playable = Runner.World()
            local now = Now()
            local late = false
            if level ~= nil then
                unknownSince, unknownTicks = nil, 0
            else
                unknownSince = unknownSince or now
                unknownTicks = unknownTicks + 30
                if unknownSince ~= nil and now ~= nil then
                    late = now - unknownSince > WORLD_WAIT_MS
                else
                    late = unknownTicks * 33 > WORLD_WAIT_MS
                end
            end
            if not (playable or late) then return end
            RealExt.Events.Tick:Unsubscribe(sub)
            Begin(playable and (", in level " .. level)
                or (", no level could be read in " .. (WORLD_WAIT_MS // 1000) .. " s"))
        end)
    end)
end

-- --- The offline harness (Mac/Tests/StartupTests.cpp) ------------------------------

-- No session, no game: only the cases marked offline, run at once; the result
-- is the number of failures (errors and uncovered functions).
function Runner.RunOffline()
    C.offline = true
    local plan = Runner.Plan(function (case) return case.offline == true end)
    local all = Runner.Plan(nil)
    Emit("begin", "offline", #plan, "offline harness")
    for _, case in ipairs(plan) do Emit(case.id or "-", case.name, "planned", case.risk) end
    local uncovered = Runner.Uncovered(all)
    for _, name in ipairs(uncovered) do
        Result({ name = name }, "uncovered", "registered, but no case of this context names it")
    end
    for _, case in ipairs(plan) do
        if case.async then
            Result(case, "skip", "asynchronous: needs ticks")
        else
            RunCase(case)
        end
    end
    Emit("end", counts.pass, counts.error, counts.skip, counts.crashed, counts.uncovered)
    return counts.error + counts.uncovered
end

-- Outside a mod (the offline harness loads the files itself) the case files
-- hand their library over through this global, and the harness's last chunk
-- (Mac/Tests/InGame/Offline.lua) runs them.
if rawget(_G, "ModuleUUID") == nil then
    Bg3mseIgtOffline = Runner.AddLibrary
    Bg3mseIgtRunOffline = Runner.RunOffline
    Bg3mseIgtOfflineEvents = Runner.SetEvents
end

return Runner
