-- The module load's Lua state (ExtensionStateBase::LuaLoadPreinitBootstrap):
-- the client's state of ExtensionStateContext::Load, made when the game loads
-- its stats (ScriptExtender::OnStatsLoadGuarded). The events of that load
-- are thrown here and in no state the runner lives in: ModuleLoadStarted
-- right after this bootstrap, StatsStructureLoaded while the stats are
-- parsed (Hooks::OnParseDataBuffers); StatsLoaded already in the next
-- (Game) state, where the runner sees it. What fires here is written down,
-- with the trigger's RunId, for the runner to report (Runner.lua,
-- ReportEvents). Nothing happens without the trigger file.

local TRIGGER = "bg3mse-ingame-tests.json"
local RECORD = "bg3mse-ingame-tests/load-events-"

local function Record()
    local ok, text = pcall(Ext.IO.LoadFile, TRIGGER)
    if not ok or type(text) ~= "string" then return end
    local okJ, trigger = pcall(Ext.Json.Parse, text)
    if not okJ or type(trigger) ~= "table" or trigger.Run ~= true then return end
    local runId = tostring(trigger.RunId or "1")
    local context = Ext.IsServer() and "server" or "client"
    local path = RECORD .. context .. ".json"

    local function Seen(name, detail)
        local record = nil
        local okL, old = pcall(Ext.IO.LoadFile, path)
        if okL and type(old) == "string" then
            local okP, value = pcall(Ext.Json.Parse, old)
            if okP and type(value) == "table" and value.RunId == runId and type(value.Seen) == "table" then
                record = value
            end
        end
        record = record or { RunId = runId, Seen = {} }
        if record.Seen[name] == nil then
            record.Seen[name] = detail
            pcall(Ext.IO.SaveFile, path, Ext.Json.Stringify(record))
        end
    end

    for _, name in ipairs({ "ModuleLoadStarted", "StatsStructureLoaded", "StatsLoaded" }) do
        pcall(function ()
            Ext.Events[name]:Subscribe(function ()
                Seen("Ext.Events." .. name, "the stats load of this launch")
            end)
        end)
    end
end

Record()
