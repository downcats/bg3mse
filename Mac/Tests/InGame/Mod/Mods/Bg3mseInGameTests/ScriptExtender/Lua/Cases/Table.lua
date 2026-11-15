-- Ext.Table (BG3Extender/Lua/Libs/Table.inl).

local library = { name = "Table" }

function library.define(C)
    return {
        { "Ext.Table.Find", id = "LUA-031", risk = "pure", offline = true, run = function (E)
            C.equal(E.Table.Find({ "a", "b", "c" }, "b"), 2, "the index of b")
            C.equal(E.Table.Find({ "a" }, "z"), nil, "a value not in the table")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
