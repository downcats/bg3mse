-- Ext.Osiris (BG3Extender/Lua/Server/LuaServer.cpp) and the Osi table
-- (Lua/Osiris/*). Server only. The story is touched through a story timer of
-- this mod's own name, 100 ms long, and read-only queries; Norbyte's
-- OsirisTests.lua (which deletes from DB_Players) is WP-24's and not run.

local library = { name = "Osiris" }

function library.define(C)
    local TIMER = "Bg3mseIgtTimer"
    return {
        { "Ext.Osiris.RegisterListener", id = "OSIF-06", ctx = "server", async = true, run = function (E, M, done)
            local sawCall = false
            local callId = E.Osiris.RegisterListener("TimerLaunch", 2, "before", function (name, ms)
                if name == TIMER then sawCall = true end
            end)
            local eventId
            eventId = E.Osiris.RegisterListener("TimerFinished", 1, "after", function (name)
                if name == TIMER then
                    E.Osiris.UnregisterListener(eventId)
                    done(sawCall, sawCall and "call and event listeners ran" or "the event ran, the call listener did not")
                end
            end)
            Osi.TimerLaunch(TIMER, 100)
            E.Osiris.UnregisterListener(callId)
        end },
        { "Ext.Osiris.UnregisterListener", id = "OSIF-09", ctx = "server", run = function (E)
            local id = E.Osiris.RegisterListener("TimerFinished", 1, "after", function () end)
            C.equal(E.Osiris.UnregisterListener(id), true, "UnregisterListener")
        end },
        { "Osi#Query", id = "OSIF-04", ctx = "server", run = function (E)
            local host = Osi.GetHostCharacter()
            C.check(type(host) == "string" and host ~= "", "GetHostCharacter() gave %s", tostring(host))
            return host
        end },
        { "Osi#DatabaseGet", id = "OSIF-05", ctx = "server", run = function (E)
            local host = Osi.GetHostCharacter()
            local rows = Osi.DB_Players:Get(nil)
            -- The story's GUIDSTRINGs may carry a name before the UUID
            -- (Name_xxxxxxxx-...); GetHostCharacter gives the UUID alone.
            local function Uuid(s) return type(s) == "string" and s:sub(-36) or s end
            local found = false
            local seen = {}
            for _, row in ipairs(rows) do
                seen[#seen + 1] = tostring(row[1])
                if Uuid(row[1]) == Uuid(host) then found = true end
            end
            C.check(found, "the host %s is not in DB_Players: %s", host, table.concat(seen, ", "))
        end },
        { "Osi#Metadata", id = "OSIF-26", ctx = "server", run = function (E)
            -- Exists and Type take an arity (Lua/Osiris/FunctionProxy.inl).
            C.equal(Osi.DB_Players:Exists(1), true, "DB_Players:Exists(1)")
            -- Type gives an OsiFunctionType, an enum value (FunctionProxy.inl): its
            -- label names the kind.
            local kind = Osi.DB_Players:Type(1)
            C.equal(kind and kind.Label, "DB", "DB_Players:Type(1).Label")
            C.type(Osi.DB_Players.Arities, "table", "DB_Players.Arities")
            -- Arities lists the arities a name has; InputArities the input
            -- parameters of each, out parameters left out (NameCache.inl:5-20,
            -- FunctionProxy.inl:151-176): the same for a database, one fewer
            -- for the query GetHostCharacter([out] _Character).
            local function List(t) return table.concat(t, ",") end
            C.equal(List(Osi.DB_Players.Arities), "1", "DB_Players.Arities")
            C.equal(List(Osi.DB_Players.InputArities), "1", "DB_Players.InputArities")
            C.equal(List(Osi.GetHostCharacter.Arities), "1", "GetHostCharacter.Arities")
            C.equal(List(Osi.GetHostCharacter.InputArities), "0", "GetHostCharacter.InputArities")
            -- Delete, on a database of the story test mod's own goal that no
            -- rule reads (StoryMod/.../Goals/Bg3mseStoryTests.txt): a row of
            -- this case's is put in, deleted, and read back.
            -- A name the story does not have is nil in Osi (LuaNameResolver.inl:15-35).
            local db = Osi.DB_Bg3mseStory_Pong
            if rawequal(db, nil) or not db:Exists(1) then
                return tostring(kind) .. "; InputArities 1 and 0; Delete not tried: DB_Bg3mseStory_Pong is not in this story (the story test mod)"
            end
            local ROW = "bg3mse-igt-delete-probe"
            Osi.DB_Bg3mseStory_Pong(ROW)
            local before = #db:Get(ROW)
            db:Delete(ROW)
            local after = #db:Get(ROW)
            C.equal(before, 1, "rows after the insert")
            C.equal(after, 0, "rows after Delete")
            return tostring(kind) .. "; InputArities 1 and 0, Delete took the row out [rows: OSIF-26]"
        end },
        { "Osi#NRD_LuaCall", id = "OSIF-14", ctx = "server", risk = "unproved", run = function (E)
            -- NRD_LuaCall is one of the extender's functions added to the
            -- story header (Osiris/Functions/LuaFunctions.cpp), and only when
            -- the configuration's ExtendStory is on (it is off unless set)
            -- and a mod asks for the Osiris feature: a story has it only
            -- once compiled so (a new game, a story merge), on Windows too.
            if rawequal(Osi.NRD_LuaCall, nil) then
                C.skip("this story has no NRD_LuaCall: it needs ExtendStory, a mod with the Osiris feature and a story compiled with both")
            end
            local got = nil
            Mods[C.ModTable].Bg3mseIgtNrdProbe = function (x) got = x end
            Osi.NRD_LuaCall(C.ModTable, "Bg3mseIgtNrdProbe", "hello")
            Mods[C.ModTable].Bg3mseIgtNrdProbe = nil
            C.equal(got, "hello", "the argument NRD_LuaCall passed")
        end },
        { "Osi#DatabaseListener", id = "OSIF-07", ctx = "server",
          skip = "needs a database of a goal; the story mod's Osi#DatabaseListener (Bg3mseStoryTests, a new game) covers it" },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
