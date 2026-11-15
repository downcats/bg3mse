-- The UI's state instances and widgets read through Ext.UI.GetStateMachine
-- (ui::UIStateInstance and ui::UIWidget, GameDefinitions/UI.h), client only,
-- on the UI thread (Ext.UI.Defer), with nothing written. Where each member
-- is in the macOS game is proved from the game's code (Mac/Layout/GameShapes.cpp,
-- the shape of the UI's widgets and state instances); here what the members
-- give is checked against what the game is known to have put there: a
-- widget has its state's GUID and player, the template it was made from,
-- and a file name; a state has a template with a name, and the tutorial
-- layer of that template.

local library = { name = "UiWidgets" }

function library.define(C)
    local MAX_STATES = 200

    local function IsGuid(value)
        return type(value) == "string" and #value == 36
    end

    local function Walk(root)
        local states, widgets, named, deepest = 0, 0, 0, 0
        local layouts, owners = {}, {}
        local function Keys(set)
            local list = {}
            for key in pairs(set) do list[#list + 1] = tostring(key) end
            table.sort(list)
            return table.concat(list, ",")
        end
        local function Visit(state, depth)
            if states >= MAX_STATES then return end
            states = states + 1
            if depth > deepest then deepest = depth end
            local template = C.notNil(state.State, "a state's template")
            C.type(template.Name, "string", "the template's name")
            -- Once, as a control for the widgets' templates below: a
            -- property the game registered with NoesisGUI on a class
            -- Norbyte's Lua does know (ls.State), read through NoesisGUI's
            -- type information and as the member he declares.
            if states == 1 then
                C.equal(template:GetProperty("Name"), template.Name, "a state template's Name in NoesisGUI's type information and as a member")
            end
            C.type(state.PlayerID, "number", "a state's player")
            C.check(IsGuid(state.StateGuid), "a state's GUID is " .. tostring(state.StateGuid))
            C.check(IsGuid(state.field_B8), "the GUID of a state's stack is " .. tostring(state.field_B8))
            C.type(state.Flags, "number", "a state's flags")
            C.type(state.Flags2, "number", "a state's second flags")
            layouts[state.Layout] = true
            owners[state.Owner] = true
            C.equal(state.TutorialLayer, template.TutorialLayer, "a state's tutorial layer and its template's")
            -- The macOS game's state keeps no widget metadata.
            C.equal(#state.WidgetMetadata, 0, "the widget metadata of a state")
            C.notNil(state.StateWidgets, "a state's collection of widget templates")
            for _, widget in ipairs(state.Widgets) do
                widgets = widgets + 1
                C.equal(widget.StateId, state.StateGuid, "a widget's state id and its state's GUID")
                C.equal(widget.PlayerId, state.PlayerID, "a widget's player and its state's")
                local file = C.type(widget.FileName, "string", "a widget's file name")
                if file ~= "" then named = named + 1 end
                C.check(math.type(widget.LayerIndex) == "integer" and widget.LayerIndex >= 0, "a widget's layer is " .. tostring(widget.LayerIndex))
                C.check(math.type(widget.IndexInLayer) == "integer" and widget.IndexInLayer >= 0,
                    "a widget's index in its layer is " .. tostring(widget.IndexInLayer))
                -- The template is NoesisGUI's ls.StateWidget. Norbyte's Lua
                -- knows no class of that name (Symbols.inl leaves
                -- ui::UIStateWidget its C++ name, which no class has), so on
                -- every platform the object is a Noesis::BaseComponent and
                -- its members are the properties the game registered with
                -- NoesisGUI (ObjectHelpers::FallbackGetProperty). What the
                -- object is, and what NoesisGUI has of it, is said before
                -- the member is read.
                local data = C.notNil(widget.WidgetData, "the template a widget was made from")
                C.equal(tostring(data.Type), "ls.StateWidget", "the class of the template a widget was made from")
                local filename = data:GetProperty("FileName")
                if type(filename) ~= "string" then
                    error(string.format("an ls.StateWidget has no FileName in NoesisGUI's type information (GetProperty gave %s); "
                        .. "its own properties there are: %s", tostring(filename), Keys(data:DirectProperties())), 0)
                end
                C.equal(data.FileName, filename, "the widget template's file name, as a member")
                C.type(widget.Flags, "number", "a widget's flags")
                C.type(widget.Flags2, "number", "a widget's second flags")
                C.type(widget.Flags3, "number", "a widget's third flags")
            end
            for _, stack in ipairs(state.States) do
                for _, child in ipairs(stack) do
                    Visit(child, depth + 1)
                end
            end
        end
        Visit(root, 0)
        return states, widgets, named, deepest, Keys(layouts), Keys(owners)
    end

    return {
        { "Ext.UI: the state instances and their widgets, read", id = "LC-64", ctx = "client", risk = "proved", async = true,
            run = function (E, M, done)
                E.UI.Defer(function ()
                    local ok, result = pcall(function ()
                        local machine = C.notNil(E.UI.GetStateMachine(), "GetStateMachine")
                        local root = C.notNil(machine.State, "the state machine's root state")
                        local states, widgets, named, deepest, layouts, owners = Walk(root)
                        C.check(states >= 1, "no state instance")
                        return string.format("%d states to depth %d, %d widgets (%d with a file name); layouts %s; owners %s",
                            states, deepest, widgets, named, layouts, owners)
                    end)
                    done(ok, tostring(result))
                end)
            end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
