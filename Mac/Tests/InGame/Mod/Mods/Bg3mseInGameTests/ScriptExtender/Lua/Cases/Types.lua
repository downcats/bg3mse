-- Ext.Types (BG3Extender/Lua/Libs/Types.inl) and Ext.Types.GenerateIdeHelpers
-- (LuaScripts/Libs/IdeHelpersGenerator.lua).

local library = { name = "Types" }

function library.define(C)
    -- A mapped object the session always has: the game's base module.
    local function Module(E)
        return C.notNil(E.Mod.GetBaseMod(), "Ext.Mod.GetBaseMod()")
    end

    -- A non-empty set proxy from one of the character's components.
    local function FindSet(E)
        local character = C.character()
        for name, component in pairs(character:GetAllComponents()) do
            local ok, found = pcall(function ()
                for key, value in pairs(component) do
                    if E.Types.GetValueType(value) == "Set" and #value > 0 then
                        return value
                    end
                end
            end)
            if ok and found ~= nil then return found, name end
        end
        -- The sets that are seldom empty in a level: what each character
        -- sees (eoc::sight::EntityViewshedComponent), on the server also
        -- what each viewshed participant can see.
        local candidates = { { "SightEntityViewshed", "Viewshed" } }
        if E.IsServer() then candidates[#candidates + 1] = { "ServerViewshedParticipant", "CanSee" } end
        for _, c in ipairs(candidates) do
            local ok, found, where = pcall(function ()
                for _, entity in ipairs(E.Entity.GetAllEntitiesWithComponent(c[1]) or {}) do
                    local set = entity[c[1]][c[2]]
                    if set ~= nil and #set > 0 then return set, c[1] .. "." .. c[2] end
                end
            end)
            if ok and found ~= nil then return found, where end
        end
        C.skip("no non-empty set in the character's components, SightEntityViewshed.Viewshed or ServerViewshedParticipant.CanSee")
    end

    return {
        { "Ext.Types.GetValueType", id = "LUA-083", risk = "pure", offline = true, run = function (E)
            C.equal(E.Types.GetValueType(1), "number", "GetValueType(1)")
            C.equal(E.Types.GetValueType({}), "table", "GetValueType({})")
        end },
        { "Ext.Types.GetBaseValueType", id = "LUA-083", risk = "pure", offline = true, run = function (E)
            C.equal(E.Types.GetBaseValueType("x"), "string", "GetBaseValueType('x')")
        end },
        { "Ext.Types.GetObjectType", id = "LUA-083", run = function (E)
            C.equal(E.Types.GetObjectType(Module(E)), "Module", "the base module's type")
        end },
        { "Ext.Types.GetTypeInfo", id = "LUA-083", risk = "pure", offline = true, run = function (E)
            local info = C.notNil(E.Types.GetTypeInfo("Module"), "GetTypeInfo('Module')")
            C.equal(info.Kind, "Object", "the kind of Module")
            C.notNil(info.Members.Info, "the member Module.Info")
            C.equal(E.Types.GetTypeInfo("bg3mse::NoSuchType"), nil, "an unknown type")
        end },
        { "Ext.Types.TypeOf", id = "LUA-083", run = function (E)
            local info = E.Types.TypeOf(Module(E))
            C.equal(info.TypeName, "Module", "TypeOf(module).TypeName")
            -- The native name is MSVC's typeid name on every system
            -- (LuaTypeInformation.cpp, BEGIN_CLS_TN): bg3se::Module is a struct
            -- (GameDefinitions/Module.h:55, inside BEGIN_SE()).
            C.equal(info.NativeName, "struct bg3se::Module", "TypeOf(module).NativeName")
            return tostring(info.NativeName) .. " [rows: G-147]"
        end },
        { "Ext.Types.IsA", id = "LUA-083", run = function (E)
            C.equal(E.Types.IsA(Module(E), "Module"), true, "IsA(module, 'Module')")
            C.equal(E.Types.IsA(Module(E), "ModManager"), false, "IsA(module, 'ModManager')")
        end },
        { "Ext.Types.GetAllTypes", id = "LUA-083", risk = "pure", offline = true, run = function (E)
            local all = E.Types.GetAllTypes()
            C.check(#all > 1000, "only %d types", #all)
            return #all .. " types"
        end },
        { "Ext.Types.Validate", id = "LUA-084", run = function (E)
            C.equal(E.Types.Validate(Module(E)), true, "Validate(base module)")
        end },
        { "Ext.Types.Serialize", id = "LUA-084", run = function (E)
            local t = E.Types.Serialize(Module(E))
            C.type(t, "table", "the serialized module")
            C.equal(t.Info.ModuleUUIDString, C.GustavUUID, "the serialized module's UUID")
        end },
        { "Ext.Types.Unserialize", id = "LUA-084", run = function (E)
            -- Writes back what was read: the object is left as it was.
            local mod = Module(E)
            local before = E.Types.Serialize(mod)
            E.Types.Unserialize(mod, before)
            C.equal(E.Json.Stringify(E.Types.Serialize(mod)), E.Json.Stringify(before), "the module after writing it back")
        end },
        { "Ext.Types.Construct", id = "LUA-084", risk = "pure", offline = true, run = function (E)
            -- Norbyte's body ends in "TODO; return 0" on every platform: no value.
            C.equal(E.Types.Construct("Module"), nil, "Construct('Module')")
            C.raises(E.Types.Construct, "bg3mse::NoSuchType")
        end },
        { "Ext.Types.GetHashSetValueAt", id = "LUA-083", run = function (E)
            local set, where = FindSet(E)
            -- Every element by index, the same as the set's own iteration
            -- gives at that index (LuaSetProxy.h: both read keys()), and
            -- nothing past the end.
            local members = {}
            for i, v in pairs(set) do members[i] = v end
            local n = #set
            for i = 1, n do
                local value = C.notNil(E.Types.GetHashSetValueAt(set, i), "element " .. i .. " of " .. tostring(where))
                C.check(C.same(members[i], value), "element %d of %s is %s, its iteration gives %s", i, tostring(where),
                    tostring(value), tostring(members[i]))
            end
            C.equal(E.Types.GetHashSetValueAt(set, n + 1), nil, "the element past the end")
            return tostring(where) .. ", " .. n .. " elements"
        end },
        { "Ext.Types.GetFunctionLocation", id = "LUA-083", risk = "pure", offline = true, run = function (E)
            local source, line = E.Types.GetFunctionLocation(function () end)
            C.type(source, "string", "the source")
            C.check(math.type(line) == "integer" and line > 0, "the line is %s", tostring(line))
        end },
        { "Ext.Types.AddCustomFunction", id = "LUA-084", run = function (E)
            -- Lives in this Lua state only, under a name of this mod's own.
            C.equal(E.Types.AddCustomFunction("Module", "Bg3mseIgtProbe", function (self) return "probe:" .. self.Info.Name end), true, "AddCustomFunction")
            C.equal(Module(E):Bg3mseIgtProbe(), "probe:" .. Module(E).Info.Name, "the custom function's result")
        end },
        { "Ext.Types.AddCustomProperty", id = "LUA-084", run = function (E)
            C.equal(E.Types.AddCustomProperty("Module", "Bg3mseIgtProbeProperty", function (self) return 7 end), true, "AddCustomProperty")
            C.equal(Module(E).Bg3mseIgtProbeProperty, 7, "the custom property")
        end },
        -- Loaded by the server's start-up only (ServerStartup.lua); it reads
        -- the stats manager, so not offline. In the game
        -- the text goes to the scratch folder, for a diff against
        -- BG3Extender/IdeHelpers/ExtIdeHelpers.lua (LUA-013, LUA-085).
        { "Ext.Types.GenerateIdeHelpers", id = "LUA-013", ctx = "server", risk = "pure", run = function (E)
            local path = (not C.offline) and (C.ScratchDir .. "ExtIdeHelpers.lua") or nil
            local text = E.Types.GenerateIdeHelpers(path)
            C.check(type(text) == "string" and #text > 100000, "the helpers are %s characters", type(text) == "string" and #text or "no")
            return #text .. " characters" .. (path and (", written to Script Extender/" .. path) or "")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
