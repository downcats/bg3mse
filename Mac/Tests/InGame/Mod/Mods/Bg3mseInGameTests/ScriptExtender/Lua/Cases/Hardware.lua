-- The cases that need the owner's hands: a controller, typing, the scroll
-- wheel, the window's size, HDR, the console's F6. Client only, each opt-in
-- through the trigger file's Extras ("Controller", "Typing", "Wheel",
-- "Resize", "HDR", "F6"), so that a normal run never waits for them. Each
-- shows a window saying what to do and waits for it (one at a time:
-- exclusive), with a Skip button; what was seen is the case's detail. The
-- client's closing notice lists the opt-in cases that were not exercised.
-- The checker prints next to the HDR case the overlay's own lines about
-- the frames it draws on (Mac/Overlay/MetalBackend.cpp: "IMGUI: the game
-- presents in ...").

local library = { name = "Hardware" }

local WAIT_MS = 240000

function library.define(C)
    -- A window with the owner's instructions and buttons. buttons: list of
    -- { label, fn }. A Skip button ends the case as skipped. Returns the
    -- window and a function that closes it.
    local function Prompt(E, done, title, lines, buttons)
        local window = E.IMGUI.NewWindow("bg3mse in-game test: " .. title)
        for _, line in ipairs(lines) do window:AddText(line) end
        local closed = false
        local function Close()
            if closed then return end
            closed = true
            pcall(function () window:Destroy() end)
        end
        for _, b in ipairs(buttons or {}) do
            local button = window:AddButton(b[1])
            button.OnClick = function () b[2](Close) end
        end
        local skip = window:AddButton("Skip this test")
        skip.OnClick = function ()
            Close()
            done("skip", "the owner pressed Skip")
        end
        pcall(function () window:SetFocus() end)
        return window, Close
    end

    -- Subscribes fn to an event for the case's life; the returned function
    -- unsubscribes.
    local function Listen(E, name, fn)
        local id = E.Events[name]:Subscribe(fn)
        return function () E.Events[name]:Unsubscribe(id) end
    end

    -- Ends the case: closes, unsubscribes, reports once.
    local function Ender(done, ...)
        local cleanups = { ... }
        local over = false
        return function (ok, detail)
            if over then return end
            over = true
            for _, c in ipairs(cleanups) do pcall(c) end
            done(ok, detail)
        end, cleanups
    end

    local function case(name, id, optIn, run)
        return { name, id = id, ctx = "client", risk = "unproved", async = true, exclusive = true,
            timeout = WAIT_MS, optIn = optIn, run = run }
    end

    -- The owner's yes or no.
    local function Confirm(E, done, title, lines, yes, no, detail)
        local finish
        local _, close = Prompt(E, function (...) finish(...) end, title, lines, {
            { yes, function (c) c(); finish(true, detail and detail() or "the owner confirmed") end },
            { no, function (c) c(); finish(false, (detail and detail() or "") .. " (the owner said: " .. no .. ")") end },
        })
        finish = Ender(done, close)
        return finish
    end

    return {
        case("Ext.Events.ControllerButtonInput#owner", "EV-17", "Controller", function (E, M, done)
            local seen, buttons = {}, {}
            local finish, cleanups = Ender(done)
            local _, close = Prompt(E, function (...) finish(...) end, "controller buttons", {
                "With the game controller:",
                "  1. press and release the A button (Cross on a PlayStation pad),",
                "  2. press and release the B button (Circle).",
                "The test ends by itself once both have gone down and up.",
            })
            cleanups[#cleanups + 1] = close
            cleanups[#cleanups + 1] = Listen(E, "ControllerButtonInput", function (e)
                local name = tostring(e.Button)
                seen[#seen + 1] = string.format("%s %s (Pressed %s, device %s)", name, tostring(e.Event), tostring(e.Pressed), tostring(e.DeviceId))
                buttons[name] = buttons[name] or {}
                buttons[name][e.Pressed and "down" or "up"] = true
                local a, b = buttons.A, buttons.B
                if a and a.down and a.up and b and b.down and b.up then
                    finish(true, table.concat(seen, "; "))
                end
            end)
        end),
        case("Ext.Events.ControllerAxisInput#owner", "EV-16", "Controller", function (E, M, done)
            local peak = {}
            local finish, cleanups = Ender(done)
            local _, close = Prompt(E, function (...) finish(...) end, "controller sticks", {
                "With the game controller:",
                "  1. push the left stick all the way to the right, then let it go,",
                "  2. pull the right trigger all the way, then let it go.",
                "The test ends by itself once both went past three quarters.",
            })
            cleanups[#cleanups + 1] = close
            local function Detail()
                local parts = {}
                for axis, p in pairs(peak) do
                    parts[#parts + 1] = string.format("%s %.3f..%.3f (device %s)", axis, p.min, p.max, tostring(p.device))
                end
                table.sort(parts)
                return table.concat(parts, "; ")
            end
            cleanups[#cleanups + 1] = Listen(E, "ControllerAxisInput", function (e)
                local axis = tostring(e.Axis)
                local p = peak[axis] or { min = 0, max = 0, device = e.DeviceId }
                peak[axis] = p
                p.min = math.min(p.min, e.Value)
                p.max = math.max(p.max, e.Value)
                if math.abs(e.Value) > 1.0001 then
                    finish(false, "a value outside -1..1: " .. axis .. " " .. tostring(e.Value) .. "; " .. Detail())
                elseif peak.LeftX and peak.LeftX.max > 0.75 and peak.TriggerRight and peak.TriggerRight.max > 0.75 then
                    finish(true, Detail())
                end
            end)
        end),
        case("Ext.IMGUI#controllerNavigation", "LC-10", "Controller", function (E, M, done)
            -- Norbyte's overlay turns on ImGui's gamepad navigation
            -- (IMGUI.cpp: ImGuiConfigFlags_NavEnableGamepad).
            local finish, cleanups = Ender(done)
            local window, close = Prompt(E, function (...) finish(...) end, "controller navigation", {
                "With the game controller only (no mouse, no keyboard):",
                "  move to the button \"Second\" below with the D-pad and press A (Cross).",
                "If the game takes the D-pad instead, press Skip.",
            })
            cleanups[#cleanups + 1] = close
            local first = window:AddButton("First")
            first.OnClick = function () finish(false, "First was pressed, not Second") end
            local second = window:AddButton("Second")
            second.OnClick = function () finish(true, "Second pressed through the controller") end
        end),
        case("Ext.IMGUI#textInput", "LC-10", "Typing", function (E, M, done)
            local finish, cleanups = Ender(done)
            local text = ""
            local window, close = Prompt(E, function (...) finish(...) end, "typing", {
                "1. Click the empty field below and type:  deja vu  with the accents, deja as d e-acute j a-grave,",
                "   using dead keys (US layout: Option+E then E gives e-acute; Option+` then A gives a-grave).",
                "2. Optional: switch to an input method (e.g. Japanese Kana), type ka, confirm, switch back.",
                "3. Select all of the grey field's text, copy it (Cmd+C), click the empty field, paste (Cmd+V).",
                "4. Press Done.",
            })
            cleanups[#cleanups + 1] = close
            local source = window:AddInputText("copy from here", "bg3mse-paste-ok")
            -- A bitmask property is one boolean per flag (P_BITMASK).
            source.ReadOnly = true
            source.AutoSelectAll = true
            local field = window:AddInputText("type here", "")
            field.OnChange = function (_, value) text = value end
            local doneButton = window:AddButton("Done")
            doneButton.OnClick = function ()
                text = field.Text or text
                local codepoints = {}
                local kana = false
                -- Decoded here: the extender gives Lua no utf8 library.
                local i = 1
                while i <= #text do
                    local b = text:byte(i)
                    local n = b >= 0xF0 and 4 or b >= 0xE0 and 3 or b >= 0xC0 and 2 or 1
                    if n > 1 then
                        local cp = b & (0xFF >> (n + 1))
                        for k = 1, n - 1 do cp = (cp << 6) | ((text:byte(i + k) or 0) & 0x3F) end
                        codepoints[#codepoints + 1] = string.format("U+%04X", cp)
                        if cp >= 0x3040 and cp <= 0x30FF then kana = true end
                    end
                    i = i + n
                end
                local detail = string.format("typed %q; beyond ASCII: %s; input method text %s", text,
                    #codepoints > 0 and table.concat(codepoints, " ") or "none", kana and "seen" or "not seen")
                local accents = text:find("d\u{e9}j\u{e0} vu", 1, true) ~= nil
                local pasted = text:find("bg3mse-paste-ok", 1, true) ~= nil
                if accents and pasted then
                    finish(true, detail)
                else
                    finish(false, (accents and "" or "no \"d\u{e9}j\u{e0} vu\"; ") .. (pasted and "" or "no pasted text; ") .. detail)
                end
            end
        end),
        case("Ext.Events.MouseWheelInput#owner", "EV-15", "Wheel", function (E, M, done)
            local up, down, seen = false, false, {}
            local finish, cleanups = Ender(done)
            local _, close = Prompt(E, function (...) finish(...) end, "scroll wheel", {
                "Over the game (not over this window): turn the scroll wheel one notch",
                "away from you, then one notch towards you (or swipe two fingers on a trackpad).",
                "The test ends by itself once both directions were seen.",
            })
            cleanups[#cleanups + 1] = close
            cleanups[#cleanups + 1] = Listen(E, "MouseWheelInput", function (e)
                if #seen < 12 then seen[#seen + 1] = string.format("%.2f,%.2f at %d,%d", e.ScrollX, e.ScrollY, e.X, e.Y) end
                if e.ScrollY > 0 then up = true elseif e.ScrollY < 0 then down = true end
                if up and down then finish(true, table.concat(seen, "; ")) end
            end)
        end),
        case("Ext.Events.ViewportResized#owner", "EV-18", "Resize", function (E, M, done)
            local sizes = {}
            local finish, cleanups = Ender(done)
            local before = E.IMGUI.GetViewportSize()
            local window, close = Prompt(E, function (...) finish(...) end, "window size", {
                "Change the game window's size and put it back:",
                "  in windowed mode drag a corner of the window, or switch between",
                "  fullscreen and windowed in the game's video options and back.",
                "Then press Done.",
            })
            cleanups[#cleanups + 1] = close
            cleanups[#cleanups + 1] = Listen(E, "ViewportResized", function (e)
                if #sizes < 20 then sizes[#sizes + 1] = e.Width .. "x" .. e.Height end
            end)
            local doneButton = window:AddButton("Done")
            doneButton.OnClick = function ()
                -- The overlay's size follows on its next frame.
                E.Timer.WaitForRealtime(500, function ()
                    local now = E.IMGUI.GetViewportSize()
                    local detail = string.format("viewport %sx%s before, %sx%s after; events: %s", before[1], before[2],
                        now[1], now[2], #sizes > 0 and table.concat(sizes, ", ") or "none")
                    if #sizes == 0 then
                        finish(false, "no ViewportResized event; " .. detail)
                    elseif sizes[#sizes] ~= now[1] .. "x" .. now[2] then
                        finish(false, "the last event's size is not the viewport's; " .. detail)
                    else
                        finish(true, detail)
                    end
                end)
            end
        end),
        case("Ext.IMGUI#hdr", "LC-10", "HDR", function (E, M, done)
            local viewport = function ()
                local v = E.IMGUI.GetViewportSize()
                return "viewport " .. v[1] .. "x" .. v[2] .. "; the overlay's colour space is in the checker's == overlay section"
            end
            Confirm(E, done, "HDR", {
                "Only with a display that can show HDR:",
                "  1. turn HDR on in the game's video options (Display: HDR) and close the options,",
                "  2. look at this window: its text should be as white as the game's menu text,",
                "     not grey, not glowing, and its colours not washed out.",
                "  3. turn HDR off again in the video options.",
                "Then answer below.",
            }, "It looked right", "It looked wrong", viewport)
        end),
        case("Ext.Debug#consoleF6", "EV-12", "F6", function (E, M, done)
            -- The console window (CreateConsole) is the overlay's; F6 hides
            -- and shows it. Whether the key also reached Lua is reported.
            local keys = {}
            local stop = Listen(E, "KeyInput", function (e)
                if tostring(e.Key) == "F6" and #keys < 8 then keys[#keys + 1] = tostring(e.Event) end
            end)
            local finish = Confirm(E, function (ok, detail) pcall(stop); done(ok, detail) end, "console F6", {
                "Needs the console window (CreateConsole in ScriptExtenderSettings.json).",
                "  1. press F6: the console window hides,",
                "  2. press F6 again: it shows again.",
                "Then answer below.",
            }, "It hid and came back", "It did not", function ()
                return "F6 seen by Lua's KeyInput: " .. (#keys > 0 and table.concat(keys, ",") or "never")
            end)
            return finish
        end),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
