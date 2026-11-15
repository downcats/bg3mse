-- The last chunk the offline start-up harness runs in each Lua state
-- (Mac/Tests/StartupTests.cpp --lua), after Runner.lua and the case files:
-- the cases marked offline, and every registered function checked against
-- the cases. The result is the number of failures.
return Bg3mseIgtRunOffline()
