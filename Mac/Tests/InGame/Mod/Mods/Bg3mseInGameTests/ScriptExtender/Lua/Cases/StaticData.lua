-- Ext.StaticData (BG3Extender/Lua/Libs/StaticData.inl): the game's guid
-- resources and the icon atlas. Norbyte's StaticDataTests.lua checks more.

local library = { name = "StaticData" }

function library.define(C)
    local LORE = "d21368ac-c776-465c-9dcf-6123dd52734f" -- ClassDescription LoreCollege (StaticDataTests.lua)
    local function Icon(E)
        local icon = E.Stats.Get("Target_MainHandAttack").Icon
        if icon == nil or icon == "" then C.skip("Target_MainHandAttack has no icon") end
        return icon
    end
    return {
        { "Ext.StaticData.GetAll", id = "LUA-071", run = function (E)
            local all = E.StaticData.GetAll("ClassDescription")
            local found = false
            for _, g in ipairs(all) do if g == LORE then found = true end end
            C.check(found, "LoreCollege is not among %d class descriptions", #all)
        end },
        { "Ext.StaticData.Get", id = "LUA-071", run = function (E)
            local res = C.notNil(E.StaticData.Get(LORE, "ClassDescription"), "Get(LoreCollege)")
            C.equal(res.Name, "LoreCollege", "Name")
            C.equal(E.Types.Validate(res), true, "Validate")
        end },
        { "Ext.StaticData.GetSources", id = "LUA-071", run = function (E)
            local sources = C.notNil(E.StaticData.GetSources("ClassDescription"), "GetSources")
            C.check(C.count(sources) > 0, "no sources")
        end },
        { "Ext.StaticData.GetByModId", id = "LUA-071", run = function (E)
            local list = E.StaticData.GetByModId("ClassDescription", C.SharedUUID)
            C.check(list ~= nil and #list > 0, "no class descriptions from Shared")
        end },
        { "Ext.StaticData.Create", id = "LUA-071", risk = "unproved", run = function (E)
            -- A new resource of this run's own GUID, in the session's memory.
            local guid = E.Utils.GenerateGuid()
            local res = C.notNil(E.StaticData.Create("Tag", guid), "Create('Tag')")
            C.equal(res.ResourceUUID, guid, "the new resource's GUID")
        end },
        { "Ext.StaticData.ClearResourceBank", id = "LUA-072", skip = "empties one of the game's resource banks" },
        { "Ext.StaticData.SyncResourceBank", id = "LUA-072", risk = "unproved", run = function (E)
            -- GuidResourceBankBase::PostLoad (StaticData.inl), the slot the
            -- declared-virtuals check proves (Mac/Layout/DeclaredVirtuals.cpp):
            -- the game's own post-load pass over the Tag bank, after a tag of
            -- this run's own GUID is created, as a mod does it. Every tag the
            -- bank had and the new one are still there after it.
            local before = E.StaticData.GetAll("Tag")
            local guid = E.Utils.GenerateGuid()
            C.notNil(E.StaticData.Create("Tag", guid), "Create('Tag')")
            E.StaticData.SyncResourceBank("Tag")
            local after = E.StaticData.GetAll("Tag")
            C.check(#after >= #before + 1, "the Tag bank has %d resources after the sync, %d before", #after, #before)
            local tag = C.notNil(E.StaticData.Get(guid, "Tag"), "the new tag after the sync")
            C.equal(tag.ResourceUUID, guid, "its GUID")
            C.notNil(E.StaticData.Get(before[1], "Tag"), "the bank's first tag after the sync")
            return #before .. " tags before, " .. #after .. " after"
        end },
        { "Ext.StaticData.LoadResource", id = "UPS-01", risk = "unproved", run = function (E)
            -- A dialogue of the game's resource bank, loaded into this
            -- side's dialog manager the way the game does before it plays
            -- one (dlg::DialogManager::PerformDialogLoad), and held by this
            -- Lua state until it is released.
            local manager = C.notNil(E.Utils.GetDialogManager(), "the dialog manager of this side")
            local all = E.Resource.GetAll("Dialog")
            if all == nil or #all == 0 then C.skip("the resource bank has no dialogue") end
            local guid = all[1]
            local before = #manager.CachedDialogs
            local dialog = C.notNil(E.StaticData.LoadResource(guid, "Dialog"), "LoadResource of " .. guid)
            local uuid, refs = dialog.UUID, dialog.RefCount
            C.check(type(uuid) == "string" and uuid ~= "", "the dialogue's UUID is %s", tostring(uuid))
            C.check(math.type(refs) == "integer" and refs >= 1, "RefCount after LoadResource is %s", tostring(refs))
            -- It is one of the manager's cached dialogues.
            local cached = 0
            for _, d in ipairs(manager.CachedDialogs) do
                if d.UUID == uuid then cached = cached + 1 end
            end
            C.equal(cached, 1, "cached dialogues with its UUID")
            C.check(#manager.CachedDialogs >= before, "the manager has %d cached dialogues, %d before", #manager.CachedDialogs, before)
            -- While it is held a second call gives the same dialogue and takes no further reference.
            local again = C.notNil(E.StaticData.LoadResource(guid, "Dialog"), "a second LoadResource")
            C.equal(again.UUID, uuid, "the second call's dialogue")
            C.equal(dialog.RefCount, refs, "RefCount after a second LoadResource")
            -- Released: one reference less, and with the last one the time the
            -- manager counts its unloading from.
            C.equal(E.StaticData.ReleaseResource(guid, "Dialog"), true, "ReleaseResource")
            C.equal(dialog.RefCount, refs - 1, "RefCount after ReleaseResource")
            if refs == 1 then C.check(dialog.UnloadedTimeStamp > 0, "UnloadedTimeStamp is %s", tostring(dialog.UnloadedTimeStamp)) end
            C.equal(E.StaticData.ReleaseResource(guid, "Dialog"), false, "a second ReleaseResource")
            -- Only dialogues.
            C.raises(function () E.StaticData.LoadResource(guid, "Texture") end)
            return string.format("%s: %d nodes' dialogue %s held with %d reference(s), then released; %d cached before, %d after",
                guid, C.count(dialog.Nodes), uuid, refs, before, #manager.CachedDialogs)
        end },
        { "Ext.StaticData.LoadResource#unknown", id = "UPS-01", run = function (E)
            -- A GUID the bank has no dialogue for: nothing, and nothing held.
            local guid = E.Utils.GenerateGuid()
            C.equal(E.StaticData.LoadResource(guid, "Dialog"), nil, "LoadResource of a GUID that is no dialogue's")
            C.equal(E.StaticData.ReleaseResource(guid, "Dialog"), false, "ReleaseResource after it")
        end },
        { "Ext.StaticData.ReleaseResource", id = "UPS-02", risk = "pure", offline = true, run = function (E)
            -- Nothing is held for a GUID that was never loaded; a type other
            -- than Dialog is an error.
            local guid = E.Utils.GenerateGuid()
            C.equal(E.StaticData.ReleaseResource(guid, "Dialog"), false, "ReleaseResource of what was never loaded")
            C.raises(function () E.StaticData.ReleaseResource(guid, "Texture") end)
        end },
        { "Ext.StaticData.GetTextureAtlasManager", id = "LUA-073", run = function (E)
            local manager = E.StaticData.GetTextureAtlasManager()
            if E.IsClient() then C.notNil(manager, "the client's texture atlas map") end
            return manager == nil and "nil" or "present"
        end },
        { "Ext.StaticData.GetIconAtlas", id = "LUA-073", ctx = "client", run = function (E)
            local icon = Icon(E)
            C.notNil(E.StaticData.GetIconAtlas(icon), "the atlas of " .. icon)
            return icon
        end },
        { "Ext.StaticData.GetIconUVs", id = "LUA-073", ctx = "client", run = function (E)
            local icon = Icon(E)
            local uv = C.notNil(E.StaticData.GetIconUVs(icon), "the UVs of " .. icon)
            return E.Json.Stringify(uv, { Beautify = false, IterateUserdata = true })
        end },
        { "Ext.StaticData.GetIconAtlas#server", id = "LUA-073", ctx = "server", run = function (E)
            local atlas = E.StaticData.GetIconAtlas(Icon(E))
            return atlas == nil and "nil on the server" or "present"
        end },
        { "Ext.StaticData.GetIconUVs#server", id = "LUA-073", ctx = "server", run = function (E)
            local uv = E.StaticData.GetIconUVs(Icon(E))
            return uv == nil and "nil on the server" or "present"
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
