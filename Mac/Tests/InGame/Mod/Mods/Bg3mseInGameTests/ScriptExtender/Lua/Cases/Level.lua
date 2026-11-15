-- Ext.Level (BG3Extender/Lua/Libs/Level.inl), registered for the client and
-- the server alike, plus four server-only functions. Every query starts at
-- the character's position. AiGrid, AiPath and the physics scene have no
-- layout proof (LUA-065..068): all of these run after the proved cases.

local library = { name = "Level" }

function library.define(C)
    local STATIC_DYNAMIC = 0x05         -- PhysicsType Static | Dynamic
    local TERRAIN_SCENERY = 0x10 | 0x800 -- PhysicsGroupFlags Terrain | Scenery
    local function Above(E)
        local p = C.position()
        return { p[1], p[2] + 2.0, p[3] }, { p[1], p[2] - 10.0, p[3] }, p
    end
    local function Ahead(E)
        local p = C.position()
        return { p[1] + 1.5, p[2], p[3] }
    end
    local function case(name, id, run, extra)
        local c = { "Ext.Level." .. name, id = id, risk = "unproved", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    -- What an overlap test at the character's feet found (Level.inl:391-411
    -- hand back the PhysicsHitAll the scene filled, Physics.h:136-144),
    -- reported: how many hits, and how far from the character the nearest
    -- hit position is (the TestBox/TestSphere slot pair, Physics.h:171-186,
    -- reads its centre where the caller put it). What an overlap writes as a
    -- position is the game's, so only a position near the character names
    -- the row.
    local function Overlaps(E, hits, what)
        local n = math.max(#hits.Shapes, #hits.Positions, #hits.PhysicsGroup, #hits.Distances, #hits.Normals)
        local p, nearest = C.position(), nil
        for _, pos in ipairs(hits.Positions) do
            -- An overlap may leave a hit's position unset (zero).
            if pos[1] ~= 0 or pos[2] ~= 0 or pos[3] ~= 0 then
                local d = math.sqrt((pos[1] - p[1]) ^ 2 + (pos[2] - p[2]) ^ 2 + (pos[3] - p[3]) ^ 2)
                nearest = math.min(nearest or d, d)
            end
        end
        return string.format("%d hit(s): %d shapes, %d positions%s%s", n, #hits.Shapes, #hits.Positions,
            nearest and string.format(", nearest %.2f m from the character", nearest) or ", no position set",
            (nearest ~= nil and nearest < 5.0) and " [rows: G-087]" or "")
    end
    local function FindId(E, path)
        for id = 0, 4096 do
            if C.same(E.Level.GetPathById(id), path) then return id end
        end
        return nil
    end

    return {
        case("GetEntitiesOnTile", "LUA-065", function (E)
            local list = E.Level.GetEntitiesOnTile(C.position())
            C.type(list, "table", "GetEntitiesOnTile")
            return #list .. " entities on the character's tile"
        end),
        case("GetTileDebugInfo", "LUA-065", function (E)
            local info = C.notNil(E.Level.GetTileDebugInfo(C.position()), "GetTileDebugInfo")
            return E.Json.Stringify(info, { Beautify = false, IterateUserdata = true, LimitDepth = 1 })
        end),
        case("GetHeightsAt", "LUA-065", function (E)
            local p = C.position()
            local heights = E.Level.GetHeightsAt(p[1], p[3])
            C.check(type(heights) == "table" and #heights > 0, "no heights under the character")
            local nearest = math.huge
            for _, h in ipairs(heights) do nearest = math.min(nearest, math.abs(h - p[2])) end
            C.check(nearest < 2.0, "no height within 2 m of the character's (%s): %s", p[2], E.Json.Stringify(heights, { Beautify = false }))
        end),
        case("BeginPathfindingImmediate", "LUA-066", function (E)
            local path = C.notNil(E.Level.BeginPathfindingImmediate(C.character(), Ahead(E)), "BeginPathfindingImmediate")
            E.Level.ReleasePath(path)
        end),
        case("FindPath", "LUA-067", function (E)
            local path = C.notNil(E.Level.BeginPathfindingImmediate(C.character(), Ahead(E)), "BeginPathfindingImmediate")
            local ok, found = pcall(E.Level.FindPath, path)
            E.Level.ReleasePath(path)
            C.check(ok, "FindPath raised: %s", tostring(found))
            C.equal(found, true, "a path 1.5 m ahead was found")
        end),
        case("GetPathById", "LUA-066", function (E)
            local path = C.notNil(E.Level.BeginPathfindingImmediate(C.character(), Ahead(E)), "BeginPathfindingImmediate")
            local id = FindId(E, path)
            E.Level.ReleasePath(path)
            C.notNil(id, "the path's id in the grid's map")
            return "id " .. id
        end),
        case("GetActivePathfindingRequests", "LUA-066", function (E)
            local path = C.notNil(E.Level.BeginPathfindingImmediate(C.character(), Ahead(E)), "BeginPathfindingImmediate")
            local found = false
            for _, p in ipairs(E.Level.GetActivePathfindingRequests()) do if p == path then found = true end end
            E.Level.ReleasePath(path)
            C.check(found, "the new path is not among the active requests")
        end),
        case("ReleasePath", "LUA-066", function (E)
            local path = C.notNil(E.Level.BeginPathfindingImmediate(C.character(), Ahead(E)), "BeginPathfindingImmediate")
            E.Level.ReleasePath(path)
            for _, p in ipairs(E.Level.GetActivePathfindingRequests()) do
                C.check(p ~= path, "the released path is still active")
            end
        end),
        case("BeginPathfinding", "LUA-066", function (E, M, done)
            local path = E.Level.BeginPathfinding(C.character(), Ahead(E), function (p)
                done(p.GoalFound == true, "GoalFound " .. tostring(p.GoalFound))
            end)
            C.notNil(path, "BeginPathfinding")
        end, { async = true }),

        case("RaycastClosest", "LUA-068", function (E)
            local from, to, p = Above(E)
            local hit = C.notNil(E.Level.RaycastClosest(from, to, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "a hit straight down")
            C.check(math.abs(hit.Position[2] - p[2]) < 2.0, "the ground is at %s, the character at %s", tostring(hit.Position[2]), p[2])
            return E.Json.Stringify(hit.Position, { Beautify = false })
        end),
        case("RaycastAny", "LUA-068", function (E)
            local from, to = Above(E)
            C.equal(E.Level.RaycastAny(from, to, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), true, "a hit straight down")
        end),
        case("RaycastAll", "LUA-068", function (E)
            local from, to = Above(E)
            local hits = C.notNil(E.Level.RaycastAll(from, to, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "RaycastAll")
            C.check(#hits.Positions > 0, "no hits straight down")
        end),
        case("SweepSphereClosest", "LUA-068", function (E)
            local from, to = Above(E)
            C.notNil(E.Level.SweepSphereClosest(from, to, 0.2, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "a sphere swept down")
        end),
        case("SweepCapsuleClosest", "LUA-068", function (E)
            local from, to = Above(E)
            C.notNil(E.Level.SweepCapsuleClosest(from, to, 0.2, 0.3, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "a capsule swept down")
        end),
        case("SweepBoxClosest", "LUA-068", function (E)
            local from, to = Above(E)
            C.notNil(E.Level.SweepBoxClosest(from, to, { 0.2, 0.2, 0.2 }, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "a box swept down")
        end),
        case("SweepSphereAll", "LUA-068", function (E)
            local from, to = Above(E)
            C.notNil(E.Level.SweepSphereAll(from, to, 0.2, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "SweepSphereAll")
        end),
        case("SweepCapsuleAll", "LUA-068", function (E)
            local from, to = Above(E)
            C.notNil(E.Level.SweepCapsuleAll(from, to, 0.2, 0.3, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "SweepCapsuleAll")
        end),
        case("SweepBoxAll", "LUA-068", function (E)
            local from, to = Above(E)
            C.notNil(E.Level.SweepBoxAll(from, to, { 0.2, 0.2, 0.2 }, STATIC_DYNAMIC, TERRAIN_SCENERY, 0, 0), "SweepBoxAll")
        end),
        case("TestBox", "LUA-068", function (E)
            local hits = C.notNil(E.Level.TestBox(C.position(), { 1, 1, 1 }, STATIC_DYNAMIC, TERRAIN_SCENERY, 0), "TestBox")
            return Overlaps(E, hits, "TestBox")
        end),
        case("TestSphere", "LUA-068", function (E)
            local hits = C.notNil(E.Level.TestSphere(C.position(), 1.0, STATIC_DYNAMIC, TERRAIN_SCENERY, 0), "TestSphere")
            return Overlaps(E, hits, "TestSphere")
        end),

        -- Server only.
        case("CreateSurfaceAction", "LUA-069", function (E)
            -- Made and never executed (the one object stays allocated; nothing
            -- of it reaches the level or the save).
            local action = C.notNil(E.Level.CreateSurfaceAction("CreateSurface"), "CreateSurfaceAction")
            return E.Types.GetObjectType(action)
        end, { ctx = "server" }),
        case("ExecuteSurfaceAction", "LUA-069", function (E, M, done)
            -- A puddle of water, 0.5 m across, 4 m beside the character, for
            -- 6 s: the surface manager runs the action (SurfaceManager
            -- AddAction), the tile shows Water, and once the duration is
            -- over the tile is back to what it was. Visible: a small puddle
            -- for a few seconds; nobody stands in it, so nobody gets wet.
            -- Opt-in ("Extras": ["Surface"]): esv::CreateSurfaceAction has
            -- no layout proof (the game builds it inline: no constructor to
            -- size it by, unlike its sibling actions in Mac/Layout/
            -- BoundObjects.cpp), and the case writes four of its members.
            -- The guard below only refuses an action that cannot be a new
            -- one; it is no proof.
            local p = C.position()
            local spot = { p[1] + 4.0, p[2], p[3] }
            local function Surface()
                local tile = E.Level.GetTileDebugInfo(spot)
                if tile == nil then return nil end
                return tostring(tile.GroundSurface)
            end
            local before = Surface()
            if before == nil then C.skip("no AI grid tile 4 m beside the character") end
            if before ~= "None" then C.skip("the tile 4 m beside the character already has a surface: " .. before) end
            local action = C.notNil(E.Level.CreateSurfaceAction("CreateSurface"), "CreateSurfaceAction")
            local fresh = tostring(action.SurfaceType) == "None" and type(action.Duration) == "number"
                and action.Duration >= 0 and action.Duration < 1e6 and type(action.Radius) == "number"
                and action.Radius >= 0 and action.Radius < 1e3
            if not fresh then
                C.skip("a new action does not read as one (SurfaceType " .. tostring(action.SurfaceType) .. ", Duration "
                    .. tostring(action.Duration) .. ", Radius " .. tostring(action.Radius) .. "): not executed")
            end
            action.Position = spot
            action.SurfaceType = "Water"
            action.Duration = 6.0
            action.Radius = 0.5
            E.Level.ExecuteSurfaceAction(action)
            -- The surface manager owns the action from here on: it is not read again.
            local started, appeared = E.Timer.MonotonicTime(), nil
            local sub
            sub = E.Events.Tick:Subscribe(function ()
                local now = E.Timer.MonotonicTime()
                local ok, surface = pcall(Surface)
                if not ok then
                    E.Events.Tick:Unsubscribe(sub)
                    done(false, "GetTileDebugInfo raised: " .. tostring(surface))
                elseif appeared == nil and surface == "Water" then
                    appeared = now - started
                elseif appeared ~= nil and surface == before then
                    E.Events.Tick:Unsubscribe(sub)
                    done(true, string.format("Water after %d ms, gone again after %d ms", appeared, now - started))
                elseif now - started > 25000 then
                    E.Events.Tick:Unsubscribe(sub)
                    done(false, appeared == nil and ("no Water on the tile within 25 s (it shows " .. tostring(surface) .. ")")
                        or ("the Water was still there after 25 s (it came after " .. appeared .. " ms)"))
                end
            end)
        end, { ctx = "server", async = true, timeout = 30000, optIn = "Surface" }),
        case("GetLevelInfo", "LUA-070", function (E)
            -- Norbyte's GetLevelInfo (Level.inl) looks the name up in the
            -- server's LevelManager -> LocalTemplateManager -> LevelDataManager
            -- map. Run 2 found no entry for the character's Level component's
            -- name. Here every name the game gives a level in this session is
            -- tried: the character's, its root level's, every root level's,
            -- the sub-levels' and the region Osiris gives. Some names found
            -- and not the current level's: a wrong name. None at all: the
            -- entry was not found, or the LevelData it gave failed Lua's
            -- validation (an error line names the property; run 3's was
            -- TrackedTemplates, whose values the macOS game keeps as arrays),
            -- after which every LevelData reads as nil.
            local character = C.character()
            local component = character.Level
            if component == nil then C.skip("the character has no Level component") end
            local names, order = {}, {}
            local function Add(name, source)
                if name == nil then return end
                name = tostring(name)
                if name ~= "" and names[name] == nil and #order < 400 then
                    names[name] = source
                    order[#order + 1] = name
                end
            end
            Add(component.LevelName, "Level")
            local root = nil
            pcall(function ()
                local entity = component.RootLevel
                if entity ~= nil and entity.LevelRoot ~= nil then root = tostring(entity.LevelRoot.LevelName) end
            end)
            Add(root, "root")
            pcall(function ()
                for _, entity in ipairs(E.Entity.GetAllEntitiesWithComponent("LevelRoot")) do Add(entity.LevelRoot.LevelName, "LevelRoot") end
            end)
            pcall(function ()
                for _, entity in ipairs(E.Entity.GetAllEntitiesWithComponent("LevelInstance")) do
                    Add(entity.LevelInstance.SubLevelName, "sub-level")
                    Add(entity.LevelInstance.LevelInstanceTemplate, "sub-level template")
                end
            end)
            if character.Uuid ~= nil and Osi.GetRegion ~= nil then
                local ok, region = pcall(Osi.GetRegion, character.Uuid.EntityUuid)
                if ok then Add(region, "region") end
            end

            local found = {}
            for _, name in ipairs(order) do
                local info = E.Level.GetLevelInfo(name)
                if info ~= nil then
                    local desc = info.LevelDesc
                    found[#found + 1] = string.format("%s (%s; LevelDesc %s/%s)", name, names[name],
                        tostring(desc and desc.LevelName), tostring(desc and desc.UniqueKey))
                end
            end
            local current = root or tostring(component.LevelName)
            local summary = string.format("level %s, root %s; %d names tried, %d with an entry: %s",
                tostring(component.LevelName), tostring(root), #order, #found, table.concat(found, ", ", 1, math.min(#found, 8)))
            C.check(#found > 0, "no name gives a LevelData (none found, or the class failed validation: see the error lines before this): %s", summary)
            local info = E.Level.GetLevelInfo(current)
            C.check(info ~= nil, "the current level %s has no entry, other names do (a wrong name?): %s", current, summary)
            C.equal(info.LevelDesc and tostring(info.LevelDesc.LevelName), current, "the entry's LevelDesc.LevelName")
            return summary
        end, { ctx = "server" }),
        case("AddActivePersistentLevelTemplate", "LUA-070", function (E)
            -- Only the refusal: a parent level the server's level manager does
            -- not have (its map of levels, proved) adds nothing anywhere.
            C.equal(E.Level.AddActivePersistentLevelTemplate("Bg3mseIgtNoSuchLevel", "Bg3mseIgtNoSuchSubLevel", "Bg3mseIgtNoSuchInstance"),
                nil, "AddActivePersistentLevelTemplate on a level that does not exist")
            return "refusal path only"
        end, { ctx = "server" }),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
