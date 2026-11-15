-- Ext.Entity (BG3Extender/Lua/Libs/Entity.inl), the entity proxy's
-- read-only methods (Lua/Shared/Proxies/LuaEntityProxy.inl) and Ext.System
-- (LuaSharedLibs.cpp). Subscriptions are taken out again in the same case.

local library = { name = "Entity" }

function library.define(C)
    local function CharacterComponent(E)
        return E.IsServer() and "ServerCharacter" or "ClientCharacter"
    end
    local function SystemName(E)
        return E.IsServer() and "ServerBoost" or "PickingHelper"
    end
    local function SubscribeAndUnsubscribe(E, subscribe)
        local character = C.character()
        local id = subscribe(character)
        C.notNil(id, "the subscription id")
        C.equal(E.Entity.Unsubscribe(id), true, "Unsubscribe")
        return "subscription only: what fires it was not provoked"
    end

    local cases = {
        { "Ext.Entity.Get", id = "LUA-052", run = function (E)
            local character, guid = C.character()
            C.equal(E.Entity.Get("11111111-2222-3333-4444-123412341234"), nil, "an entity that does not exist")
            C.equal(E.Entity.Get(nil), nil, "Get(nil)")
            C.notNil(E.Entity.Get(guid), "Get(guid)")
            C.equal(E.Entity.Get(character), character, "Get(entity)")
            local netId = character:GetNetId()
            if netId ~= nil then C.equal(E.Entity.Get(netId), character, "Get(netId)") end
        end },
        { "Ext.Entity.HandleToUuid", id = "LUA-053", run = function (E)
            local character, guid = C.character()
            C.equal(E.Entity.HandleToUuid(character), guid, "HandleToUuid")
        end },
        { "Ext.Entity.UuidToHandle", id = "LUA-053", run = function (E)
            local character, guid = C.character()
            C.equal(E.Entity.UuidToHandle(guid), character, "UuidToHandle")
        end },
        { "Ext.Entity.GetAllEntitiesWithUuid", id = "LUA-054", risk = "unproved", run = function (E)
            local character, guid = C.character()
            local all = E.Entity.GetAllEntitiesWithUuid()
            C.equal(all[guid], character, "the character in the map")
            return C.count(all) .. " entities"
        end },
        { "Ext.Entity.GetAllEntitiesWithComponent", id = "LUA-054", risk = "unproved", run = function (E)
            local character = C.character()
            local list = E.Entity.GetAllEntitiesWithComponent(CharacterComponent(E))
            local found = false
            for _, e in ipairs(list) do if e == character then found = true end end
            C.check(found, "the character is not among %d entities with %s", #list, CharacterComponent(E))
            return #list .. " characters"
        end },
        { "Ext.Entity.GetAllEntities", id = "LUA-054", risk = "unproved", run = function (E)
            local all = E.Entity.GetAllEntities()
            C.check(#all > 100, "only %d entities", #all)
            return #all .. " entities"
        end },
        { "Ext.Entity.GetEntitiesAroundPosition", id = "LUA-055", risk = "unproved", run = function (E)
            local character = C.character()
            local pos = C.position()
            local list = E.Entity.GetEntitiesAroundPosition(pos, 3.0, true, false)
            local found = false
            for _, e in ipairs(list) do if e == character then found = true end end
            C.check(found, "the character is not within 3 m of itself (%d entities found)", #list)
        end },
        -- Create and Destroy only record a change in the world's command
        -- buffer (EntitySystem.cpp, CreateEntityImmediate, DestroyEntity);
        -- the world's update applies it. IsAlive looks at the storage, so a
        -- new entity is alive from the next update on, and destroying it in
        -- the frame it was made in is refused, on Windows too.
        { "Ext.Entity.Create", id = "LUA-056", risk = "unproved", async = true, run = function (E, M, done)
            local entity = C.notNil(E.Entity.Create(), "Create()")
            local ticks = 0
            local sub
            sub = E.Events.Tick:Subscribe(function ()
                ticks = ticks + 1
                if entity:IsAlive() then
                    E.Events.Tick:Unsubscribe(sub)
                    local destroyed = E.Entity.Destroy(entity)
                    done(destroyed == true, "alive after " .. ticks .. " ticks; Destroy gave " .. tostring(destroyed))
                elseif ticks > 60 then
                    E.Events.Tick:Unsubscribe(sub)
                    done(false, "not alive after 60 ticks")
                end
            end)
        end },
        { "Ext.Entity.Destroy", id = "LUA-056", risk = "unproved", async = true, run = function (E, M, done)
            local entity = C.notNil(E.Entity.Create(), "Create()")
            local ticks = 0
            local destroyedAt = nil
            local sub
            sub = E.Events.Tick:Subscribe(function ()
                ticks = ticks + 1
                if destroyedAt == nil then
                    if entity:IsAlive() then
                        if E.Entity.Destroy(entity) ~= true then
                            E.Events.Tick:Unsubscribe(sub)
                            done(false, "Destroy of a live entity gave false")
                            return
                        end
                        destroyedAt = ticks
                    elseif ticks > 60 then
                        E.Events.Tick:Unsubscribe(sub)
                        done(false, "not alive after 60 ticks: nothing to destroy")
                    end
                elseif not entity:IsAlive() then
                    E.Events.Tick:Unsubscribe(sub)
                    done(true, "gone " .. (ticks - destroyedAt) .. " ticks after Destroy")
                elseif ticks - destroyedAt > 60 then
                    E.Events.Tick:Unsubscribe(sub)
                    done(false, "still alive 60 ticks after Destroy")
                end
            end)
        end },
        { "Ext.Entity.Subscribe", id = "LUA-058", risk = "unproved", run = function (E)
            return SubscribeAndUnsubscribe(E, function (character)
                return E.Entity.Subscribe("Health", function () end, character)
            end)
        end },
        { "Ext.Entity.OnChange", id = "LUA-058", risk = "unproved", run = function (E)
            return SubscribeAndUnsubscribe(E, function (character)
                return E.Entity.OnChange("Health", function () end, character)
            end)
        end },
        { "Ext.Entity.OnSystemUpdate", id = "LUA-058", risk = "unproved", async = true, run = function (E, M, done)
            local name = SystemName(E)
            local id
            id = E.Entity.OnSystemUpdate(name, function ()
                E.Entity.Unsubscribe(id)
                done(true, name)
            end, false)
        end },
        { "Ext.Entity.OnSystemPostUpdate", id = "LUA-058", risk = "unproved", async = true, run = function (E, M, done)
            local name = SystemName(E)
            local id
            id = E.Entity.OnSystemPostUpdate(name, function ()
                E.Entity.Unsubscribe(id)
                done(true, name)
            end, false)
        end },
        { "Ext.Entity.Unsubscribe", id = "LUA-057", risk = "unproved", run = function (E)
            local id = E.Entity.OnCreate("Health", function () end)
            C.equal(E.Entity.Unsubscribe(id), true, "Unsubscribe of a live subscription")
            C.equal(E.Entity.Unsubscribe(id), false, "Unsubscribe a second time")
        end },
        { "Ext.Entity.SetupTracing", id = "LUA-059", risk = "unproved", run = function (E)
            E.Entity.SetupTracing({ TrackECB = true, TrackImmediateWorldCache = true, TrackReplication = true,
                TrackModifications = false, ExcludeModificationComponents = {} })
        end },
        { "Ext.Entity.EnableTracing", id = "LUA-059", risk = "unproved", async = true, run = function (E, M, done)
            if not E.Debug.IsDeveloperMode() then C.skip("tracing needs developer mode") end
            E.Entity.EnableTracing(true)
            E.OnNextTick(function ()
                local ok, err = pcall(function ()
                    C.notNil(E.Entity.GetTrace(), "GetTrace() while tracing")
                end)
                E.Entity.EnableTracing(false)
                E.Entity.ClearTrace()
                done(ok, err)
            end)
        end },
        { "Ext.Entity.GetTrace", id = "LUA-059", risk = "unproved", run = function (E)
            C.notNil(E.Entity.GetTrace(), "GetTrace()")
        end },
        { "Ext.Entity.ClearTrace", id = "LUA-059", risk = "unproved", run = function (E)
            E.Entity.ClearTrace()
        end },
        { "Ext.Entity.GetRegisteredComponentTypes", id = "LUA-060", run = function (E)
            local types = E.Entity.GetRegisteredComponentTypes()
            C.check(#types > 1000, "only %d component types", #types)
            local mapped = E.Entity.GetRegisteredComponentTypes(false, true)
            return #types .. " types, " .. #mapped .. " mapped"
        end },

        -- The proxy (LUA-061, LUA-062): read-only methods.
        { "Ext.Entity.Get#GetComponent", id = "LUA-061", run = function (E)
            local character = C.character()
            local name = C.notNil(character:GetComponent("DisplayName"), "GetComponent('DisplayName')")
            C.equal(name.Name, character.DisplayName.Name, "the component by method and by property")
            return name.Name
        end },
        { "Ext.Entity.Get#GetAllComponents", id = "LUA-062", run = function (E)
            local character = C.character()
            local all = character:GetAllComponents()
            C.type(all.Transform, "userdata", "the Transform component")
            return C.count(all) .. " components"
        end },
        { "Ext.Entity.Get#GetAllComponentNames", id = "LUA-062", run = function (E)
            local names = C.character():GetAllComponentNames()
            C.check(#names > 10, "only %d component names", #names)
        end },
        { "Ext.Entity.Get#HasRawComponent", id = "LUA-062", run = function (E)
            local names = C.character():GetAllComponentNames(false)
            C.equal(C.character():HasRawComponent(names[1]), true, "HasRawComponent(" .. tostring(names[1]) .. ")")
        end },
        { "Ext.Entity.Get#IsAlive", id = "LUA-062", run = function (E)
            C.equal(C.character():IsAlive(), true, "IsAlive")
        end },
        { "Ext.Entity.Get#GetNetId", id = "LUA-062", run = function (E)
            local id = C.character():GetNetId()
            C.check(math.type(id) == "integer", "GetNetId gave %s", tostring(id))
        end },
        -- GetEntityType, GetSalt and GetIndex left the entity proxy in
        -- Norbyte's "Add Ext.Entity.Create" (his ECSTests.lua still calls them).
        { "Ext.Entity.Get#Identity", id = "LUA-062", run = function (E)
            local character = C.character()
            C.type(tostring(character), "string", "tostring")
        end },
        { "Ext.Entity.Get#GetReplicationFlags", id = "LUA-062", ctx = "server", risk = "unproved", run = function (E)
            local flags = C.character():GetReplicationFlags("DisplayName")
            C.check(math.type(flags) == "integer", "GetReplicationFlags gave %s", tostring(flags))
        end },
        { "Ext.Entity.Get#WasChanged", id = "LUA-062", risk = "unproved", run = function (E)
            local character = C.character()
            C.type(character:WasChanged("Transform"), "boolean", "WasChanged")
            C.type(character:WasAdded("Transform"), "boolean", "WasAdded")
            C.type(character:WasEntityAdded(), "boolean", "WasEntityAdded")
        end },
        -- Every component of the character read through its property map and
        -- validated (LUA-064): the list of classes that fail.
        { "Ext.Entity.Get#ValidateAll", id = "LUA-064", risk = "unproved", run = function (E)
            local character = C.character()
            local failed, checked = {}, 0
            for name, component in pairs(character:GetAllComponents()) do
                checked = checked + 1
                local ok, valid = pcall(E.Types.Validate, component)
                if not ok or valid ~= true then failed[#failed + 1] = name .. (ok and "" or ("(" .. tostring(valid) .. ")")) end
            end
            table.sort(failed)
            C.check(#failed == 0, "%d of %d components fail validation: %s", #failed, checked, table.concat(failed, ", "))
            return checked .. " components valid"
        end },
        { "Ext.System", id = "LUA-012", risk = "unproved", run = function (E)
            -- Only the systems of ExposedSystemTypes.inl: the picking helper
            -- the update cases use is not one.
            local name = E.IsServer() and "ServerBoost" or "ClientVisual"
            C.notNil(E.System[name], "Ext.System." .. name)
            return name
        end },
    }

    -- The component create/destroy subscriptions: taken and given back.
    local function Subscription(fname)
        return { "Ext.Entity." .. fname, id = "LUA-057", risk = "unproved", run = function (E)
            return SubscribeAndUnsubscribe(E, function (character)
                return E.Entity[fname]("Health", function () end, character)
            end)
        end }
    end
    for _, fname in ipairs({ "OnCreate", "OnCreateDeferred", "OnCreateOnce", "OnCreateDeferredOnce",
            "OnDestroy", "OnDestroyDeferred", "OnDestroyOnce", "OnDestroyDeferredOnce" }) do
        cases[#cases + 1] = Subscription(fname)
    end
    return cases
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
