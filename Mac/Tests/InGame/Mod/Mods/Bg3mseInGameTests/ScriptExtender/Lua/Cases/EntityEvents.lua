-- Entity events that fire (audit 2, WP-35): a component created and
-- removed on an entity this mod makes, with OnCreate/OnDestroy (immediate
-- and deferred) subscribed and their arguments checked; a replicated
-- component of the host marked for replication on the server, with the
-- server's and the client's OnChange subscribed; a user and a mod variable
-- set on the server and read on the client. Cases/Entity.lua's cases of the
-- same functions only subscribe and unsubscribe; their names stay as they
-- are.
--
-- Norbyte's handlers get (entity, component type, component) for the
-- component events (Lua/Shared/EntityComponentEvents.inl) and (entity,
-- component type, component, flags) for replication
-- (Lua/Client/ClientEntityReplicationEvents.inl, Lua/Server/
-- ServerEntityReplicationEvents.inl).

local library = { name = "EntityEvents" }

-- eoc::ValueComponent (Components/Stats.h): plain data (Value, Rarity,
-- Unique) that no system acts on for an entity with nothing else.
local COMPONENT = "Value"
-- The replicated component the server marks; its value is not changed
-- (Norbyte's ECSTests.lua, TestECSReplication, uses the same).
local REPLICATED = "DisplayName"
local USER_VAR = "Bg3mseIgtToClient"
local MOD_VAR = "Bg3mseIgtModToClient"
-- What the client's bootstrap saw replicated (Runner.bootstrap runs it only
-- with the trigger file).
local seenReplication = {}

local function Uuid(entity)
    local ok, id = pcall(function () return entity.Uuid.EntityUuid end)
    return ok and id or nil
end

function library.bootstrap(C, E, M)
    -- Registered in both contexts before the session runs, as Norbyte's
    -- documentation asks of a synced variable (Vars.inl).
    local flags = { Server = true, Client = true, SyncToClient = true, Persistent = false }
    E.Vars.RegisterUserVariable(USER_VAR, flags)
    E.Vars.RegisterModVariable(C.ModuleUUID, MOD_VAR, flags)
    if E.IsClient() then
        -- Every replication of DisplayName the client gets, the host's
        -- picked out later by its UUID: the client's run starts after the
        -- server's, which is when the server marks it.
        E.Entity.OnChange(REPLICATED, function (entity, type, component, flags)
            local id = Uuid(entity)
            if id ~= nil and seenReplication[id] == nil then
                seenReplication[id] = { type = tostring(type), component = component ~= nil, flags = flags }
            end
        end)
    end
end

function library.define(C)
    local function Rows(detail, rows) return detail .. " [rows: " .. rows .. "]" end

    -- Ticks until test() is true, then done(true, detail()), or done(false) after `ticks`.
    local function Until(E, done, ticks, test, detail, failure)
        local n, sub = 0, nil
        sub = E.Events.Tick:Subscribe(function ()
            n = n + 1
            local ok, yes = pcall(test)
            if not ok then
                E.Events.Tick:Unsubscribe(sub)
                done(false, tostring(yes))
            elseif yes then
                E.Events.Tick:Unsubscribe(sub)
                done(true, detail(n))
            elseif n > ticks then
                E.Events.Tick:Unsubscribe(sub)
                done(false, failure(n))
            end
        end)
    end

    return {
        { "Ext.Entity.OnCreate#fired", id = "LUA-057", risk = "unproved", async = true, timeout = 20000, run = function (E, M, done)
            local entity = C.notNil(E.Entity.Create(), "Create()")
            local seen = { create = {}, destroy = {} }
            local subs = {}
            local function Record(list, kind)
                return function (e, type, component)
                    if e == entity then
                        list[#list + 1] = { kind = kind, type = tostring(type), value = component and component.Value }
                    end
                end
            end
            subs[#subs + 1] = E.Entity.OnCreate(COMPONENT, Record(seen.create, "immediate"))
            subs[#subs + 1] = E.Entity.OnCreateDeferred(COMPONENT, Record(seen.create, "deferred"))
            subs[#subs + 1] = E.Entity.OnDestroy(COMPONENT, Record(seen.destroy, "immediate"))
            subs[#subs + 1] = E.Entity.OnDestroyDeferred(COMPONENT, Record(seen.destroy, "deferred"))
            local function Over(ok, detail)
                for _, s in ipairs(subs) do E.Entity.Unsubscribe(s) end
                pcall(E.Entity.Destroy, entity)
                done(ok, detail)
            end
            local stage, ticks = "alive", 0
            local sub
            sub = E.Events.Tick:Subscribe(function ()
                ticks = ticks + 1
                local ok, err = pcall(function ()
                    if stage == "alive" and entity:IsAlive() then
                        local component = C.notNil(entity:CreateComponent(COMPONENT), "CreateComponent('" .. COMPONENT .. "')")
                        component.Value = 42
                        stage = "created"
                    elseif stage == "created" and #seen.create >= 2 then
                        C.equal(entity:HasRawComponent("eoc::ValueComponent"), true, "the component after creation")
                        C.equal(entity.Value.Value, 42, "the component's Value")
                        entity:RemoveComponent(COMPONENT)
                        stage = "removed"
                    elseif stage == "removed" and #seen.destroy >= 2 then
                        stage = "over"
                    end
                end)
                if not ok or stage == "over" or ticks > 300 then
                    E.Events.Tick:Unsubscribe(sub)
                    local function List(l)
                        local parts = {}
                        for _, x in ipairs(l) do parts[#parts + 1] = x.kind .. " of " .. x.type .. (x.value and (", Value " .. tostring(x.value)) or "") end
                        return table.concat(parts, ", ")
                    end
                    if not ok then return Over(false, err) end
                    if stage ~= "over" then
                        return Over(false, string.format("stuck at %s after %d ticks; created: %s; destroyed: %s",
                            stage, ticks, List(seen.create), List(seen.destroy)))
                    end
                    Over(true, Rows(string.format("on an entity of this mod's own: created (%s), removed (%s), in %d ticks",
                        List(seen.create), List(seen.destroy), ticks), "VMT-11, EVT-42, LAY2-14, LAY2-13, LAY-30, SHIM-55, LUA-062, LUA-094"))
                end
            end)
        end },
        { "Ext.Entity.OnChange#server", id = "LS-20", ctx = "server", risk = "unproved", async = true, run = function (E, M, done)
            local character, guid = C.character()
            local seen = nil
            local sub = E.Entity.OnChange(REPLICATED, function (entity, type, component, flags)
                if entity == character and seen == nil then seen = { type = tostring(type), flags = flags } end
            end, character)
            character:Replicate(REPLICATED)
            Until(E, function (ok, detail)
                E.Entity.Unsubscribe(sub)
                done(ok, detail)
            end, 120, function () return seen ~= nil end,
                function (n) return Rows(string.format("the host's %s, %d tick(s) after Replicate (type %s, flags %s)",
                    REPLICATED, n, seen.type, tostring(seen.flags)), "LUA-058, EVT-43, LAY-28") end,
                function (n) return "no OnChange in " .. n .. " ticks after Replicate" end)
        end },
        { "Ext.Entity.OnChange#client", id = "LC-60", ctx = "client", risk = "unproved", run = function (E)
            local _, guid = C.character()
            local seen = seenReplication[guid]
            if seen == nil then
                if C.trigger == nil or C.offline then C.skip("no bootstrap subscription in this harness") end
                local others = 0
                for _ in pairs(seenReplication) do others = others + 1 end
                error(string.format("no replication of the host's %s reached the client's OnChange (%d other entities' did)", REPLICATED, others))
            end
            return Rows(string.format("the host's %s, replicated from the server (type %s, component %s, flags %s)",
                REPLICATED, seen.type, tostring(seen.component), tostring(seen.flags)), "LUA-058, VMT-12, EVT-43, G-066, G-121, G-122, G-123, LAY-28")
        end },
        { "Ext.Vars#toClient", id = "NET-13", ctx = "server", run = function (E)
            local character = C.character()
            character.Vars[USER_VAR] = 4242
            E.Vars.GetModVariables(C.ModuleUUID)[MOD_VAR] = 4343
            E.Vars.SyncUserVariables()
            E.Vars.SyncModVariables()
            C.equal(character.Vars[USER_VAR], 4242, "the user variable on the server")
            return Rows("set on the server and synced: the client case reads them", "LUA-080, LUA-081")
        end },
        { "Ext.Vars#toClient#client", id = "NET-13", ctx = "client", async = true, run = function (E, M, done)
            local character = C.character()
            Until(E, done, 300, function ()
                return character.Vars[USER_VAR] == 4242 and E.Vars.GetModVariables(C.ModuleUUID)[MOD_VAR] == 4343
            end, function (n) return Rows("the server's user and mod variable read on the client (" .. n .. " ticks)", "LUA-080, LUA-081") end,
            function () return string.format("the client reads %s and %s", tostring(character.Vars[USER_VAR]),
                tostring(E.Vars.GetModVariables(C.ModuleUUID)[MOD_VAR])) end)
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
