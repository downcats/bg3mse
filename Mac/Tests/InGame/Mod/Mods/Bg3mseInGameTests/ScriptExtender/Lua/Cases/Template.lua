-- Ext.Template (BG3Extender/Lua/Libs/ClientTemplate.inl, ServerTemplate.inl).
-- A template is pushed to Lua through GameObjectTemplate::GetType, a virtual
-- function the slot check does not cover (LUA-075).

local library = { name = "Template" }

function library.define(C)
    local function TemplateId(E)
        local character = C.character()
        local original = character.OriginalTemplate
        if original == nil or original.OriginalTemplate == nil or original.OriginalTemplate == "" then
            C.skip("the character has no OriginalTemplate")
        end
        return original.OriginalTemplate
    end
    local function case(name, id, run, extra)
        local c = { "Ext.Template." .. name, id = id, risk = "unproved", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    local function FirstKey(map)
        for k in pairs(map) do return k end
    end
    return {
        case("GetRootTemplate", "LUA-075", function (E)
            local id = TemplateId(E)
            local t = C.notNil(E.Template.GetRootTemplate(id), "GetRootTemplate(" .. id .. ")")
            C.equal(t.Id, id, "the template's Id")
            return E.Types.GetObjectType(t)
        end),
        case("GetTemplate", "LUA-075", function (E)
            local id = TemplateId(E)
            C.equal(C.notNil(E.Template.GetTemplate(id), "GetTemplate").Id, id, "the template's Id")
        end),
        case("GetAllRootTemplates", "LUA-075", function (E)
            local all = C.notNil(E.Template.GetAllRootTemplates(), "GetAllRootTemplates")
            C.notNil(all[TemplateId(E)], "the character's template in the map")
            return C.count(all) .. " root templates"
        end),
        -- Server only (LS-02, LS-03).
        case("GetAllCacheTemplates", "LS-02", function (E)
            local all = C.notNil(E.Template.GetAllCacheTemplates(), "GetAllCacheTemplates")
            return C.count(all) .. " cache templates"
        end, { ctx = "server" }),
        case("GetCacheTemplate", "LS-02", function (E)
            local id = FirstKey(E.Template.GetAllCacheTemplates() or {})
            if id == nil then C.skip("no cache templates") end
            C.notNil(E.Template.GetCacheTemplate(id), "GetCacheTemplate(" .. tostring(id) .. ")")
        end, { ctx = "server" }),
        case("GetAllLocalTemplates", "LS-03", function (E)
            local all = C.notNil(E.Template.GetAllLocalTemplates(), "GetAllLocalTemplates")
            return C.count(all) .. " local templates"
        end, { ctx = "server" }),
        case("GetLocalTemplate", "LS-03", function (E)
            local id = FirstKey(E.Template.GetAllLocalTemplates() or {})
            if id == nil then C.skip("no local templates") end
            C.notNil(E.Template.GetLocalTemplate(id), "GetLocalTemplate(" .. tostring(id) .. ")")
        end, { ctx = "server" }),
        case("GetAllLocalCacheTemplates", "LS-03", function (E)
            local all = C.notNil(E.Template.GetAllLocalCacheTemplates(), "GetAllLocalCacheTemplates")
            return C.count(all) .. " local cache templates"
        end, { ctx = "server" }),
        case("GetLocalCacheTemplate", "LS-03", function (E)
            local id = FirstKey(E.Template.GetAllLocalCacheTemplates() or {})
            if id == nil then C.skip("no local cache templates") end
            C.notNil(E.Template.GetLocalCacheTemplate(id), "GetLocalCacheTemplate(" .. tostring(id) .. ")")
        end, { ctx = "server" }),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
