-- Ext.Audio (BG3Extender/Lua/Libs/ClientAudio.inl), client only. Every call
-- names a switch, state, RTPC, event or bank that does not exist (prefix
-- Bg3mseIgt), so the Wwise manager is called through each virtual slot
-- (LC-07) and nothing changes what the game plays; the result is recorded.
-- PauseAllSounds and ResumeAllSounds run as a pair. The cases with a #name
-- pass one of the sound manager's own objects by name (LC-05: "Listener1",
-- "HUD1", read from ww::WwiseManager's arrays, whose macOS layout was fixed):
-- each reads the object's id and hands it to Wwise with a name that does not
-- exist; Stop on the HUD object ends at most a UI sound playing at that moment.
-- Those show only that the read does not fault. PostEvent#HUD1 tells a right
-- id from a wrong one: it posts a real event, the shortest UI sound of the
-- game's Sound resources, on the HUD object, which Wwise accepts only for an
-- object the game registered. That sound is heard once.

local library = { name = "Audio" }

function library.define(C)
    local NO = "Bg3mseIgtNoSuch"
    local function case(name, id, run, extra)
        local c = { "Ext.Audio." .. name, id = id, ctx = "client", risk = "unproved", run = run }
        for k, v in pairs(extra or {}) do c[k] = v end
        return c
    end
    local function Result(v) return "returned " .. tostring(v) end
    return {
        case("SetSwitch", "LC-01", function (E) return Result(E.Audio.SetSwitch("Global", NO .. "Group", NO .. "Switch")) end),
        case("SetState", "LC-01", function (E) return Result(E.Audio.SetState(NO .. "Group", NO .. "State")) end),
        case("SetRTPC", "LC-01", function (E) return Result(E.Audio.SetRTPC("Global", NO .. "Rtpc", 0.0)) end),
        case("GetRTPC", "LC-01", function (E)
            local v = E.Audio.GetRTPC("Global", NO .. "Rtpc")
            C.type(v, "number", "GetRTPC")
            return Result(v)
        end),
        case("ResetRTPC", "LC-01", function (E) E.Audio.ResetRTPC("Global", NO .. "Rtpc") end),
        case("Stop", "LC-01", function (E)
            -- The cinematic HUD object: silent outside cinematics.
            E.Audio.Stop("CineHUD")
        end),
        case("PauseAllSounds", "LC-01", function (E)
            E.Audio.PauseAllSounds()
            E.Audio.ResumeAllSounds()
        end),
        case("ResumeAllSounds", "LC-01", function (E)
            E.Audio.PauseAllSounds()
            E.Audio.ResumeAllSounds()
        end),
        case("PostEvent", "LC-01", function (E) return Result(E.Audio.PostEvent("Global", NO .. "Event")) end),
        case("LoadEvent", "LC-02", function (E) return Result(E.Audio.LoadEvent(NO .. "Event")) end),
        case("UnloadEvent", "LC-02", function (E) return Result(E.Audio.UnloadEvent(NO .. "Event")) end),
        case("PlayExternalSound", "LC-04", function (E)
            return Result(E.Audio.PlayExternalSound("Global", NO .. "Event", "Mods/Bg3mseInGameTests/Probe/" .. NO .. ".wem", 0))
        end),
        case("LoadBank", "LC-03", function (E) return Result(E.Audio.LoadBank(NO .. "Bank")) end),
        case("UnloadBank", "LC-02", function (E) return Result(E.Audio.UnloadBank(NO .. "Bank")) end),
        case("PrepareBank", "LC-03", function (E) return Result(E.Audio.PrepareBank(NO .. "Bank")) end),
        case("UnprepareBank", "LC-02", function (E) return Result(E.Audio.UnprepareBank(NO .. "Bank")) end),
        case("GetRTPC#Listener1", "LC-05", function (E)
            local v = E.Audio.GetRTPC("Listener1", NO .. "Rtpc")
            C.type(v, "number", "GetRTPC")
            return Result(v)
        end),
        case("SetRTPC#Listener1", "LC-05", function (E) return Result(E.Audio.SetRTPC("Listener1", NO .. "Rtpc", 0.0)) end),
        case("GetRTPC#HUD1", "LC-05", function (E)
            local v = E.Audio.GetRTPC("HUD1", NO .. "Rtpc")
            C.type(v, "number", "GetRTPC")
            return Result(v)
        end),
        case("ResetRTPC#HUD1", "LC-05", function (E) E.Audio.ResetRTPC("HUD1", NO .. "Rtpc") end),
        case("SetSwitch#HUD1", "LC-05", function (E) return Result(E.Audio.SetSwitch("HUD1", NO .. "Group", NO .. "Switch")) end),
        case("PostEvent#HUD1", "LC-05", function (E)
            local best, length = nil, math.huge
            for _, id in ipairs(E.Resource.GetAll("Sound") or {}) do
                local res = E.Resource.Get(id, "Sound")
                local event = res and tostring(res.SoundEvent) or ""
                local duration = res and tonumber(res.Duration) or 0
                if event:upper():find("UI") and duration > 0 and duration <= 1.0 and duration < length then
                    best, length = event, duration
                end
            end
            if best == nil then C.skip("no Sound resource of a UI event up to 1 s long") end
            local loaded = E.Audio.LoadEvent(best)
            local posted = E.Audio.PostEvent("HUD1", best)
            C.equal(posted, true, "PostEvent(HUD1, " .. best .. ") (LoadEvent gave " .. tostring(loaded) .. ")")
            return string.format("%s (%.2f s) posted on HUD1", best, length)
        end),
        case("PostEvent#HUD1-unknown", "LC-05", function (E) return Result(E.Audio.PostEvent("HUD1", NO .. "Event")) end),
        case("Stop#HUD1", "LC-05", function (E) E.Audio.Stop("HUD1") end),
        case("Stop#unknown", "LC-05", function (E)
            -- An unknown name is a Lua error (GetSoundObjectId); a known one is not.
            -- Its message names the object without the player digit, which
            -- GetSoundObjectId strips first (ClientAudio.inl:41-44, 66).
            local ok, err = pcall(E.Audio.Stop, "Bg3mseIgtNoSuchObject1")
            C.check(not ok, "Stop of an unknown object returned")
            local text = "Unknown built-in sound object name: Bg3mseIgtNoSuchObject"
            local at = tostring(err):find(text, 1, true)
            C.check(at ~= nil, "the error is '%s'", tostring(err))
            C.check(tostring(err):sub(at + #text, at + #text) ~= "1", "the name kept its player digit: '%s'", tostring(err))
            return text .. " [rows: G-125]"
        end),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
