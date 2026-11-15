-- Ext.Loca (BG3Extender/Lua/Libs/Localization.inl). The writes use a handle
-- and a key of this mod's own, which nothing in the game reads.

local library = { name = "Loca" }

function library.define(C)
    local HANDLE = "hbg3mseigt0000000000000000000000000001"
    local KEY = "Bg3mseInGameTestsProbeKey"
    return {
        { "Ext.Loca.GetTranslatedString", id = "LUA-034", run = function (E)
            C.equal(E.Loca.GetTranslatedString("hbg3mseigtnosuchhandle", "fallback"), "fallback", "an unknown handle")
            local character = C.character()
            local name = character.DisplayName
            if name == nil then C.skip("the character has no DisplayName") end
            local handle = name.NameKey.Handle.Handle
            -- Name is a TranslatedString object; Get() gives its text.
            local text = name.Name:Get()
            C.equal(E.Loca.GetTranslatedString(handle), text, "the character's name by its handle")
            return text
        end },
        { "Ext.Loca.UpdateTranslatedString", id = "LUA-035", run = function (E)
            C.equal(E.Loca.UpdateTranslatedString(HANDLE, "bg3mse probe text"), true, "UpdateTranslatedString")
            C.equal(E.Loca.GetTranslatedString(HANDLE), "bg3mse probe text", "the text read back")
        end },
        { "Ext.Loca.UpdateTranslatedStringKey", id = "LUA-035", run = function (E)
            C.equal(E.Loca.UpdateTranslatedStringKey(KEY, HANDLE), true, "UpdateTranslatedStringKey")
        end },
        { "Ext.Loca.GetTranslatedStringKey", id = "LUA-035", run = function (E)
            E.Loca.UpdateTranslatedStringKey(KEY, HANDLE)
            -- The handle itself, as a string (LuaSerializers.cpp, TranslatedString).
            C.equal(E.Loca.GetTranslatedStringKey(KEY), HANDLE, "the key's handle")
        end },
        { "Ext.Loca.GetAllTranslatedStringKeys#game", id = "LUA-035", run = function (E)
            -- The game's own keys (TranslatedStringKeyManager, whose map's
            -- macOS layout is proved): each names a handle with a text.
            local keys = C.notNil(E.Loca.GetAllTranslatedStringKeys(), "GetAllTranslatedStringKeys")
            local n, tried, texts, example = 0, 0, 0, nil
            for key, _ in pairs(keys) do
                n = n + 1
                if key ~= KEY and tried < 20 then
                    tried = tried + 1
                    local handle = E.Loca.GetTranslatedStringKey(key)
                    local text = handle and E.Loca.GetTranslatedString(handle) or nil
                    if type(text) == "string" and text ~= "" then
                        texts = texts + 1
                        example = example or (tostring(key) .. " = " .. text)
                    end
                end
            end
            C.check(n > 1, "only %d key(s)", n)
            C.check(texts > 0, "none of %d game keys gave a text", tried)
            return string.format("%d keys; %d of %d read gave a text, e.g. %s", n, texts, tried, example)
        end },
        { "Ext.Loca.GetAllTranslatedStringKeys", id = "LUA-035", run = function (E)
            E.Loca.UpdateTranslatedStringKey(KEY, HANDLE)
            local keys = C.notNil(E.Loca.GetAllTranslatedStringKeys(), "GetAllTranslatedStringKeys")
            C.notNil(keys[KEY], "this mod's key in the table")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
