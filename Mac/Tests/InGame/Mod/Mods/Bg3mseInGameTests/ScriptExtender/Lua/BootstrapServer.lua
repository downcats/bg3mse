local Runner = Ext.Require("Runner.lua")
for _, name in ipairs(Ext.Require("Libraries.lua")) do
    Runner.AddLibrary(Ext.Require("Cases/" .. name .. ".lua"))
end
Runner.SetEvents(Ext.Require("Cases/Events.lua"))
Runner.Bootstrap(Ext)
