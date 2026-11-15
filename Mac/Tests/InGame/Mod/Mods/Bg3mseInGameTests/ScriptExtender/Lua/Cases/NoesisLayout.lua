-- NoesisGUI's objects read through Ext.UI (BG3Extender/Lua/Libs/ClientUI),
-- client only, on the UI thread (Ext.UI.Defer), with nothing written: the
-- root, a walk of the visual tree, each element's type, name and parents,
-- properties of each kind the stored values keep (a string, a number, a
-- boolean, an enumeration, a structure, a component) and the root's data
-- context. Nothing boxed is read (Tag): his code pushes a boxed value's
-- address as the value. What the members read are is proved in Mac/Layout/GameShapes.cpp
-- (the Noesis shapes).

local library = { name = "NoesisLayout" }

function library.define(C)
    local DEPTH, MAX_NODES = 4, 200

    -- One read, kept with what it gave or the error it raised.
    local function Read(object, name)
        local ok, value = pcall(function () return object[name] end)
        return ok, value
    end

    local function Walk(E, root)
        local seen, deepest, named, framework = 0, 0, 0, 0
        local function Visit(node, depth)
            if seen >= MAX_NODES then return end
            seen = seen + 1
            if depth > deepest then deepest = depth end
            C.type(node.Type, "string", "an element's type")
            local info = C.notNil(node:TypeInfo(), "an element's TypeInfo")
            C.type(info.IsInterface, "boolean", "the type's IsInterface")
            local ok, name = Read(node, "Name")
            if ok and type(name) == "string" and name ~= "" then named = named + 1 end
            -- A framework element: its logical children and parents.
            local fe, children = Read(node, "ChildrenCount")
            if fe and type(children) == "number" then
                framework = framework + 1
                Read(node, "Parent")
                Read(node, "TemplatedParent")
            end
            if depth >= DEPTH then return end
            local count = C.type(node.VisualChildrenCount, "number", "an element's visual children")
            for i = 1, count do
                local child = node:VisualChild(i)
                if child ~= nil then
                    C.notNil(child.VisualParent, "a child's visual parent")
                    Visit(child, depth + 1)
                end
            end
        end
        Visit(root, 0)
        return seen, deepest, named, framework
    end

    -- Properties of each kind the stored values hold, on the first elements
    -- that have them: the kind's Lua type, or the error his code gave.
    local KINDS = {
        { "Name", "string" }, { "Width", "number" }, { "Opacity", "number" }, { "IsEnabled", "boolean" },
        { "Visibility", nil }, { "Margin", nil }, { "DataContext", nil },
    }

    local function Kinds(root)
        local found, errors = {}, {}
        local queue, at = { root }, 1
        while at <= #queue and at <= MAX_NODES do
            local node = queue[at]
            at = at + 1
            for _, kind in ipairs(KINDS) do
                if found[kind[1]] == nil then
                    local ok, value = Read(node, kind[1])
                    if not ok then
                        errors[kind[1]] = tostring(value)
                    elseif value ~= nil then
                        found[kind[1]] = type(value)
                        if kind[2] then C.type(value, kind[2], kind[1]) end
                    end
                end
            end
            for i = 1, node.VisualChildrenCount do
                local child = node:VisualChild(i)
                if child ~= nil then queue[#queue + 1] = child end
            end
        end
        local parts = {}
        for _, kind in ipairs(KINDS) do
            local k = kind[1]
            parts[#parts + 1] = k .. " " .. (found[k] or (errors[k] and ("error: " .. errors[k])) or "absent")
        end
        return found, table.concat(parts, "; ")
    end

    local function DataContext(root)
        local ok, context = Read(root, "DataContext")
        if not ok then return "the root's data context: error: " .. tostring(context) end
        if context == nil then return "the root has no data context" end
        local okType, typeName = Read(context, "Type")
        local okInfo, info = pcall(function () return context:TypeInfo() end)
        local properties = okInfo and info and info.Properties and #info.Properties or 0
        return string.format("data context %s (%d properties)", okType and tostring(typeName) or "?", properties)
    end

    return {
        { "Ext.UI: NoesisGUI's tree, read", id = "LC-30", ctx = "client", risk = "proved", async = true,
            run = function (E, M, done)
                E.UI.Defer(function ()
                    local ok, result = pcall(function ()
                        local root = C.notNil(E.UI.GetRoot(), "GetRoot")
                        local info = C.notNil(root:TypeInfo(), "the root's TypeInfo")
                        C.notNil(info.Base, "the root type's base")
                        C.type(#info.Properties, "number", "the root type's properties")
                        C.type(#info.DependencyProperties, "number", "the root type's dependency properties")
                        C.type(root.NumReferences, "number", "the root's references")
                        local seen, deepest, named, framework = Walk(E, root)
                        local found, kinds = Kinds(root)
                        C.equal(found.Name, "string", "a Name read as a string")
                        C.equal(found.IsEnabled, "boolean", "an IsEnabled read as a boolean")
                        C.equal(found.Width, "number", "a Width read as a number")
                        return string.format("root %s, %d elements to depth %d (%d named, %d framework elements); %s; %s",
                            root.Type, seen, deepest, named, framework, kinds, DataContext(root))
                    end)
                    done(ok, tostring(result))
                end)
            end },
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
