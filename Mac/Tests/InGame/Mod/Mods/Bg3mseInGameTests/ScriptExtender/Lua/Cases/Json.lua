-- Ext.Json (BG3Extender/Lua/Libs/Json.inl, JsonBinary.inl): rapidjson, and the
-- binary format the net messages use.

local library = { name = "Json" }

function library.define(C)
    return {
        { "Ext.Json.Parse", id = "LUA-028", risk = "pure", offline = true, run = function (E)
            local v = E.Json.Parse('{"a":[1,2.5,"x",true,null],"b":{"c":-3}}')
            C.equal(v.a[1], 1, "a[1]")
            C.equal(math.type(v.a[1]), "integer", "the type of a[1]")
            C.near(v.a[2], 2.5, "a[2]")
            C.equal(v.a[3], "x", "a[3]")
            C.equal(v.a[4], true, "a[4]")
            C.equal(v.b.c, -3, "b.c")
            C.raises(E.Json.Parse, "{")
        end },
        { "Ext.Json.Stringify", id = "LUA-029", risk = "pure", offline = true, run = function (E)
            -- Number text is rapidjson's on both systems; these are the values
            -- whose text a C library could write differently.
            local text = E.Json.Stringify({ 0.1, 1e300, 123456789012345, -2.5, 1 / 3 }, { Beautify = false })
            C.equal(text, "[0.1,1e300,123456789012345,-2.5,0.3333333333333333]", "the text of the numbers")
            local binary = E.Json.Stringify({ k = { 1, "two", 3.5 } }, { Binary = true })
            local back = E.Json.Parse(binary, true)
            C.equal(back.k[2], "two", "a value after the binary round trip")
            C.near(back.k[3], 3.5, "a number after the binary round trip")
            return text
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
