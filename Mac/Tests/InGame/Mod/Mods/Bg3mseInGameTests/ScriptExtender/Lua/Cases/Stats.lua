-- Ext.Stats (BG3Extender/Lua/Libs/Stats.inl, StatFunctors.inl,
-- StatAttributes.inl, LuaScripts/Libs/Stats.lua). What the cases create has
-- a name of this mod's own (prefix Bg3mseIgt_) and lives in the session's
-- memory only: the stats database is not part of a save. The one change to a
-- game entry, TreasureTable.Update, writes back what the table holds.

local library = { name = "Stats" }

function library.define(C)
    local function FirstOf(E, statType)
        local names = E.Stats.GetStats(statType)
        if names == nil or #names == 0 then C.skip("no stats of type " .. statType) end
        return names[1]
    end

    local function Sub(name, id, run, extra)
        local case = { "Ext.Stats." .. name, id = id, run = run }
        for k, v in pairs(extra or {}) do case[k] = v end
        return case
    end

    -- A boost prototype's GUID, from a boost the character has.
    local function BoostGuid(E)
        local character = C.character()
        local container = character.BoostsContainer
        if container == nil then C.skip("the character has no BoostsContainer") end
        for _, entry in pairs(container.Boosts) do
            local list = entry.Boosts or entry
            for _, boost in pairs(list) do
                local info = boost.BoostInfo
                if info ~= nil and info.Prototype ~= nil and info.Prototype ~= "00000000-0000-0000-0000-000000000000" then
                    return info.Prototype
                end
            end
        end
        C.skip("no boost of the character names a prototype")
    end

    return {
        { "Ext.Stats.GetStatsManager", id = "LUA-042", run = function (E)
            local manager = C.notNil(E.Stats.GetStatsManager(), "GetStatsManager()")
            -- A named element manager shows its elements as Values.
            C.check(#manager.ModifierLists.Values > 0, "no modifier lists")
        end },
        { "Ext.Stats.GetModifierAttributes", id = "LUA-042", run = function (E)
            local attributes = E.Stats.GetModifierAttributes("SpellData")
            C.notNil(attributes.SpellType, "SpellData's attribute SpellType")
        end },
        { "Ext.Stats.GetStats", id = "LUA-041", run = function (E)
            local spells = E.Stats.GetStats("SpellData")
            C.check(#spells > 1000, "only %d spells", #spells)
            C.check(#E.Stats.GetStats() > #spells, "all stats are not more than the spells")
            return #spells .. " spells"
        end },
        { "Ext.Stats.GetStatsLoadedBefore", id = "LUA-043", run = function (E)
            -- This mod loads no stats: everything of the type was loaded before it.
            local before = E.Stats.GetStatsLoadedBefore(C.ModuleUUID, "SpellData")
            C.check(#before > 1000, "only %d spells loaded before this mod", #before)
            local stat = E.Stats.Get("Target_MainHandAttack")
            C.check(stat.ModId ~= nil and stat.ModId ~= "" and E.Mod.IsModLoaded(stat.ModId),
                "Target_MainHandAttack's ModId is %s", tostring(stat.ModId))
            return "ModId " .. tostring(stat.ModId) .. ", OriginalModId " .. tostring(stat.OriginalModId)
        end },
        { "Ext.Stats.Get", id = "LUA-041", run = function (E)
            local stat = C.notNil(E.Stats.Get("Target_MainHandAttack"), "Get('Target_MainHandAttack')")
            C.equal(stat.Name, "Target_MainHandAttack", "Name")
            C.equal(E.Stats.Get("Bg3mseIgt_NoSuchStat", nil, false), nil, "a stat that does not exist")
        end },
        { "Ext.Stats.GetCachedSpell", id = "LUA-044", risk = "unproved", run = function (E)
            C.notNil(E.Stats.GetCachedSpell("Target_MainHandAttack"), "the prototype of Target_MainHandAttack")
        end },
        { "Ext.Stats.GetCachedStatus", id = "LUA-044", risk = "unproved", run = function (E)
            local name = FirstOf(E, "StatusData")
            C.notNil(E.Stats.GetCachedStatus(name), "the prototype of " .. name)
            return name
        end },
        { "Ext.Stats.GetCachedPassive", id = "LUA-044", risk = "unproved", run = function (E)
            local name = FirstOf(E, "PassiveData")
            C.notNil(E.Stats.GetCachedPassive(name), "the prototype of " .. name)
            return name
        end },
        { "Ext.Stats.GetCachedInterrupt", id = "LUA-044", risk = "unproved", run = function (E)
            local name = FirstOf(E, "InterruptData")
            C.notNil(E.Stats.GetCachedInterrupt(name), "the prototype of " .. name)
            return name
        end },
        { "Ext.Stats.GetCachedBoost", id = "LUA-044", risk = "unproved", run = function (E)
            local guid = BoostGuid(E)
            local boost = C.notNil(E.Stats.GetCachedBoost(guid), "the boost prototype " .. guid)
            -- The prototype as the boost manager keeps it (layout proved).
            C.equal(E.Types.Validate(boost), true, "the boost prototype passes validation")
            return string.format("%s: %s %s", guid, tostring(boost.TypeName), tostring(boost.Boost))
        end },
        { "Ext.Stats.Create", id = "LUA-045", run = function (E)
            local name = "Bg3mseIgt_ProbeSpell_" .. (E.IsServer() and "S" or "C")
            local stat = E.Stats.Get(name, nil, false) or E.Stats.Create(name, "SpellData", "Target_MainHandAttack")
            C.notNil(stat, "Create")
            C.equal(stat.Name, name, "the new stat's name")
            C.equal(stat.SpellType, E.Stats.Get("Target_MainHandAttack").SpellType, "an attribute copied from the template")
        end },
        { "Ext.Stats.Sync", id = "LUA-040", run = function (E)
            local name = "Bg3mseIgt_ProbeSpell_" .. (E.IsServer() and "S" or "C")
            if E.Stats.Get(name, nil, false) == nil then E.Stats.Create(name, "SpellData", "Target_MainHandAttack") end
            E.Stats.Sync(name)
            C.notNil(E.Stats.GetCachedSpell(name), "the synced spell's prototype")
        end },
        { "Ext.Stats.Sync#Again", id = "LUA-049", run = function (E)
            -- A second sync rebuilds a prototype that is there: Norbyte's
            -- SyncStat clears its animations first, and the set of names in
            -- them (SpellPrototypeAnimationData::Part2) was filled by the game.
            local name = "Bg3mseIgt_ProbeSpell_" .. (E.IsServer() and "S" or "C")
            if E.Stats.Get(name, nil, false) == nil then E.Stats.Create(name, "SpellData", "Target_MainHandAttack") end
            E.Stats.Sync(name)
            local first = C.notNil(E.Stats.GetCachedSpell(name), "the prototype after the first sync")
            local names = #first.SpellAnimation.Part2
            E.Stats.Sync(name)
            local again = C.notNil(E.Stats.GetCachedSpell(name), "the prototype after the second sync")
            C.equal(#again.SpellAnimation.Part2, names, "the names in the animation after the second sync")
            return names .. " names in the animation's set"
        end },
        { "Ext.Stats.SetPersistence", id = "LUA-045", run = function (E)
            -- Deprecated: only a warning on every platform.
            E.Stats.SetPersistence("Bg3mseIgt_ProbeSpell_" .. (E.IsServer() and "S" or "C"), false)
        end },
        { "Ext.Stats.EnumLabelToIndex", id = "LUA-042", risk = "pure", offline = true, run = function (E)
            local index = E.Stats.EnumLabelToIndex("DamageType", "Fire")
            C.check(math.type(index) == "integer", "EnumLabelToIndex gave %s", tostring(index))
            C.equal(index, E.Enums.DamageType.Fire.Value, "the index of DamageType.Fire")
        end },
        { "Ext.Stats.EnumIndexToLabel", id = "LUA-042", risk = "pure", offline = true, run = function (E)
            C.equal(E.Stats.EnumIndexToLabel("DamageType", E.Enums.DamageType.Fire.Value), "Fire", "the label of Fire's index")
        end },
        { "Ext.Stats.AddAttribute", id = "LUA-042", run = function (E)
            -- After the stats files are loaded the function refuses (an error
            -- line and false, as on Windows); adding one is for StatsStructureLoaded.
            C.equal(E.Stats.AddAttribute("SpellData", "Bg3mseIgtProbe", "FixedString"), false, "AddAttribute after load")
            return "refusal path only"
        end },
        { "Ext.Stats.AddEnumerationValue", id = "LUA-042", run = function (E)
            -- A label cannot be taken out again: only the refusal for a type that does not exist.
            C.equal(E.Stats.AddEnumerationValue("Bg3mseIgtNoSuchType", "X"), nil, "AddEnumerationValue on an unknown type")
            return "refusal path only"
        end },
        { "Ext.Stats.PrepareFunctorParams", id = "LUA-046", risk = "unproved", run = function (E)
            local params = C.notNil(E.Stats.PrepareFunctorParams("Target"), "PrepareFunctorParams('Target')")
            return E.Types.GetObjectType(params)
        end },
        { "Ext.Stats.LoadStatsFile", id = "LUA-049", run = function (E)
            -- This mod's own stats file: one new spell, by SetRawAttribute.
            E.Stats.LoadStatsFile("Mods/Bg3mseInGameTests/Probe/Stats.txt", false)
            local stat = C.notNil(E.Stats.Get("Bg3mseIgt_LoadedSpell", nil, false), "the spell the file creates")
            C.equal(stat.Cooldown, "OncePerTurn", "the attribute the file sets with SetRawAttribute")
        end },
        { "Ext.Stats.Get#SetRawAttribute", id = "LUA-048", run = function (E)
            local name = "Bg3mseIgt_ProbeSpell_" .. (E.IsServer() and "S" or "C")
            local stat = E.Stats.Get(name, nil, false) or E.Stats.Create(name, "SpellData", "Target_MainHandAttack")
            C.equal(stat:SetRawAttribute("Cooldown", "OncePerTurn"), true, "SetRawAttribute")
            C.equal(stat.Cooldown, "OncePerTurn", "the attribute after SetRawAttribute")
        end },

        Sub("SpellSet.GetAll", "LUA-047", function (E)
            local all = E.Stats.SpellSet.GetAll()
            C.check(#all > 0, "no spell sets")
        end),
        Sub("SpellSet.Get", "LUA-047", function (E)
            local first = E.Stats.SpellSet.GetAll()[1]
            if first == nil then C.skip("no spell sets") end
            C.equal(E.Stats.SpellSet.Get(first.Name).Name, first.Name, "the set by name")
        end),
        Sub("SpellSet.Create", "LUA-047", function (E)
            local name = "Bg3mseIgt_SpellSet"
            local set = E.Stats.SpellSet.Get(name) or E.Stats.SpellSet.Create(name)
            C.equal(set.Name, name, "the new set's name")
        end),
        Sub("EquipmentSet.GetAll", "LUA-047", function (E)
            C.check(#E.Stats.EquipmentSet.GetAll() > 0, "no equipment sets")
        end),
        Sub("EquipmentSet.Get", "LUA-047", function (E)
            local first = E.Stats.EquipmentSet.GetAll()[1]
            if first == nil then C.skip("no equipment sets") end
            C.equal(E.Stats.EquipmentSet.Get(first.Name).Name, first.Name, "the set by name")
        end),
        Sub("EquipmentSet.Create", "LUA-047", function (E)
            local name = "Bg3mseIgt_EquipmentSet"
            local set = E.Stats.EquipmentSet.Get(name) or E.Stats.EquipmentSet.Create(name)
            C.equal(set.Name, name, "the new set's name")
        end),
        Sub("TreasureTable.Get", "LUA-047", function (E)
            local name = FirstOf(E, "TreasureTable")
            C.equal(E.Stats.TreasureTable.Get(name).Name, name, "the table by name")
        end),
        Sub("TreasureTable.GetLegacy", "LUA-047", function (E)
            local name = FirstOf(E, "TreasureTable")
            C.equal(E.Stats.TreasureTable.GetLegacy(name).Name, name, "the legacy table's name")
        end),
        Sub("TreasureTable.Update", "LUA-047", function (E)
            -- Update reads into the table of that name: here a game table,
            -- written back as it is (a table made by Update is
            -- Cases/StatsSync.lua's TreasureTable#Create).
            -- Reading empties the table's subtables first and raises on a
            -- category that names nothing, so only a table whose every
            -- category names an existing table or category is written.
            local function Resolves(legacy)
                for _, sub in ipairs(legacy.SubTables or {}) do
                    for _, cat in ipairs(sub.Categories or {}) do
                        if cat.TreasureTable ~= nil then
                            if not E.Stats.TreasureTable.GetLegacy(cat.TreasureTable) then return false end
                        elseif cat.TreasureCategory ~= nil then
                            if not E.Stats.TreasureCategory.GetLegacy(cat.TreasureCategory) then return false end
                        else
                            return false
                        end
                    end
                end
                return true
            end
            local name = nil
            for _, candidate in ipairs(E.Stats.GetStats("TreasureTable") or {}) do
                local legacy = E.Stats.TreasureTable.GetLegacy(candidate)
                if legacy and legacy.SubTables and #legacy.SubTables > 0 and Resolves(legacy) then
                    name = candidate
                    break
                end
            end
            if name == nil then C.skip("no treasure table whose categories all resolve") end
            local before = E.Json.Stringify(E.Stats.TreasureTable.GetLegacy(name), { Beautify = false })
            E.Stats.TreasureTable.Update(E.Stats.TreasureTable.GetLegacy(name))
            local after = E.Json.Stringify(E.Stats.TreasureTable.GetLegacy(name), { Beautify = false })
            C.equal(after, before, "the table " .. name .. " after Update")
        end),
        Sub("TreasureCategory.GetLegacy", "LUA-047", function (E)
            local name = FirstOf(E, "TreasureCategory")
            C.notNil(E.Stats.TreasureCategory.GetLegacy(name), "the category " .. name)
        end),
        Sub("TreasureCategory.Update", "LUA-047", function (E)
            E.Stats.TreasureCategory.Update("Bg3mseIgt_TreasureCategory", { Category = "Bg3mseIgt_TreasureCategory", Items = {} })
            C.notNil(E.Stats.TreasureCategory.GetLegacy("Bg3mseIgt_TreasureCategory"), "the new category")
        end),
        Sub("ItemCombo.GetAll", "LUA-047", function (E)
            C.check(#E.Stats.ItemCombo.GetAll() > 0, "no item combinations")
        end),
        Sub("ItemCombo.Get", "LUA-047", function (E)
            local first = E.Stats.ItemCombo.GetAll()[1]
            if first == nil then C.skip("no item combinations") end
            C.equal(E.Stats.ItemCombo.Get(first.Name).Name, first.Name, "the combination by name")
        end),
        Sub("ItemCombo.Create", "LUA-047", function (E)
            local name = "Bg3mseIgt_ItemCombo"
            local combo = E.Stats.ItemCombo.Get(name) or E.Stats.ItemCombo.Create(name)
            C.equal(combo.Name, name, "the new combination's name")
        end),
        Sub("ItemComboPreview.GetAll", "LUA-047", function (E)
            C.type(E.Stats.ItemComboPreview.GetAll(), "table")
        end),
        Sub("ItemComboPreview.Get", "LUA-047", function (E)
            -- The game files each of its own under Type_Name
            -- (CItemCombinationManager::LoadText); Create files under the name.
            local first = nil
            for _, data in ipairs(E.Stats.ItemComboPreview.GetAll()) do
                if data.Type ~= nil and data.Type ~= "" then first = data break end
            end
            if first == nil then C.skip("no preview data of the game's") end
            local key = first.Type .. "_" .. first.Name
            C.equal(C.notNil(E.Stats.ItemComboPreview.Get(key), "the preview data " .. key).Name, first.Name, "the preview data by its key")
        end),
        Sub("ItemComboPreview.Create", "LUA-047", function (E)
            local name = "Bg3mseIgt_ItemComboPreview"
            local data = E.Stats.ItemComboPreview.Get(name) or E.Stats.ItemComboPreview.Create(name)
            C.equal(data.Name, name, "the new preview data's name")
        end),
        Sub("ItemComboProperty.GetAll", "LUA-047", function (E)
            C.type(E.Stats.ItemComboProperty.GetAll(), "table")
        end),
        Sub("ItemComboProperty.Get", "LUA-047", function (E)
            -- GetAll gives none in this game: the property this mod makes.
            local name = "Bg3mseIgt_ItemComboProperty"
            local _ = E.Stats.ItemComboProperty.Get(name) or E.Stats.ItemComboProperty.Create(name)
            C.equal(C.notNil(E.Stats.ItemComboProperty.Get(name), "the property by name").Name, name, "the property's name")
        end),
        Sub("ItemComboProperty.Create", "LUA-047", function (E)
            local name = "Bg3mseIgt_ItemComboProperty"
            local prop = E.Stats.ItemComboProperty.Get(name) or E.Stats.ItemComboProperty.Create(name)
            C.equal(prop.Name, name, "the new property's name")
        end),
        Sub("ItemGroup.GetAll", "LUA-047", function (E)
            C.check(#E.Stats.ItemGroup.GetAll() > 0, "no item groups")
        end),
        Sub("ItemGroup.Get", "LUA-047", function (E)
            local first = E.Stats.ItemGroup.GetAll()[1]
            if first == nil then C.skip("no item groups") end
            C.equal(E.Stats.ItemGroup.Get(first.Name).Name, first.Name, "the group by name")
        end),
        Sub("NameGroup.GetAll", "LUA-047", function (E)
            C.type(E.Stats.NameGroup.GetAll(), "table")
        end),
        Sub("NameGroup.Get", "LUA-047", function (E)
            local first = E.Stats.NameGroup.GetAll()[1]
            if first == nil then C.skip("no name groups") end
            C.equal(E.Stats.NameGroup.Get(first.Name).Name, first.Name, "the name group by name")
        end),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
