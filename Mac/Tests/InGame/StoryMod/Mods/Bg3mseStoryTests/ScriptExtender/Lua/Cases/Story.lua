-- The story tests: the goal this mod ships (Story/RawFiles/Goals/
-- Bg3mseStoryTests.txt) against Norbyte's story extension. The goal is
-- compiled into the story of a new game when the extender's settings have
-- ExtendStory on (and DisableStoryCompilation off): Norbyte's compile hook
-- runs the preprocessor (this mod's "Preprocessor" flag: the EXTENDER_ONLY
-- parts are kept, the NO_EXTENDER part is dropped) and the header hook adds
-- his functions to story_header.div (this mod's "Osiris" flag)
-- (Osiris/Shared/CustomFunctions.cpp). Each case checks one direction:
--   Lua -> story   a database insert, Get and Delete; a procedure; a query
--                  of the goal's own that a rule calls, and one that none
--                  calls (an error); a game call (TimerLaunch)
--   story -> Lua   NRD_LuaCall from a rule; NRD_LuaQuery1 with an answer
--                  and without one
--   listeners      Ext.Osiris.RegisterListener before/after on an insert,
--                  beforeDelete/afterDelete on a delete, on a procedure, a
--                  query and a game event
-- Every fact a case inserts is deleted again; the story keeps only the
-- goal's own INIT fact. Nothing is saved: the owner never saves this game.

local library = { name = "Story" }

-- What the story's calls into Lua saw (Mods.Bg3mseStoryTests, set up at the
-- bootstrap: NRD_LuaCall and NRD_LuaQuery look functions up there).
local calls = { Ping = {}, Timer = {}, Answer = {}, IsYes = {} }

function library.bootstrap(C, E, M)
    if not E.IsServer() then return end
    local mod = Mods[C.ModTable]
    mod.OnPing = function (value) calls.Ping[#calls.Ping + 1] = value end
    mod.OnTimer = function (name) calls.Timer[#calls.Timer + 1] = name end
    mod.Answer = function (value)
        calls.Answer[#calls.Answer + 1] = value
        return value .. "-answer"
    end
    mod.IsYes = function (value)
        calls.IsYes[#calls.IsYes + 1] = value
        return value == "yes"
    end
end

function library.define(C)
    local function Has(list, value)
        for _, v in ipairs(list) do if v == value then return true end end
        return false
    end
    local function Rows(db, ...)
        return Osi[db]:Get(...) or {}
    end
    -- The goal's story is there and was compiled by the extender's hooks.
    local function NeedGoal()
        local ok, rows = pcall(Rows, "DB_Bg3mseStory_Init", nil)
        if not ok or #rows == 0 then
            C.skip("the goal is not in this story: its INIT fact DB_Bg3mseStory_Init(1) is missing (a new game started with the mod is needed)")
        end
    end
    local function NeedExtension()
        NeedGoal()
        if rawequal(Osi.NRD_LuaCall, nil) or not Osi.NRD_LuaCall:Exists(3) then
            C.skip("the story has no NRD_LuaCall/3: it was not compiled with ExtendStory")
        end
    end
    local function server(name, id, run, extra)
        local c = { name, id = id, ctx = "server", risk = "unproved", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end

    return {
        { "Story#client", id = "OSIF-01", ctx = "client", risk = "pure", run = function (E)
            -- The story is the server's: no Ext.Osiris on the client
            -- (LuaServer.cpp registers it for the server only).
            C.equal(rawget(E, "Osiris"), nil, "Ext.Osiris on the client")
            return "no Ext.Osiris on the client"
        end },

        server("Story#compiled", "OSIF-14", function (E)
            NeedGoal()
            local nrd = not rawequal(Osi.NRD_LuaCall, nil) and Osi.NRD_LuaCall:Exists(3)
            -- A name the story does not have is nil in Osi (LuaNameResolver.inl):
            -- with the preprocessor, this database is never compiled.
            local withoutDb = Osi.DB_Bg3mseStory_WithoutExtender
            local without = not rawequal(withoutDb, nil) and withoutDb:Exists(1)
            local parts = {
                "the goal's INIT ran",
                nrd and "NRD_LuaCall/3 is in the story (the header hook added it)" or "NRD_LuaCall/3 is NOT in the story",
                without and "the NO_EXTENDER part was compiled (no preprocessing)" or "the NO_EXTENDER part was dropped (the compile hook preprocessed it)",
            }
            C.check(nrd and not without, "%s", table.concat(parts, "; "))
            return table.concat(parts, "; ")
        end),

        server("Osi.NRD_LuaCall#fromStory", "OSIF-14", function (E)
            NeedExtension()
            local before = #calls.Ping
            Osi.DB_Bg3mseStory_Ping("bg3mse-ping")
            local pong = #Rows("DB_Bg3mseStory_Pong", "bg3mse-ping")
            Osi.DB_Bg3mseStory_Ping:Delete("bg3mse-ping")
            Osi.DB_Bg3mseStory_Pong:Delete("bg3mse-ping")
            C.equal(pong, 1, "DB_Bg3mseStory_Pong rows for the ping")
            C.check(#calls.Ping == before + 1 and calls.Ping[#calls.Ping] == "bg3mse-ping",
                "NRD_LuaCall did not reach OnPing with the value (calls: %s)", table.concat(calls.Ping, ","))
            return "insert -> rule -> NRD_LuaCall -> Mods.Bg3mseStoryTests.OnPing(\"bg3mse-ping\")"
        end),

        server("Osi.NRD_LuaQuery1#answer", "OSIF-15", function (E)
            NeedExtension()
            Osi.DB_Bg3mseStory_Ask("bg3mse-ask")
            local rows = Rows("DB_Bg3mseStory_Answer", "bg3mse-ask", nil)
            Osi.DB_Bg3mseStory_Ask:Delete("bg3mse-ask")
            for _, row in ipairs(rows) do Osi.DB_Bg3mseStory_Answer:Delete(row[1], row[2]) end
            C.check(Has(calls.Answer, "bg3mse-ask"), "the query did not reach Answer")
            C.equal(#rows, 1, "DB_Bg3mseStory_Answer rows")
            C.equal(rows[1][2], "bg3mse-ask-answer", "the query's out value")
            return "the rule's NRD_LuaQuery1 gave " .. tostring(rows[1][2])
        end),

        server("Osi.NRD_LuaQuery1#boolean", "OSIF-15", function (E)
            NeedExtension()
            Osi.DB_Bg3mseStory_Check("yes")
            Osi.DB_Bg3mseStory_Check("no")
            local yes = #Rows("DB_Bg3mseStory_Checked", "yes")
            local no = #Rows("DB_Bg3mseStory_Checked", "no")
            Osi.DB_Bg3mseStory_Check:Delete("yes")
            Osi.DB_Bg3mseStory_Check:Delete("no")
            Osi.DB_Bg3mseStory_Checked:Delete("yes")
            C.check(Has(calls.IsYes, "yes") and Has(calls.IsYes, "no"), "the query did not reach IsYes for both")
            C.equal(yes, 1, "the rule's result for a true answer")
            C.equal(no, 0, "the rule's result for a false answer")
        end),

        server("Osi#DatabaseListener", "OSIF-07", function (E)
            NeedGoal()
            local order = {}
            local ids = {}
            for _, when in ipairs({ "before", "after", "beforeDelete", "afterDelete" }) do
                ids[#ids + 1] = E.Osiris.RegisterListener("DB_Bg3mseStory_Note", 1, when, function (value)
                    order[#order + 1] = when .. ":" .. tostring(value)
                end)
            end
            -- The database the story's own rule fills (the goal's KB section:
            -- DB_Bg3mseStory_Note inserts into DB_Bg3mseStory_Noted): a change
            -- Osiris makes, not Lua.
            local byStory = {}
            ids[#ids + 1] = E.Osiris.RegisterListener("DB_Bg3mseStory_Noted", 1, "after", function (value)
                byStory[#byStory + 1] = tostring(value)
            end)
            Osi.DB_Bg3mseStory_Note("bg3mse-note")
            local noted = #Rows("DB_Bg3mseStory_Noted", "bg3mse-note")
            Osi.DB_Bg3mseStory_Note:Delete("bg3mse-note")
            Osi.DB_Bg3mseStory_Noted:Delete("bg3mse-note")
            for _, id in ipairs(ids) do E.Osiris.UnregisterListener(id) end
            C.equal(noted, 1, "the rule on DB_Bg3mseStory_Note ran")
            local want = "before:bg3mse-note,after:bg3mse-note,beforeDelete:bg3mse-note,afterDelete:bg3mse-note"
            C.equal(table.concat(order, ","), want, "the listeners' calls")
            C.equal(table.concat(byStory, ","), "bg3mse-note", "the listener on the database the story's rule fills")
            return want .. "; DB_Bg3mseStory_Noted's listener saw the story's own insert [rows: G-084, W1-04]"
        end),

        server("Osi#DatabaseGetDelete", "OSIF-05", function (E)
            NeedGoal()
            Osi.DB_Bg3mseStory_Note("bg3mse-a")
            Osi.DB_Bg3mseStory_Note("bg3mse-b")
            local all = #Rows("DB_Bg3mseStory_Note", nil)
            local one = #Rows("DB_Bg3mseStory_Note", "bg3mse-a")
            Osi.DB_Bg3mseStory_Note:Delete("bg3mse-a")
            Osi.DB_Bg3mseStory_Note:Delete("bg3mse-b")
            Osi.DB_Bg3mseStory_Noted:Delete("bg3mse-a")
            Osi.DB_Bg3mseStory_Noted:Delete("bg3mse-b")
            local after = #Rows("DB_Bg3mseStory_Note", nil)
            C.equal(all, 2, "rows with nil for the column")
            C.equal(one, 1, "rows for one value")
            C.equal(after, 0, "rows after deleting both")
            return "two tuples inserted from Lua, read back by Get, deleted [rows: W1-04, OSIF-26]"
        end),

        server("Osi#Procedure", "OSIF-04", function (E)
            NeedGoal()
            local order = {}
            local before = E.Osiris.RegisterListener("PROC_Bg3mseStory_Proc", 1, "before", function (v) order[#order + 1] = "before:" .. tostring(v) end)
            local after = E.Osiris.RegisterListener("PROC_Bg3mseStory_Proc", 1, "after", function (v) order[#order + 1] = "after:" .. tostring(v) end)
            Osi.PROC_Bg3mseStory_Proc("bg3mse-proc")
            E.Osiris.UnregisterListener(before)
            E.Osiris.UnregisterListener(after)
            local ran = #Rows("DB_Bg3mseStory_ProcRan", "bg3mse-proc")
            Osi.DB_Bg3mseStory_ProcRan:Delete("bg3mse-proc")
            C.equal(ran, 1, "the procedure's rule ran")
            C.equal(table.concat(order, ","), "before:bg3mse-proc,after:bg3mse-proc", "the procedure's listeners")
            return "PROC_Bg3mseStory_Proc called from Lua ran its rule [rows: W1-04]"
        end),

        server("Osi#UserQuery", "OSIF-04", function (E)
            NeedGoal()
            local seen = 0
            local id = E.Osiris.RegisterListener("QRY_Bg3mseStory_IsInit", 1, "after", function () seen = seen + 1 end)
            local yes = Osi.QRY_Bg3mseStory_IsInit(1)
            local no = Osi.QRY_Bg3mseStory_IsInit(2)
            E.Osiris.UnregisterListener(id)
            local ran = #Rows("DB_Bg3mseStory_QueryRan", 1)
            Osi.DB_Bg3mseStory_QueryRan:Delete(1)
            C.equal(yes, true, "QRY_Bg3mseStory_IsInit(1)")
            C.equal(no, false, "QRY_Bg3mseStory_IsInit(2)")
            C.equal(ran, 1, "the query's THEN ran once")
            return "the query's listener ran " .. seen .. " time(s) [rows: W1-04]"
        end),

        server("Osi#UserQueryUncalled", "OSIF-04", function (E)
            NeedGoal()
            C.equal(Osi.QRY_Bg3mseStory_Uncalled:Exists(1), true, "QRY_Bg3mseStory_Uncalled/1 in the story")
            local ok, err = pcall(Osi.QRY_Bg3mseStory_Uncalled, 1)
            C.equal(ok, false, "QRY_Bg3mseStory_Uncalled(1) raised an error")
            C.equal(tostring(err):find("has no node in the story", 1, true) ~= nil, true, "the error of QRY_Bg3mseStory_Uncalled(1), " .. tostring(err) .. ",")
            C.equal(#Rows("DB_Bg3mseStory_QueryRan", 1), 0, "rows of DB_Bg3mseStory_QueryRan")
            return "a query no rule calls raised a Lua error: " .. tostring(err)
        end),

        server("Osi#GameEvent", "OSIF-06", function (E, M, done)
            NeedGoal()
            local timersBefore = #calls.Timer
            local listened = false
            local id
            id = E.Osiris.RegisterListener("TimerFinished", 1, "after", function (name)
                if name ~= "Bg3mseStoryTimer" then return end
                E.Osiris.UnregisterListener(id)
                listened = true
                -- The goal's rule has run by now (the listener is "after").
                local seen = #Rows("DB_Bg3mseStory_TimerSeen", 1)
                Osi.DB_Bg3mseStory_TimerSeen:Delete(1)
                local nrd = #calls.Timer > timersBefore
                local extended = not rawequal(Osi.NRD_LuaCall, nil) and Osi.NRD_LuaCall:Exists(3)
                if seen ~= 1 then
                    done(false, "the goal's TimerFinished rule did not run")
                elseif extended and not nrd then
                    done(false, "the rule ran, but its NRD_LuaCall did not reach OnTimer")
                else
                    done(true, "the rule ran" .. (extended and ", and called OnTimer through NRD_LuaCall" or " (no extension: no NRD_LuaCall)"))
                end
            end)
            Osi.TimerLaunch("Bg3mseStoryTimer", 100)
        end, { async = true }),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
