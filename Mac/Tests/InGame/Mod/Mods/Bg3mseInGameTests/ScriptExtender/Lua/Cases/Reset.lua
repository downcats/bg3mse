-- The reset phase (Runner.lua): after the main phase each context resets its
-- own Lua state with Ext.Debug.Reset (developer mode only, Debug.inl), and the
-- state the reset makes, bootstrapped again from the same files, runs these.
-- A case's before runs in the state that ends; what it returns is handed to
-- its run in the new one. What a reset keeps and what it rebuilds is
-- Norbyte's ScriptExtender::ResetLuaState and ExtensionStateBase::
-- LuaResetInternal: the Lua state is made anew, the variables of Ext.Vars stay
-- in the extension state, and PersistentVars come back only from a save.
--
-- Also here, offline: the runner's choice of what to do after a crash or a
-- reset (Runner.Decide), which no in-game run can provoke at will.

local library = { name = "Reset" }

function library.define(C)
    local USER = "Bg3mseInGameTestsResetUser"
    local MOD = "Bg3mseInGameTestsResetMod"
    local MARK = "Bg3mseIgtBeforeReset"
    local function UserFlags(E)
        if E.IsServer() then
            return { Server = true, Client = false, Persistent = false, SyncOnTick = false }
        end
        return { Server = false, Client = true, WriteableOnServer = false, WriteableOnClient = true, Persistent = false, SyncOnTick = false }
    end
    local function First(order, name)
        for i, n in ipairs(order) do if n == name then return i end end
        return nil
    end
    local function decide(progress, how)
        return C.decide(progress, "1", "P", how)
    end
    local function Progress(fields)
        local p = { RunId = "1", Plan = "P" }
        for k, v in pairs(fields) do p[k] = v end
        return p
    end
    local function Is(d, action, fields)
        C.equal(d.action, action, "the action")
        for k, v in pairs(fields or {}) do C.equal(d[k], v, k) end
    end

    return {
        { "Ext.Debug.Reset", id = "LUA-037", phase = "reset", resetFirst = true,
            before = function (E)
                -- A global of the state that ends; the new state must not have it.
                rawset(_G, MARK, true)
                return { at = E.Timer.MonotonicTime() }
            end,
            run = function (E, M, carry)
                C.equal(rawget(_G, MARK), nil, "the old state's global in the new state")
                C.type(carry, "table", "what the old state handed over")
                return "the run went on in a new Lua state, " .. (E.Timer.MonotonicTime() - carry.at) .. " ms after asking"
            end },
        { "Ext.Debug.Reset#events", id = "LUA-037", phase = "reset", run = function (E)
            -- ScriptExtender::ResetLuaState: the new state's bootstrap
            -- (ExtensionState::BootstrapLua, the session running) throws
            -- ModuleResume, then the reset's callback throws ModuleResume,
            -- SessionLoading and SessionLoaded (the session is running) and
            -- ResetCompleted.
            local order = C.eventOrder
            local text = table.concat(order, " ", 1, math.min(#order, 12))
            local resume, loading, loaded, done = First(order, "ModuleResume"), First(order, "SessionLoading"),
                First(order, "SessionLoaded"), First(order, "ResetCompleted")
            C.check(resume and loading and loaded and done, "an event is missing: %s", text)
            C.check(resume < loading and loading < loaded and loaded < done, "out of order: %s", text)
            local resumes = 0
            for _, n in ipairs(order) do if n == "ModuleResume" then resumes = resumes + 1 end end
            C.equal(resumes, 2, "ModuleResume thrown by the bootstrap and by the reset (" .. text .. ")")
            return text
        end },
        { "Ext.Vars.RegisterUserVariable#reset", id = "LUA-080", phase = "reset",
            before = function (E)
                E.Vars.RegisterUserVariable(USER, UserFlags(E))
                local character = C.character()
                character.Vars[USER] = 21
                E.Vars.SyncUserVariables()
                return 21
            end,
            run = function (E, M, value)
                E.Vars.RegisterUserVariable(USER, UserFlags(E))
                local character = C.character()
                local back = character.Vars[USER]
                character.Vars[USER] = nil
                E.Vars.SyncUserVariables()
                C.equal(back, value, "the user variable set before the reset")
            end },
        { "Ext.Vars.GetModVariables#reset", id = "LUA-081", phase = "reset",
            before = function (E)
                E.Vars.RegisterModVariable(C.ModuleUUID, MOD, UserFlags(E))
                E.Vars.GetModVariables(C.ModuleUUID)[MOD] = 22
                E.Vars.SyncModVariables()
                return 22
            end,
            run = function (E, M, value)
                E.Vars.RegisterModVariable(C.ModuleUUID, MOD, UserFlags(E))
                local vars = C.notNil(E.Vars.GetModVariables(C.ModuleUUID), "GetModVariables")
                local back = vars[MOD]
                vars[MOD] = nil
                E.Vars.SyncModVariables()
                C.equal(back, value, "the mod variable set before the reset")
            end },
        { "Mods.PersistentVars#reset", id = "LUA-082", ctx = "server", phase = "reset",
            before = function (E)
                -- What a save would write (LuaServer.cpp GetModPersistentVars,
                -- BuiltinLibraryServer.lua _GetModPersistentVars). Nothing is saved.
                Mods[C.ModTable].PersistentVars = { Probe = "bg3mse" }
                local json = E._Internal._GetModPersistentVars(C.ModTable)
                Mods[C.ModTable].PersistentVars = nil
                return json
            end,
            run = function (E, M, json)
                C.type(json, "string", "the old state's PersistentVars as a save takes them")
                -- A reset restores no PersistentVars: only a save's do
                -- (SavegameSerializer -> RestoreModPersistentVars).
                C.equal(Mods[C.ModTable].PersistentVars, nil, "PersistentVars after the reset")
                -- What a load does with a save's: the restore path.
                E._Internal._RestoreModPersistentVars(C.ModTable, json)
                local back = Mods[C.ModTable].PersistentVars
                Mods[C.ModTable].PersistentVars = nil
                C.equal(type(back) == "table" and back.Probe or nil, "bg3mse", "PersistentVars.Probe restored")
                return json
            end },

        -- The runner's decision (pure: no game).
        { "Runner.Decide#fresh", risk = "pure", offline = true, run = function ()
            Is(decide(nil, "running"), "fresh")
            Is(decide(Progress({ RunId = "2" }), "running"), "fresh")
            Is(decide(nil, "reset"), "none")
            Is(decide(Progress({ Finished = true }), "running"), "finished")
            Is(decide(Progress({ Finished = true }), "reset"), "none")
        end },
        { "Runner.Decide#main", risk = "pure", offline = true, run = function ()
            Is(decide(Progress({ Next = 4 }), "running"), "resume-main")
            -- A reset that came in the main phase (typed by hand): go on after the case.
            Is(decide(Progress({ Next = 4 }), "reset"), "resume-main")
        end },
        { "Runner.Decide#reset", risk = "pure", offline = true, run = function ()
            -- The reset the run asked for has come.
            Is(decide(Progress({ Phase = "reset", Stage = "requested", ResetNext = 0 }), "reset"), "run-reset", { first = 1, crashed = nil })
            -- ... asked for again after case 3 ended a launch.
            Is(decide(Progress({ Phase = "reset", Stage = "requested", ResetNext = 3, Crashed = 3 }), "reset"), "run-reset", { first = 4, crashed = 3 })
            -- A case of the reset phase reset the state itself.
            Is(decide(Progress({ Phase = "reset", Stage = "running", ResetNext = 2 }), "reset"), "run-reset", { first = 3, crashed = 2, why = "reset" })
        end },
        { "Runner.Decide#crashed", risk = "pure", offline = true, run = function ()
            -- The launch ended during the reset: report it, do not reset again.
            Is(decide(Progress({ Phase = "reset", Stage = "requested", ResetNext = 0 }), "running"), "reset-crashed", { first = 1, again = false })
            Is(decide(Progress({ Phase = "reset", Stage = "requested", ResetNext = 3, Crashed = 3 }), "running"), "reset-crashed", { first = 3, again = true })
            -- The launch ended inside case 2 of the reset phase: reset again, go on at 3.
            Is(decide(Progress({ Phase = "reset", Stage = "running", ResetNext = 2 }), "running"), "request-reset", { first = 3, crashed = 2 })
            Is(decide(Progress({ Phase = "reset", Stage = "events" }), "running"), "events")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
