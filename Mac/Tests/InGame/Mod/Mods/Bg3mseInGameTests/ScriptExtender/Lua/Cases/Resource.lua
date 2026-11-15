-- Ext.Resource (BG3Extender/Lua/Libs/StaticData.inl). Norbyte's
-- ResourceTests.lua (client) checks more.

local library = { name = "Resource" }

function library.define(C)
    local function FirstPackage(E)
        local packages = E.Resource.GetPackages()
        if #packages == 0 then C.skip("no resource packages") end
        for _, p in ipairs(packages) do
            if #E.Resource.GetPackageResources(p) > 0 then return p end
        end
        C.skip("no package has resources")
    end
    return {
        { "Ext.Resource.GetAll", id = "LUA-074", risk = "unproved", run = function (E)
            local all = E.Resource.GetAll("Visual")
            C.check(#all > 0, "no visual resources")
            return #all .. " visuals"
        end },
        { "Ext.Resource.Get", id = "LUA-074", risk = "unproved", run = function (E)
            local id = E.Resource.GetAll("Visual")[1]
            if id == nil then C.skip("no visual resources") end
            local res = C.notNil(E.Resource.Get(id, "Visual"), "Get(" .. id .. ")")
            C.equal(res.Guid, id, "the resource's id")
        end },
        { "Ext.Resource.GetPackages", id = "LUA-074", risk = "unproved", run = function (E)
            C.check(#E.Resource.GetPackages() > 0, "no packages")
        end },
        { "Ext.Resource.GetPackageResources", id = "LUA-074", risk = "unproved", run = function (E)
            local p = FirstPackage(E)
            return p .. ": " .. #E.Resource.GetPackageResources(p)
        end },
        { "Ext.Resource.GetPackageResource", id = "LUA-074", risk = "unproved", run = function (E)
            local p = FirstPackage(E)
            local id = E.Resource.GetPackageResources(p)[1]
            C.notNil(E.Resource.GetPackageResource(p, id), "GetPackageResource(" .. p .. ", " .. id .. ")")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
