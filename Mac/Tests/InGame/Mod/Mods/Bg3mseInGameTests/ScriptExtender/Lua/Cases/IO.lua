-- Ext.IO (BG3Extender/Lua/Libs/IO.inl, Extender/Shared/ScriptHelpers.cpp).
-- Writes go to the scratch folder under the profile's Script Extender folder;
-- the path overrides map this mod's own two probe files onto each other,
-- one way on the server and the other on the client.

local library = { name = "IO" }

function library.define(C)
    local probe = function () return C.ScratchDir .. "io-probe.txt" end
    local A = "Mods/Bg3mseInGameTests/Probe/A.txt"
    local B = "Mods/Bg3mseInGameTests/Probe/B.txt"
    -- The file a context overrides, the file read instead, and what each holds.
    local function overridePair(E)
        if E.IsServer() then return A, B, "probe%-a", "probe%-b" end
        return B, A, "probe%-b", "probe%-a"
    end
    return {
        { "Ext.IO.SaveFile", id = "LUA-024", risk = "pure", run = function (E)
            C.equal(E.IO.SaveFile(probe(), "abc"), true, "SaveFile")
            C.equal(E.IO.LoadFile(probe()), "abc", "the file read back")
        end },
        { "Ext.IO.AppendFile", id = "LUA-025", risk = "pure", run = function (E)
            C.equal(E.IO.SaveFile(probe(), "abc"), true, "SaveFile")
            C.equal(E.IO.AppendFile(probe(), "def"), true, "AppendFile")
            C.equal(E.IO.LoadFile(probe()), "abcdef", "the file read back")
        end },
        { "Ext.IO.LoadFile", id = "LUA-023", run = function (E)
            C.equal(E.IO.LoadFile(C.ScratchDir .. "no-such-file.txt"), nil, "a missing user file")
            -- From this mod's package, through the game's file reader.
            local meta = E.IO.LoadFile("Mods/Bg3mseInGameTests/meta.lsx", "data")
            C.check(type(meta) == "string" and meta:find(C.ModuleUUID, 1, true) ~= nil, "meta.lsx from the package")
            C.raises(E.IO.LoadFile, probe(), "nosuchcontext")
        end },
        { "Ext.IO.FindFiles", id = "LUA-026", run = function (E)
            local files = E.IO.FindFiles("Mods/Bg3mseInGameTests/Probe", "*.txt", "data", false, true)
            C.type(files, "table", "FindFiles")
            local names = {}
            for _, name in ipairs(files) do names[name] = true end
            C.check(names["A.txt"] and names["B.txt"], "the package's probe files are not listed: %s", E.Json.Stringify(files, { Beautify = false }))
            return E.Json.Stringify(files, { Beautify = false })
        end },
        { "Ext.IO.AddPathOverride", id = "LUA-027", run = function (E)
            -- The overrides are the process's, not a context's, and one
            -- that is there stays (ScriptExtender::AddPathOverride inserts):
            -- the server maps A.txt onto B.txt, the client B.txt onto A.txt.
            local from, to, before, after = overridePair(E)
            if E.IO.GetPathOverride(from) == nil then
                C.check((E.IO.LoadFile(from, "data") or ""):find(before) ~= nil, "%s before the override", from)
            end
            E.IO.AddPathOverride(from, to)
            local ok, text = pcall(E.IO.LoadFile, from, "data")
            C.check(ok and (text or ""):find(after) ~= nil, "%s was not read from %s: %s", from, to, tostring(text))
        end },
        { "Ext.IO.GetPathOverride", id = "LUA-027", run = function (E)
            local from, to = overridePair(E)
            E.IO.AddPathOverride(from, to)
            local over = E.IO.GetPathOverride(from)
            local name = to:match("[^/]+$")
            C.check(over ~= nil and over:find(name, 1, true) ~= nil, "GetPathOverride gave %s", tostring(over))
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
