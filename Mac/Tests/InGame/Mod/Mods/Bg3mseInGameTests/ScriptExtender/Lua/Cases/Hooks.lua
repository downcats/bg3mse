-- The hooks of the damage functor and of the story that a normal run does not
-- reach (audit section D, WP-11 and WP-24), provoked or checked by the cases
-- themselves. Server only.
--
-- The damage case deals one point of fire damage from the host to the host
-- through the story's ApplyDamage, which runs the game's deal-damage functor
-- (esv::OsirisGameFunctions::ApplyDamage calls
-- esv::functor::StatsFunctorDealDamage::Execute, which calls
-- esv::StatsSystem::ApplyDamage): the hooks behind DealDamage, DealtDamage
-- and BeforeDealDamage.
--
-- The functor cases run stats functors of this mod's own spells through
-- Ext.Stats.ExecuteFunctors and ExecuteFunctor (StatFunctors.inl), which call
-- the game's esv::ExecuteStatsFunctor for the AttackTarget context, the
-- function behind the ExecuteFunctor and AfterExecuteFunctor events. Caster
-- and target are the host. Each calls the game in its run, so that a crash is
-- put down to it, and reads the hit points a second later. The functors run
-- on the server only: on the client the same functions would run the
-- server's code on the client's thread.
--
-- What Norbyte's API hands a script: a stat's functor attribute is the list
-- of its groups (Array<stats::FunctorGroup>, StatsObjectLua.inl), and a
-- group's Functors the list of its functors (FunctorGroup::GetFunctors), so a
-- stat gives ExecuteFunctor its argument but never a stats::Functors set,
-- which ExecuteFunctors takes. A script gets a set from the game: the one
-- being run, in the ExecuteFunctor event (e.Functor), or one a component or
-- status holds. The ExecuteFunctors case takes the set the game runs for its
-- own ExecuteFunctor call and runs it once more from the event: its damage
-- functor deals two points of fire damage in all. The ExecuteFunctor case
-- heals three (RegainHitPoints): with the point the ApplyDamage case takes,
-- the host ends with the hit points it had before these cases.
--
-- The way from a stat's attribute to a functor, and from the event to its
-- set, are written once (FUNCTOR_PATHS): the cases follow them in the game,
-- and an offline case follows them through the type information, so a step
-- that the types do not have fails without the game.

local library = { name = "Hooks" }

function library.define(C)
    local function Uuid(s) return type(s) == "string" and s:sub(-36) or s end
    local function UuidOf(entity)
        local ok, uuid = pcall(function () return entity.Uuid.EntityUuid end)
        return ok and uuid or nil
    end

    -- The ways the functor cases go through the game's objects: from what
    -- (a type name), by which steps (a number indexes a list, a string names
    -- a member), to what. A stat's functor attribute is pushed as
    -- Array<stats::FunctorGroup> (StatsObjectLua.inl, StatsFunctors), which
    -- the type information has no member to say.
    local FUNCTOR_PATHS = {
        spellFunctor = { from = "Array<stats::FunctorGroup>", steps = { 1, "Functors", 1 }, to = "stats::Functor" },
        spellFunctors = { from = "Array<stats::FunctorGroup>", steps = { 1, "Functors" }, to = "Array<stats::Functor>" },
        eventSet = { from = "esv::lua::ExecuteFunctorEventParams", steps = { "Functor" }, to = "stats::Functors" },
        setFunctors = { from = "stats::Functors", steps = { "FunctorList" }, to = "Array<stats::Functor>" },
    }

    local function Follow(value, path, what)
        for _, step in ipairs(path.steps) do
            value = C.notNil(value[step], string.format("%s: step %s", what, tostring(step)))
        end
        return value
    end

    -- The type a path ends at, from the type information alone.
    local function FollowTypes(E, path)
        local info = E.Types.GetTypeInfo(path.from)
        local element = path.from:match("^Array<(.+)>$")
        if not info and element then
            -- A list type no member has is not registered: its element is.
            info = { Kind = "Array", TypeName = path.from, ElementType = E.Types.GetTypeInfo(element) }
        end
        C.notNil(info, "the type information of " .. path.from)
        local where = path.from
        for _, step in ipairs(path.steps) do
            if type(step) == "number" then
                C.check(info.Kind == "Array", "%s is a %s, not a list: it has no element %d", where, tostring(info.Kind), step)
                info = info.ElementType
            else
                C.check(info.Kind == "Object", "%s is a %s, not an object: it has no member %s", where, tostring(info.Kind), step)
                local owner, member = info, nil
                while owner and not member do
                    member = owner.Members and owner.Members[step]
                    owner = owner.ParentType
                end
                C.check(member ~= nil, "%s has no member %s", where, step)
                info = member
            end
            C.notNil(info, "the type after " .. where .. " " .. tostring(step))
            where = info.TypeName
        end
        return where
    end

    -- One functor case: a spell of this mod's own with the functor text as
    -- its SpellProperties, its functor run on the host, the events checked
    -- in the call and the hit points read a second later.
    local function Functors(E, done, f)
        local character = C.character()
        local hp = character.Health and character.Health.Hp
        if type(hp) ~= "number" or hp <= 4 then C.skip("the host has " .. tostring(hp) .. " hit points; the case needs more than 4") end
        local stat = E.Stats.Get(f.spell, nil, false) or E.Stats.Create(f.spell, "SpellData", "Target_MainHandAttack")
        C.notNil(stat, "the spell " .. f.spell)
        C.equal(stat:SetRawAttribute("SpellProperties", f.text), true, "SetRawAttribute SpellProperties")
        local groups = C.notNil(stat.SpellProperties, "the spell's SpellProperties")
        C.equal(#groups, 1, "functor groups parsed from " .. f.text)
        C.equal(#Follow(groups, FUNCTOR_PATHS.spellFunctors, "SpellProperties"), 1, "functors parsed from " .. f.text)
        local functor = Follow(groups, FUNCTOR_PATHS.spellFunctor, "SpellProperties")
        C.equal(tostring(functor.TypeId), f.type, "the functor's type")

        local params = C.notNil(E.Stats.PrepareFunctorParams("AttackTarget"), "PrepareFunctorParams('AttackTarget')")
        params.Caster = character
        params.Target = character
        local p = C.position()
        params.Position = { p[1], p[2], p[3] }

        local seen = { damages = 0 }
        local ids = {}
        ids.ExecuteFunctor = E.Events.ExecuteFunctor:Subscribe(function (e)
            seen.before = seen.before or tostring(e.Params and e.Params.Type)
            if f.again and not seen.again then
                -- Set first: the call below throws this event again.
                seen.again = true
                local ok, err = pcall(function ()
                    local set = Follow(e, FUNCTOR_PATHS.eventSet, "the event")
                    seen.setSize = #Follow(set, FUNCTOR_PATHS.setFunctors, "the event's set")
                    f.again(set, params)
                end)
                if not ok then seen.againError = tostring(err) end
            end
        end)
        ids.AfterExecuteFunctor = E.Events.AfterExecuteFunctor:Subscribe(function (e)
            seen.after = seen.after or tostring(e.Params and e.Params.Type)
        end)
        ids.DealDamage = E.Events.DealDamage:Subscribe(function (e)
            local ok, target = pcall(function () return e.Target.Uuid.EntityUuid end)
            seen.damages = seen.damages + 1
            seen.damage = seen.damage or ((ok and tostring(target) or "?") .. " " .. tostring(e.Hit and e.Hit.DamageType))
        end)
        local before = character.Health.Hp
        local okCall, errCall = pcall(E.Stats.ExecuteFunctor, functor, params)
        for name, id in pairs(ids) do E.Events[name]:Unsubscribe(id) end
        C.check(okCall, "the call raised: %s", tostring(errCall))
        C.equal(seen.before, "AttackTarget", "the ExecuteFunctor event's context")
        C.equal(seen.after, "AttackTarget", "the AfterExecuteFunctor event's context")
        if f.again then
            C.check(seen.againError == nil, "ExecuteFunctors from the event raised: %s", tostring(seen.againError))
            C.equal(seen.setSize, 1, "the functors of the set the event was given")
        end
        if f.damages then
            local host = tostring(character.Uuid and character.Uuid.EntityUuid)
            C.equal(seen.damages, f.damages, "DealDamage events in the call")
            C.check(seen.damage:sub(1, #host) == host, "DealDamage's target is not the host: %s", seen.damage)
        end
        E.Timer.WaitFor(1000, function ()
            local now = character.Health.Hp
            done(true, string.format("%s on the host%s: hit points %d at the call, %d a second later%s; ExecuteFunctor and AfterExecuteFunctor seen",
                f.text, f.again and ", run again from the event through ExecuteFunctors" or "", before, now,
                seen.damage and string.format("; DealDamage %s (%d)", seen.damage, seen.damages) or ""))
        end)
    end

    local CLIENT_FUNCTORS = "registered on the client too, but it calls the server's esv::ExecuteStatsFunctor "
        .. "(StatFunctors.inl): on the client's thread that would run the server's code there; the server cases cover it"

    return {
        { "Ext.Events.DealDamage#ApplyDamage", id = "HOOK-13", ctx = "server", risk = "unproved", async = true, run = function (E, M, done)
            local character, guid = C.character()
            local hp = character.Health and character.Health.Hp
            if type(hp) ~= "number" or hp <= 2 then C.skip("the host has " .. tostring(hp) .. " hit points; the case takes one") end

            local seen = {}
            local ids = {}
            local function Unsubscribe()
                for name, id in pairs(ids) do E.Events[name]:Unsubscribe(id) end
                ids = {}
            end
            -- An error in a handler would stay in the event's dispatch: it is
            -- kept, and reported instead of the event not having fired.
            local function Guarded(name, handler)
                return function (e)
                    local ok, err = pcall(handler, e)
                    if not ok and not seen.error then seen.error = name .. ": " .. tostring(err) end
                end
            end
            ids.BeforeDealDamage = E.Events.BeforeDealDamage:Subscribe(Guarded("BeforeDealDamage", function (e)
                seen.before = { total = e.Hit.TotalDamageDone, type = tostring(e.Hit.DamageType), list = #e.Attack.DamageList }
            end))
            ids.DealDamage = E.Events.DealDamage:Subscribe(Guarded("DealDamage", function (e)
                seen.deal = {
                    target = UuidOf(e.Target), caster = UuidOf(e.Caster), caster2 = UuidOf(e.Caster2),
                    hitWith = tostring(e.HitWith), story = e.StoryActionId, fromItem = e.IsFromItem,
                    spell = e.SpellId and tostring(e.SpellId.Prototype), spell2 = e.SpellId2 and tostring(e.SpellId2.Prototype),
                    damageType = tostring(e.Hit.DamageType),
                }
            end))
            ids.DealtDamage = E.Events.DealtDamage:Subscribe(Guarded("DealtDamage", function (e)
                seen.dealt = { total = e.Result.Hit.TotalDamageDone, attack = e.Result.Attack.TotalDamageDone, target = UuidOf(e.Target) }
            end))

            local ok, err = pcall(Osi.ApplyDamage, guid, 1, "Fire", guid)
            local function Finish()
                Unsubscribe()
                if not ok then return done(false, "Osi.ApplyDamage: " .. tostring(err)) end
                if seen.error then return done(false, seen.error) end
                if not seen.deal then return done(false, "DealDamage did not fire") end
                if not seen.dealt then return done(false, "DealtDamage did not fire") end
                if not seen.before then return done(false, "BeforeDealDamage did not fire") end
                local d = seen.deal
                if d.target ~= Uuid(guid) then return done(false, "DealDamage's target is " .. tostring(d.target) .. ", not the host") end
                if type(d.story) ~= "number" then return done(false, "StoryActionId is " .. tostring(d.story)) end
                if type(seen.dealt.total) ~= "number" then return done(false, "the result's TotalDamageDone is " .. tostring(seen.dealt.total)) end
                done(true, string.format(
                    "DealDamage target=host caster=%s caster2=%s hitWith=%s story=%s fromItem=%s spell=%s spell2=%s type=%s; "
                    .. "BeforeDealDamage total=%s type=%s damages=%s; DealtDamage total=%s attack=%s",
                    tostring(d.caster == Uuid(guid) and "host" or d.caster), tostring(d.caster2), d.hitWith, tostring(d.story),
                    tostring(d.fromItem), tostring(d.spell), tostring(d.spell2), d.damageType,
                    tostring(seen.before.total), seen.before.type, tostring(seen.before.list),
                    tostring(seen.dealt.total), tostring(seen.dealt.attack)))
            end
            if seen.deal and seen.dealt and seen.before then return Finish() end
            E.Timer.WaitFor(1000, Finish)
        end },
        { "Ext.Stats.ExecuteFunctors", id = "LUA-046", ctx = "server", risk = "unproved", async = true, timeout = 20000, run = function (E, M, done)
            Functors(E, done, {
                spell = "Bg3mseIgt_FunctorDamage", text = "DealDamage(1,Fire)", type = "DealDamage", damages = 2,
                again = function (set, params) E.Stats.ExecuteFunctors(set, params) end })
        end },
        { "Ext.Stats.ExecuteFunctor", id = "LUA-046", ctx = "server", risk = "unproved", async = true, timeout = 20000, run = function (E, M, done)
            Functors(E, done, { spell = "Bg3mseIgt_FunctorHeal", text = "RegainHitPoints(3)", type = "RegainHitPoints" })
        end },
        { "Ext.Stats.ExecuteFunctor#paths", id = "LUA-046", risk = "pure", offline = true, run = function (E)
            -- The functor cases' ways through the game's objects, through the
            -- type information: each step is one the types have, and each ends
            -- at what the function it feeds takes.
            for name, path in pairs(FUNCTOR_PATHS) do
                C.equal(FollowTypes(E, path), path.to, "where " .. name .. " ends")
            end
            local stats = C.notNil(E.Types.GetTypeInfo("Module_Stats"), "the type information of Ext.Stats")
            local function FirstParam(fn)
                local method = C.notNil(stats.Methods[fn], "Ext.Stats." .. fn)
                return method.Params[1] and method.Params[1].TypeName
            end
            C.equal(FirstParam("ExecuteFunctor"), FUNCTOR_PATHS.spellFunctor.to, "what ExecuteFunctor takes")
            C.equal(FirstParam("ExecuteFunctors"), FUNCTOR_PATHS.eventSet.to, "what ExecuteFunctors takes")
        end },
        { "Ext.Stats.ExecuteFunctors#client", id = "LUA-046", ctx = "client", skip = CLIENT_FUNCTORS },
        { "Ext.Stats.ExecuteFunctor#client", id = "LUA-046", ctx = "client", skip = CLIENT_FUNCTORS },
        { "Ext.Osiris.RegisterListener#LeveledUp/1", id = "LOG-05", ctx = "server", run = function (E)
            -- The game registers the LeveledUp event with one parameter
            -- (esv::OsirisGameFunctions, RegisterNotification(..., 1, ...)):
            -- LeveledUp/2, which a mod of the owner's asks for, is not in any
            -- story. LeveledUp/1 must be found by name and arity.
            local id = E.Osiris.RegisterListener("LeveledUp", 1, "after", function () end)
            C.type(id, "number", "RegisterListener(LeveledUp, 1)")
            C.equal(E.Osiris.UnregisterListener(id), true, "UnregisterListener")
        end },
        { "Osi#DB_Players after the session loaded", id = "LOG-04", ctx = "server", run = function (E)
            -- The first server ticks of a session load run before the story
            -- is loaded (on Windows too: ScriptExtender::PreUpdate throws Tick
            -- in every state); once the session is loaded the database is there.
            local db = C.notNil(Osi.DB_Players, "Osi.DB_Players")
            local rows = db:Get(nil)
            C.type(rows, "table", "DB_Players:Get(nil)")
            return #rows .. " players"
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
