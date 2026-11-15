-- Ext.Stats in depth (audit 2, WP-36): Sync of each stats type that has a
-- prototype, with the prototype read back on the server and, from the same
-- process's prototype managers, on the client; the attribute kinds whose
-- setter on macOS is bg3mse's own (Mac/Binding/StatsFunctions.cpp:
-- SetPropertyString, PassivePrototype::Init, InterruptPrototype::Init,
-- ParseStaticBoosts); a functor run in each of the nine contexts Norbyte's
-- ExecuteFunctors knows (Lua/Libs/StatFunctors.inl); the stats entries
-- that are not objects written and read back.
--
-- Everything made here has a name of this mod's own (Bg3mseIgt_) and lives
-- in the session's memory only, as in Cases/Stats.lua. A case that proves
-- more than its own row names the others at the end of its detail as
-- "[rows: ...]".
--
-- Sync (Stats.inl) runs RPGStats::SyncWithPrototypeManager, which fills the
-- prototype of the process's manager (Stats.cpp); it sends nothing over the
-- network. On the host the client's Lua state reads the same managers, so
-- the client cases see what the server's cases synced; they run after the
-- server's run has finished (Runner.lua), and skip when it made nothing.

local library = { name = "StatsSync" }

function library.define(C)
    local function Rows(detail, rows)
        return detail .. " [rows: " .. rows .. "]"
    end

    -- A stats entry of this mod's own of the type, copied from the first of
    -- the game's entries that `pick` accepts (the first at all without it).
    local function Make(E, name, statType, pick)
        local stat = E.Stats.Get(name, nil, false)
        if stat ~= nil then return stat end
        local template = nil
        for _, candidate in ipairs(E.Stats.GetStats(statType) or {}) do
            if pick == nil or pick(E.Stats.Get(candidate)) then
                template = candidate
                break
            end
        end
        if template == nil then C.skip("no " .. statType .. " entry to copy") end
        return C.notNil(E.Stats.Create(name, statType, template), "Create " .. name .. " from " .. template)
    end

    local function Attributes(E, statType)
        return E.Stats.GetModifierAttributes(statType) or {}
    end

    local function Set(stat, attribute, value)
        C.equal(stat:SetRawAttribute(attribute, value), true, "SetRawAttribute " .. attribute .. " = " .. value)
    end

    local function Count(list)
        local n = 0
        if list == nil then return 0 end
        for _ in pairs(list) do n = n + 1 end
        return n
    end

    -- How many of the game's spells with a parent the parent case asks for.
    local USING_WANTED = 10

    local PASSIVE = "Bg3mseIgt_SyncPassive"
    local INTERRUPT = "Bg3mseIgt_SyncInterrupt"
    local STATUS = "Bg3mseIgt_SyncStatus"

    -- The values the server's cases give and the client's cases expect.
    local PASSIVE_GROUP = "Bg3mseIgtToggleGroup"
    local INTERRUPT_STACK = "Bg3mseIgtInterruptStack"
    local STATUS_STACK = "Bg3mseIgtStatusStack"

    local function CheckPassive(E, boosts)
        local proto = C.notNil(E.Stats.GetCachedPassive(PASSIVE), "the passive's prototype")
        C.equal(proto.Name, PASSIVE, "the prototype's Name")
        C.equal(Count(proto.Boosts), boosts, "the prototype's Boosts (ParseStaticBoosts)")
        C.equal(proto.ToggleGroup, PASSIVE_GROUP, "the prototype's ToggleGroup")
        C.equal(proto.PriorityOrder, 7, "the prototype's PriorityOrder")
        return proto
    end

    local function CheckInterrupt(E)
        local proto = C.notNil(E.Stats.GetCachedInterrupt(INTERRUPT), "the interrupt's prototype")
        C.equal(proto.Name, INTERRUPT, "the prototype's Name")
        C.equal(proto.Stack, INTERRUPT_STACK, "the prototype's Stack")
        C.equal(Count(proto.Costs), 1, "the prototype's Costs (ParseActionResourceCostWithError)")
        return proto
    end

    local function CheckStatus(E, boosts)
        local proto = C.notNil(E.Stats.GetCachedStatus(STATUS), "the status's prototype")
        C.equal(proto.StatusName, STATUS, "the prototype's StatusName")
        C.equal(Count(proto.Boosts), boosts, "the prototype's Boosts (ParseStaticBoosts)")
        return proto
    end

    local cases = {
        { "Ext.Stats.Sync#PassiveData", id = "SYM-36", ctx = "server", risk = "unproved", run = function (E)
            local stat = Make(E, PASSIVE, "PassiveData")
            local attrs = Attributes(E, "PassiveData")
            for _, a in ipairs({ "Boosts", "ToggleGroup", "PriorityOrder", "BoostConditions" }) do
                C.notNil(attrs[a], "PassiveData's attribute " .. a)
            end
            Set(stat, "Boosts", "AC(1);Initiative(2)")
            Set(stat, "ToggleGroup", PASSIVE_GROUP)
            Set(stat, "PriorityOrder", "7")
            Set(stat, "BoostConditions", "not Dead()")
            E.Stats.Sync(PASSIVE)
            CheckPassive(E, 2)
            -- A second sync of the same passive: Clean() and filled again.
            Set(stat, "Boosts", "AC(1)")
            E.Stats.Sync(PASSIVE)
            local proto = CheckPassive(E, 1)
            return Rows(string.format("PassivePrototype::Init: Boosts 2 then 1 after a second Sync, ToggleGroup %s, PriorityOrder %d",
                tostring(proto.ToggleGroup), proto.PriorityOrder), "LUA-040, SYM2-05, SYM-40")
        end },
        { "Ext.Stats.Sync#InterruptData", id = "SYM-38", ctx = "server", risk = "unproved", run = function (E)
            local stat = Make(E, INTERRUPT, "InterruptData")
            local attrs = Attributes(E, "InterruptData")
            for _, a in ipairs({ "Cost", "Stack" }) do C.notNil(attrs[a], "InterruptData's attribute " .. a) end
            Set(stat, "Cost", "ReactionActionPoint:1")
            Set(stat, "Stack", INTERRUPT_STACK)
            E.Stats.Sync(INTERRUPT)
            local proto = CheckInterrupt(E)
            return Rows(string.format("InterruptPrototype::Init: Stack %s, %d cost, context %s",
                tostring(proto.Stack), Count(proto.Costs), tostring(proto.InterruptContext)), "LUA-040, SYM2-06")
        end },
        { "Ext.Stats.Sync#StatusData", id = "SYM-33", ctx = "server", risk = "unproved", run = function (E)
            -- A BOOST status: StatusPrototypeManager::SyncStat parses its
            -- Boosts with ParseStaticBoosts (Stats.cpp).
            local stat = Make(E, STATUS, "StatusData", function (s) return s ~= nil and tostring(s.StatusType) == "BOOST" end)
            local attrs = Attributes(E, "StatusData")
            for _, a in ipairs({ "Boosts", "StackId" }) do C.notNil(attrs[a], "StatusData's attribute " .. a) end
            Set(stat, "Boosts", "AC(2);Initiative(1)")
            Set(stat, "StackId", STATUS_STACK)
            E.Stats.Sync(STATUS)
            CheckStatus(E, 2)
            return Rows("StatusPrototype::Init with 2 static boosts", "LUA-040, SYM-40, SYM2-07")
        end },
        { "Ext.Stats.Sync#PassiveData#client", id = "SYM-36", ctx = "client", risk = "unproved", run = function (E)
            if E.Stats.Get(PASSIVE, nil, false) == nil then C.skip("the server's case made no " .. PASSIVE .. " in this session") end
            CheckPassive(E, 1)
            return Rows("the server's synced passive, read from the client's state", "LUA-040")
        end },
        { "Ext.Stats.Sync#InterruptData#client", id = "SYM-38", ctx = "client", risk = "unproved", run = function (E)
            if E.Stats.Get(INTERRUPT, nil, false) == nil then C.skip("the server's case made no " .. INTERRUPT .. " in this session") end
            CheckInterrupt(E)
            return Rows("the server's synced interrupt, read from the client's state", "LUA-040")
        end },
        { "Ext.Stats.Sync#StatusData#client", id = "SYM-33", ctx = "client", risk = "unproved", run = function (E)
            if E.Stats.Get(STATUS, nil, false) == nil then C.skip("the server's case made no " .. STATUS .. " in this session") end
            CheckStatus(E, 2)
            return Rows("the server's synced status, read from the client's state", "LUA-040")
        end },
        { "Ext.Stats.Sync#SpellData#client", id = "LUA-040", ctx = "client", run = function (E)
            -- Cases/Stats.lua's server spell, synced there.
            local name = "Bg3mseIgt_ProbeSpell_S"
            local stat = E.Stats.Get(name, nil, false)
            if stat == nil then C.skip("the server's Stats cases made no " .. name .. " in this session") end
            C.equal(stat.Cooldown, "OncePerTurn", "the attribute the server's case set")
            C.notNil(E.Stats.GetCachedSpell(name), "the spell's prototype")
            return "the server's synced spell, read from the client's state"
        end },

        -- SetPropertyString's branches (Mac/Binding/StatsFunctions.cpp), by
        -- the type of the attribute's list of values: each set by
        -- SetRawAttribute and read back by the attribute's getter
        -- (Stats/StatsObjectLua.inl, Object::TryPushValue).
        { "Ext.Stats.Get#SetRawAttribute#kinds", id = "SYM2-04", ctx = "server", risk = "unproved", run = function (E)
            local kinds = {
                StatsFunctors = { value = "RegainHitPoints(1)", check = function (v)
                    C.equal(Count(v), 1, "the functor groups")
                    C.equal(Count(v[1].Functors), 1, "the functors of the group")
                end },
                Conditions = { value = "not Dead()", check = function (v) C.equal(v, "not Dead()", "the condition's text") end },
                TargetConditions = { value = "Character() and not Dead()", check = function (v)
                    C.equal(v, "Character() and not Dead()", "the condition's text") end },
                UseConditions = { value = "not Dead()", check = function (v) C.equal(v, "not Dead()", "the condition's text") end },
                RollConditions = { value = "not Dead()", check = function (v)
                    C.type(v, "table", "the roll conditions")
                    local found = false
                    for _, text in pairs(v) do if text == "not Dead()" then found = true end end
                    C.check(found, "the roll condition's text is not among %s", E.Json.Stringify(v, { Beautify = false }))
                end },
                Requirements = { value = "!Combat", check = function (v)
                    C.check(Count(v) >= 1, "no requirement read back")
                    local r = v[1]
                    C.equal(tostring(r.Requirement), "Combat", "the requirement")
                    C.equal(r.Not, true, "the requirement's Not")
                end },
                AIFlags = { value = "CanNotUse", check = function (v) C.equal(v, "CanNotUse", "the AI flags") end },
            }
            local types = { "SpellData", "StatusData", "PassiveData", "InterruptData", "Character", "Weapon", "Armor", "Object" }
            local done, missing = {}, {}
            for kind, k in pairs(kinds) do
                local found = nil
                for _, statType in ipairs(types) do
                    for attribute, listName in pairs(Attributes(E, statType)) do
                        if listName == kind then found = { statType, attribute } break end
                    end
                    if found then break end
                end
                if found == nil then
                    missing[#missing + 1] = kind
                else
                    local stat = Make(E, "Bg3mseIgt_Raw_" .. kind, found[1])
                    Set(stat, found[2], k.value)
                    local ok, err = pcall(k.check, stat[found[2]])
                    C.check(ok, "%s %s.%s: %s", kind, found[1], found[2], tostring(err))
                    done[#done + 1] = kind .. " (" .. found[1] .. "." .. found[2] .. ")"
                end
            end
            table.sort(done)
            table.sort(missing)
            C.check(#done >= 5, "only %d kinds found: %s", #done, table.concat(done, ", "))
            return Rows("set and read back: " .. table.concat(done, ", ")
                .. (#missing > 0 and ("; no attribute of these types has the kinds " .. table.concat(missing, ", ")) or ""), "LUA-048, LUA-051")
        end },
        { "Ext.Stats.Get#AttributeWrite", id = "LUA-051", run = function (E)
            -- obj.Attr = value (StatAttributes.inl, FallbackSet) of the kinds
            -- the setter takes: an enumeration, a fixed string.
            local name = "Bg3mseIgt_WriteSpell_" .. (E.IsServer() and "S" or "C")
            local stat = Make(E, name, "SpellData", function (s) return s ~= nil and s.Name == "Target_MainHandAttack" end)
            stat.Cooldown = "OncePerShortRest"
            C.equal(stat.Cooldown, "OncePerShortRest", "Cooldown after the write")
            stat.Icon = "Bg3mseIgtIcon"
            C.equal(stat.Icon, "Bg3mseIgtIcon", "Icon after the write")
            -- Create copies the template's values and leaves the new entry
            -- without a parent (RPGStats::CreateObject, Object::CopyFrom), on
            -- Windows too: Using reads empty.
            C.equal(stat.Using, "", "Using of an entry Create made from a template")
            C.equal(stat.ModifierList, "SpellData", "ModifierList")
            local copy = Make(E, name .. "_Copy", "SpellData")
            C.equal(copy:CopyFrom(name), true, "CopyFrom")
            C.equal(copy.Cooldown, "OncePerShortRest", "Cooldown copied")
            return "Cooldown, Icon written and read back; Using empty, ModifierList; CopyFrom"
        end },
        { "Ext.Stats.Get#Using", id = "LUA-051", run = function (E)
            -- The parent the game's stats files give an entry ("using"): the
            -- object holds the parent's handle (stats::Object::Using), and
            -- the property is the name of the entry with that handle.
            local looked, withParent, first = 0, 0, nil
            for _, candidate in ipairs(E.Stats.GetStats("SpellData") or {}) do
                if withParent >= USING_WANTED then break end
                looked = looked + 1
                local stat = C.notNil(E.Stats.Get(candidate), "the entry " .. candidate)
                local using = C.type(stat.Using, "string", "Using of " .. candidate)
                if using ~= "" then
                    local parent = C.notNil(E.Stats.Get(using), "the parent " .. using .. " of " .. candidate)
                    C.equal(parent.Name, using, "the parent's name")
                    C.equal(parent.ModifierList, stat.ModifierList, "the parent's type")
                    withParent = withParent + 1
                    first = first or (candidate .. " using " .. using)
                end
            end
            C.check(withParent > 0, "none of the %d SpellData entries has a parent", looked)
            return string.format("%d of the first %d SpellData entries have a parent, each an entry of the same type; %s", withParent, looked, first)
        end },
    }

    -- One functor in each context ExecuteFunctors knows, on the host: it
    -- heals the host by one hit point, which the game would do anyway at
    -- rest, so the session keeps nothing of it. The ExecuteFunctor and
    -- AfterExecuteFunctor events must name the context (FunctorEvents.inl).
    -- One at a time (exclusive), so that the hit points each reads are its own.
    local CONTEXTS = {
        { "AttackTarget", "LUA-046", "SYM-44" },
        { "AttackPosition", "SYM-45", "HOOK-20" },
        { "Move", "SYM-46", "HOOK-21" },
        { "Target", "SYM-47", "HOOK-22" },
        { "NearbyAttacked", "SYM-48", "HOOK-23" },
        { "NearbyAttacking", "SYM-49", "HOOK-24" },
        { "Equip", "SYM-50", "HOOK-25" },
        { "Source", "SYM-51", "HOOK-26" },
        { "Interrupt", "SYM-52", "HOOK-27" },
    }
    -- The members of each context's parameters that the case sets: the host
    -- in each entity, the host's position where the context has one
    -- (GameDefinitions/Stats/Functors.h; a Target context names its source
    -- only, an Equip, Source or Interrupt context has no position).
    local FIELDS = {
        AttackTarget = { "Caster", "Target" }, AttackPosition = { "Caster" }, Move = { "Caster", "Target", "Source" },
        Target = { "Source" }, NearbyAttacked = { "OriginalSource", "Source", "Target" },
        NearbyAttacking = { "OriginalSource", "Source", "Target" }, Equip = { "Caster", "Target" }, Source = { "Source" },
        Interrupt = { "Source", "Target", "Observer" },
    }
    local POSITION = { AttackTarget = true, AttackPosition = true, Move = true, Target = true, NearbyAttacked = true,
        NearbyAttacking = true }
    cases[#cases + 1] = { "Ext.Stats.ExecuteFunctor#fields", id = "LUA-046", risk = "pure", offline = true, run = function (E)
        -- What the context cases set, through the type information: each is
        -- a member the context's parameters have.
        local checked = 0
        for _, c in ipairs(CONTEXTS) do
            local context = c[1]
            local typeName = "stats::" .. context .. "ContextData"
            local info = C.notNil(E.Types.GetTypeInfo(typeName), "the type information of " .. typeName)
            local function Has(member)
                local owner = info
                while owner do
                    if owner.Members and owner.Members[member] then return true end
                    owner = owner.ParentType
                end
                return false
            end
            for _, field in ipairs(FIELDS[context]) do
                C.check(Has(field), "%s has no member %s", typeName, field)
                checked = checked + 1
            end
            C.equal(Has("Position"), POSITION[context] == true, "whether " .. typeName .. " has a Position")
        end
        return checked .. " members of the nine contexts' parameters"
    end }
    for _, c in ipairs(CONTEXTS) do
        local context, id, hook = c[1], c[2], c[3]
        cases[#cases + 1] = { "Ext.Stats.ExecuteFunctor#" .. context, id = id, ctx = "server", risk = "unproved", async = true,
            exclusive = true, timeout = 20000, run = function (E, M, done)
                local character = C.character()
                local spell = Make(E, "Bg3mseIgt_FunctorContext_" .. context, "SpellData")
                Set(spell, "SpellProperties", "RegainHitPoints(1)")
                local functor = C.notNil(spell.SpellProperties[1].Functors[1], "the spell's functor")
                local params = C.notNil(E.Stats.PrepareFunctorParams(context), "PrepareFunctorParams('" .. context .. "')")
                for _, field in ipairs(FIELDS[context]) do params[field] = character end
                local p = C.position()
                if POSITION[context] then params.Position = { p[1], p[2], p[3] } end
                local seen = {}
                local ids = {}
                ids.ExecuteFunctor = E.Events.ExecuteFunctor:Subscribe(function (e)
                    seen.before = seen.before or tostring(e.Params and e.Params.Type)
                end)
                ids.AfterExecuteFunctor = E.Events.AfterExecuteFunctor:Subscribe(function (e)
                    seen.after = seen.after or tostring(e.Params and e.Params.Type)
                end)
                local before = character.Health.Hp
                local ok, err = pcall(E.Stats.ExecuteFunctor, functor, params)
                for name, sub in pairs(ids) do E.Events[name]:Unsubscribe(sub) end
                C.check(ok, "the call raised: %s", tostring(err))
                C.equal(seen.before, context, "the ExecuteFunctor event's context")
                C.equal(seen.after, context, "the AfterExecuteFunctor event's context")
                E.Timer.WaitFor(500, function ()
                    done(true, Rows(string.format("RegainHitPoints(1) in %s: ExecuteFunctor and AfterExecuteFunctor named it; hit points %d, then %d",
                        context, before, character.Health.Hp), hook .. (id == "LUA-046" and "" or ", LUA-046")))
                end)
            end }
    end

    -- The stats entries that are not objects: written and read back.
    local function SameJson(E, a, b, what)
        C.equal(E.Json.Stringify(a, { Beautify = false }), E.Json.Stringify(b, { Beautify = false }), what)
    end
    local more = {
        { "Ext.Stats.SpellSet#WriteBack", id = "LUA-047", run = function (E)
            local game = E.Stats.SpellSet.GetAll()[1]
            if game == nil then C.skip("no spell sets") end
            local name = "Bg3mseIgt_SpellSetCopy_" .. (E.IsServer() and "S" or "C")
            local set = E.Stats.SpellSet.Get(name) or E.Stats.SpellSet.Create(name)
            local spells = {}
            for _, s in pairs(game.Spells) do spells[#spells + 1] = s end
            table.sort(spells, function (a, b) return tostring(a) < tostring(b) end)
            set.Spells = spells
            local back = {}
            for _, s in pairs(E.Stats.SpellSet.Get(name).Spells) do back[#back + 1] = s end
            table.sort(back, function (a, b) return tostring(a) < tostring(b) end)
            SameJson(E, back, spells, "the spells written into " .. name)
            return string.format("%d spells of %s written into %s and read back", #spells, game.Name, name)
        end },
        { "Ext.Stats.EquipmentSet#WriteBack", id = "LUA-047", run = function (E)
            local game = E.Stats.EquipmentSet.GetAll()[1]
            if game == nil then C.skip("no equipment sets") end
            local name = "Bg3mseIgt_EquipmentSetCopy_" .. (E.IsServer() and "S" or "C")
            local set = E.Stats.EquipmentSet.Get(name) or E.Stats.EquipmentSet.Create(name)
            set.WeaponSetType = game.WeaponSetType
            C.equal(E.Stats.EquipmentSet.Get(name).WeaponSetType, game.WeaponSetType, "WeaponSetType written")
            local groups = 0
            for _, g in pairs(game.EquipmentGroups) do
                groups = groups + 1
                C.type(g.Name, "string", "a group's Name")
            end
            return string.format("%s: WeaponSetType %s written into %s; %d groups read", game.Name, tostring(game.WeaponSetType), name, groups)
        end },
        { "Ext.Stats.ItemCombo#WriteBack", id = "LUA-047", run = function (E)
            local game = nil
            for _, combo in ipairs(E.Stats.ItemCombo.GetAll()) do
                if #combo.Ingredients > 0 and #combo.Results > 0 then game = combo break end
            end
            if game == nil then C.skip("no item combination with ingredients and results") end
            local name = "Bg3mseIgt_ItemComboCopy_" .. (E.IsServer() and "S" or "C")
            local combo = E.Stats.ItemCombo.Get(name) or E.Stats.ItemCombo.Create(name)
            local ingredients = E.Types.Serialize(game.Ingredients)
            local results = E.Types.Serialize(game.Results)
            combo.Ingredients = ingredients
            combo.Results = results
            combo.AlchemyCombineType = game.AlchemyCombineType
            local back = E.Stats.ItemCombo.Get(name)
            SameJson(E, E.Types.Serialize(back.Ingredients), ingredients, "the ingredients written")
            SameJson(E, E.Types.Serialize(back.Results), results, "the results written")
            return string.format("%s: %d ingredients, %d results written into %s and read back", game.Name, #ingredients, #results, name)
        end },
        { "Ext.Stats.TreasureCategory#Values", id = "LUA-047", run = function (E)
            -- The items of a category the game reads, not only its name:
            -- each a name and seven numbers (stats::TreasureCategoryItem).
            -- Nothing of the game's is written here; #WriteBack writes.
            local NUMBERS = { "Priority", "MinAmount", "MaxAmount", "ActPart", "Unique", "MinLevel", "MaxLevel" }
            for _, candidate in ipairs(E.Stats.GetStats("TreasureCategory") or {}) do
                local legacy = E.Stats.TreasureCategory.GetLegacy(candidate)
                if legacy and legacy.Items and #legacy.Items > 0 then
                    C.equal(legacy.Category, candidate, "the category's name")
                    for i, item in ipairs(legacy.Items) do
                        C.check(type(item.Name) == "string" and item.Name ~= "", "item %d of %s has the name %s", i, candidate, tostring(item.Name))
                        for _, number in ipairs(NUMBERS) do
                            C.check(math.type(item[number]) == "integer", "%s of item %d of %s is %s", number, i, candidate, tostring(item[number]))
                        end
                    end
                    SameJson(E, E.Stats.TreasureCategory.GetLegacy(candidate), legacy, "the category read again")
                    local item = legacy.Items[1]
                    return string.format("%s: %d items, the first %s, priority %d, amount %d to %d, levels %d to %d", candidate, #legacy.Items,
                        item.Name, item.Priority, item.MinAmount, item.MaxAmount, item.MinLevel, item.MaxLevel)
                end
            end
            C.skip("no treasure category with items")
        end },
        { "Ext.Stats.TreasureCategory#WriteBack", id = "LUA-047", risk = "unproved", run = function (E)
            -- A game category's items written into a category of this mod's
            -- own, read back, and that one written back as it was read:
            -- Update reads each item into one it allocates
            -- (LuaSerializers.cpp), the second time into a category that
            -- has items already. The game's own category is left as it is:
            -- its items are shared with the other categories that list them.
            local game = nil
            for _, candidate in ipairs(E.Stats.GetStats("TreasureCategory") or {}) do
                local legacy = E.Stats.TreasureCategory.GetLegacy(candidate)
                if legacy and legacy.Items and #legacy.Items > 0 then game = legacy break end
            end
            if game == nil then C.skip("no treasure category with items") end
            local name = "Bg3mseIgt_TreasureCategoryCopy_" .. (E.IsServer() and "S" or "C")
            E.Stats.TreasureCategory.Update(name, { Category = name, Items = game.Items })
            local copy = C.notNil(E.Stats.TreasureCategory.GetLegacy(name), "the category " .. name)
            C.equal(copy.Category, name, "the copy's name")
            SameJson(E, copy.Items, game.Items, "the items written")
            E.Stats.TreasureCategory.Update(name, copy)
            SameJson(E, E.Stats.TreasureCategory.GetLegacy(name), copy, "the category after it was written back")
            SameJson(E, E.Stats.TreasureCategory.GetLegacy(game.Category), game, "the game's category, which was only read")
            return string.format("%d items of %s written into %s, read back, and written back unchanged", #game.Items, game.Category, name)
        end },
        { "Ext.Stats.TreasureTable#Create", id = "LUA-047", risk = "unproved", run = function (E)
            -- Update with a name no table has makes the table
            -- (UpdateTreasureTable, LuaSerializers.cpp): one of this mod's
            -- own, whose subtable draws from a category of this mod's own.
            local suffix = E.IsServer() and "S" or "C"
            local category = "Bg3mseIgt_TreasureTableCategory_" .. suffix
            local name = "Bg3mseIgt_TreasureTable_" .. suffix
            E.Stats.TreasureCategory.Update(category, { Category = category, Items = {
                { Name = "Bg3mseIgt_TreasureItem", Priority = 1, MinAmount = 1, MaxAmount = 1, ActPart = 0, Unique = 0, MinLevel = 0, MaxLevel = 0 } } })
            local existed = E.Stats.TreasureTable.GetLegacy(name) ~= nil
            E.Stats.TreasureTable.Update({ Name = name, MinLevel = 1, MaxLevel = 12, SubTables = {
                { TotalCount = 1, Categories = { { TreasureCategory = category, Frequency = 2 } }, DropCounts = { { Chance = 1, Amount = 1 } } } } })
            local made = C.notNil(E.Stats.TreasureTable.GetLegacy(name), "the table " .. name)
            C.equal(made.Name, name, "the table's name")
            C.equal(made.MinLevel, 1, "MinLevel")
            C.equal(made.MaxLevel, 12, "MaxLevel")
            C.equal(#made.SubTables, 1, "the number of subtables")
            local sub = made.SubTables[1]
            C.equal(#sub.Categories, 1, "the number of categories")
            C.equal(sub.Categories[1].TreasureCategory, category, "the category the subtable names")
            C.equal(sub.Categories[1].Frequency, 2, "the category's Frequency")
            C.equal(sub.TotalCount, 1, "TotalCount (the sum of the drop counts' amounts)")
            C.equal(E.Stats.TreasureTable.Get(name).Name, name, "the table through Get")
            return string.format("%s %s by Update: one subtable drawing from %s", name, existed and "written again" or "made", category)
        end },
        { "Ext.Stats.TreasureTable#Values", id = "LUA-047", run = function (E)
            -- The values of a table the game reads, not only its name: the
            -- first table that has a subtable with a category (the game has
            -- tables of one empty subtable).
            for _, candidate in ipairs(E.Stats.GetStats("TreasureTable") or {}) do
                local legacy = E.Stats.TreasureTable.GetLegacy(candidate)
                for index, sub in ipairs(legacy and legacy.SubTables or {}) do
                    if #(sub.Categories or {}) > 0 then
                        local cat = sub.Categories[1]
                        C.check(cat.TreasureTable ~= nil or cat.TreasureCategory ~= nil, "a category of %s names nothing", candidate)
                        C.type(cat.Frequency, "number", "a category's Frequency")
                        return string.format("%s: %d subtables, subtable %d's first category %s, frequency %s", candidate, #legacy.SubTables,
                            index, tostring(cat.TreasureTable or cat.TreasureCategory), tostring(cat.Frequency))
                    end
                end
            end
            C.skip("no treasure table with a category")
        end },
    }
    for _, c in ipairs(more) do cases[#cases + 1] = c end
    return cases
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
