-- The two-machine cases: one machine hosts a multiplayer game, the other
-- joins it; one of them may be a Windows PC with Norbyte's own extender, so
-- nothing here is particular to macOS, and no case assumes which side it is
-- on: it asks (Ext.Net.IsHost) and reports. Opt-in ("Extras":
-- ["Multiplayer"]); the run sheet adds "Only": ["MP."] so that nothing else
-- runs. Names start with "MP."; the checker lines both machines' logs up by
-- them.
--
-- How the sides find each other: each client asks the server ("Hello") on
-- this mod's channel, every few seconds for as long as its Lua state lives,
-- so that a state made by a session that was loaded again finds the others
-- too; the server keeps every client's answer by the user id its handler is
-- given (Lua/Libs/NetChannel.lua: handlers get (msg, user)). The server's
-- cases wait until both the host's client and a client of another machine
-- have said hello, then work against the remote one. The receiving side
-- checks what arrived and answers with an "Ack", which the sending side's
-- case reports.
--
-- The user id a handler is given names the sender's peer, not always its
-- user: a message that came over the network carries the peer alone
-- (peer << 16, as ServerNetworking.inl takes it from the message's context),
-- one from the host's own client its user (the local queue, ClientNet.inl:
-- GameServer::GetLocalUserId). Norbyte's server functions that take a
-- character work on the peer of its user in the same way (ServerNet.inl:
-- PlayerHasExtender, BroadcastMessage's exclusion). So a user's character is
-- looked up by peer among the players, and when a case needs it, not at the
-- hello: the players get their characters some time after the session runs.
--
-- Norbyte's rules the cases check (Lua/Libs/Net.inl, NetChannel.lua,
-- Json binary): a channel payload goes through Ext.Json with Binary when both
-- sides speak protocol 2, so floats become float32 and every key of a table
-- that is not an array becomes a string; a payload whose serialized form is
-- 0xff000 bytes or more is not sent (an error in the sender's log, no Lua
-- error); a request whose handler fails gets no reply; Sync sends no stat to
-- another machine (Stats.inl, Stats.cpp: it refreshes this process's
-- prototypes, and ExtenderProtocol.proto has no message for a stat), while
-- the host's own client reads the server's stats: one process has one stats
-- manager, and what was created in it stays for as long as the process does.

local library = { name = "Multiplayer" }

local CHANNEL = "MP"
local LEGACY = "Bg3mseIgtMPLegacy"
local USER_VAR = "Bg3mseIgtMPUser"
local MOD_VAR = "Bg3mseIgtMPMod"
-- The way back has variables of its own: the cases of both ways run at the
-- same time, and one must not overwrite what the other waits for.
local USER_BACK = "Bg3mseIgtMPUserBack"
local MOD_BACK = "Bg3mseIgtMPModBack"
local STAT = "Bg3mseIgt_MPStat"
local LIMIT = 0xff000
local WAIT_PEERS_MS = 15 * 60 * 1000
local WAIT_MS = 10 * 60 * 1000
-- How long a server case waits for the hellos in a Lua state that the
-- hellos' own case did not run in, and for the remote player's character.
local REMOTE_WAIT_MS = 60 * 1000
local HELLO_EVERY_MS = 3000

local channel = nil
local inbox = {}          -- by Kind: list of { msg, user }
local legacy = {}         -- the legacy payloads that arrived
local peers = {}          -- server: by user id, what each client said
local lastRemote = nil    -- server: the user id of the last hello of another machine's client
local noRemote = false    -- server: the wait for the hellos ended without one
local me = nil            -- client: the server's answer to Hello
local broadcasts = {}     -- client: the ids of the broadcasts that arrived

-- The payload of every Lua type a message can carry, and what must come back.
local function Deep(n)
    local t = { Level = n }
    if n > 1 then t.Inner = Deep(n - 1) end
    return t
end
local function TypesPayload()
    return {
        Kind = "Types",
        True = true, False = false,
        Int = 1234567890123, MaxInt = math.maxinteger, MinInt = math.mininteger, Zero = 0,
        Half = 0.5, Tenth = 0.1,
        Str = "bg3mse", Empty = "", Utf8 = "d\u{e9}j\u{e0} vu \u{65e5}\u{672c}", Bytes = "\0\1\127\128\255",
        Array = { 1, 2, 3 }, Map = { a = 1, b = { c = "d" } }, EmptyTable = {}, Deep = Deep(20),
    }
end

-- What differs from TypesPayload in what arrived; empty when nothing does.
local function TypesDiff(got)
    local want = TypesPayload()
    local diffs = {}
    local function Diff(what, ok, value)
        if not ok then diffs[#diffs + 1] = what .. "=" .. tostring(value) end
    end
    if type(got) ~= "table" then return { "not a table: " .. tostring(got) } end
    for _, k in ipairs({ "True", "False", "Int", "MaxInt", "MinInt", "Zero", "Str", "Empty", "Utf8", "Bytes" }) do
        Diff(k, got[k] == want[k] and math.type(got[k]) == math.type(want[k]), got[k])
    end
    Diff("Half", got.Half == 0.5, got.Half)
    Diff("Tenth", type(got.Tenth) == "number" and math.abs(got.Tenth - 0.1) < 1e-7, got.Tenth)
    Diff("Array", type(got.Array) == "table" and got.Array[1] == 1 and got.Array[2] == 2 and got.Array[3] == 3 and #got.Array == 3,
        type(got.Array) == "table" and #got.Array or got.Array)
    Diff("Map", type(got.Map) == "table" and got.Map.a == 1 and type(got.Map.b) == "table" and got.Map.b.c == "d", got.Map)
    Diff("EmptyTable", type(got.EmptyTable) == "table" and next(got.EmptyTable) == nil, got.EmptyTable)
    local d, depth = got.Deep, 0
    while type(d) == "table" do depth = depth + 1; d = d.Inner end
    Diff("Deep", depth == 20, depth)
    Diff("Nil", got.Nil == nil, got.Nil)
    return diffs
end

-- A string of a given length and its checksum (both ends compute it).
local function Big(length)
    return string.rep("bg3mse-0123456789", length // 17 + 1):sub(1, length)
end
local function Checksum(s)
    local a, b = 1, 0
    for i = 1, #s, 4096 do
        local bytes = { s:byte(i, math.min(i + 4095, #s)) }
        for j = 1, #bytes do
            a = (a + bytes[j]) % 65521
            b = (b + a) % 65521
        end
    end
    return b * 65536 + a
end
-- Close to the limit: the message table's own serialized bytes are well under 1 KiB.
local LARGE = LIMIT - 1024

local function Record(kind, msg, user)
    inbox[kind] = inbox[kind] or {}
    table.insert(inbox[kind], { msg = msg, user = user })
end

-- A user id's peer. Ids come as written, signed or not (LuaPush.h, LuaGet.h).
local function Peer(id)
    return (id & 0xffffffff) >> 16
end

-- Server: the players' characters (the story's DB_Players) with the user of
-- each: the one its character is played by (esv::Character::UserID, which
-- Norbyte's server functions read) and the one it is reserved for.
local function Players(E)
    local list = {}
    local ok, rows = pcall(function () return Osi.DB_Players:Get(nil) end)
    for _, row in ipairs(ok and rows or {}) do
        local p = { Guid = row[1] }
        local okE, entity = pcall(E.Entity.Get, row[1])
        if okE and entity ~= nil then
            pcall(function () p.Uuid = entity.Uuid.EntityUuid end)
            pcall(function () p.Owner = entity.ServerCharacter.UserID end)
            pcall(function () p.Reserved = entity.UserReservedFor.UserID end)
        end
        list[#list + 1] = p
    end
    return list
end

local function DescribePlayers(E)
    local parts = {}
    for _, p in ipairs(Players(E)) do
        parts[#parts + 1] = string.format("%s played by user %s, reserved for user %s", tostring(p.Uuid or p.Guid),
            tostring(p.Owner), tostring(p.Reserved))
    end
    if #parts == 0 then return "no player in DB_Players" end
    return table.concat(parts, "; ")
end

-- Server: the character of a user id as a handler is given it: the player
-- with that user, or else one of that user's peer.
local function CharacterOf(E, user)
    if type(user) ~= "number" or Peer(user) == 0xffff then return nil end
    local players = Players(E)
    for _, field in ipairs({ "Owner", "Reserved" }) do
        for _, exact in ipairs({ true, false }) do
            for _, p in ipairs(players) do
                local id = p[field]
                if type(id) == "number" and p.Uuid ~= nil and Peer(id) ~= 0xffff and Peer(id) == Peer(user)
                    and (not exact or (id & 0xffff) == (user & 0xffff)) then
                    return p.Uuid
                end
            end
        end
    end
    return nil
end

function library.bootstrap(C, E, M)
    if not C.optedIn("Multiplayer") then return end
    local flags = { Server = true, Client = true, SyncToClient = true, SyncToServer = true,
        WriteableOnServer = true, WriteableOnClient = true, Persistent = false, SyncOnTick = true }
    E.Vars.RegisterUserVariable(USER_VAR, flags)
    E.Vars.RegisterModVariable(C.ModuleUUID, MOD_VAR, flags)
    E.Vars.RegisterUserVariable(USER_BACK, flags)
    E.Vars.RegisterModVariable(C.ModuleUUID, MOD_BACK, flags)

    if E.IsServer() then
        channel = M.Net.CreateChannel(C.ModuleUUID, CHANNEL, function (msg, user)
            Record(msg.Kind, msg, user)
        end, function (msg, user)
            if msg.Kind == "Hello" then
                peers[user] = { Version = msg.Version, IsHost = msg.IsHost, NetVersion = msg.NetVersion }
                if msg.IsHost == false then lastRemote = user end
                return { User = user, Version = E.Utils.Version(), NetVersion = E.Net.Version() }
            elseif msg.Kind == "Echo" then
                return { Echo = msg.Value }
            elseif msg.Kind == "Fail" then
                error("bg3mse in-game test: this request fails on purpose")
            end
        end)
        E.RegisterNetListener(LEGACY, function (_, payload, user) legacy[#legacy + 1] = tostring(payload) .. "@" .. tostring(user) end)
    else
        channel = M.Net.CreateChannel(C.ModuleUUID, CHANNEL, function (msg, user)
            if msg.Kind == "Bcast" then broadcasts[#broadcasts + 1] = msg.Id end
            Record(msg.Kind, msg, user)
        end, function (msg)
            if msg.Kind == "Echo" then
                return { Echo = msg.Value }
            elseif msg.Kind == "Broadcasts" then
                return { Got = broadcasts, Legacy = legacy }
            elseif msg.Kind == "ReadVars" then
                local entity = msg.Character and E.Entity.Get(msg.Character)
                local vars = E.Vars.GetModVariables(C.ModuleUUID)
                return { User = entity and entity.Vars[USER_VAR], Mod = vars and vars[MOD_VAR] }
            elseif msg.Kind == "WriteVars" then
                local entity = msg.Character and E.Entity.Get(msg.Character)
                if entity == nil then
                    return { Written = false, Why = "no entity on this client for the character " .. tostring(msg.Character) }
                end
                entity.Vars[USER_BACK] = msg.User
                E.Vars.GetModVariables(C.ModuleUUID)[MOD_BACK] = msg.Mod
                E.Vars.SyncUserVariables()
                E.Vars.SyncModVariables()
                return { Written = true }
            elseif msg.Kind == "Stat" then
                local stat = E.Stats.Get(msg.Name, nil, false)
                return { Exists = stat ~= nil }
            end
        end)
        E.RegisterNetListener(LEGACY, function (_, payload) legacy[#legacy + 1] = tostring(payload) end)

        -- The hello, for as long as this state lives: the server's state may
        -- be newer than this one (a Lua reset), and a hello sent before the
        -- server listens is lost.
        local last, running = nil, false
        local function IsRunning(state)
            return state == "Running" or tostring(state) == "Running"
        end
        E.Events.GameStateChanged:Subscribe(function (e) running = IsRunning(e.ToState) end)
        E.Events.Tick:Subscribe(function ()
            local now = E.Timer.MonotonicTime()
            if last ~= nil and now - last < HELLO_EVERY_MS then return end
            -- A state made by a Lua reset sees no change of the game's state.
            if not running then
                local ok, state = pcall(E.Utils.GetGameState)
                running = ok and IsRunning(state)
                if not running then return end
            end
            last = now
            pcall(function ()
                channel:RequestToServer({ Kind = "Hello", Version = E.Utils.Version(), IsHost = E.Net.IsHost(), NetVersion = E.Net.Version() },
                    function (reply)
                        if type(reply) == "table" and reply.User ~= nil then me = reply end
                    end)
            end)
        end)
    end
end

function library.define(C)
    -- Calls check every tick until it gives true or false (with a detail),
    -- or the time is up.
    local function Await(E, ms, check, finish, what)
        local started = E.Timer.MonotonicTime()
        local sub
        sub = E.Events.Tick:Subscribe(function ()
            local ok, result, detail = pcall(check)
            if not ok then
                E.Events.Tick:Unsubscribe(sub)
                finish(false, result)
            elseif result ~= nil then
                E.Events.Tick:Unsubscribe(sub)
                finish(result, detail)
            elseif E.Timer.MonotonicTime() - started > ms then
                E.Events.Tick:Unsubscribe(sub)
                finish(false, "nothing within " .. (ms // 1000) .. " s: " .. tostring(what))
            end
        end)
    end
    local function NeedChannel()
        if channel == nil then C.skip("the bootstrap made no channel (the trigger's Extras has no \"Multiplayer\")") end
    end
    -- Server: the host's own client.
    local function HostClient()
        for user, p in pairs(peers) do if p.IsHost == true then return user, p end end
        return nil
    end
    -- A step of a case that runs later, on a tick or in a reply: what it
    -- raises ends the case (a skip as a skip).
    local function Step(done, fn, ...)
        local ok, err = pcall(fn, ...)
        if not ok then done(false, err) end
    end
    -- Server: calls fn(remoteUser, hostUser, character) once a client of
    -- another machine has said hello to this Lua state (and the host's own,
    -- for as long as the wait lasts), with the remote player's character
    -- when the case needs it. The hellos' own case has waited the long wait;
    -- a state made after it (the session was loaded again) hears the clients
    -- within seconds.
    local function WithRemote(E, done, needCharacter, fn)
        if channel == nil then
            done("skip", "the bootstrap made no channel (the trigger's Extras has no \"Multiplayer\")")
            return
        end
        if noRemote then
            done("skip", "no client of another machine said hello")
            return
        end
        local started = E.Timer.MonotonicTime()
        local sub
        sub = E.Events.Tick:Subscribe(function ()
            local late = E.Timer.MonotonicTime() - started > REMOTE_WAIT_MS
            local remote, host = lastRemote, HostClient()
            local character = nil
            if remote ~= nil and needCharacter then
                local ok, found = pcall(CharacterOf, E, remote)
                if ok then character = found end
            end
            local ready = remote ~= nil and (host ~= nil or late) and (character ~= nil or not needCharacter)
            if not ready and not late then return end
            E.Events.Tick:Unsubscribe(sub)
            if remote == nil then
                done("skip", "no client of another machine said hello")
            elseif not ready then
                done(false, string.format("no player's character is of user %d's peer (%d) after %d s: %s", remote, Peer(remote),
                    REMOTE_WAIT_MS // 1000, DescribePlayers(E)))
            else
                Step(done, fn, remote, host, character)
            end
        end)
    end
    local function Ack(E, of)
        return function ()
            local list = inbox.Ack or {}
            for _, a in ipairs(list) do
                if a.msg.Of == of then return a.msg.Ok == true, a.msg.Detail end
            end
        end
    end
    -- Client: whether this is the client of the machine that joined.
    local function NeedRemote(E)
        if E.Net.IsHost() then C.elsewhere("the host's client: the network cases are the other machine's client") end
        if me == nil then C.skip("no answer to Hello from the server") end
    end
    local function server(name, run, extra)
        local c = { "MP." .. name, id = "NET-20", ctx = "server", optIn = "Multiplayer", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    local function client(name, run, extra)
        local c = { "MP." .. name, id = "NET-20", ctx = "client", optIn = "Multiplayer", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    local ASYNC = { async = true, timeout = WAIT_MS }

    -- The receiving side of a channel message: wait for it, check it, and
    -- answer the sender with an Ack.
    local function Receive(E, done, kind, check, reply)
        Await(E, WAIT_MS, function ()
            local list = inbox[kind]
            if list == nil or #list == 0 then return nil end
            local ok, detail = check(list)
            reply({ Kind = "Ack", Of = kind, Ok = ok, Detail = detail }, list[1].user)
            return ok, detail
        end, done, "a " .. kind .. " message")
    end
    local function CheckTypes(list)
        local diffs = TypesDiff(list[1].msg)
        return #diffs == 0, #diffs == 0 and ("every value as sent (Tenth " .. string.format("%.17g", list[1].msg.Tenth) .. ")")
            or ("differs: " .. table.concat(diffs, ", "))
    end
    local function CheckLarge(list)
        local msg = list[1].msg
        local ok = type(msg.Data) == "string" and #msg.Data == msg.Length and Checksum(msg.Data) == msg.Checksum
        return ok, string.format("%s bytes, checksum %s (sent %s, %s)", tostring(msg.Data and #msg.Data), tostring(msg.Data and Checksum(msg.Data)),
            tostring(msg.Length), tostring(msg.Checksum))
    end
    local function CheckOverLimit(list, E)
        local over = inbox.OverLimit
        if over ~= nil and #over > 0 then return false, "the message over the limit arrived" end
        return true, "the message after it arrived, the one over the limit did not"
    end
    local function SendLarge(send)
        local data = Big(LARGE)
        send({ Kind = "Large", Data = data, Length = #data, Checksum = Checksum(data) })
    end
    local function SendOverLimit(send)
        send({ Kind = "OverLimit", Data = Big(LIMIT + 64) })
        send({ Kind = "AfterOverLimit" })
    end
    -- Server: answers the client that sent the message.
    local function ToSender(ack, user) channel:SendToClient(ack, user) end

    return {
        -- --- Both sides: who is who ---------------------------------------------
        server("Server#peers", function (E, M, done)
            NeedChannel()
            local heard = nil
            Await(E, WAIT_PEERS_MS - 2000, function ()
                local host = HostClient()
                if lastRemote == nil or host == nil then return nil end
                -- The players' characters, when they are there within a while.
                local now = E.Timer.MonotonicTime()
                heard = heard or now
                if now - heard < REMOTE_WAIT_MS and (CharacterOf(E, lastRemote) == nil or CharacterOf(E, host) == nil) then
                    return nil
                end
                local parts = {}
                for user, p in pairs(peers) do
                    parts[#parts + 1] = string.format("user %d: %s client, extender %s, protocol %s, character %s", user,
                        p.IsHost and "the host's" or "another machine's", tostring(p.Version), tostring(p.NetVersion),
                        tostring(CharacterOf(E, user)))
                end
                table.sort(parts)
                return true, table.concat(parts, "; ") .. "; players: " .. DescribePlayers(E)
            end, function (ok, detail)
                if not ok then noRemote = lastRemote == nil end
                done(ok, detail)
            end, "the hellos of the host's client and of another machine's client")
        end, { async = true, timeout = WAIT_PEERS_MS, exclusive = true }),
        client("Client#hello", function (E, M, done)
            NeedChannel()
            Await(E, WAIT_PEERS_MS - 2000, function ()
                if me == nil then return nil end
                -- The user id is the one the server's handler was given.
                return true, string.format(
                    "this is %s; the server heard user %s; this side's extender %s, the server's %s; protocol %s here, %s there",
                    E.Net.IsHost() and "the host's client" or "another machine's client", tostring(me.User),
                    tostring(E.Utils.Version()), tostring(me.Version), tostring(E.Net.Version()), tostring(me.NetVersion))
            end, done, "the server's answer to this client's hello")
        end, { async = true, timeout = WAIT_PEERS_MS, exclusive = true }),

        server("Ext.Net.IsHost#server", function (E)
            C.equal(E.Net.IsHost(), true, "IsHost on the server")
        end),
        client("Ext.Net.IsHost#client", function (E)
            local host = C.type(E.Net.IsHost(), "boolean", "IsHost")
            return host and "true: the host's client" or "false: another machine's client"
        end),
        server("Ext.Net.Version#server", function (E)
            local v = E.Net.Version()
            C.equal(v, 2, "the lowest protocol version over the clients")
        end),
        client("Ext.Net.Version#client", function (E)
            C.equal(E.Net.Version(), 2, "the protocol version agreed with the server")
        end),
        server("Ext.Utils.Version#peers", function (E, M, done)
            WithRemote(E, done, false, function ()
                local parts = { "server " .. tostring(E.Utils.Version()) }
                local ok = E.Utils.Version() >= 33
                local users = {}
                for user in pairs(peers) do users[#users + 1] = user end
                table.sort(users)
                for _, user in ipairs(users) do
                    local p = peers[user]
                    parts[#parts + 1] = "user " .. user .. " " .. tostring(p.Version)
                    ok = ok and type(p.Version) == "number" and p.Version >= 33
                end
                C.check(ok, "an extender older than v33: %s", table.concat(parts, ", "))
                done(true, table.concat(parts, ", "))
            end)
        end, ASYNC),
        server("Ext.Net.PlayerHasExtender#remote", function (E, M, done)
            WithRemote(E, done, true, function (user, _, character)
                C.equal(E.Net.PlayerHasExtender(character), true, "PlayerHasExtender(the remote player)")
                done(true, string.format("%s, the character of user %d's peer", character, user))
            end)
        end, ASYNC),

        -- --- Channel messages, each way --------------------------------------------
        server("Channel.SendToClient#types", function (E, M, done)
            WithRemote(E, done, false, function (user)
                channel:SendToClient(TypesPayload(), user)
                Await(E, WAIT_MS, Ack(E, "Types"), done, "the remote client's Ack")
            end)
        end, ASYNC),
        client("Channel.FromServer#types", function (E, M, done)
            NeedRemote(E)
            Receive(E, done, "Types", CheckTypes, function (ack) channel:SendToServer(ack) end)
        end, ASYNC),
        server("Channel.SendToClient#large", function (E, M, done)
            WithRemote(E, done, false, function (user)
                SendLarge(function (m) channel:SendToClient(m, user) end)
                Await(E, WAIT_MS, Ack(E, "Large"), done, "the remote client's Ack")
            end)
        end, ASYNC),
        client("Channel.FromServer#large", function (E, M, done)
            NeedRemote(E)
            Receive(E, done, "Large", CheckLarge, function (ack) channel:SendToServer(ack) end)
        end, ASYNC),
        server("Channel.SendToClient#overLimit", function (E, M, done)
            WithRemote(E, done, false, function (user)
                SendOverLimit(function (m) channel:SendToClient(m, user) end)
                Await(E, WAIT_MS, Ack(E, "AfterOverLimit"), done, "the remote client's Ack")
            end)
        end, ASYNC),
        client("Channel.FromServer#overLimit", function (E, M, done)
            NeedRemote(E)
            Receive(E, done, "AfterOverLimit", CheckOverLimit, function (ack) channel:SendToServer(ack) end)
        end, ASYNC),

        client("Channel.SendToServer#types", function (E, M, done)
            NeedRemote(E)
            channel:SendToServer(TypesPayload())
            Await(E, WAIT_MS, Ack(E, "Types"), done, "the server's Ack")
        end, ASYNC),
        server("Channel.FromClient#types", function (E, M, done)
            NeedChannel()
            Receive(E, done, "Types", CheckTypes, ToSender)
        end, ASYNC),
        client("Channel.SendToServer#large", function (E, M, done)
            NeedRemote(E)
            SendLarge(function (m) channel:SendToServer(m) end)
            Await(E, WAIT_MS, Ack(E, "Large"), done, "the server's Ack")
        end, ASYNC),
        server("Channel.FromClient#large", function (E, M, done)
            NeedChannel()
            Receive(E, done, "Large", CheckLarge, ToSender)
        end, ASYNC),
        client("Channel.SendToServer#overLimit", function (E, M, done)
            NeedRemote(E)
            SendOverLimit(function (m) channel:SendToServer(m) end)
            Await(E, WAIT_MS, Ack(E, "AfterOverLimit"), done, "the server's Ack")
        end, ASYNC),
        server("Channel.FromClient#overLimit", function (E, M, done)
            NeedChannel()
            Receive(E, done, "AfterOverLimit", CheckOverLimit, ToSender)
        end, ASYNC),

        -- --- Requests and replies ---------------------------------------------------
        server("Channel.RequestToClient#echo", function (E, M, done)
            WithRemote(E, done, false, function (user)
                channel:RequestToClient({ Kind = "Echo", Value = TypesPayload() }, user, function (reply)
                    local diffs = TypesDiff(reply and reply.Echo)
                    done(#diffs == 0, #diffs == 0 and "the remote client's reply as sent" or table.concat(diffs, ", "))
                end)
            end)
        end, ASYNC),
        client("Channel.RequestToServer#echo", function (E, M, done)
            NeedChannel()
            channel:RequestToServer({ Kind = "Echo", Value = TypesPayload() }, function (reply)
                local diffs = TypesDiff(reply and reply.Echo)
                done(#diffs == 0, #diffs == 0 and "the server's reply as sent" or table.concat(diffs, ", "))
            end)
        end, ASYNC),
        client("Channel.RequestToServer#fails", function (E, M, done)
            NeedChannel()
            local replied = false
            channel:RequestToServer({ Kind = "Fail" }, function () replied = true end)
            E.Timer.WaitForRealtime(5000, function ()
                done(not replied, replied and "a reply came to a request whose handler failed" or "no reply in 5 s, as for a failed handler")
            end)
        end, ASYNC),

        -- --- Broadcasts and the legacy messages ----------------------------------------
        server("Channel.Broadcast#exclude", function (E, M, done)
            WithRemote(E, done, true, function (remoteUser, hostUser, character)
                channel:Broadcast({ Kind = "Bcast", Id = 1 })
                channel:Broadcast({ Kind = "Bcast", Id = 2 }, character)
                E.Net.BroadcastMessage(LEGACY, "broadcast-all")
                E.Net.BroadcastMessage(LEGACY, "broadcast-not-remote", character)
                E.Net.PostMessageToUser(remoteUser, LEGACY, "toUser")
                E.Net.PostMessageToClient(character, LEGACY, "toClient")
                E.Timer.WaitForRealtime(3000, function ()
                    local answers, pending = {}, 0
                    local function Ask(user, label)
                        pending = pending + 1
                        channel:RequestToClient({ Kind = "Broadcasts" }, user, function (reply)
                            answers[label] = reply
                            pending = pending - 1
                            if pending > 0 then return end
                            local function Ids(r) return table.concat((r and r.Got) or {}, ",") end
                            local function Legacy(r) local l = (r and r.Legacy) or {}; table.sort(l); return table.concat(l, ",") end
                            local detail = string.format("remote got %s (legacy %s); host's client got %s (legacy %s)",
                                Ids(answers.remote), Legacy(answers.remote), Ids(answers.host), Legacy(answers.host))
                            local ok = Ids(answers.remote) == "1"
                                and Legacy(answers.remote) == "broadcast-all,toClient,toUser"
                                and (answers.host == nil or (Ids(answers.host) == "1,2"
                                    and Legacy(answers.host) == "broadcast-all,broadcast-not-remote"))
                            done(ok, detail)
                        end)
                    end
                    Step(done, function ()
                        Ask(remoteUser, "remote")
                        if hostUser ~= nil then Ask(hostUser, "host") end
                    end)
                end)
            end)
        end, ASYNC),

        -- --- User and mod variables, each way --------------------------------------------
        server("Vars.User#serverToClient", function (E, M, done)
            WithRemote(E, done, true, function (user, _, character)
                local entity = C.notNil(E.Entity.Get(character), "the remote user's character " .. character)
                local value = "server-" .. tostring(E.Timer.MonotonicTime())
                entity.Vars[USER_VAR] = value
                E.Vars.SyncUserVariables()
                local ticks = 0
                Await(E, 20000, function ()
                    ticks = ticks + 1
                    if inbox.UserVarSeen and inbox.UserVarSeen.value == value then return true, "the remote client read " .. value end
                    if ticks % 15 ~= 1 then return nil end
                    channel:RequestToClient({ Kind = "ReadVars", Character = character }, user, function (reply)
                        if reply and reply.User == value then inbox.UserVarSeen = { value = value } end
                    end)
                end, done, "the remote client reading " .. value)
            end)
        end, ASYNC),
        server("Vars.Mod#serverToClient", function (E, M, done)
            WithRemote(E, done, false, function (user)
                local value = "server-" .. tostring(E.Timer.MonotonicTime())
                E.Vars.GetModVariables(C.ModuleUUID)[MOD_VAR] = value
                E.Vars.SyncModVariables()
                local ticks = 0
                Await(E, 20000, function ()
                    ticks = ticks + 1
                    if inbox.ModVarSeen and inbox.ModVarSeen.value == value then return true, "the remote client read " .. value end
                    if ticks % 15 ~= 1 then return nil end
                    channel:RequestToClient({ Kind = "ReadVars" }, user, function (reply)
                        if reply and reply.Mod == value then inbox.ModVarSeen = { value = value } end
                    end)
                end, done, "the remote client reading " .. value)
            end)
        end, ASYNC),
        server("Vars#clientToServer", function (E, M, done)
            WithRemote(E, done, true, function (user, _, character)
                local entity = C.notNil(E.Entity.Get(character), "the remote user's character " .. character)
                local userValue, modValue = "client-user-" .. user, "client-mod-" .. user
                channel:RequestToClient({ Kind = "WriteVars", Character = character, User = userValue, Mod = modValue }, user, function (reply)
                    if not (reply and reply.Written) then
                        done(false, "the remote client did not write: " .. tostring(reply and reply.Why))
                        return
                    end
                    Await(E, 20000, function ()
                        local u = entity.Vars[USER_BACK]
                        local m = E.Vars.GetModVariables(C.ModuleUUID)[MOD_BACK]
                        if u == userValue and m == modValue then return true, "the server read both" end
                    end, done, "the server reading " .. userValue .. " and " .. modValue)
                end)
            end)
        end, ASYNC),

        -- --- A stat created on the host -------------------------------------------------
        -- Under a name no process has seen: a machine that hosted before
        -- still has the stats it made then. Each client is asked before the
        -- stat is created and after its Sync.
        server("Stats.Sync#remote", function (E, M, done)
            WithRemote(E, done, false, function (user, hostUser)
                local name = string.format("%s_%d_%d", STAT, E.Timer.ClockEpoch(), E.Timer.MonotonicTime() % 1000000)
                local function AskBoth(after)
                    local answers, pending = {}, hostUser and 2 or 1
                    local function Ask(u, label)
                        channel:RequestToClient({ Kind = "Stat", Name = name }, u, function (reply)
                            answers[label] = reply and reply.Exists
                            pending = pending - 1
                            if pending == 0 then Step(done, after, answers) end
                        end)
                    end
                    Ask(user, "remote")
                    if hostUser then Ask(hostUser, "host") end
                end
                AskBoth(function (before)
                    C.equal(E.Stats.Get(name, nil, false), nil, "the stat " .. name .. " before it is created")
                    local stat = C.notNil(E.Stats.Create(name, "SpellData", "Target_MainHandAttack"), "Create")
                    stat:Sync()
                    E.Timer.WaitForRealtime(2000, function ()
                        Step(done, AskBoth, function (after)
                            local detail = string.format("%s: before Create the host's client sees it: %s, the remote client: %s; "
                                .. "after Sync the host's client: %s, the remote client: %s", name, tostring(before.host),
                                tostring(before.remote), tostring(after.host), tostring(after.remote))
                            -- The host's client shares the server's stats;
                            -- nothing takes the stat to another machine.
                            local ok = before.remote == false and after.remote == false
                                and (hostUser == nil or (before.host == false and after.host == true))
                            done(ok, detail)
                        end)
                    end)
                end)
            end)
        end, ASYNC),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
