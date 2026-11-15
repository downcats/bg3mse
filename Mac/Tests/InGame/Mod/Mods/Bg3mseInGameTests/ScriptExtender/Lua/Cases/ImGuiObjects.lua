-- Every ImGui object class and method Norbyte's extender gives Lua
-- (BG3Extender/GameDefinitions/PropertyMaps/IMGUI.inl, the objects in
-- BG3Extender/Extender/Client/IMGUI/Objects.h and IMGUI.cpp), client only,
-- audit 2's work package WP-34. Each case builds its own window, sets every
-- property it can write and reads it back, reads the others, calls the
-- methods and checks what they did a few of the overlay's frames later, and
-- destroys the window again whatever happens.
--
-- What the overlay draws only shows in what it writes back: StatusFlags
-- (StyledRenderable::UpdateStatusFlags, after each item it draws: Visible
-- once the item was laid out in its window), LastSize and LastPosition
-- (WindowBase::EndDrawingWindow). An item that is not drawn keeps the
-- StatusFlags it had, so a case clears them (they are writable) before it
-- waits to see that something is no longer drawn.
--
-- Events the cases cause themselves: StyledRenderable:Activate() sets
-- RequestActivate, and the item's next drawing asks ImGui to activate the
-- item whose id is its Label's (PushWindowStyleChanges,
-- ImGui::ActivateItemByID); the frame after, the item behaves as if pressed.
-- ImGui keeps one such request at a time, so every case that calls
-- Activate() runs alone (exclusive). Events reach Lua after the frame
-- (IMGUIObjectManager::ClientUpdate). What needs the mouse or the keyboard
-- is in one opt-in case at the end ("ImGuiClicks" in the trigger's Extras),
-- with a window telling the owner what to do and a Skip button.
--
-- A case's detail ends with " [rows: ...]" when it also proves audit rows
-- other than its id.

local library = { name = "ImGuiObjects" }

-- Realtime for a dozen or more of the overlay's frames.
local FRAMES_MS = 500
-- How long a case waits for an event it caused.
local EVENT_MS = 3000
-- How long the opt-in case waits for the owner.
local OWNER_MS = 600000

local EVENTS = { "OnClick", "OnRightClick", "OnActivate", "OnDeactivate", "OnHoverEnter", "OnHoverLeave",
    "OnDragStart", "OnDragEnd", "OnDragDrop" }

function library.define(C)
    local FONT = "Mods/" .. C.ModuleFolder .. "/Fonts/ProggyClean.ttf"
    local FONT_KEY = "Bg3mseIgtProggy"
    -- extui::InvalidHandle (IMGUIShared.h), pushed as a lua_Integer (LuaPush.h).
    local INVALID = -1
    local TITLE = "bg3mse in-game test: imgui "

    -- The class of an object, as its __tostring names it
    -- (ImguiObjectProxyMetatable::ToString: "Button (1900000000000002)").
    local function Kind(obj)
        return (tostring(obj):match("^(%a+)"))
    end

    -- A destroyed object raises on any access (ImguiObjectProxyMetatable::Index).
    local function Dead(obj)
        return not pcall(function () return obj.Handle end)
    end

    local function Seen(obj)
        return obj.StatusFlags.Visible == true
    end

    local function V(v)
        return string.format("%gx%g", v[1], v[2])
    end

    local function Copy(v)
        local out = {}
        for i = 1, #v do out[i] = v[i] end
        return out
    end

    -- A delegate property reads back the function it was given and nil
    -- once cleared (LuaDelegate, LuaPush.h).
    local function Delegate(obj, name)
        local fn = function () end
        obj[name] = fn
        C.check(rawequal(obj[name], fn), "%s does not read back the function it was given", name)
        obj[name] = nil
        C.check(rawequal(obj[name], nil), "%s is not nil once cleared", name)
    end

    -- A label of a P_BITMASK property: one boolean property per label
    -- (AddBitfieldProperty, LuaObjectProxies.cpp). keep leaves it set.
    local function Flag(obj, label, keep, default)
        C.equal(obj[label], default or false, label .. " at first")
        obj[label] = not (default or false)
        C.equal(obj[label], not (default or false), label .. " after it was set")
        if not keep then
            obj[label] = default or false
            C.equal(obj[label], default or false, label .. " after it was put back")
        end
    end

    local placed = 0

    -- A case's run: its windows, destroyed when it ends; the steps it takes
    -- on the overlay's later frames; its end, once.
    local function Flow(E, done, body)
        local t = { windows = {}, cleanups = {}, over = false }
        function t.finish(ok, detail)
            if t.over then return end
            t.over = true
            for _, fn in ipairs(t.cleanups) do pcall(fn) end
            for i = #t.windows, 1, -1 do
                local w = t.windows[i]
                pcall(function () w:Destroy() end)
            end
            done(ok, detail)
        end
        local function Guard(fn)
            local ok, err = pcall(fn, t)
            if ok then return end
            if type(err) == "table" and err.reason ~= nil then
                t.finish("skip", err.reason)
            else
                t.finish(false, tostring(err))
            end
        end
        function t.after(ms, fn)
            E.Timer.WaitForRealtime(ms, function ()
                if not t.over then Guard(fn) end
            end)
        end
        function t.frames(fn)
            t.after(FRAMES_MS, fn)
        end
        -- Polls cond until it holds, then runs fn; otherwise, after ms,
        -- runs late (or ends the case with what was not seen).
        function t.until_(cond, what, fn, late, ms)
            local waited = 0
            local function Poll()
                if cond() then
                    fn(t)
                    return
                end
                waited = waited + 100
                if waited >= (ms or EVENT_MS) then
                    if late ~= nil then
                        late(t)
                    else
                        t.finish(false, what .. ": not seen within " .. (ms or EVENT_MS) .. " ms")
                    end
                    return
                end
                t.after(100, Poll)
            end
            t.after(100, Poll)
        end
        function t.cleanup(fn)
            t.cleanups[#t.cleanups + 1] = fn
        end
        -- A window of the case's own, on screen and drawn as asked: no
        -- imgui.ini state of an earlier run (NoSavedSettings), not collapsed.
        function t.window(title, size)
            local w = C.notNil(E.IMGUI.NewWindow(TITLE .. title), "NewWindow")
            t.windows[#t.windows + 1] = w
            w.NoSavedSettings = true
            local slot = placed % 8
            placed = placed + 1
            w:SetPos({ 60 + slot * 40, 80 + slot * 30 }, "Always")
            w:SetSize(size or { 520, 420 }, "Always")
            w:SetCollapsed(false, "Always")
            return w
        end
        Guard(body)
        return t
    end

    local function case(name, id, body, extra)
        local c = { "Ext.IMGUI." .. name, id = id, ctx = "client", async = true,
            run = function (E, M, done) Flow(E, done, function (t) body(t, E) end) end }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end

    -- An icon the game has and its atlas: the main-hand attack's icon, as
    -- StaticData.lua takes it, else the first of the atlas map's icons.
    local function IconAndAtlas(E)
        local icon = nil
        pcall(function () icon = E.Stats.Get("Target_MainHandAttack").Icon end)
        if type(icon) ~= "string" or icon == "" or E.StaticData.GetIconAtlas(icon) == nil then
            icon = nil
            local manager = E.StaticData.GetTextureAtlasManager()
            if manager ~= nil then
                local names = {}
                pcall(function ()
                    for name in pairs(manager.IconMap) do
                        names[#names + 1] = tostring(name)
                        if #names >= 64 then break end
                    end
                end)
                table.sort(names)
                icon = names[1]
            end
        end
        if icon == nil then C.skip("no icon in the client's texture atlas map") end
        -- The atlas object lives as long as the call it was fetched in
        -- (Lua/Shared/LuaLifetime.h), and the image cases go on in later
        -- frames: what they read of it is copied here.
        local atlas = C.notNil(E.StaticData.GetIconAtlas(icon), "GetIconAtlas(" .. icon .. ")")
        return icon, { Path = atlas.Path, TextureUuid = atlas.TextureUuid, IconWidth = atlas.IconWidth, IconHeight = atlas.IconHeight,
            TextureWidth = atlas.TextureWidth, TextureHeight = atlas.TextureHeight }
    end

    local function NotBound(what)
        return what .. " was not bound: ImageReference::Bind found no icon or texture (IMGUI.cpp:43-104: "
            .. "GameTexturesUsable, the atlas map, or the texture load refused it)"
    end

    -- A widget class with a Value: its value, range, components and flags
    -- (TreeParent::AddDrag and the others, IMGUI.cpp:669-728).
    local function ValueWidget(t, w, kind, el, o)
        C.equal(Kind(el), kind, "the class")
        C.vecNear(el.Value, o.initial, "Value as added")
        if o.min ~= nil then
            C.vecNear(el.Min, o.min, "Min as added")
            C.vecNear(el.Max, o.max, "Max as added")
            el.Min = o.newMin
            C.vecNear(el.Min, o.newMin, "Min")
            el.Max = o.newMax
            C.vecNear(el.Max, o.newMax, "Max")
        end
        C.equal(el.Components, 1, "Components at first")
        el.Components = o.components
        C.equal(el.Components, o.components, "Components")
        -- The three widgets of floats are made with the item flag that
        -- keeps ImGui's older behaviour, the value following every edit
        -- (their constructors, IMGUI.cpp); the three of integers are not.
        C.equal(el.LiveEditOnInputScalar, o.liveEdit == true, "LiveEditOnInputScalar at first")
        if o.speed ~= nil then
            C.near(el.Speed, 1.0, "Speed at first")
            el.Speed = o.speed
            C.near(el.Speed, o.speed, "Speed")
        end
        if o.step ~= nil then
            C.check(rawequal(el.Step, nil), "Step before one is set is %s", tostring(el.Step))
            el.Step = o.step
            C.near(el.Step, o.step, "Step")
            el.Step = nil
            C.check(rawequal(el.Step, nil), "Step once cleared is %s", tostring(el.Step))
            el.Step = o.step
        end
        el.Value = o.value
        C.vecNear(el.Value, o.value, "Value")
        Flag(el, o.flag, true)
        if o.vertical then
            C.equal(el.Vertical, false, "Vertical at first")
            C.vecNear(el.VerticalSize, { 1, 1 }, "VerticalSize at first")
            el.Vertical = true
            C.equal(el.Vertical, true, "Vertical")
            el.VerticalSize = { 30, 110 }
            C.vecNear(el.VerticalSize, { 30, 110 }, "VerticalSize")
        end
        Delegate(el, "OnChange")
        t.frames(function ()
            C.check(Seen(el), "the %s was not drawn", kind)
            C.vecNear(el.Value, o.value, "Value after the frames")
            t.finish(true, string.format("%s drawn with %d components%s", kind, o.components,
                o.vertical and ", vertical" or ""))
        end)
    end

    return {
        case("Renderable#members", "W1-11", function (t, E)
            local w = t.window("Renderable")
            local text = w:AddText("bg3mse Renderable child")
            C.type(w.Handle, "number", "Window.Handle")
            -- The object's type is in the handle's top byte (IMGUIObjectPool::Create).
            C.equal(w.Handle >> 56, 0, "the type in a Window's Handle (IMGUIObjectType::Window)")
            C.equal(text.Handle >> 56, 16, "the type in a Text's Handle (IMGUIObjectType::Text)")
            C.equal(w.Parent, INVALID, "a window's Parent")
            C.check(rawequal(w.ParentElement, nil), "a window has a ParentElement")
            C.equal(text.Parent, w.Handle, "the child's Parent")
            C.check(text.ParentElement == w, "the child's ParentElement is not its window")
            local data = { mark = "bg3mse" }
            C.check(rawequal(text.UserData, nil), "UserData before it is set")
            text.UserData = data
            C.check(rawequal(text.UserData, data), "UserData does not read back the table it was given")
            text.UserData = nil
            C.check(rawequal(text.UserData, nil), "UserData once cleared")
            C.raises(function () text.Handle = 1 end)
            C.raises(function () text.Parent = 1 end)
            C.raises(function () text.ParentElement = w end)
            C.raises(function () text.Destroy = 1 end)
            C.raises(function () return text.NoSuchProperty end)
            C.raises(function () text.NoSuchProperty = 1 end)
            text:Destroy()
            C.check(Dead(text), "the child is still there after Destroy")
            C.equal(#w.Children, 0, "the window's children after the child's Destroy")
            t.frames(function ()
                w:Destroy()
                C.check(Dead(w), "the window is still there after Destroy")
                t.finish(true, "Handle, Parent, ParentElement, UserData; Destroy of a child and of a window")
            end)
        end),

        case("StyledRenderable#members", "W1-12", function (t, E)
            -- The mod's font (IMGUI.lua's LoadFont case loads it too; loading
            -- it again with the same size is a no-op that returns true).
            C.equal(E.IMGUI.LoadFont(FONT_KEY, FONT, 13.0), true, "LoadFont of the mod's font")
            local w = t.window("StyledRenderable")
            local a = w:AddText("bg3mse styled A")
            local b = w:AddText("bg3mse styled B")
            local hidden = w:AddText("bg3mse styled hidden")
            -- What Objects.h starts them with.
            C.equal(a.Label, "bg3mse styled A", "Label")
            C.equal(a.IDContext, "", "IDContext at first")
            C.equal(a.Visible, true, "Visible at first")
            C.equal(a.Font, "", "Font at first")
            C.check(rawequal(a.PositionOffset, nil) and rawequal(a.AbsolutePosition, nil), "positions at first")
            C.check(rawequal(a.ItemWidth, nil) and rawequal(a.TextWrapPos, nil), "widths at first")
            a.Label = "bg3mse styled A2"
            C.equal(a.Label, "bg3mse styled A2", "Label")
            a.IDContext = "bg3mseStyledA"
            C.equal(a.IDContext, "bg3mseStyledA", "IDContext")
            b.SameLine = true
            C.equal(b.SameLine, true, "SameLine")
            b.SamePosition = true
            C.equal(b.SamePosition, true, "SamePosition")
            b.SamePosition = false
            hidden.Visible = false
            C.equal(hidden.Visible, false, "Visible")
            a.WasHovered = true
            C.equal(a.WasHovered, true, "WasHovered")
            a.WasHovered = false
            a.CanDrag = true
            C.equal(a.CanDrag, true, "CanDrag")
            a.CanDrag = false
            C.equal(a.DragDropType, "", "DragDropType at first")
            a.DragDropType = "Bg3mseIgtStyled"
            C.equal(a.DragDropType, "Bg3mseIgtStyled", "DragDropType")
            -- StyledRenderable::SetDragDropType: at most the payload type's 32 characters.
            C.raises(function () a.DragDropType = string.rep("x", 40) end)
            C.equal(a.DragDropType, "Bg3mseIgtStyled", "DragDropType after a refused one")
            C.check(rawequal(a.DragPreview, nil), "DragPreview while nothing is dragged")
            a.Font = FONT_KEY
            C.equal(a.Font, FONT_KEY, "Font")
            a.PositionOffset = { 4, 2 }
            C.vecNear(a.PositionOffset, { 4, 2 }, "PositionOffset")
            a.AbsolutePosition = { 10, 40 }
            C.vecNear(a.AbsolutePosition, { 10, 40 }, "AbsolutePosition")
            a.AbsolutePosition = nil
            C.check(rawequal(a.AbsolutePosition, nil), "AbsolutePosition once cleared")
            a.ItemWidth = 160
            C.near(a.ItemWidth, 160, "ItemWidth")
            a.TextWrapPos = 400
            C.near(a.TextWrapPos, 400, "TextWrapPos")
            -- ItemFlags: GuiItemFlags labels.
            Flag(a, "Disabled")
            Flag(a, "NoTabStop")
            -- DragFlags, DropFlags and StatusFlags are bitfield values: a
            -- label, a list of labels or a number in; an object out whose
            -- labels read as booleans (LuaBitfieldValue.inl).
            a.DragFlags = { "NoPreviewTooltip" }
            C.equal(a.DragFlags.NoPreviewTooltip, true, "DragFlags.NoPreviewTooltip")
            a.DropFlags = "NoDrawDefaultRect"
            C.equal(a.DropFlags.NoDrawDefaultRect, true, "DropFlags.NoDrawDefaultRect")
            a.StatusFlags = { "Edited" }
            C.equal(a.StatusFlags.Edited, true, "StatusFlags as written")
            a.StatusFlags = 0
            for _, ev in ipairs(EVENTS) do Delegate(a, ev) end
            -- Style variables and colours (StyledRenderable::GetStyleVar and
            -- the others): nil until set; a variable of two reads as its first.
            C.check(rawequal(a:GetStyle("Alpha"), nil), "GetStyle before SetStyle")
            a:SetStyle("Alpha", 0.75)
            C.near(a:GetStyle("Alpha"), 0.75, "GetStyle(Alpha)")
            a:SetStyle("Alpha", 0.9)
            C.near(a:GetStyle("Alpha"), 0.9, "GetStyle(Alpha) set again")
            a:SetStyle("FramePadding", 4, 6)
            C.near(a:GetStyle("FramePadding"), 4, "GetStyle(FramePadding)")
            C.check(rawequal(a:GetColor("Text"), nil), "GetColor before SetColor")
            a:SetColor("Text", { 1.0, 0.8, 0.4, 1.0 })
            C.vecNear(a:GetColor("Text"), { 1.0, 0.8, 0.4, 1.0 }, "GetColor(Text)")
            C.equal(a.RequestActivate, false, "RequestActivate at first")
            -- A Text has no item id to activate: the request is all there is.
            b.RequestActivate = true
            C.equal(b.RequestActivate, true, "RequestActivate")
            a:Activate()
            C.equal(a.RequestActivate, true, "RequestActivate after Activate")
            t.frames(function ()
                C.equal(a.RequestActivate, false, "RequestActivate after the next drawing (PushWindowStyleChanges clears it)")
                C.check(Seen(a), "the styled text was not drawn")
                C.check(Seen(b), "the text on the same line was not drawn")
                C.check(not Seen(hidden), "the text with Visible = false was drawn")
                C.type(a.StatusFlags.__Value, "number", "StatusFlags.__Value")
                t.finish(true, "every member written and read back; drawn in the mod's font; Visible = false not drawn [rows: LC-12, OVL-38]")
            end)
        end),

        case("TreeParent#children", "W1-13", function (t, E)
            local w = t.window("TreeParent", { 560, 700 })
            local list = {}
            local function Add(kind, obj, label)
                C.equal(Kind(obj), kind, "what Add" .. kind .. " made")
                C.equal(obj.Parent, w.Handle, kind .. ".Parent")
                if label ~= nil then C.equal(obj.Label, label, kind .. ".Label") end
                list[#list + 1] = obj
                return obj
            end
            local group = Add("Group", w:AddGroup("bg3mse all group"), "bg3mse all group")
            Add("CollapsingHeader", w:AddCollapsingHeader("bg3mse all header"), "bg3mse all header")
            Add("TabBar", w:AddTabBar("bg3mse all tabs"), "bg3mse all tabs")
            Add("Tree", w:AddTree("bg3mse all tree"), "bg3mse all tree")
            local tbl = Add("Table", w:AddTable("bg3mse all table", 2), "bg3mse all table")
            C.equal(tbl.Columns, 2, "the table's Columns as added")
            Add("Popup", w:AddPopup("bg3mse all popup"), "bg3mse all popup")
            Add("Menu", w:AddMenu("bg3mse all menu"), "bg3mse all menu")
            local sel = Add("Selectable", w:AddSelectable("bg3mse all selectable"), "bg3mse all selectable")
            C.check(rawequal(sel.Size, nil), "a Selectable's Size when none is given")
            local sel2 = Add("Selectable", w:AddSelectable("bg3mse all selectable sized", { "AllowDoubleClick" }, { 140, 24 }))
            C.equal(sel2.AllowDoubleClick, true, "the Selectable's flags as added")
            C.vecNear(sel2.Size, { 140, 24 }, "the Selectable's Size as added")
            Add("Button", w:AddButton("bg3mse all button"), "bg3mse all button")
            Add("Text", w:AddText("bg3mse all text"), "bg3mse all text")
            Add("TextLink", w:AddTextLink("bg3mse all link"), "bg3mse all link")
            Add("BulletText", w:AddBulletText("bg3mse all bullet"), "bg3mse all bullet")
            Add("SeparatorText", w:AddSeparatorText("bg3mse all separator text"), "bg3mse all separator text")
            Add("Spacing", w:AddSpacing())
            local dummy = Add("Dummy", w:AddDummy(12, 8))
            C.near(dummy.Width, 12, "the Dummy's Width as added")
            C.near(dummy.Height, 8, "the Dummy's Height as added")
            Add("NewLine", w:AddNewLine())
            Add("Separator", w:AddSeparator())
            C.equal(Add("Checkbox", w:AddCheckbox("bg3mse all checkbox", true)).Checked, true, "Checked as added")
            C.equal(Add("RadioButton", w:AddRadioButton("bg3mse all radio", true)).Active, true, "Active as added")
            C.equal(Add("InputText", w:AddInputText("bg3mse all input", "abc")).Text, "abc", "Text as added")
            Add("Combo", w:AddCombo("bg3mse all combo"), "bg3mse all combo")
            local drag = Add("DragScalar", w:AddDrag("bg3mse all drag", 0.25, 0, 1))
            C.vecNear(drag.Value, { 0.25, 0.25, 0.25, 0.25 }, "the drag's Value as added")
            C.vecNear(Add("DragInt", w:AddDragInt("bg3mse all drag int", 3, 0, 10)).Max, { 10, 10, 10, 10 }, "the int drag's Max")
            C.vecNear(Add("SliderScalar", w:AddSlider("bg3mse all slider", 0.5, 0, 2)).Max, { 2, 2, 2, 2 }, "the slider's Max")
            C.vecNear(Add("SliderInt", w:AddSliderInt("bg3mse all slider int", 4, 0, 8)).Value, { 4, 4, 4, 4 }, "the int slider's Value")
            C.vecNear(Add("InputScalar", w:AddInputScalar("bg3mse all input scalar", 1.5)).Value, { 1.5, 1.5, 1.5, 1.5 }, "the scalar's Value")
            C.vecNear(Add("InputInt", w:AddInputInt("bg3mse all input int", 7)).Value, { 7, 7, 7, 7 }, "the int's Value")
            C.vecNear(Add("ColorEdit", w:AddColorEdit("bg3mse all color edit", { 0.2, 0.4, 0.6 })).Color, { 0.2, 0.4, 0.6, 1.0 }, "the colour as added")
            C.vecNear(Add("ColorPicker", w:AddColorPicker("bg3mse all color picker", { 0.6, 0.4, 0.2 })).Color, { 0.6, 0.4, 0.2, 1.0 }, "the picker's colour")
            Add("ProgressBar", w:AddProgressBar())
            -- Last: a child window without a Size takes the room left.
            Add("ChildWindow", w:AddChildWindow("bg3mse all child window"), "bg3mse all child window")
            local children = w.Children
            C.equal(#children, #list, "the window's Children")
            for i, obj in ipairs(list) do
                C.check(children[i] == obj, "Children[%d] is not the %s added there", i, Kind(obj))
            end

            -- RemoveChild destroys; DetachChild keeps the object without a
            -- parent; AttachChild takes one without a parent only;
            -- RemoveAllChildren destroys them all (IMGUI.cpp:755-812).
            local first = group:AddText("bg3mse all group child 1")
            local second = group:AddText("bg3mse all group child 2")
            C.equal(#group.Children, 2, "the group's Children")
            C.equal(group:RemoveChild(first), true, "RemoveChild")
            C.check(Dead(first), "the removed child is still there")
            C.equal(#group.Children, 1, "the group's Children after RemoveChild")
            -- A destroyed object as an argument raises (LuaImguiProxy.inl, GetGeneric).
            C.raises(function () group:RemoveChild(first) end)
            C.equal(group:DetachChild(second), true, "DetachChild")
            C.check(not Dead(second), "the detached child was destroyed")
            C.equal(second.Parent, INVALID, "the detached child's Parent")
            C.check(rawequal(second.ParentElement, nil), "the detached child has a ParentElement")
            C.equal(#group.Children, 0, "the group's Children after DetachChild")
            C.equal(group:DetachChild(second), false, "DetachChild of what is not a child")
            local other = w:AddGroup("bg3mse all other group")
            C.equal(other:AttachChild(second), true, "AttachChild")
            C.check(second.ParentElement == other, "the attached child's ParentElement")
            C.equal(#other.Children, 1, "the other group's Children after AttachChild")
            C.equal(group:AttachChild(second), false, "AttachChild of what has a parent")
            local third = other:AddButton("bg3mse all other group button")
            other:RemoveAllChildren()
            C.equal(#other.Children, 0, "Children after RemoveAllChildren")
            C.check(Dead(second) and Dead(third), "RemoveAllChildren left a child")
            t.frames(function ()
                -- The window is taller than the screen may be, and ImGui marks
                -- Visible only what is inside it (ItemAdd): the drawing of each
                -- class is its own case's; here the first rows only.
                C.check(w.LastSize[2] > 0, "the window was not drawn")
                for _, obj in ipairs({ list[2], list[4], list[7], list[8] }) do
                    C.check(Seen(obj), "the %s at the top was not drawn", Kind(obj))
                end
                local kinds = {}
                for _, obj in ipairs(list) do kinds[Kind(obj)] = true end
                w:Destroy()
                for _, obj in ipairs(list) do
                    C.check(Dead(obj), "the %s outlived its window", Kind(obj))
                end
                C.check(Dead(other), "the other group outlived its window")
                t.finish(true, string.format("%d children of %d classes added, drawn and destroyed with the window [rows: LC-14, W1-29]",
                    #list, C.count(kinds)))
            end)
        end),

        case("WindowBase#members", "W1-14", function (t, E)
            local w = t.window("WindowBase")
            w:AddText("bg3mse WindowBase: this window moves and resizes by itself")
            Flag(w, "NoResize", true)
            Flag(w, "NoScrollWithMouse", true)
            w:SetPos({ 200, 160 }, "Always")
            w:SetSize({ 420, 260 }, "Always")
            w:SetBgAlpha(0.85)
            w:SetFocus()
            w:SetScroll({ 0, 0 })
            w:SetContentSize({ 400, 600 })
            t.frames(function ()
                C.vecNear(w.LastPosition, { 200, 160 }, "LastPosition after SetPos", 1)
                C.vecNear(w.LastSize, { 420, 260 }, "LastSize after SetSize", 1)
                -- The constraints clamp the size asked for in the same frame.
                w:SetSizeConstraints({ 300, 200 }, { 480, 320 })
                w:SetSize({ 900, 900 }, "Always")
                t.frames(function ()
                    C.vecNear(w.LastSize, { 480, 320 }, "LastSize within the size constraints", 1)
                    w:SetSizeConstraints()
                    w:SetContentSize()
                    w:SetScroll()
                    w:SetBgAlpha()
                    -- A pivot of 0.5: the window's centre at the position.
                    w:SetPos({ 600, 400 }, "Always", { 0.5, 0.5 })
                    t.frames(function ()
                        C.vecNear(w.LastPosition, { 360, 240 }, "LastPosition with a 0.5 pivot", 1)
                        w:SetCollapsed(true, "Always")
                        t.frames(function ()
                            local collapsed = w.LastSize
                            C.check(collapsed[2] < 320, "a collapsed window's LastSize is %s", V(collapsed))
                            w:SetCollapsed(false, "Always")
                            t.frames(function ()
                                C.vecNear(w.LastSize, { 480, 320 }, "LastSize expanded again", 1)
                                t.finish(true, "SetPos, SetSize, SetSizeConstraints, pivot and SetCollapsed seen in LastPosition and LastSize; collapsed " .. V(collapsed))
                            end)
                        end)
                    end)
                end)
            end)
        end),

        case("Window#members", "W1-15", function (t, E)
            local w = t.window("Window")
            w:AddText("bg3mse Window: this window hides and comes back by itself")
            C.equal(w.Open, true, "Open at first")
            Flag(w, "Closeable", true)
            C.equal(tostring(w.Scaling), "Absolute", "Scaling at first")
            w.Scaling = "Scaled"
            C.equal(tostring(w.Scaling), "Scaled", "Scaling")
            w.Scaling = "Absolute"
            Delegate(w, "OnClose")
            C.check(rawequal(w.MainMenu, nil), "MainMenu before AddMainMenu")
            C.equal(w.MenuBar, false, "the MenuBar flag before AddMainMenu")
            local bar = w:AddMainMenu()
            C.equal(Kind(bar), "MenuBar", "what AddMainMenu made")
            C.check(w.MainMenu == bar, "MainMenu is not the menu bar")
            C.equal(w.MenuBar, true, "the MenuBar flag after AddMainMenu")
            C.check(w:AddMainMenu() == bar, "a second AddMainMenu made another menu bar")
            C.equal(#w.Children, 2, "the window's Children (the text and the menu bar)")
            Flag(w, "NoCollapse", true)
            t.frames(function ()
                local at = w.LastPosition
                -- A window with Open = false is not drawn (Window::BeginRender):
                -- the position asked for waits for its next drawing.
                w.Open = false
                C.equal(w.Open, false, "Open")
                w:SetPos({ at[1] + 50, at[2] + 30 }, "Always")
                t.frames(function ()
                    C.vecNear(w.LastPosition, at, "a closed window's LastPosition", 0.5)
                    w.Open = true
                    t.frames(function ()
                        C.vecNear(w.LastPosition, { at[1] + 50, at[2] + 30 }, "LastPosition once open again", 1)
                        t.finish(true, "Open = false stopped its drawing and true brought it back; Closeable, Scaling, OnClose, AddMainMenu, MainMenu")
                    end)
                end)
            end)
        end),

        case("Menu#AddItem", "W1-26", function (t, E)
            local w = t.window("Menu")
            local bar = w:AddMainMenu()
            local menu = bar:AddMenu("bg3mse menu")
            C.equal(Kind(menu), "Menu", "what AddMenu made in the menu bar")
            C.equal(menu.Parent, bar.Handle, "the menu's Parent")
            local item = menu:AddItem("bg3mse item", "Ctrl+B")
            C.equal(Kind(item), "MenuItem", "what AddItem made")
            C.equal(item.Label, "bg3mse item", "the item's Label")
            C.equal(item.Shortcut, "Ctrl+B", "Shortcut as added")
            C.equal(item.Enabled, true, "Enabled at first")
            item.Enabled = false
            C.equal(item.Enabled, false, "Enabled")
            item.Enabled = true
            item.Shortcut = "Ctrl+N"
            C.equal(item.Shortcut, "Ctrl+N", "Shortcut")
            local plain = menu:AddItem("bg3mse plain item")
            C.check(rawequal(plain.Shortcut, nil), "Shortcut when none is given")
            plain.Shortcut = "Ctrl+P"
            plain.Shortcut = nil
            C.check(rawequal(plain.Shortcut, nil), "Shortcut once cleared")
            Delegate(item, "OnClick")
            local body = w:AddMenu("bg3mse body menu")
            body:AddItem("bg3mse body item")
            t.frames(function ()
                C.check(Seen(menu), "the menu in the menu bar was not drawn")
                C.check(Seen(body), "the menu in the window was not drawn")
                -- A closed menu draws none of its items (Menu::BeginRender).
                C.check(not Seen(item), "a closed menu's item was drawn")
                t.finish(true, "a menu bar's menu and a window's menu drawn; their items wait for the menu to open [rows: W1-27, LC-14]")
            end)
        end),

        -- Activate on a menu: Menu::BeginRender draws BeginMenu(Label) under
        -- PushID(Label), and BeginMenuEx's Selectable("") gets GetID(""), the top
        -- of that ID stack (ImHashStr with no text returns the seed), which is the
        -- id Activate asks for, so a code activation opens the menu. The item then
        -- is a MenuItem whose id is GetID(Label) in the menu's window. The menu
        -- is in the menu bar: in a window's body (the vertical branch of
        -- BeginMenuEx) an open menu closes again whenever the mouse is over
        -- that window but off the menu, which a case cannot rule out.
        case("MenuItem#OnClick", "W1-27", function (t, E)
            local w = t.window("MenuItem")
            local menu = w:AddMainMenu():AddMenu("bg3mse mi menu")
            local item = menu:AddItem("bg3mse mi item")
            local clicks, arg = 0, nil
            item.OnClick = function (h) clicks = clicks + 1; arg = h end
            t.frames(function ()
                C.check(Seen(menu), "the menu was not drawn")
                C.check(not Seen(item), "a closed menu's item was drawn")
                menu:Activate()
                t.until_(function () return Seen(item) end, "the item once the menu opened", function ()
                    item:Activate()
                    t.until_(function () return clicks > 0 end, "OnClick after the item's Activate", function ()
                        C.check(arg == item, "OnClick's argument is not the item")
                        C.equal(clicks, 1, "OnClick calls")
                        t.finish(true, "Activate opened the menu and pressed its item: OnClick")
                    end)
                end, function ()
                    t.finish("skip", "the menu did not open on Activate; its item's OnClick is left to the ImGuiClicks window")
                end)
            end)
        end, { exclusive = true }),

        case("CollapsingHeader#Flags", "W1-16", function (t, E)
            local w = t.window("CollapsingHeader")
            local open = w:AddCollapsingHeader("bg3mse header open")
            C.equal(Kind(open), "CollapsingHeader", "the class")
            Flag(open, "DefaultOpen", true)
            C.equal(open.Open, false, "Open before the header was drawn")
            C.raises(function () open.Open = true end)
            local inOpen = open:AddText("bg3mse in the open header")
            local closed = w:AddCollapsingHeader("bg3mse header closed")
            Flag(closed, "Bullet")
            local inClosed = closed:AddText("bg3mse in the closed header")
            Delegate(closed, "OnExpand")
            Delegate(closed, "OnCollapse")
            local expanded, collapsed, arg = 0, 0, nil
            closed.OnExpand = function (h) expanded = expanded + 1; arg = h end
            closed.OnCollapse = function () collapsed = collapsed + 1 end
            t.frames(function ()
                C.check(Seen(inOpen), "the DefaultOpen header's text was not drawn")
                C.check(not Seen(inClosed), "the closed header's text was drawn")
                C.equal(open.Open, true, "Open of the DefaultOpen header")
                C.equal(closed.Open, false, "Open of the closed header")
                -- An activated tree node toggles (imgui_widgets.cpp, TreeNodeBehavior).
                closed:Activate()
                t.until_(function () return expanded > 0 end, "OnExpand after Activate", function ()
                    C.check(arg == closed, "OnExpand's argument is not the header")
                    C.check(Seen(inClosed), "the opened header's text was not drawn")
                    C.equal(closed.Open, true, "Open after Activate")
                    closed:Activate()
                    t.until_(function () return collapsed > 0 end, "OnCollapse after a second Activate", function ()
                        C.equal(closed.Open, false, "Open after the second Activate")
                        -- GuiCond Once by default (CollapsingHeader::SetOpen),
                        -- which a header that was toggled does not take.
                        closed:SetOpen(true)
                        inClosed.StatusFlags = 0
                        t.frames(function ()
                            C.check(not Seen(inClosed), "SetOpen(true) with Once opened a header that had been toggled")
                            closed:SetOpen(true, "Always")
                            t.frames(function ()
                                C.check(Seen(inClosed), "the header's text was not drawn after SetOpen(true, Always)")
                                C.equal(closed.Open, true, "Open after SetOpen")
                                -- SetNextItemOpen is no toggle: IsItemToggledOpen stays false.
                                C.equal(expanded, 1, "OnExpand calls after SetOpen")
                                C.equal(collapsed, 1, "OnCollapse calls after SetOpen")
                                t.finish(true, "DefaultOpen drew its text; Activate toggled the other with OnExpand and OnCollapse; SetOpen opened it without either")
                            end)
                        end)
                    end)
                end)
            end)
        end, { exclusive = true }),

        case("TabBar#AddTabItem", "W1-17", function (t, E)
            local w = t.window("TabBar")
            local bar = w:AddTabBar("bg3mse tabs")
            C.equal(Kind(bar), "TabBar", "the class")
            Flag(bar, "Reorderable", true)
            local one = bar:AddTabItem("bg3mse tab one")
            local two = bar:AddTabItem("bg3mse tab two")
            C.equal(Kind(one), "TabItem", "what AddTabItem made")
            C.equal(one.Parent, bar.Handle, "the tab's Parent")
            Flag(two, "NoTooltip", true)
            local inOne = one:AddText("bg3mse in tab one")
            local inTwo = two:AddText("bg3mse in tab two")
            t.frames(function ()
                C.check(Seen(inOne), "the first tab's text was not drawn")
                C.check(not Seen(inTwo), "the second tab's text was drawn while the first is selected")
                -- ImGuiTabItemFlags_SetSelected: the bar selects that tab, in
                -- the frame after the one that submits the flag (TabItemEx
                -- queues it, the next TabBarLayout takes it: imgui_widgets.cpp),
                -- so the first tab is drawn once more. An element that is not
                -- drawn keeps the status flags of its last frame
                -- (StyledRenderable::Render), so the first tab's text is
                -- cleared once the second is seen, and must then stay clear.
                two.SetSelected = true
                C.equal(two.SetSelected, true, "SetSelected")
                t.until_(function () return Seen(inTwo) end, "the second tab's text once selected", function ()
                    inOne.StatusFlags = 0
                    t.frames(function ()
                        C.check(not Seen(inOne), "the first tab's text is still drawn")
                        two.SetSelected = false
                        t.finish(true, "the first tab drawn, then the second once SetSelected [rows: W1-18]")
                    end)
                end)
            end)
        end),

        case("Tree#SetOpen", "W1-19", function (t, E)
            local w = t.window("Tree")
            local tree = w:AddTree("bg3mse tree")
            C.equal(Kind(tree), "Tree", "the class")
            Flag(tree, "SpanAvailWidth", true)
            local inside = tree:AddText("bg3mse in the tree")
            local toggles = 0
            Delegate(tree, "OnExpand")
            Delegate(tree, "OnCollapse")
            tree.OnExpand = function () toggles = toggles + 1 end
            tree.OnCollapse = function () toggles = toggles + 1 end
            t.frames(function ()
                C.check(not Seen(inside), "a closed tree's text was drawn")
                -- GuiCond Once by default (Tree::SetOpen).
                tree:SetOpen(true)
                t.frames(function ()
                    C.check(Seen(inside), "the tree's text was not drawn after SetOpen(true)")
                    tree:SetOpen(false, "Always")
                    inside.StatusFlags = 0
                    t.frames(function ()
                        C.check(not Seen(inside), "the tree's text still drawn after SetOpen(false)")
                        -- SetNextItemOpen is no toggle: IsItemToggledOpen stays false.
                        C.equal(toggles, 0, "OnExpand and OnCollapse calls after SetOpen")
                        t.finish(true, "SetOpen opened and closed it without OnExpand or OnCollapse")
                    end)
                end)
            end)
        end),

        case("Tree#OnExpand", "W1-19", function (t, E)
            local w = t.window("Tree activated")
            local tree = w:AddTree("bg3mse tree activated")
            local inside = tree:AddText("bg3mse in the activated tree")
            local expanded, collapsed, arg = 0, 0, nil
            tree.OnExpand = function (h) expanded = expanded + 1; arg = h end
            tree.OnCollapse = function () collapsed = collapsed + 1 end
            t.frames(function ()
                tree:Activate()
                t.until_(function () return expanded > 0 end, "OnExpand after Activate", function ()
                    C.check(arg == tree, "OnExpand's argument is not the tree")
                    C.check(Seen(inside), "the opened tree's text was not drawn")
                    tree:Activate()
                    t.until_(function () return collapsed > 0 end, "OnCollapse after a second Activate", function ()
                        C.equal(expanded, 1, "OnExpand calls")
                        t.finish(true, "Activate toggled it: OnExpand, then OnCollapse")
                    end)
                end)
            end)
        end, { exclusive = true }),

        case("Table#members", "W1-22", function (t, E)
            local w = t.window("Table")
            local tbl = w:AddTable("bg3mse table", 3)
            C.equal(Kind(tbl), "Table", "the class")
            C.equal(tbl.Columns, 3, "Columns as added")
            tbl.Columns = 2
            C.equal(tbl.Columns, 2, "Columns")
            Flag(tbl, "Borders", true)
            Flag(tbl, "RowBg", true)
            Flag(tbl, "NoSavedSettings", true)
            C.equal(tbl.FreezeRows, 0, "FreezeRows at first")
            tbl.FreezeRows = 1
            C.equal(tbl.FreezeRows, 1, "FreezeRows")
            tbl.FreezeCols = 1
            C.equal(tbl.FreezeCols, 1, "FreezeCols")
            tbl.FreezeCols = 0
            -- The width of the table's content when it scrolls sideways (BeginTable's inner_width).
            C.near(tbl.InnerWidth, 0.0, "InnerWidth at first")
            tbl.InnerWidth = 480
            C.near(tbl.InnerWidth, 480.0, "InnerWidth")
            tbl.InnerWidth = 0
            C.equal(tbl.ShowHeader, false, "ShowHeader at first")
            tbl.ShowHeader = true
            C.equal(tbl.ShowHeader, true, "ShowHeader")
            C.equal(tbl.AngledHeader, false, "AngledHeader at first")
            tbl.AngledHeader = true
            C.equal(tbl.AngledHeader, true, "AngledHeader")
            tbl.AngledHeader = false
            C.equal(tbl.OptimizedDraw, false, "OptimizedDraw at first")
            tbl.OptimizedDraw = true
            C.equal(tbl.OptimizedDraw, true, "OptimizedDraw")
            C.check(rawequal(tbl.Size, nil), "Size at first")
            tbl.Size = { 360, 140 }
            C.vecNear(tbl.Size, { 360, 140 }, "Size")
            Delegate(tbl, "OnSortChanged")
            C.equal(#tbl.ColumnDefs, 0, "ColumnDefs at first")
            tbl:AddColumn("bg3mse name", { "WidthFixed" }, 140)
            tbl:AddColumn("bg3mse value")
            C.equal(#tbl.ColumnDefs, 2, "ColumnDefs after AddColumn")
            C.equal(tbl.ColumnDefs[1].Name, "bg3mse name", "ColumnDefs[1].Name")
            C.equal(tbl.ColumnDefs[1].WidthFixed, true, "ColumnDefs[1].WidthFixed")
            C.near(tbl.ColumnDefs[1].Width, 140, "ColumnDefs[1].Width")
            C.near(tbl.ColumnDefs[2].Width, 0, "ColumnDefs[2].Width when none is given")
            -- A width with neither WidthFixed nor WidthStretch trips IM_ASSERT in
            -- TableSetupColumn under the table's default sizing (imgui_tables.cpp:1622),
            -- so the column is made fixed before it is given one.
            Flag(tbl.ColumnDefs[2], "WidthFixed", true)
            tbl.ColumnDefs[2].Width = 90
            C.near(tbl.ColumnDefs[2].Width, 90, "ColumnDefs[2].Width")
            tbl.ColumnDefs[2].Name = "bg3mse val"
            C.equal(tbl.ColumnDefs[2].Name, "bg3mse val", "ColumnDefs[2].Name")
            Flag(tbl.ColumnDefs[2], "NoResize", true)
            local texts = {}
            for r = 1, 2 do
                local row = tbl:AddRow()
                C.equal(Kind(row), "TableRow", "what AddRow made")
                C.near(row.MinHeight, 0, "MinHeight at first")
                row.MinHeight = 22
                C.near(row.MinHeight, 22, "MinHeight")
                Flag(row, "Headers")
                for c = 1, 2 do
                    local cell = row:AddCell()
                    C.equal(Kind(cell), "TableCell", "what AddCell made")
                    texts[#texts + 1] = cell:AddText(string.format("bg3mse cell %d.%d", r, c))
                end
            end
            t.frames(function ()
                for i, text in ipairs(texts) do C.check(Seen(text), "cell text %d was not drawn", i) end
                C.equal(#tbl.Sorting, 0, "Sorting of a table that does not sort")
                t.finish(true, "2 columns, 2 rows of 2 cells drawn; every member written and read back [rows: W1-20, W1-23, LC-14]")
            end)
        end),

        case("Table#OnSortChanged", "W1-22", function (t, E)
            local w = t.window("Table sorted")
            local tbl = w:AddTable("bg3mse sorted table", 2)
            tbl.Sortable = true
            tbl.NoSavedSettings = true
            tbl.ShowHeader = true
            tbl:AddColumn("bg3mse key", { "DefaultSort" })
            tbl:AddColumn("bg3mse other")
            local row = tbl:AddRow()
            row:AddCell():AddText("bg3mse b")
            row:AddCell():AddText("bg3mse 2")
            local calls, arg = 0, nil
            tbl.OnSortChanged = function (h) calls = calls + 1; arg = h end
            -- The sort specs are dirty on the table's first frame (Table::UpdateSorting).
            t.until_(function () return calls > 0 end, "OnSortChanged on the first frame", function ()
                t.frames(function ()
                    C.equal(calls, 1, "OnSortChanged calls")
                    C.check(arg == tbl, "OnSortChanged's argument is not the table")
                    C.equal(#tbl.Sorting, 1, "Sorting")
                    C.equal(tbl.Sorting[1].ColumnIndex, 0, "Sorting[1].ColumnIndex")
                    C.equal(tostring(tbl.Sorting[1].Direction), "Ascending", "Sorting[1].Direction")
                    tbl.Sorting[1].ColumnIndex = 1
                    C.equal(tbl.Sorting[1].ColumnIndex, 1, "Sorting[1].ColumnIndex written")
                    tbl.Sorting[1].Direction = "Descending"
                    C.equal(tostring(tbl.Sorting[1].Direction), "Descending", "Sorting[1].Direction written")
                    t.finish(true, "OnSortChanged once; Sorting {0, Ascending} [rows: W1-21]")
                end)
            end)
        end),

        case("ChildWindow#members", "W1-25", function (t, E)
            local w = t.window("ChildWindow")
            local child = w:AddChildWindow("bg3mse child window")
            C.equal(Kind(child), "ChildWindow", "the class")
            C.check(rawequal(child.Size, nil), "Size at first")
            child.Size = { 220, 90 }
            C.vecNear(child.Size, { 220, 90 }, "Size")
            Flag(child, "Border", true)
            Flag(child, "NoScrollbar", true)
            local inside = child:AddText("bg3mse in the child window")
            child:SetScroll({ 0, 0 })
            child:SetContentSize({ 220, 300 })
            t.frames(function ()
                C.check(Seen(inside), "the child window's text was not drawn")
                C.vecNear(child.LastSize, { 220, 90 }, "the child window's LastSize", 1)
                local pos, outer = child.LastPosition, w.LastPosition
                C.check(pos[1] >= outer[1] and pos[2] >= outer[2], "the child window at %s, its window at %s", V(pos), V(outer))
                t.finish(true, "drawn " .. V(child.LastSize) .. " inside its window")
            end)
        end),

        case("Popup#Open", "W1-24", function (t, E)
            local w = t.window("Popup")
            local popup = w:AddPopup("bg3mse popup")
            C.equal(Kind(popup), "Popup", "the class")
            Flag(popup, "AlwaysAutoResize", true)
            local inside = popup:AddText("bg3mse in the popup")
            popup:SetBgAlpha(0.95)
            t.frames(function ()
                C.check(not Seen(inside), "a popup that was not opened was drawn")
                popup:Open()
                t.frames(function ()
                    C.check(Seen(inside), "the popup's text was not drawn after Open")
                    C.check(popup.LastSize[1] > 0, "the open popup's LastSize is %s", V(popup.LastSize))
                    -- Open again with flags (GuiPopupFlags): already open, it stays.
                    popup:Open({ "NoOpenOverExistingPopup" })
                    t.frames(function ()
                        local size = popup.LastSize
                        -- Another window taking the focus closes it (ImGui's
                        -- FocusWindow, ClosePopupsOverWindow).
                        local other = t.window("Popup closer", { 320, 120 })
                        other:AddText("bg3mse this window takes the focus")
                        other:SetFocus()
                        -- The popup's window is drawn before the other one in
                        -- the frame that takes the focus: its text is cleared after.
                        t.frames(function ()
                            inside.StatusFlags = 0
                            t.frames(function ()
                                C.check(not Seen(inside), "the popup is still drawn after another window took the focus")
                                t.finish(true, "drawn " .. V(size) .. " after Open; closed when another window took the focus")
                            end)
                        end)
                    end)
                end)
            end)
        end, { exclusive = true }),

        case("Tooltip#members", "LC-14", function (t, E)
            local w = t.window("Tooltip")
            local owner = w:AddButton("bg3mse tooltip owner")
            local tip = owner:Tooltip()
            C.equal(Kind(tip), "Tooltip", "what Tooltip() made")
            C.check(owner:Tooltip() == tip, "a second Tooltip() made another tooltip")
            C.equal(tip.Parent, INVALID, "the tooltip's Parent")
            tip:AddText("bg3mse tooltip text")
            C.equal(#tip.Children, 1, "the tooltip's Children")
            Flag(tip, "AlwaysAutoResize", true)
            -- Its window requests wait until it is drawn, on hover
            -- (Tooltip::BeginRender); the opt-in case hovers one.
            tip:SetBgAlpha(0.9)
            tip:SetSize({ 200, 60 })
            t.frames(function ()
                -- A tooltip has no parent and is not destroyed with its owner
                -- (StyledRenderable::Tooltip, IMGUI.cpp:422-428): the case destroys it.
                tip:Destroy()
                C.check(Dead(tip), "the tooltip is still there after Destroy")
                -- The owner keeps the handle: Tooltip() gives the destroyed one back.
                C.check(Dead(owner:Tooltip()), "Tooltip() after the tooltip's Destroy made a new one")
                t.finish(true, "one tooltip per owner; destroyed by hand, the owner hands back the dead one")
            end)
        end),

        case("Text#kinds", "LC-14", function (t, E)
            local w = t.window("Text", { 520, 560 })
            local text = w:AddText("bg3mse text")
            local link = w:AddTextLink("bg3mse text link")
            local bullet = w:AddBulletText("bg3mse bullet text")
            local sepText = w:AddSeparatorText("bg3mse separator text")
            local sepEmpty = w:AddSeparatorText("")
            local spacing = w:AddSpacing()
            local dummy = w:AddDummy(40, 10)
            dummy.Width = 60
            C.near(dummy.Width, 60, "Dummy.Width")
            dummy.Height = 12
            C.near(dummy.Height, 12, "Dummy.Height")
            local newline = w:AddNewLine()
            local separator = w:AddSeparator()
            C.equal(Kind(spacing), "Spacing", "AddSpacing")
            C.equal(Kind(newline), "NewLine", "AddNewLine")
            text.TextWrapPos = 200
            t.frames(function ()
                for _, el in ipairs({ text, link, bullet, sepText, sepEmpty, dummy, separator }) do
                    C.check(Seen(el), "the %s was not drawn", Kind(el))
                end
                t.finish(true, "Text, TextLink, BulletText, SeparatorText (and empty), Spacing, Dummy, NewLine, Separator drawn [rows: W1-29]")
            end)
        end),

        case("TextLink#OnClick", "LC-14", function (t, E)
            local w = t.window("TextLink")
            local link = w:AddTextLink("bg3mse link to activate")
            local clicks = 0
            link.OnClick = function () clicks = clicks + 1 end
            t.frames(function ()
                link:Activate()
                t.until_(function () return clicks > 0 end, "OnClick after Activate", function ()
                    t.finish(true, "Activate pressed the link: OnClick")
                end)
            end)
        end, { exclusive = true }),

        case("Button#OnClick", "W1-31", function (t, E)
            local w = t.window("Button")
            local button = w:AddButton("bg3mse button to activate")
            C.equal(Kind(button), "Button", "the class")
            C.check(rawequal(button.Size, nil), "Size at first")
            button.Size = { 220, 34 }
            C.vecNear(button.Size, { 220, 34 }, "Size")
            Flag(button, "MouseButtonRight")
            local clicks, activated, deactivated, arg = 0, 0, 0, nil
            button.OnClick = function (h) clicks = clicks + 1; arg = h end
            button.OnActivate = function () activated = activated + 1 end
            button.OnDeactivate = function () deactivated = deactivated + 1 end
            t.frames(function ()
                button:Activate()
                C.equal(button.RequestActivate, true, "RequestActivate after Activate")
                t.until_(function () return clicks > 0 end, "OnClick after Activate", function ()
                    C.check(arg == button, "OnClick's argument is not the button")
                    -- ButtonBehavior holds the active id for the frame it is
                    -- pressed in and lets go the next.
                    t.until_(function () return deactivated > 0 end, "the button's OnDeactivate", function ()
                        C.equal(clicks, 1, "OnClick calls")
                        C.equal(activated, 1, "OnActivate calls")
                        t.finish(true, "Activate pressed it: OnClick, OnActivate, OnDeactivate [rows: W1-12]")
                    end)
                end)
            end)
        end, { exclusive = true }),

        case("Selectable#OnClick", "W1-30", function (t, E)
            local w = t.window("Selectable")
            local sel = w:AddSelectable("bg3mse selectable to activate")
            C.equal(sel.Selected, false, "Selected at first")
            sel.Selected = true
            C.equal(sel.Selected, true, "Selected")
            sel.Selected = false
            sel.Size = { 240, 24 }
            C.vecNear(sel.Size, { 240, 24 }, "Size")
            Flag(sel, "SpanAllColumns")
            Delegate(sel, "OnDoubleClick")
            local clicks, doubles = 0, 0
            sel.OnClick = function () clicks = clicks + 1 end
            sel.OnDoubleClick = function () doubles = doubles + 1 end
            t.frames(function ()
                sel:Activate()
                t.until_(function () return clicks > 0 end, "OnClick after Activate", function ()
                    -- ImGui::Selectable toggles Selected on a press, and
                    -- Selectable::StyledRender toggles it again unless the
                    -- press is the second click of a double click: after a
                    -- single press it is what it was.
                    C.equal(sel.Selected, false, "Selected after one press")
                    C.equal(doubles, 0, "OnDoubleClick calls after one press")
                    t.finish(true, "Activate pressed it: OnClick, no OnDoubleClick, Selected as before")
                end)
            end)
        end, { exclusive = true }),

        case("Checkbox#OnChange", "W1-33", function (t, E)
            local w = t.window("Checkbox")
            local box = w:AddCheckbox("bg3mse checkbox to activate")
            C.equal(box.Checked, false, "Checked at first")
            box.Checked = true
            C.equal(box.Checked, true, "Checked")
            box.Checked = false
            Delegate(box, "OnChange")
            local changes, value = 0, nil
            box.OnChange = function (_, v) changes = changes + 1; value = v end
            t.frames(function ()
                box:Activate()
                t.until_(function () return changes > 0 end, "OnChange after Activate", function ()
                    C.equal(value, true, "OnChange's value")
                    C.equal(box.Checked, true, "Checked after the press")
                    t.finish(true, "Activate toggled it: OnChange(true)")
                end)
            end)
        end, { exclusive = true }),

        case("RadioButton#OnChange", "W1-34", function (t, E)
            local w = t.window("RadioButton")
            local radio = w:AddRadioButton("bg3mse radio to activate")
            C.equal(radio.Active, false, "Active at first")
            radio.Active = true
            C.equal(radio.Active, true, "Active")
            radio.Active = false
            Delegate(radio, "OnChange")
            local changes, value = 0, nil
            radio.OnChange = function (_, v) changes = changes + 1; value = v end
            t.frames(function ()
                radio:Activate()
                t.until_(function () return changes > 0 end, "OnChange after Activate", function ()
                    -- RadioButton::StyledRender passes Active and does not change it.
                    C.equal(value, false, "OnChange's value")
                    C.equal(radio.Active, false, "Active after the press")
                    t.finish(true, "Activate pressed it: OnChange(false), Active unchanged as Norbyte's code leaves it")
                end)
            end)
        end, { exclusive = true }),

        case("InputText#OnActivate", "W1-35", function (t, E)
            local w = t.window("InputText")
            local input = w:AddInputText("bg3mse input to activate", "bg3mse text")
            C.equal(input.Text, "bg3mse text", "Text as added")
            input.Text = "bg3mse edited"
            C.equal(input.Text, "bg3mse edited", "Text")
            C.check(rawequal(input.Hint, nil), "Hint at first")
            input.Hint = "bg3mse hint"
            C.equal(input.Hint, "bg3mse hint", "Hint")
            C.check(rawequal(input.SizeHint, nil), "SizeHint at first")
            input.SizeHint = { 260, 0 }
            C.vecNear(input.SizeHint, { 260, 0 }, "SizeHint")
            Flag(input, "CharsUppercase")
            Flag(input, "AutoSelectAll", true)
            Delegate(input, "OnChange")
            local after = w:AddButton("bg3mse after the input")
            local activated, deactivated = 0, 0
            input.OnActivate = function () activated = activated + 1 end
            input.OnDeactivate = function () deactivated = deactivated + 1 end
            t.frames(function ()
                input:Activate()
                t.until_(function () return activated > 0 end, "the input's OnActivate", function ()
                    -- Activating another item ends the editing.
                    after:Activate()
                    t.until_(function () return deactivated > 0 end, "the input's OnDeactivate", function ()
                        C.equal(input.Text, "bg3mse edited", "Text after the editing")
                        t.finish(true, "Activate started editing (OnActivate); activating a button ended it (OnDeactivate) [rows: W1-12]")
                    end)
                end, function ()
                    -- ImGui starts editing by code only when its last
                    -- navigation input was the keyboard (imgui_widgets.cpp
                    -- InputTextEx, input_requested_by_nav), not a controller.
                    after:Activate()
                    t.frames(function ()
                        t.finish("skip", "no OnActivate: ImGui edits an activated input only after keyboard navigation (InputTextEx); a controller may have been the last input")
                    end)
                end)
            end)
        end, { exclusive = true }),

        case("Combo#members", "W1-36", function (t, E)
            local w = t.window("Combo")
            local combo = w:AddCombo("bg3mse combo")
            C.equal(combo.SelectedIndex, -1, "SelectedIndex at first")
            C.equal(#combo.Options, 0, "Options at first")
            combo.Options = { "bg3mse one", "bg3mse two", "bg3mse three" }
            C.equal(#combo.Options, 3, "Options")
            C.equal(combo.Options[2], "bg3mse two", "Options[2]")
            -- 0-based, as ImGui counts (Combo::StyledRender).
            combo.SelectedIndex = 1
            C.equal(combo.SelectedIndex, 1, "SelectedIndex")
            Flag(combo, "HeightSmall", true)
            Delegate(combo, "OnChange")
            t.frames(function ()
                C.check(Seen(combo), "the combo was not drawn")
                C.equal(combo.SelectedIndex, 1, "SelectedIndex after the frames")
                t.finish(true, "3 options, the second selected, drawn")
            end)
        end),

        case("DragScalar#members", "W1-37", function (t, E)
            local w = t.window("DragScalar")
            ValueWidget(t, w, "DragScalar", w:AddDrag("bg3mse drag", 0.25, 0, 1), {
                initial = { 0.25, 0.25, 0.25, 0.25 }, min = { 0, 0, 0, 0 }, max = { 1, 1, 1, 1 },
                newMin = { -1, -1, -1, -1 }, newMax = { 2, 2, 2, 2 },
                components = 3, value = { 0.1, 0.2, 0.3, 0.4 }, flag = "AlwaysClamp", liveEdit = true, speed = 0.05 })
        end),

        case("DragInt#members", "W1-38", function (t, E)
            local w = t.window("DragInt")
            ValueWidget(t, w, "DragInt", w:AddDragInt("bg3mse drag int", 3, 0, 10), {
                initial = { 3, 3, 3, 3 }, min = { 0, 0, 0, 0 }, max = { 10, 10, 10, 10 },
                newMin = { -5, -5, -5, -5 }, newMax = { 20, 20, 20, 20 },
                components = 2, value = { 4, 5, 6, 7 }, flag = "NoInput", speed = 0.5 })
        end),

        case("SliderScalar#members", "W1-39", function (t, E)
            local w = t.window("SliderScalar")
            ValueWidget(t, w, "SliderScalar", w:AddSlider("bg3mse slider", 0.5, 0, 2), {
                initial = { 0.5, 0.5, 0.5, 0.5 }, min = { 0, 0, 0, 0 }, max = { 2, 2, 2, 2 },
                newMin = { -1, -1, -1, -1 }, newMax = { 3, 3, 3, 3 },
                components = 2, value = { 1.5, 0.5, 0, 0 }, flag = "AlwaysClamp", vertical = true, liveEdit = true })
        end),

        case("SliderInt#members", "W1-40", function (t, E)
            local w = t.window("SliderInt")
            ValueWidget(t, w, "SliderInt", w:AddSliderInt("bg3mse slider int", 4, 0, 8), {
                initial = { 4, 4, 4, 4 }, min = { 0, 0, 0, 0 }, max = { 8, 8, 8, 8 },
                newMin = { 0, 0, 0, 0 }, newMax = { 16, 16, 16, 16 },
                components = 2, value = { 6, 2, 0, 0 }, flag = "NoInput", vertical = true })
        end),

        case("InputScalar#members", "W1-41", function (t, E)
            local w = t.window("InputScalar")
            ValueWidget(t, w, "InputScalar", w:AddInputScalar("bg3mse input scalar", 1.5), {
                initial = { 1.5, 1.5, 1.5, 1.5 }, components = 2, value = { 2.5, -1.25, 0, 0 }, flag = "AutoSelectAll",
                liveEdit = true, step = 0.25 })
        end),

        case("InputInt#members", "W1-42", function (t, E)
            local w = t.window("InputInt")
            ValueWidget(t, w, "InputInt", w:AddInputInt("bg3mse input int", 7), {
                initial = { 7, 7, 7, 7 }, components = 3, value = { 9, -3, 12, 0 }, flag = "NoUndoRedo", step = 2 })
        end),

        case("ColorEdit#members", "W1-43", function (t, E)
            local w = t.window("ColorEdit")
            local edit = w:AddColorEdit("bg3mse colour", { 0.2, 0.4, 0.6 })
            C.vecNear(edit.Color, { 0.2, 0.4, 0.6, 1.0 }, "Color as added")
            -- Objects.h starts it with ImGuiColorEditFlags_DefaultOptions_.
            C.equal(edit.Uint8, true, "the Uint8 flag of the default options")
            C.equal(edit.DisplayRGB, true, "the DisplayRGB flag of the default options")
            edit.Color = { 0.9, 0.1, 0.1, 0.5 }
            C.vecNear(edit.Color, { 0.9, 0.1, 0.1, 0.5 }, "Color")
            Flag(edit, "NoAlpha", true)
            Delegate(edit, "OnChange")
            t.frames(function ()
                C.check(Seen(edit), "the colour edit was not drawn")
                t.finish(true, "drawn with the default options and NoAlpha")
            end)
        end),

        case("ColorPicker#members", "W1-44", function (t, E)
            local w = t.window("ColorPicker", { 520, 480 })
            local picker = w:AddColorPicker("bg3mse colour picker", { 0.6, 0.4, 0.2 })
            C.vecNear(picker.Color, { 0.6, 0.4, 0.2, 1.0 }, "Color as added")
            C.equal(picker.PickerHueBar, true, "the PickerHueBar flag of the default options")
            picker.Color = { 0.1, 0.8, 0.3, 1.0 }
            C.vecNear(picker.Color, { 0.1, 0.8, 0.3, 1.0 }, "Color")
            Flag(picker, "NoSidePreview", true)
            Delegate(picker, "OnChange")
            t.frames(function ()
                C.check(Seen(picker), "the colour picker was not drawn")
                t.finish(true, "drawn with the default options and NoSidePreview")
            end)
        end),

        case("ProgressBar#members", "W1-45", function (t, E)
            local w = t.window("ProgressBar")
            local bar = w:AddProgressBar()
            C.equal(Kind(bar), "ProgressBar", "the class")
            C.near(bar.Value, 0, "Value at first")
            bar.Value = 0.4
            C.near(bar.Value, 0.4, "Value")
            C.vecNear(bar.Size, { 0, 0 }, "Size at first")
            bar.Size = { 240, 18 }
            C.vecNear(bar.Size, { 240, 18 }, "Size")
            C.equal(bar.Overlay, "", "Overlay at first")
            bar.Overlay = "bg3mse 40%"
            C.equal(bar.Overlay, "bg3mse 40%", "Overlay")
            t.frames(function ()
                C.check(Seen(bar), "the progress bar was not drawn")
                t.finish(true, "drawn at 0.4 with its overlay text")
            end)
        end),

        case("SetFontScaleMultiplier#rebuild", "OVL-23", function (t, E)
            -- IMGUIManager::UpdateStyle gives ImGui the scale of its fonts
            -- (FontScaleMain); the glyphs of the new size are added to the
            -- atlas as they are drawn. The multiplier goes back to 1.0 on
            -- every path.
            t.cleanup(function () E.IMGUI.SetFontScaleMultiplier(1.0) end)
            local w = t.window("font scale")
            w.AlwaysAutoResize = true
            w:AddText("bg3mse this window's text grows a quarter, then shrinks back")
            local proggy = w:AddText("bg3mse and this line is in ProggyClean")
            if E.IMGUI.LoadFont(FONT_KEY, FONT, 13.0) then proggy.Font = FONT_KEY end
            t.frames(function ()
                local before = Copy(w.LastSize)
                E.IMGUI.SetFontScaleMultiplier(1.25)
                t.frames(function ()
                    local during = Copy(w.LastSize)
                    E.IMGUI.SetFontScaleMultiplier(1.0)
                    t.frames(function ()
                        local after = Copy(w.LastSize)
                        C.vecNear(after, before, "LastSize back at 1.0", 2)
                        -- Still drawn after both rebuilds: a new position shows.
                        local at = w.LastPosition
                        w:SetPos({ at[1] + 30, at[2] + 20 }, "Always")
                        t.frames(function ()
                            C.vecNear(w.LastPosition, { at[1] + 30, at[2] + 20 }, "LastPosition after the rebuilds", 1)
                            t.finish(true, string.format("LastSize %s, %s at 1.25, %s back at 1.0 [rows: LC-13]",
                                V(before), V(during), V(after)))
                        end)
                    end)
                end)
            end)
        end, { exclusive = true }),

        case("SetUIScaleMultiplier#rebuild", "LC-13", function (t, E)
            -- IMGUIManager::UpdateStyle scales the style's sizes (ScaleAllSizes)
            -- by the multiplier: a window that fits its content grows.
            t.cleanup(function () E.IMGUI.SetUIScaleMultiplier(1.0) end)
            local w = t.window("UI scale")
            w.AlwaysAutoResize = true
            w:AddText("bg3mse this window's padding grows a quarter, then shrinks back")
            w:AddButton("bg3mse a button")
            t.frames(function ()
                local before = Copy(w.LastSize)
                E.IMGUI.SetUIScaleMultiplier(1.25)
                t.frames(function ()
                    local during = Copy(w.LastSize)
                    C.check(during[1] > before[1] + 0.5, "LastSize at 1.25 is %s, at 1.0 %s", V(during), V(before))
                    E.IMGUI.SetUIScaleMultiplier(1.0)
                    t.frames(function ()
                        local after = Copy(w.LastSize)
                        C.vecNear(after, before, "LastSize back at 1.0", 2)
                        t.finish(true, string.format("LastSize %s, %s at 1.25, %s back at 1.0", V(before), V(during), V(after)))
                    end)
                end)
            end)
        end, { exclusive = true }),

        -- The atlas behind an icon, by its path: StaticData.inl's
        -- GetTextureAtlas reads TextureAtlasMap::AtlasMap, but Norbyte's
        -- module does not register it, so the case reads the map itself.
        case("Image#atlasByPath", "G-136", function (t, E)
            local icon, atlas = IconAndAtlas(E)
            local manager = C.notNil(E.StaticData.GetTextureAtlasManager(), "GetTextureAtlasManager")
            local path = atlas.Path
            C.check(type(path) == "string" and path ~= "", "the atlas of %s has no Path", icon)
            local same = C.notNil(manager.AtlasMap[path], "AtlasMap[" .. path .. "]")
            C.equal(same.Path, path, "the atlas found by its path")
            C.equal(same.TextureUuid, atlas.TextureUuid, "its TextureUuid")
            local byFunction = nil
            pcall(function () byFunction = E.StaticData.GetTextureAtlas end)
            local note = "Ext.StaticData has no GetTextureAtlas (not in StaticData.inl's module)"
            if type(byFunction) == "function" then
                local found = C.notNil(byFunction(path), "GetTextureAtlas(" .. path .. ")")
                C.equal(found.TextureUuid, atlas.TextureUuid, "GetTextureAtlas' TextureUuid")
                note = "GetTextureAtlas(path) agrees"
            end
            t.finish(true, string.format("%s: %s, %dx%d icons; %s", icon, path, atlas.IconWidth, atlas.IconHeight, note))
        end),

        case("Image#icon", "OVL-02", function (t, E)
            local icon, atlas = IconAndAtlas(E)
            local uv = C.notNil(E.StaticData.GetIconUVs(icon), "GetIconUVs(" .. icon .. ")")
            local w = t.window("Image icon")
            local img = w:AddImage(icon)
            C.equal(Kind(img), "Image", "what AddImage made")
            local size = img.ImageData.Size
            C.check(size[1] > 0 and size[2] > 0, NotBound("the icon " .. icon))
            C.equal(img.ImageData.Icon, icon, "ImageData.Icon")
            C.equal(img.ImageData.TextureResource, atlas.TextureUuid, "ImageData.TextureResource (the atlas texture)")
            C.vecNear(size, { atlas.IconWidth, atlas.IconHeight }, "ImageData.Size (the atlas' icon size)")
            C.vecNear(img.ImageData.UV0, uv.UV0, "ImageData.UV0")
            C.vecNear(img.ImageData.UV1, uv.UV1, "ImageData.UV1")
            img.ImageData.Size = { 64, 64 }
            C.vecNear(img.ImageData.Size, { 64, 64 }, "ImageData.Size written")
            img.ImageData.UV0 = uv.UV0
            C.vecNear(img.ImageData.UV0, uv.UV0, "ImageData.UV0 written")
            img.ImageData.UV1 = uv.UV1
            C.raises(function () img.ImageData.Icon = "x" end)
            C.raises(function () img.ImageData.TextureResource = "x" end)
            C.vecNear(img.Tint, { 1, 1, 1, 1 }, "Tint at first")
            img.Tint = { 1, 1, 1, 0.9 }
            C.vecNear(img.Tint, { 1, 1, 1, 0.9 }, "Tint")
            img.Border = { 0, 0, 0, 1 }
            C.vecNear(img.Border, { 0, 0, 0, 1 }, "Border (deprecated)")
            local sized = w:AddImage(icon, { 40, 40 }, { 0, 0 }, { 1, 1 })
            C.vecNear(sized.ImageData.Size, { 40, 40 }, "the size given to AddImage")
            C.vecNear(sized.ImageData.UV1, { 1, 1 }, "the UV1 given to AddImage")
            -- AddIcon is AddImage with a size (deprecated, a warning in the log).
            local viaIcon = w:AddIcon(icon, { 48, 48 })
            C.equal(Kind(viaIcon), "Image", "what AddIcon made")
            C.equal(viaIcon.ImageData.Icon, icon, "AddIcon's ImageData.Icon")
            C.vecNear(viaIcon.ImageData.Size, { 48, 48 }, "AddIcon's size")
            t.frames(function ()
                for _, el in ipairs({ img, sized, viaIcon }) do
                    C.check(Seen(el), "an icon image was not drawn (ImageReference::PrepareRender bound no texture)")
                end
                t.finish(true, string.format("%s %dx%d from %s drawn [rows: G-002, OVL-43, W1-10, W1-28, G-006, OVL-06, G-007, OVL-07, OVL-21, ENG-12, LC-14]",
                    icon, atlas.IconWidth, atlas.IconHeight, atlas.TextureUuid))
            end)
        end, { risk = "unproved" }),

        case("Image#texture", "OVL-43", function (t, E)
            local _, atlas = IconAndAtlas(E)
            local guid = atlas.TextureUuid
            local w = t.window("Image texture")
            local img = w:AddImage(guid)
            local size = img.ImageData.Size
            C.check(size[1] > 0 and size[2] > 0, NotBound("the texture " .. guid))
            C.equal(img.ImageData.TextureResource, guid, "ImageData.TextureResource")
            C.equal(img.ImageData.Icon, "", "ImageData.Icon of a texture")
            C.vecNear(img.ImageData.UV0, { 0, 0 }, "ImageData.UV0")
            C.vecNear(img.ImageData.UV1, { 1, 1 }, "ImageData.UV1")
            img.ImageData.Size = { 128, 128 }
            t.frames(function ()
                C.check(Seen(img), "the texture image was not drawn (ImageReference::PrepareRender bound no texture)")
                t.finish(true, string.format("%s loaded %s (the atlas says %dx%d), drawn [rows: G-006, OVL-06, G-007, OVL-07, OVL-21, ENG-12, W1-10]",
                    guid, V(size), atlas.TextureWidth, atlas.TextureHeight))
            end)
        end, { risk = "unproved" }),

        case("Image#release", "OVL-44", function (t, E)
            -- IMGUITextureLoader: the last reference's release unloads the
            -- texture three frames later (DeleteAfterFrames); a new image of it
            -- loads it again. Alone, so that no other image holds it.
            local _, atlas = IconAndAtlas(E)
            local guid = atlas.TextureUuid
            local w = t.window("Image release")
            local img = w:AddImage(guid)
            C.check(img.ImageData.Size[1] > 0, NotBound("the texture " .. guid))
            img.ImageData.Size = { 96, 96 }
            t.frames(function ()
                C.check(Seen(img), "the texture image was not drawn")
                img:Destroy()
                C.check(Dead(img), "the image is still there after Destroy")
                t.frames(function ()
                    local again = w:AddImage(guid)
                    C.check(again.ImageData.Size[1] > 0, NotBound("the texture loaded again " .. guid))
                    again.ImageData.Size = { 96, 96 }
                    t.frames(function ()
                        C.check(Seen(again), "the texture loaded again was not drawn")
                        t.finish(true, "drawn, destroyed, released, loaded and drawn again [rows: SYM-75]")
                    end)
                end)
            end)
        end, { risk = "unproved", exclusive = true }),

        case("ImageButton#OnClick", "W1-32", function (t, E)
            local icon = IconAndAtlas(E)
            local w = t.window("ImageButton")
            local button = w:AddImageButton("bg3mse image button", icon, { 40, 40 })
            C.equal(Kind(button), "ImageButton", "what AddImageButton made")
            C.check(button.Image.Size[1] > 0, NotBound("the image button's icon " .. icon))
            C.equal(button.Image.Icon, icon, "Image.Icon")
            C.vecNear(button.Image.Size, { 40, 40 }, "Image.Size as added")
            local uvs = w:AddImageButton("bg3mse image button uv", icon, { 32, 32 }, { 0, 0 }, { 0.5, 0.5 })
            C.vecNear(uvs.Image.UV1, { 0.5, 0.5 }, "the UV1 given to AddImageButton")
            C.vecNear(button.Background, { 0, 0, 0, 0 }, "Background at first")
            button.Background = { 0, 0, 0, 0.4 }
            C.vecNear(button.Background, { 0, 0, 0, 0.4 }, "Background")
            button.Tint = { 1, 0.9, 0.9, 1 }
            C.vecNear(button.Tint, { 1, 0.9, 0.9, 1 }, "Tint")
            Flag(button, "MouseButtonMiddle")
            local clicks = 0
            button.OnClick = function () clicks = clicks + 1 end
            t.frames(function ()
                C.check(Seen(button), "the image button was not drawn (no texture)")
                button:Activate()
                t.until_(function () return clicks > 0 end, "OnClick after Activate", function ()
                    t.finish(true, "drawn with " .. icon .. "; Activate pressed it: OnClick [rows: W1-10]")
                end)
            end)
        end, { risk = "unproved", exclusive = true }),

        -- What needs the owner's mouse and keyboard, in one window with a
        -- checklist: hover, tooltip, right click, drag and drop, a menu item,
        -- the combo, sliders, drags, typing, colours and a window's close
        -- button. It ends once every one was seen, or with Skip, or with
        -- "Finish now" (an error listing what was not seen).
        case("StyledRenderable#mouse", "W1-12", function (t, E)
            local seen, order = {}, {}
            local lines = {}
            local w = t.window("by hand", { 760, 980 })
            local closer = nil
            local tip = nil
            w:AddText("Do each of these once, with the mouse and keyboard; each line gets an [x] when seen.")
            w:AddText("The test ends by itself when all are done (scroll this window for the rest). Skip ends it as skipped.")
            local checklist = {
                { "hover", "move the mouse over \"bg3mse hand hover me\", then away" },
                { "tooltip", "  (while over it, its tooltip shows)" },
                { "rightclick", "right-click \"bg3mse hand right-click me\"" },
                { "drag", "drag \"bg3mse hand drag me\" onto \"bg3mse hand drop here\" and let go" },
                { "menu", "open the menu \"bg3mse hand menu\" at the top and pick \"bg3mse hand pick me\"" },
                { "combo", "in \"bg3mse hand combo\" choose \"bg3mse hand second\"" },
                { "slider", "drag \"bg3mse hand slider\"" },
                { "vslider", "drag the vertical slider \"bg3mse hand vertical\"" },
                { "drag value", "drag sideways on \"bg3mse hand drag value\"" },
                { "drag int", "drag sideways on \"bg3mse hand drag int\"" },
                { "text", "click \"bg3mse hand type here\" and type a few letters" },
                { "scalar", "type a number in \"bg3mse hand type a number\"" },
                { "int", "type a whole number in \"bg3mse hand type a whole number\"" },
                { "colour", "change \"bg3mse hand colour\" (drag one of its numbers)" },
                { "picker", "click inside the square of \"bg3mse hand colour picker\"" },
                { "close", "close the small window \"bg3mse in-game test: imgui close me\" with its x" },
            }
            for _, item in ipairs(checklist) do
                lines[item[1]] = w:AddText("[ ] " .. item[2])
                order[#order + 1] = item
            end
            local function Missing()
                local missing = {}
                for _, item in ipairs(order) do
                    if not seen[item[1]] then missing[#missing + 1] = item[1] end
                end
                return missing
            end
            local function Seen_(key, what)
                if t.over or seen[key] then return end
                seen[key] = what or true
                for _, item in ipairs(order) do
                    if item[1] == key then lines[key].Label = "[x] " .. item[2] end
                end
                if #Missing() == 0 then
                    t.finish(true, "all 16 seen [rows: W1-15, W1-26, W1-27, W1-35, W1-36, W1-37, W1-38, W1-39, W1-40, W1-41, W1-42, W1-43, W1-44, LC-14]")
                end
            end
            local skip = w:AddButton("Skip this test")
            skip.OnClick = function () t.finish("skip", "the owner pressed Skip") end
            local stop = w:AddButton("Finish now")
            stop.SameLine = true
            stop.OnClick = function () t.finish(false, "the owner finished; not seen: " .. table.concat(Missing(), ", ")) end
            w:AddSeparator()

            local hover = w:AddText("bg3mse hand hover me")
            hover.OnHoverEnter = function () seen.hoverEnter = true end
            hover.OnHoverLeave = function ()
                if seen.hoverEnter then Seen_("hover") end
            end
            tip = hover:Tooltip()
            tip:AddText("bg3mse hand the tooltip")
            t.cleanup(function () tip:Destroy() end)

            local right = w:AddButton("bg3mse hand right-click me")
            right.OnRightClick = function (h) if h == right then Seen_("rightclick") end end

            local source = w:AddButton("bg3mse hand drag me")
            source.CanDrag = true
            source.DragDropType = "Bg3mseIgtDrag"
            local target = w:AddButton("bg3mse hand drop here")
            target.SameLine = true
            target.DragDropType = "Bg3mseIgtDrag"
            local dragged = {}
            source.OnDragStart = function (h, preview)
                -- HandleDragDrop made a Group for the preview and set DragPreview.
                dragged.start = h == source and preview ~= nil and source.DragPreview == preview
                if preview ~= nil then preview:AddText("bg3mse hand dragging") end
            end
            source.OnDragEnd = function (h) dragged["end"] = h == source end
            target.OnDragDrop = function (h, from) dragged.drop = h == target and from == source end

            local menu = w:AddMainMenu():AddMenu("bg3mse hand menu")
            local item = menu:AddItem("bg3mse hand pick me", "Ctrl+M")
            item.OnClick = function () Seen_("menu") end

            local combo = w:AddCombo("bg3mse hand combo")
            combo.Options = { "bg3mse hand first", "bg3mse hand second", "bg3mse hand third" }
            combo.SelectedIndex = 0
            combo.OnChange = function (_, index)
                if index == 1 and combo.SelectedIndex == 1 then Seen_("combo") end
            end
            local function Changed(el, key)
                el.OnChange = function (h, value)
                    if h == el and value ~= nil then Seen_(key) end
                end
            end
            Changed(w:AddSlider("bg3mse hand slider", 0.5, 0, 1), "slider")
            local vertical = w:AddSliderInt("bg3mse hand vertical", 5, 0, 10)
            vertical.Vertical = true
            vertical.VerticalSize = { 30, 110 }
            Changed(vertical, "vslider")
            Changed(w:AddDrag("bg3mse hand drag value", 0.5, 0, 1), "drag value")
            Changed(w:AddDragInt("bg3mse hand drag int", 5, 0, 100), "drag int")
            local text = w:AddInputText("bg3mse hand type here", "")
            text.OnChange = function (_, value) if type(value) == "string" and value ~= "" then Seen_("text") end end
            Changed(w:AddInputScalar("bg3mse hand type a number", 0), "scalar")
            Changed(w:AddInputInt("bg3mse hand type a whole number", 0), "int")
            Changed(w:AddColorEdit("bg3mse hand colour", { 0.5, 0.5, 0.5 }), "colour")
            Changed(w:AddColorPicker("bg3mse hand colour picker", { 0.5, 0.5, 0.5 }), "picker")

            closer = t.window("close me", { 360, 120 })
            closer:SetPos({ 900, 120 }, "Always")
            closer:AddText("bg3mse hand close this window with its x")
            closer.Closeable = true
            closer.OnClose = function (h)
                if h == closer and closer.Open == false then Seen_("close") end
            end
            w:SetFocus()

            -- What is seen over several events: polled.
            local function Poll()
                if t.over then return end
                if not seen.tooltip and tip.LastSize[1] > 0 then Seen_("tooltip") end
                if not seen.drag and dragged.start and dragged.drop and dragged["end"] then Seen_("drag") end
                t.after(200, Poll)
            end
            t.after(200, Poll)
            t.after(OWNER_MS - 5000, function ()
                t.finish(false, "the owner did not finish in time; not seen: " .. table.concat(Missing(), ", "))
            end)
        end, { optIn = "ImGuiClicks", exclusive = true, risk = "unproved", timeout = OWNER_MS }),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
