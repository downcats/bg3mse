-- Ext.Net (BG3Extender/Lua/Libs/ClientNet.inl, ServerNet.inl) and the
-- channels over it (LuaScripts/Libs/NetChannel.lua, NetworkManager.lua).
--
-- Messages with this mod's module GUID go through the local queue in single
-- player; messages without one (the legacy channels) through the game's own
-- network (NET-01..03, NET-12). The server's cases send once; the client's
-- handlers, set up at its bootstrap, record what arrives. The client's
-- FromServer case asks the server to send again and waits for the arrivals:
-- after a crash the two contexts resume in different launches, so what the
-- server sent in an earlier process never reaches this one.

local library = { name = "Net" }

local LEGACY_TO_SERVER = "Bg3mseIgtLegacyToServer"
local LEGACY_TO_CLIENT = "Bg3mseIgtLegacyToClient"
local LEGACY_BROADCAST = "Bg3mseIgtLegacyBroadcast"

local received = {}
local channel = nil
local pendingPong = nil
-- The client's FromServer case, called on each arrival while it waits.
local onReceived = nil

-- The sends of the server's cases; the client's FromServer case has them
-- made again, in its own launch.
local function SendAll(C, E, user)
    local _, guid = C.character()
    E.Net.BroadcastMessage(LEGACY_BROADCAST, "broadcast")
    E.Net.PostMessageToClient(guid, LEGACY_TO_CLIENT, "toClient")
    E.Net.PostMessageToUser(user, LEGACY_TO_CLIENT, "toUser")
    channel:SendToClient({ Kind = "toClient" }, guid)
    channel:Broadcast({ Kind = "broadcast" })
end

function library.bootstrap(C, E, M)
    if E.IsServer() then
        channel = M.Net.CreateChannel(C.ModuleUUID, "Probe",
            function (msg, user) received[#received + 1] = msg end,
            function (request, user)
                if request.SendAll then
                    local ok, err = pcall(SendAll, C, E, user)
                    return { Sent = ok, Error = (not ok) and tostring(err) or nil }
                end
                return { Echo = request.Value, User = user }
            end)
        E.RegisterNetListener(LEGACY_TO_SERVER, function (ch, payload, user)
            E.Net.PostMessageToUser(user, LEGACY_TO_CLIENT, "pong:" .. tostring(payload))
        end)
    else
        local function Record(what)
            received[#received + 1] = what
            if onReceived ~= nil then onReceived() end
        end
        channel = M.Net.CreateChannel(C.ModuleUUID, "Probe",
            function (msg) Record("module:" .. tostring(msg.Kind)) end)
        local function Legacy(ch, payload)
            if type(payload) == "string" and payload:sub(1, 5) == "pong:" and pendingPong ~= nil then
                local done = pendingPong
                pendingPong = nil
                done(payload == "pong:ping", payload)
            else
                Record("legacy:" .. ch .. ":" .. tostring(payload))
            end
        end
        E.RegisterNetListener(LEGACY_TO_CLIENT, Legacy)
        E.RegisterNetListener(LEGACY_BROADCAST, Legacy)
    end
end

function library.define(C)
    local function NeedChannel()
        if channel == nil then C.skip("the bootstrap made no channel (no trigger file)") end
    end
    local function UserId()
        local character = C.character()
        local reserved = character.UserReservedFor
        if reserved == nil then C.skip("the character has no UserReservedFor") end
        return reserved.UserID
    end
    return {
        -- Server: sends.
        { "Ext.Net.BroadcastMessage", id = "NET-12", ctx = "server", run = function (E)
            E.Net.BroadcastMessage(LEGACY_BROADCAST, "broadcast")
            return "sent; arrival is the client's Ext.Net#FromServer"
        end },
        { "Ext.Net.PostMessageToClient", id = "NET-12", ctx = "server", run = function (E)
            local _, guid = C.character()
            E.Net.PostMessageToClient(guid, LEGACY_TO_CLIENT, "toClient")
            return "sent; arrival is the client's Ext.Net#FromServer"
        end },
        { "Ext.Net.PostMessageToUser", id = "NET-12", ctx = "server", run = function (E)
            E.Net.PostMessageToUser(UserId(), LEGACY_TO_CLIENT, "toUser")
            return "sent; arrival is the client's Ext.Net#FromServer"
        end },
        { "Ext.Net.PostMessageToClient#module", id = "NET-02", ctx = "server", run = function (E)
            NeedChannel()
            local _, guid = C.character()
            channel:SendToClient({ Kind = "toClient" }, guid)
            channel:Broadcast({ Kind = "broadcast" })
            return "sent; arrival is the client's Ext.Net#FromServer"
        end },
        { "Ext.Net.PlayerHasExtender", id = "NET-16", ctx = "server", run = function (E)
            local _, guid = C.character()
            C.equal(E.Net.PlayerHasExtender(guid), true, "PlayerHasExtender(host)")
        end },
        { "Ext.Net.IsHost", id = "NET-15", run = function (E)
            C.equal(E.Net.IsHost(), true, "IsHost in single player")
        end },
        { "Ext.Net.Version", id = "NET-15", run = function (E)
            local v = E.Net.Version()
            C.check(math.type(v) == "integer" and v >= 1, "Version() gave %s (0 on the client: no extender hello, NET-08)", tostring(v))
            return tostring(v)
        end },

        -- Client: what the server sent, and requests to it.
        { "Ext.Net#FromServer", id = "NET-03", ctx = "client", async = true, run = function (E, M, done)
            NeedChannel()
            local want = { "legacy:" .. LEGACY_BROADCAST .. ":broadcast", "legacy:" .. LEGACY_TO_CLIENT .. ":toClient",
                "legacy:" .. LEGACY_TO_CLIENT .. ":toUser", "module:toClient", "module:broadcast" }
            -- Only what arrives from now on: the server sends again for this case.
            received = {}
            local function Missing()
                local have = {}
                for _, r in ipairs(received) do have[r] = true end
                local missing = {}
                for _, w in ipairs(want) do if not have[w] then missing[#missing + 1] = w end end
                return missing
            end
            onReceived = function ()
                if #Missing() == 0 then
                    onReceived = nil
                    done(true, table.concat(received, ", "))
                end
            end
            channel:RequestToServer({ SendAll = true }, function (reply)
                if type(reply) ~= "table" or reply.Sent ~= true then
                    onReceived = nil
                    done(false, "the server did not send: " .. E.Json.Stringify(reply, { Beautify = false }))
                end
            end)
        end },
        { "Ext.Net.PostMessageToServer", id = "NET-03", ctx = "client", async = true, run = function (E, M, done)
            pendingPong = done
            E.Net.PostMessageToServer(LEGACY_TO_SERVER, "ping")
        end },
        { "Ext.Net.PostMessageToServer#module", id = "NET-19", ctx = "client", async = true, run = function (E, M, done)
            NeedChannel()
            channel:RequestToServer({ Value = 5 }, function (reply)
                done(type(reply) == "table" and reply.Echo == 5, "reply " .. E.Json.Stringify(reply, { Beautify = false }))
            end)
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
