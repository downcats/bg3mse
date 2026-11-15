-- Ext.Timer (BG3Extender/Lua/Libs/Timer.inl). Delays are in milliseconds.

local library = { name = "Timer" }

function library.define(C)
    return {
        { "Ext.Timer.MonotonicTime", id = "LUA-076", risk = "pure", offline = true, run = function (E)
            local a = E.Timer.MonotonicTime()
            C.check(math.type(a) == "integer" and a > 0, "MonotonicTime() gave %s", tostring(a))
            C.check(E.Timer.MonotonicTime() >= a, "the clock went back")
        end },
        { "Ext.Timer.MicrosecTime", id = "LUA-076", risk = "pure", offline = true, run = function (E)
            local a = E.Timer.MicrosecTime()
            C.check(type(a) == "number" and a > 0, "MicrosecTime() gave %s", tostring(a))
        end },
        { "Ext.Timer.GameTime", id = "LUA-076", risk = "pure", run = function (E)
            local t = E.Timer.GameTime()
            C.check(type(t) == "number" and t > 0, "GameTime() gave %s", tostring(t))
        end },
        { "Ext.Timer.ClockEpoch", id = "LUA-076", risk = "pure", offline = true, run = function (E)
            local t = E.Timer.ClockEpoch()
            -- After 2023-01-01 and before 2100.
            C.check(math.type(t) == "integer" and t > 1672531200 and t < 4102444800, "ClockEpoch() gave %s", tostring(t))
        end },
        { "Ext.Timer.ClockTime", id = "LUA-077", risk = "pure", offline = true, run = function (E)
            local t = E.Timer.ClockTime()
            C.check(type(t) == "string" and t:match("^%d%d%d%d%-%d%d%-%d%d %d%d:%d%d:%d%d%.%d%d%d%d%d%d%d$") ~= nil,
                "ClockTime() gave %s", tostring(t))
            return t
        end },
        { "Ext.Timer.WaitFor", id = "LUA-078", risk = "pure", async = true, run = function (E, M, done)
            local start = E.Timer.MonotonicTime()
            E.Timer.WaitFor(200, function () done(true, (E.Timer.MonotonicTime() - start) .. " ms") end)
        end },
        { "Ext.Timer.WaitForRealtime", id = "LUA-078", risk = "pure", async = true, run = function (E, M, done)
            local start = E.Timer.MonotonicTime()
            E.Timer.WaitForRealtime(200, function () done(true, (E.Timer.MonotonicTime() - start) .. " ms") end)
        end },
        { "Ext.Timer.Pause", id = "LUA-078", risk = "pure", run = function (E)
            local fired = false
            local handle = E.Timer.WaitFor(60000, function () fired = true end)
            C.equal(E.Timer.Pause(handle), true, "Pause")
            C.equal(E.Timer.IsPaused(handle), true, "IsPaused after Pause")
            C.equal(E.Timer.Cancel(handle), true, "Cancel")
        end },
        { "Ext.Timer.Resume", id = "LUA-078", risk = "pure", run = function (E)
            local handle = E.Timer.WaitFor(60000, function () end)
            E.Timer.Pause(handle)
            C.equal(E.Timer.Resume(handle), true, "Resume")
            C.equal(E.Timer.IsPaused(handle), false, "IsPaused after Resume")
            C.equal(E.Timer.Cancel(handle), true, "Cancel")
        end },
        { "Ext.Timer.IsPaused", id = "LUA-078", risk = "pure", run = function (E)
            local handle = E.Timer.WaitFor(60000, function () end)
            C.equal(E.Timer.IsPaused(handle), false, "IsPaused of a new timer")
            E.Timer.Cancel(handle)
        end },
        { "Ext.Timer.Cancel", id = "LUA-078", risk = "pure", async = true, run = function (E, M, done)
            local fired = false
            local handle = E.Timer.WaitFor(100, function () fired = true end)
            C.equal(E.Timer.Cancel(handle), true, "Cancel")
            E.Timer.WaitFor(400, function ()
                done(not fired, fired and "the cancelled timer fired" or nil)
            end)
        end },
        -- A persistent timer of half a second: saved only if the game saves in
        -- that half second (the install steps say not to save).
        { "Ext.Timer.RegisterPersistentHandler", id = "LUA-079", run = function (E)
            E.Timer.RegisterPersistentHandler("Bg3mseInGameTestsProbe", function () end)
        end },
        { "Ext.Timer.WaitForPersistent", id = "LUA-079", ctx = "server", async = true, run = function (E, M, done)
            E.Timer.RegisterPersistentHandler("Bg3mseInGameTestsPersistent", function (args, handle)
                done(type(args) == "table" and args.Value == 9, "arguments " .. E.Json.Stringify(args, { Beautify = false }))
            end)
            E.Timer.WaitForPersistent(500, "Bg3mseInGameTestsPersistent", { Value = 9 })
        end },
        { "Ext.Timer.WaitForPersistent#client", id = "LUA-079", ctx = "client", run = function (E)
            -- Timer.inl: the client's timers have no persistence, and say so.
            local ok, err = pcall(E.Timer.WaitForPersistent, 500, "Bg3mseInGameTestsPersistent", {})
            C.check(not ok and tostring(err):find("only supported on the server", 1, true) ~= nil,
                "WaitForPersistent on the client: %s", ok and "returned" or tostring(err))
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
