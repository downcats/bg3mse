-- Ext.UI (BG3Extender/Lua/Libs/ClientUI/Module.inl), client only: the
-- game's Noesis UI. The type registered here has this mod's own name and is
-- never put into the game's XAML.

local library = { name = "UI" }

function library.define(C)
    local function case(name, id, run, extra)
        local c = { "Ext.UI." .. name, id = id, ctx = "client", risk = "unproved", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    local TYPE = "Bg3mseIgtProbe"
    local function Register(E)
        return E.UI.RegisterType(TYPE, { Value = { Type = "String" } })
    end
    return {
        case("GetRoot", "LC-30", function (E)
            local root = C.notNil(E.UI.GetRoot(), "GetRoot")
            return tostring(E.Types.GetObjectType(root))
        end, { risk = "proved" }),
        case("GetStateMachine", "LC-31", function (E)
            local sm = C.notNil(E.UI.GetStateMachine(), "GetStateMachine")
            C.equal(E.Types.Validate(sm), true, "the state machine passes validation")
            -- RootState, a Noesis string after the Mac-only members of
            -- UIStateMachine (UI.h:535-541), read through GameUI past its
            -- withheld and aligned members (UI.h:1142-1155; ClientUI/Module.inl:75):
            -- text, not bytes of another member. State's members are not
            -- mapped on macOS (PropertyMaps/UI.inl:250-271).
            local root = C.type(sm.RootState, "string", "RootState")
            C.check(#root < 512 and not root:find("[%c\128-\255]"), "RootState is '%s'", root:sub(1, 80))
            C.check(math.type(sm.PlayerID) == "integer", "PlayerID is %s", tostring(sm.PlayerID))
            return string.format("RootState '%s', PlayerID %d%s", root, sm.PlayerID,
                #root > 0 and " [rows: LC-31, G-097, G-100, G-101, G-102]" or " (an empty RootState shows nothing)")
        end),
        case("SetState", "LC-32", function (E)
            -- Deprecated: a log line and nothing else on every platform.
            E.UI.SetState("Bg3mseIgtNoSuchState")
        end),
        case("RegisterType", "LC-33", function (E)
            C.equal(Register(E), true, "RegisterType")
        end),
        case("Instantiate", "LC-33", function (E)
            Register(E)
            local inst = C.notNil(E.UI.Instantiate(TYPE), "Instantiate")
            inst.Value = "probe"
            C.equal(inst.Value, "probe", "the instance's property read back")
        end),
        case("Box", "LC-34", function (E)
            C.notNil(E.UI.Box("bg3mse"), "Box('bg3mse')")
            C.notNil(E.UI.Box(3), "Box(3)")
            C.notNil(E.UI.Box(true), "Box(true)")
        end),
        case("GetPickingHelper", "LC-35", function (E)
            local helper = E.UI.GetPickingHelper(1) or E.UI.GetPickingHelper(0)
            C.notNil(helper, "the picking helper of player 1 or 0")
            C.equal(E.Types.Validate(helper), true, "the picking helper passes validation")
        end),
        case("GetCursorControl", "LC-35", function (E)
            local cc = C.notNil(E.UI.GetCursorControl(), "GetCursorControl")
            C.equal(E.Types.Validate(cc), true, "the cursor control passes validation")
            -- Read only: the members the layout proof covers.
            C.type(cc.ShowCursor, "boolean", "ShowCursor")
            C.type(cc.VisibleTimer, "number", "VisibleTimer")
            return string.format("ShowCursor %s, cursor %s, requested %s, overlay %s, %d overrides", tostring(cc.ShowCursor),
                tostring(cc.CursorName), tostring(cc.RequestedCursor), tostring(cc.CurrentOverlay), #cc.CursorOverrides)
        end),
        case("GetDragDrop", "LC-35", function (E)
            local player, data = 1, E.UI.GetDragDrop(1)
            if data == nil then player, data = 0, E.UI.GetDragDrop(0) end
            if data == nil then return "no drag data for player 1 or 0 (the manager's map has no entry)" end
            C.equal(E.Types.Validate(data), true, "the drag data passes validation")
            C.type(data.IsDragging, "boolean", "IsDragging")
            return string.format("player %d: IsDragging %s, Active %s, %d entities, at %s,%s", player, tostring(data.IsDragging),
                tostring(data.Active), #data.Entities, tostring(data.ScreenPosition[1]), tostring(data.ScreenPosition[2]))
        end),
        case("Defer", "LC-36", function (E, M, done)
            E.UI.Defer(function () done(true) end)
        end, { async = true }),
        case("EnableErrorReporting", "LC-37", nil,
            { skip = "writes over the game's Noesis error handler, and false writes null: neither gives back the game's own" }),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
