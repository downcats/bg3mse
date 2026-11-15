-- Ext.IMGUI (BG3Extender/Lua/Libs/ClientIMGUI.inl), client only. The window
-- is this run's own and is destroyed again.

local library = { name = "IMGUI" }

function library.define(C)
    local FONT = "Mods/" .. C.ModuleFolder .. "/Fonts/ProggyClean.ttf"
    local function case(name, id, run, extra)
        local c = { "Ext.IMGUI." .. name, id = id, ctx = "client", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    return {
        case("NewWindow", "LC-10", function (E, M, done)
            local window = C.notNil(E.IMGUI.NewWindow("bg3mse in-game tests"), "NewWindow")
            window:AddText("bg3mse in-game test window: it closes by itself")
            E.Timer.WaitForRealtime(1000, function ()
                local ok, err = pcall(function () window:Destroy() end)
                done(ok, err)
            end)
        end, { async = true }),
        case("EnableDemo", "LC-11", function (E, M, done)
            -- Deprecated: it shows nothing any more and says in the log to
            -- use ToggleDebugTools.
            E.IMGUI.EnableDemo(true)
            E.IMGUI.EnableDemo(false)
            done(true)
        end, { async = true }),
        case("ToggleDebugTools", "UPS-03", function (E, M, done)
            -- ImGui's own demo and metrics windows for a second, then none
            -- (GuiDebugToolFlags; IMGUIObjectManager::Render).
            E.IMGUI.ToggleDebugTools({ "Demo", "Metrics" })
            E.Timer.WaitForRealtime(1000, function ()
                E.IMGUI.ToggleDebugTools({})
                C.raises(function () E.IMGUI.ToggleDebugTools({ "NoSuchTool" }) end)
                done(true)
            end)
        end, { async = true }),
        case("LoadFont", "LC-12", function (E, M, done)
            -- The mod's own font (Fonts/ProggyClean.ttf, MIT), read through
            -- the game's file system as a data path (IMGUIManager::LoadFont,
            -- script::LoadExternalFile): it adds a font to the overlay's atlas
            -- for the session. A text drawn in it shows for a second.
            C.equal(E.IMGUI.LoadFont("Bg3mseIgtProggy", FONT, 13.0), true, "LoadFont of the mod's font")
            C.equal(E.IMGUI.LoadFont("Bg3mseIgtProggy", FONT, 13.0), true, "LoadFont of the same font again")
            C.equal(E.IMGUI.LoadFont("Bg3mseIgtMissing", "Mods/Bg3mseInGameTests/Fonts/NoSuchFont.ttf", 13.0), false,
                "LoadFont of a file that is not there")
            local window = C.notNil(E.IMGUI.NewWindow("bg3mse in-game tests: font"), "NewWindow")
            local text = window:AddText("bg3mse in-game test: this line is in ProggyClean")
            text.Font = "Bg3mseIgtProggy"
            E.Timer.WaitForRealtime(1000, function ()
                local ok, err = pcall(function () window:Destroy() end)
                done(ok, ok and "loaded; a line drawn in it for 1 s" or err)
            end)
        end, { async = true, risk = "unproved" }),
        case("SetScale", "LC-13", function (E)
            -- Deprecated: a warning only.
            E.IMGUI.SetScale(1.0)
        end),
        case("SetUIScaleMultiplier", "LC-13", function (E)
            -- 1.0 is the default (there is no getter to restore another value).
            E.IMGUI.SetUIScaleMultiplier(1.0)
        end),
        case("SetFontScaleMultiplier", "LC-13", function (E)
            E.IMGUI.SetFontScaleMultiplier(1.0)
        end),
        case("GetViewportSize", "LC-13", function (E)
            local size = E.IMGUI.GetViewportSize()
            C.check(size[1] > 0 and size[2] > 0, "viewport %sx%s", tostring(size[1]), tostring(size[2]))
            return size[1] .. "x" .. size[2]
        end),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
