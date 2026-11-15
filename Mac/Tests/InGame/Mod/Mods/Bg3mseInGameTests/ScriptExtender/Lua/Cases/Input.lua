-- Ext.Input (BG3Extender/Lua/Libs/ClientInput.inl), client only.
--
-- The injected key is Shift+F7: the game's default bindings
-- (Engine.pak PlayerProfiles/Default/inputconfig.json, Eoc/keyboard_1.json,
-- the Mac profile in GamePlatform.pak) bind F1-F5, F8-F10 and Ctrl+Shift+F1..F12,
-- and the extender's console window takes F6, so Shift+F7 reaches nothing.
-- On macOS the injected key goes to the game's input manager at once
-- (Mac/Overlay CocoaInput), its modifier held around it; an injected key
-- does not come back through Norbyte's handler, so no KeyInput event shows it.

local library = { name = "Input" }

function library.define(C)
    local KEY, MODIFIERS = "F7", { "LShift" }
    local function Pair(E)
        -- Down then up, so that no key stays down whatever fails between.
        local ok, err = pcall(E.Input.InjectKeyDown, KEY, MODIFIERS)
        E.Input.InjectKeyUp(KEY, MODIFIERS)
        C.check(ok, "InjectKeyDown raised: %s", tostring(err))
        return "Shift+F7 down and up"
    end
    return {
        { "Ext.Input.InjectKeyPress", id = "LC-20", ctx = "client", risk = "unproved", run = function (E)
            E.Input.InjectKeyPress(KEY, MODIFIERS)
            return "Shift+F7"
        end },
        { "Ext.Input.InjectKeyDown", id = "LC-20", ctx = "client", risk = "unproved", run = Pair },
        { "Ext.Input.InjectKeyUp", id = "LC-20", ctx = "client", risk = "unproved", run = Pair },
        { "Ext.Input.GetInputManager", id = "LC-21", ctx = "client", risk = "unproved", run = function (E)
            local manager = C.notNil(E.Input.GetInputManager(), "GetInputManager")
            C.equal(E.Types.Validate(manager), true, "the input manager passes validation")
            -- The members its code shows (Mac/Layout: the input states, the
            -- definitions, the scheme, the players' devices): a wrong layout
            -- shows as values that make no sense.
            local states = C.count(manager.InputStates)
            local definitions = C.count(manager.InputDefinitions)
            C.check(definitions > 0, "no input definitions")
            local devices = {}
            for i = 1, 4 do devices[i] = tostring(manager.PlayerDevices[i]) end
            return string.format("%d input states, %d definitions, player devices %s, scheme %s", states, definitions,
                table.concat(devices, ","), tostring(E.Types.GetObjectType(manager.InputScheme)))
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
