-- Ext.Utils (BG3Extender/Lua/Libs/Utils.inl) and what the built-in scripts
-- add to it (BuiltinLibrary.lua, Libs/Profiler.lua).

local library = { name = "Utils" }

function library.define(C)
    return {
        { "Ext.Utils.Version", id = "LUA-014", risk = "pure", offline = true, run = function (E)
            local v = E.Utils.Version()
            C.check(math.type(v) == "integer" and v >= 34, "Version() gave %s", tostring(v))
            return tostring(v)
        end },
        { "Ext.Utils.GameVersion", id = "LUA-015", risk = "pure", run = function (E)
            local v = E.Utils.GameVersion()
            C.check(type(v) == "string" and v:match("^v4%.%d+%.%d+%.%d+$") ~= nil, "GameVersion() gave %s", tostring(v))
            return v
        end },
        { "Ext.Utils.Include", id = "LUA-016", run = function (E)
            C.equal(E.Utils.Include(C.ModuleUUID, "Probe.lua"), "bg3mse-ingame-probe", "Include")
        end },
        { "Ext.Utils.LoadString", id = "LUA-017", risk = "pure", offline = true, run = function (E)
            local fn = E.Utils.LoadString("return 6 * 7")
            C.equal(fn(), 42, "the chunk's result")
            local bad, err = E.Utils.LoadString("return +")
            C.check(bad == nil and type(err) == "string", "a syntax error was not reported")
        end },
        { "Ext.Utils.GetValueType", id = "LUA-017", risk = "pure", offline = true, run = function (E)
            C.type(E.Utils.GetValueType(1), "string")
            return E.Utils.GetValueType({})
        end },
        { "Ext.Utils.IsValidHandle", id = "LUA-017", run = function (E)
            C.equal(E.Utils.IsValidHandle(1), false, "IsValidHandle(1)")
            local character = C.character()
            C.equal(E.Utils.IsValidHandle(character), true, "IsValidHandle(character)")
        end },
        { "Ext.Utils.HandleToInteger", id = "LUA-017", run = function (E)
            local character = C.character()
            local i = E.Utils.HandleToInteger(character)
            C.check(math.type(i) == "integer" and i ~= 0, "HandleToInteger gave %s", tostring(i))
        end },
        { "Ext.Utils.IntegerToHandle", id = "LUA-017", run = function (E)
            local character = C.character()
            local back = E.Utils.IntegerToHandle(E.Utils.HandleToInteger(character))
            C.equal(E.Utils.HandleToInteger(back), E.Utils.HandleToInteger(character), "the handle after a round trip")
        end },
        { "Ext.Utils.GenerateGuid", id = "LUA-018", risk = "pure", offline = true, run = function (E)
            local a, b = E.Utils.GenerateGuid(), E.Utils.GenerateGuid()
            -- Larian's text form (CoreLib/Base/Base.cpp, Guid::ToString) swaps the
            -- two bytes of the fourth group: the variant is its third digit.
            local pattern = "^%x%x%x%x%x%x%x%x%-%x%x%x%x%-4%x%x%x%-%x%x[89ab]%x%-%x%x%x%x%x%x%x%x%x%x%x%x$"
            C.check(type(a) == "string" and a:match(pattern) ~= nil, "not a version 4 GUID: %s", tostring(a))
            C.check(a ~= b, "two GUIDs are equal")
        end },
        { "Ext.Utils.ShowErrorAndExitGame", id = "LUA-019", skip = "ends the game" },
        { "Ext.Utils.ShowError", id = "LUA-019", risk = "unproved", ctx = "client", run = function (E)
            -- The game's error dialog: the owner closes it (see the install steps).
            E.Utils.ShowError("bg3mse in-game test: this message is expected. Press OK to go on.")
        end },
        { "Ext.Utils.ShowError#server", id = "LUA-019", ctx = "server",
            skip = "the same function as the client case (Utils.inl, LibraryManager::ShowStartupError): one dialog is enough" },
        { "Ext.Utils.GetGlobalSwitches", id = "LUA-020", run = function (E)
            local switches = C.notNil(E.Utils.GetGlobalSwitches(), "GetGlobalSwitches()")
            C.check(E.Types.Validate(switches), "GlobalSwitches does not pass validation")
            C.type(switches.MuteSoundWhenNotFocused, "boolean", "MuteSoundWhenNotFocused")
            -- Values, not types (a byte reads as a boolean wherever it is):
            -- members on each side of the Mac edits of Misc.h. What only the
            -- game decides is reported, and a row is named only when its
            -- check had something to check.
            local function Finite(v, what)
                C.check(type(v) == "number" and v == v and v > -1e30 and v < 1e30, "%s is %s", what, tostring(v))
                return v
            end
            local function Between(v, lo, hi, what)
                C.check(math.type(v) == "integer" and v >= lo and v <= hi, "%s is %s, expected %d..%d", what, tostring(v), lo, hi)
                return v
            end
            local rows = { "G-042", "LAY-64" }
            local language = C.type(switches.Language, "string", "Language")
            C.check(#language > 0 and #language < 64 and not language:find("[%c\128-\255]"), "Language is '%s'", language)
            -- After the twelve SoundSettings (a SoundSetting has a word more,
            -- Misc.h:50-53) and after the float MSVC pads (field_1A4,
            -- Misc.h:204-210): Rulesets is an Array<Guid> (pushed as GUID
            -- strings, LuaPush.h:75), and the options around it.
            local guid = "^%x%x%x%x%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%x%x%x%x%x%x%x%x$"
            local rulesets = switches.Rulesets
            C.check(#rulesets <= 64, "%d rulesets", #rulesets)
            for i = 1, #rulesets do
                C.check(type(rulesets[i]) == "string" and rulesets[i]:match(guid) ~= nil, "Rulesets[%d] is %s", i, tostring(rulesets[i]))
            end
            if #rulesets > 0 then rows[#rows + 1] = "G-072"; rows[#rows + 1] = "G-074" end
            Finite(switches.GameCameraSpeedMultiplier, "GameCameraSpeedMultiplier")
            Between(switches.MaxNrOfQuickSaves, 0, 100000, "MaxNrOfQuickSaves")
            Between(switches.MaxNrOfAutoSaves, 0, 100000, "MaxNrOfAutoSaves")
            Between(switches.TextBackgroundOpacity, 0, 100000, "TextBackgroundOpacity")
            Between(switches.ControllerRumbleStrength, 0, 100000, "ControllerRumbleStrength")
            -- CameraSwitches, an std::array of 4 (Misc.h:371): its type is an
            -- array of CameraGlobalSwitches only where the std::array
            -- initializer takes a size_t (BaseTypeInformation.h:285-291).
            local cameras = switches.CameraSwitches
            C.equal(#cameras, 4, "#CameraSwitches")
            local distances = {}
            for i = 1, 4 do
                distances[i] = string.format("%.1f..%.1f", Finite(cameras[i].DefaultDistanceMin, "CameraSwitches[" .. i .. "].DefaultDistanceMin"),
                    Finite(cameras[i].DefaultDistanceMax, "CameraSwitches[" .. i .. "].DefaultDistanceMax"))
            end
            local member = C.notNil(E.Types.GetTypeInfo("GlobalSwitches").Members.CameraSwitches, "the type of GlobalSwitches.CameraSwitches")
            C.equal(member.Kind, "Array", "the kind of GlobalSwitches.CameraSwitches")
            C.equal(member.ElementType and member.ElementType.TypeName, "CameraGlobalSwitches", "its element type")
            -- After SomeSettings, aligned to 16 bytes on macOS (Misc.h:373-378).
            Finite(switches.PointAndClickSoundFrequency, "PointAndClickSoundFrequency")
            local dice = C.type(switches.ActiveCustomDice, "string", "ActiveCustomDice")
            C.check(#dice < 256 and not dice:find("[%c\128-\255]"), "ActiveCustomDice is '%s'", dice:sub(1, 80))
            if #dice > 0 then rows[#rows + 1] = "G-075" end
            return string.format("Language %s; %d rulesets; saves %d/%d; camera distances %s; dice '%s'; start %s/%s [rows: %s]",
                language, #rulesets, switches.MaxNrOfQuickSaves, switches.MaxNrOfAutoSaves, table.concat(distances, " "),
                dice:sub(1, 60), tostring(switches.StartYear), tostring(switches.StartDay), table.concat(rows, ", "))
        end },
        { "Ext.Utils.GetCommandLineParams", id = "LUA-021", risk = "pure", run = function (E)
            local params = E.Utils.GetCommandLineParams()
            C.check(type(params) == "table" and #params >= 1, "no command line parameters")
            -- Utils.inl splits ToUTF8(GetCommandLineW()) at blanks; the line
            -- begins with the program, and the game's path has blanks in it
            -- ("Contents/MacOS/Baldur's Gate 3", Mac/Tools/EngineTool.cpp), so
            -- in Windows' form it is quoted.
            local line = table.concat(params, " ")
            C.equal(line:sub(1, 1), '"', "the command line's first character")
            C.check(line:find("Baldur's Gate 3", 1, true) ~= nil, "the program's name is not in '%s'", line:sub(1, 200))
            local bundle = line:find("Contents/MacOS/Baldur's Gate 3", 1, true) ~= nil
            return #params .. " parameters" .. (bundle and ", the program's path in the app bundle" or ", '" .. line:sub(1, 120) .. "'")
                .. " [rows: SHIM-03, SHIM-25]"
        end },
        { "Ext.Utils.GetDialogManager", id = "LUA-021", run = function (E)
            -- Each side has its own manager, with its own copies of the
            -- dialogues: the server's dialog system's, and on the client
            -- the one its timeline system holds.
            local manager = C.notNil(E.Utils.GetDialogManager(), "the dialog manager of this side")
            do
                -- Its members (Dialog.h:499-529), reported: whether the id the
                -- next dialog gets is not below any running dialog's, and each
                -- flag description is filed under its own name. Neither is
                -- Norbyte's to promise, so a mismatch only withholds the rows.
                local nextId = manager.NextInstanceId
                C.check(math.type(nextId) == "integer" and nextId >= 0, "NextInstanceId is %s", tostring(nextId))
                local dialogs, above = 0, 0
                for id in pairs(manager.Dialogs) do
                    dialogs = dialogs + 1
                    C.check(math.type(id) == "integer", "dialog id %s", tostring(id))
                    if id < 0 or id > nextId then above = above + 1 end
                end
                local flags, named, other = 0, 0, nil
                for name, desc in pairs(manager.FlagDescriptions) do
                    flags = flags + 1
                    if flags <= 50 then
                        if tostring(desc.Name) == tostring(name) then named = named + 1
                        elseif other == nil then other = tostring(name) .. " -> " .. tostring(desc.Name) end
                    end
                end
                local agree = above == 0 and named > 0 and other == nil
                return string.format("NextInstanceId %d, %d dialogs (%d outside 0..it), %d flag descriptions (%d of the first named by their key%s)%s",
                    nextId, dialogs, above, flags, named, other and ("; " .. other) or "",
                    agree and " [rows: LAY2-09, NG-18]" or "")
            end
        end },
        { "Ext.Utils.GetGameState", id = "LUA-021", run = function (E)
            local state = E.Utils.GetGameState()
            C.check(state == "Running" or tostring(state) == "Running", "game state is %s", tostring(state))
        end },
        { "Ext.Utils.GetMemoryUsage", id = "LUA-017", risk = "pure", offline = true, run = function (E)
            local n = E.Utils.GetMemoryUsage()
            C.check(math.type(n) == "integer" and n > 0, "GetMemoryUsage() gave %s", tostring(n))
        end },
        { "Ext.Utils.ProfileBegin", id = "LUA-022", risk = "pure", offline = true, run = function (E)
            E.Utils.ProfileBegin("bg3mse in-game test")
            E.Utils.ProfileEnd()
        end },
        { "Ext.Utils.ProfileEnd", id = "LUA-022", risk = "pure", offline = true, run = function (E)
            E.Utils.ProfileBegin("bg3mse in-game test")
            E.Utils.ProfileEnd()
        end },
        { "Ext.Utils.Profile", id = "LUA-022", risk = "pure", offline = true, run = function (E)
            local got = nil
            E.Utils.Profile(function (x) got = x end, 5)
            C.equal(got, 5, "the profiled function's argument")
        end },
        { "Ext.Utils.ProfileNamed", id = "LUA-022", risk = "pure", offline = true, run = function (E)
            local got = nil
            E.Utils.ProfileNamed("bg3mse in-game test", function (x) got = x end, 6)
            C.equal(got, 6, "the profiled function's argument")
        end },
        { "Ext.Utils.LoadTestLibrary", id = "LUA-009", run = function (E)
            -- The runner loads it (when developer mode has not) before Norbyte's suite.
            if E.Test == nil then E.Utils.LoadTestLibrary() end
            C.type(E.Test, "table", "Ext.Test")
            C.type(rawget(E.Test, "RegisteredTests"), "table", "Ext.Test.RegisteredTests")
        end },
        { "Ext.Utils.Print", id = "LUA-008", risk = "pure", offline = true, run = function (E)
            E.Utils.Print("bg3mse in-game test: Ext.Utils.Print")
        end },
        { "Ext.Utils.PrintWarning", id = "LUA-008", risk = "pure", offline = true, run = function (E)
            E.Utils.PrintWarning("bg3mse in-game test: Ext.Utils.PrintWarning (expected)")
        end },
        { "Ext.Utils.PrintError", id = "LUA-008", risk = "pure", run = function (E)
            E.Utils.PrintError("bg3mse in-game test: Ext.Utils.PrintError (expected)")
        end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
