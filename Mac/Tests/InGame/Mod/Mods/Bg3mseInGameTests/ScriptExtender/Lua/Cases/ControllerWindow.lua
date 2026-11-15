-- A window of the overlay used with the game controller alone, the way a
-- script written for Norbyte's extender offers it to a controller player:
-- opened from a ControllerButtonInput handler, given the focus by the script
-- (Window:SetFocus), moved through with the D-pad and A, which is ImGui's own
-- gamepad navigation (IMGUI.cpp: ImGuiConfigFlags_NavEnableGamepad), while
-- the handler keeps the presses from the game (PreventAction); neither
-- Norbyte's overlay nor this one keeps a controller's input from the game by
-- itself (Extender/Client/SDL.cpp takes keys and the mouse only).
--
-- Client only, opt-in through the trigger file's Extras ("ControllerWindow"),
-- one case that waits for the owner and walks them through it:
--
--   1. Start: the game's own menu for controllers opens. The case lists the
--      named buttons of that menu with the type of each one's data context
--      (an entry a script added shows here), and the owner closes the menu.
--   2. Both sticks pressed in together: the case's handler opens the test
--      window and focuses it.
--   3. In the window: a button, a checkbox, a slider's value, and out by one
--      of two buttons that say whether the game stayed still meanwhile.
--
-- Each step is printed with what a script reads of the game's controller
-- mode at that moment (the client character's input controller flag and the
-- first player's device in the input manager): a mode that changes between
-- the steps is the game's two interfaces flipping under the overlay.

local library = { name = "ControllerWindow" }

local WAIT_MS = 600000
local MENU = "GameMenu_c"       -- the game's menu widget of its controller interface

function library.define(C)
    local function Run(E, M, done)
        local steps, over, step = {}, false, 1
        local cleanups = {}
        local modeAtStart
        local prevented, events = 0, 0
        local pressed, ticked, value = false, false, 3
        local held = {}
        local prompt, promptText, window, checklist

        local function Mode()
            local mode, device = "?", "?"
            pcall(function () mode = tostring(C.session().character.ClientCharacter.InputController.Flags.ControllerMode) end)
            pcall(function () device = tostring(E.Input.GetInputManager().PlayerDevices[1]) end)
            return mode, device
        end

        local function Step(text)
            local mode, device = Mode()
            if modeAtStart == nil then modeAtStart = mode end
            local line = string.format("%s [ControllerMode %s, PlayerDevices[1] %s, %d button events, %d kept from the game]",
                text, mode, device, events, prevented)
            steps[#steps + 1] = line
            E.Utils.Print("bg3mse in-game test: controller window: " .. line)
            return mode
        end

        local function Finish(ok, detail)
            if over then return end
            over = true
            for _, c in ipairs(cleanups) do pcall(c) end
            pcall(function () window:Destroy() end)
            pcall(function () prompt:Destroy() end)
            done(ok, (detail and (detail .. "; ") or "") .. table.concat(steps, "; "))
        end

        local function Skip()
            if over then return end
            over = true
            for _, c in ipairs(cleanups) do pcall(c) end
            pcall(function () window:Destroy() end)
            pcall(function () prompt:Destroy() end)
            done("skip", "the owner pressed Skip; " .. table.concat(steps, "; "))
        end

        local function Say(text)
            pcall(function () promptText.Label = text end)
        end

        -- The named buttons of the game's controller menu, each with the
        -- type of its data context, as a script finds an entry it added.
        local function MenuButtons()
            local found, seen = {}, 0
            local function Walk(node, depth)
                if node == nil or depth > 40 or seen > 4000 then return end
                seen = seen + 1
                local name, kind = "", ""
                pcall(function () name = tostring(node:GetProperty("Name") or "") end)
                pcall(function () kind = tostring(node.Type) end)
                if name ~= "" and kind:find("Button", 1, true) then
                    local context = "none"
                    pcall(function ()
                        local dc = node:GetProperty("DataContext")
                        if dc ~= nil then context = tostring(dc.Type) end
                    end)
                    found[#found + 1] = name .. " (" .. context .. ")"
                end
                local count = 0
                pcall(function () count = node.VisualChildrenCount end)
                for i = 1, count do
                    local ok, child = pcall(function () return node:VisualChild(i) end)
                    if ok then Walk(child, depth + 1) end
                end
            end
            local root = E.UI.GetRoot()
            local content = root and root:Find("ContentRoot")
            if content == nil then return "no ContentRoot" end
            for i = 1, content.ChildrenCount do
                local widget = content:Child(i)
                local ok, name = pcall(function () return widget.Name end)
                if ok and name == MENU then
                    Walk(widget, 0)
                    return string.format("%s is shown, %d named buttons: %s", MENU, #found, table.concat(found, ", "))
                end
            end
            return MENU .. " is not among the " .. tostring(content.ChildrenCount) .. " widgets shown"
        end

        local function Checklist()
            pcall(function ()
                checklist.Label = string.format("[%s] Press me   [%s] Tick me   [%s] slider at 7 or more (now %d)",
                    pressed and "x" or " ", ticked and "x" or " ", value >= 7 and "x" or " ", value)
            end)
        end

        local function Close(how, stayedStill)
            if step ~= 3 then return end
            step = 4
            pcall(function () window.Open = false end)
            pcall(function () window.Visible = false end)
            Step("closed by " .. how)
            -- A moment later: what the game's mode is with the window gone.
            E.Timer.WaitForRealtime(700, function ()
                local mode = Step("after closing")
                local missing = {}
                if not pressed then missing[#missing + 1] = "the button was not pressed" end
                if not ticked then missing[#missing + 1] = "the checkbox was not ticked" end
                if value < 7 then missing[#missing + 1] = "the slider is at " .. tostring(value) end
                if stayedStill == false then missing[#missing + 1] = "the owner says the game reacted to the presses the handler kept" end
                if modeAtStart == "true" and mode ~= "true" then missing[#missing + 1] = "the game left its controller mode" end
                if #missing > 0 then
                    Finish(false, table.concat(missing, ", "))
                else
                    Finish(true, stayedStill and "used with the controller alone, the game stayed still" or "used with the controller alone")
                end
            end)
        end

        local function OpenWindow()
            step = 3
            window.Visible = true
            window.Open = true
            -- What a script does for a controller player: without it a
            -- window that does not take the focus when it appears has none.
            window:SetFocus()
            Say("The test window is open and has the focus. Follow the steps in it.")
            Step("opened by both sticks pressed in")
        end

        prompt = E.IMGUI.NewWindow("bg3mse in-game test: controller window")
        prompt.NoFocusOnAppearing = true
        prompt.NoNav = true
        prompt.AlwaysAutoResize = true
        prompt:SetPos({ 40, 40 }, "Always")
        prompt:AddText("With the game controller only (no mouse, no keyboard).")
        promptText = prompt:AddText("")
        -- The one thing here for the mouse: the way out when no button arrives.
        prompt:AddButton("Skip this test (with the mouse)").OnClick = Skip
        Say("Step 1: press Start (Options / Menu). The game's menu opens: look whether it lists an entry for mod settings.\n"
            .. "(If nothing happens here after Start: go on to step 2, both sticks pressed in together, L3 + R3.)")

        window = E.IMGUI.NewWindow("bg3mse in-game test: a window for the controller")
        window.NoFocusOnAppearing = true
        window.Closeable = true
        window.Visible = false
        window.Open = false
        window:SetPos({ 320, 220 }, "Always")
        window:SetSize({ 760, 520 }, "Always")
        window.OnClose = function () Close("the title bar's close button") end
        window:AddText("Opened and focused by the test's handler. With the D-pad, A (Cross) and B (Circle):")
        window:AddText("  1. move down to \"Press me\" and press A,")
        window:AddText("  2. move to \"Tick me\" and press A,")
        window:AddText("  3. move to the slider, press A, push it right with the D-pad to 7 or more, press B,")
        window:AddText("  4. move to the Close button that is true and press A.")
        window:AddText("If the D-pad does nothing here: hold X (Square) for a second and let go, then try again.")
        window:AddText("Meanwhile the game must not react to the D-pad, A or B: the handler keeps them from it.")
        checklist = window:AddText("")
        Checklist()
        window:AddButton("Press me").OnClick = function ()
            if not pressed then pressed = true; Step("the button was pressed") end
            Checklist()
        end
        window:AddCheckbox("Tick me", false).OnChange = function (_, checked)
            if checked and not ticked then ticked = true; Step("the checkbox was ticked") end
            Checklist()
        end
        window:AddSliderInt("Slider", value, 0, 10).OnChange = function (_, v)
            local now = math.floor(type(v) == "table" and v[1] or v)
            if now >= 7 and value < 7 then value = now; Step("the slider was moved to " .. tostring(now)) end
            value = now
            Checklist()
        end
        window:AddButton("Close: the game stayed still").OnClick = function () Close("its button, the game stayed still", true) end
        window:AddButton("Close: the game reacted to my presses").OnClick = function () Close("its button, the game reacted", false) end
        window:AddButton("Skip this test").OnClick = Skip

        local id = E.Events.ControllerButtonInput:Subscribe(function (e)
            if over then return end
            local name = tostring(e.Button)
            events = events + 1
            held[name] = e.Pressed or nil
            if step == 1 then
                if name == "Start" and e.Pressed then
                    step = 1.5
                    Step("Start pressed (device " .. tostring(e.DeviceId) .. ")")
                    -- The menu is built in the frames after the press.
                    local function Next(text)
                        if over or step ~= 1.5 then return end
                        Step(text)
                        step = 2
                        Say("Step 2: close the game's menu (B / Circle, or Start again). Then press both sticks in together (L3 + R3).")
                    end
                    E.Timer.WaitForRealtime(900, function ()
                        local function Look()
                            local ok, text = pcall(MenuButtons)
                            Next(ok and text or ("the game's menu could not be read: " .. tostring(text)))
                        end
                        if E.UI and E.UI.Defer then E.UI.Defer(Look) else Look() end
                    end)
                    E.Timer.WaitForRealtime(4000, function () Next("the game's menu was not read: the deferred call did not run") end)
                end
            end
            if step < 3 then
                -- Also before Start was seen: the window does not wait on step 1.
                if name == "LeftStick" or name == "RightStick" then
                    if e.Pressed then
                        e:PreventAction()
                        prevented = prevented + 1
                    end
                    if held.LeftStick and held.RightStick then OpenWindow() end
                end
            elseif step == 3 and not (name == "LeftStick" or name == "RightStick") then
                -- Start is the game's, and the way out a script gives.
                if name == "Start" then
                    if e.Pressed then Close("Start") end
                elseif e.Pressed then
                    e:PreventAction()
                    prevented = prevented + 1
                end
            end
        end)
        cleanups[#cleanups + 1] = function () E.Events.ControllerButtonInput:Unsubscribe(id) end

        Step("waiting for Start")
        E.Timer.WaitForRealtime(WAIT_MS - 10000, function ()
            Finish(false, "not finished in time, at step " .. tostring(step))
        end)
    end

    return {
        { "Ext.IMGUI#controllerWindow", id = "LC-10", ctx = "client", risk = "unproved", async = true, exclusive = true,
            timeout = WAIT_MS, optIn = "ControllerWindow", run = Run },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
