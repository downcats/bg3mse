-- Ext.Math (BG3Extender/Lua/Libs/Math.inl): glm arithmetic, no engine
-- dependency. Each case compares with a value worked out by hand. Audit row
-- LUA-086.

local library = { name = "Math" }

local I3 = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }
local I4 = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }

local function Mat4(t)
    local m = {}
    for i = 1, 16 do m[i] = t[i] end
    return m
end

function library.define(C)
    local function case(name, run)
        return { "Ext.Math." .. name, id = "LUA-086", risk = "pure", offline = true, run = run }
    end
    local H = math.pi / 2
    return {
        case("Add", function (E)
            C.vecNear(E.Math.Add({ 1, 2, 3 }, { 4, 5, 6 }), { 5, 7, 9 })
            C.vecNear(E.Math.Add({ 1, 2, 3, 4 }, 1), { 2, 3, 4, 5 })
            -- In place (a third argument, Math.inl CallFunc): the two-argument
            -- DoInPlace of TryOpOrFail (Math.inl:52-61) writes the sum into it
            -- and nothing is returned.
            local out = { 0, 0, 0 }
            C.equal(select("#", E.Math.Add({ 1, 2, 3 }, { 4, 5, 6 }, out)), 0, "values Add(a, b, out) returns")
            C.vecNear(out, { 5, 7, 9 }, "out after Add(a, b, out)")
            return "in place: out = 5,7,9 [rows: G-133]"
        end),
        case("Sub", function (E) C.vecNear(E.Math.Sub({ 5, 7, 9 }, { 4, 5, 6 }), { 1, 2, 3 }) end),
        case("Mul", function (E)
            C.vecNear(E.Math.Mul({ 1, 2, 3 }, 2), { 2, 4, 6 })
            C.vecNear(E.Math.Mul(I3, { 1, 2, 3 }), { 1, 2, 3 })
        end),
        case("Div", function (E) C.vecNear(E.Math.Div({ 2, 4, 6 }, 2), { 1, 2, 3 }) end),
        case("Reflect", function (E) C.vecNear(E.Math.Reflect({ 1, -1, 0 }, { 0, 1, 0 }), { 1, 1, 0 }) end),
        case("Angle", function (E) C.near(E.Math.Angle({ 1, 0, 0 }, { 0, 1, 0 }), H) end),
        case("Cross", function (E) C.vecNear(E.Math.Cross({ 1, 0, 0 }, { 0, 1, 0 }), { 0, 0, 1 }) end),
        case("Distance", function (E) C.near(E.Math.Distance({ 0, 0, 0 }, { 3, 4, 0 }), 5) end),
        case("Dot", function (E) C.near(E.Math.Dot({ 1, 2, 3 }, { 4, 5, 6 }), 32) end),
        case("Length", function (E) C.near(E.Math.Length({ 3, 4, 0 }), 5) end),
        case("Normalize", function (E)
            C.vecNear(E.Math.Normalize({ 0, 0, 5 }), { 0, 0, 1 })
            -- In place (a second argument): the one-argument DoInPlace of
            -- TryOpOrFail (Math.inl:39-50) writes into it, NormalizeOp
            -- assigning to the second parameter.
            local out = { 9, 9, 9 }
            C.equal(select("#", E.Math.Normalize({ 0, 3, 4 }, out)), 0, "values Normalize(v, out) returns")
            C.vecNear(out, { 0, 0.6, 0.8 }, "out after Normalize(v, out)")
            return "in place: out = 0,0.6,0.8 [rows: G-132]"
        end),
        case("Perpendicular", function (E) C.vecNear(E.Math.Perpendicular({ 1, 1, 0 }, { 1, 0, 0 }), { 0, 1, 0 }) end),
        case("Project", function (E) C.vecNear(E.Math.Project({ 1, 1, 0 }, { 1, 0, 0 }), { 1, 0, 0 }) end),
        case("Determinant", function (E)
            C.near(E.Math.Determinant({ 2, 0, 0, 0, 3, 0, 0, 0, 4 }), 24)
        end),
        case("Inverse", function (E)
            C.vecNear(E.Math.Inverse({ 2, 0, 0, 0, 4, 0, 0, 0, 8 }), { 0.5, 0, 0, 0, 0.25, 0, 0, 0, 0.125 })
        end),
        case("Transpose", function (E)
            C.vecNear(E.Math.Transpose({ 1, 2, 3, 4, 5, 6, 7, 8, 9 }), { 1, 4, 7, 2, 5, 8, 3, 6, 9 })
        end),
        case("OuterProduct", function (E)
            C.vecNear(E.Math.OuterProduct({ 1, 2, 3 }, { 1, 0, 0 }), { 1, 2, 3, 0, 0, 0, 0, 0, 0 })
        end),
        case("Rotate", function (E)
            local m = Mat4(I4)
            E.Math.Rotate(m, H, { 0, 0, 1 })
            C.vecNear(E.Math.Mul(m, { 1, 0, 0, 1 }), { 0, 1, 0, 1 })
        end),
        case("Translate", function (E)
            local m = Mat4(I4)
            E.Math.Translate(m, { 1, 2, 3 })
            C.vecNear({ m[13], m[14], m[15] }, { 1, 2, 3 })
        end),
        case("Scale", function (E)
            local m = Mat4(I4)
            E.Math.Scale(m, { 2, 3, 4 })
            C.vecNear({ m[1], m[6], m[11] }, { 2, 3, 4 })
        end),
        case("ExtractEulerAngles", function (E)
            C.vecNear(E.Math.ExtractEulerAngles(I3), { 0, 0, 0 })
        end),
        case("BuildFromEulerAngles3", function (E)
            C.vecNear(E.Math.BuildFromEulerAngles3({ 0, 0, 0 }), I3)
        end),
        case("BuildFromEulerAngles4", function (E)
            C.vecNear(E.Math.BuildFromEulerAngles4({ 0, 0, 0 }), I4)
        end),
        case("Decompose", function (E)
            local m = E.Math.BuildTranslation({ 1, 2, 3 })
            local scale, rotation, translation = { 0, 0, 0 }, { 9, 9, 9 }, { 0, 0, 0 }
            E.Math.Decompose(m, scale, rotation, translation)
            C.vecNear(scale, { 1, 1, 1 }, "scale")
            C.vecNear(rotation, { 0, 0, 0 }, "rotation")
            C.vecNear(translation, { 1, 2, 3 }, "translation")
        end),
        case("ExtractAxisAngle", function (E)
            local axis = { 0, 0, 0 }
            local angle = E.Math.ExtractAxisAngle(E.Math.BuildRotation3({ 0, 0, 1 }, H), axis)
            C.near(angle, H, "angle")
            C.vecNear(axis, { 0, 0, 1 }, "axis")
        end),
        case("BuildFromAxisAngle3", function (E)
            C.vecNear(E.Math.Mul(E.Math.BuildFromAxisAngle3({ 0, 0, 1 }, H), { 1, 0, 0 }), { 0, 1, 0 })
        end),
        case("BuildFromAxisAngle4", function (E)
            C.vecNear(E.Math.Mul(E.Math.BuildFromAxisAngle4({ 0, 0, 1 }, H), { 1, 0, 0, 1 }), { 0, 1, 0, 1 })
        end),
        case("BuildRotation3", function (E)
            C.vecNear(E.Math.Mul(E.Math.BuildRotation3({ 0, 0, 1 }, H), { 1, 0, 0 }), { 0, 1, 0 })
        end),
        case("BuildRotation4", function (E)
            C.vecNear(E.Math.Mul(E.Math.BuildRotation4({ 0, 0, 1 }, H), { 1, 0, 0, 1 }), { 0, 1, 0, 1 })
        end),
        case("BuildTranslation", function (E)
            C.vecNear(E.Math.Mul(E.Math.BuildTranslation({ 1, 2, 3 }), { 0, 0, 0, 1 }), { 1, 2, 3, 1 })
        end),
        case("BuildScale", function (E)
            C.vecNear(E.Math.Mul(E.Math.BuildScale({ 2, 3, 4 }), { 1, 1, 1, 1 }), { 2, 3, 4, 1 })
        end),
        -- Quaternions are x, y, z, w (glm's order as the extender pushes it).
        case("QuatFromEuler", function (E)
            C.vecNear(E.Math.QuatFromEuler({ 0, 0, 0 }), { 0, 0, 0, 1 })
        end),
        case("QuatFromToRotation", function (E)
            C.vecNear(E.Math.QuatRotate(E.Math.QuatFromToRotation({ 1, 0, 0 }, { 0, 1, 0 }), { 1, 0, 0 }), { 0, 1, 0 })
        end),
        case("QuatDot", function (E) C.near(E.Math.QuatDot({ 0, 0, 0, 1 }, { 0, 0, 0, 1 }), 1) end),
        case("QuatSlerp", function (E)
            C.vecNear(E.Math.QuatSlerp({ 0, 0, 0, 1 }, { 0, 0, 0, 1 }, 0.5), { 0, 0, 0, 1 })
        end),
        case("QuatToMat3", function (E) C.vecNear(E.Math.QuatToMat3({ 0, 0, 0, 1 }), I3) end),
        case("QuatToMat4", function (E) C.vecNear(E.Math.QuatToMat4({ 0, 0, 0, 1 }), I4) end),
        case("Mat3ToQuat", function (E) C.vecNear(E.Math.Mat3ToQuat(I3), { 0, 0, 0, 1 }) end),
        case("Mat4ToQuat", function (E) C.vecNear(E.Math.Mat4ToQuat(I4), { 0, 0, 0, 1 }) end),
        case("QuatNormalize", function (E) C.vecNear(E.Math.QuatNormalize({ 0, 0, 0, 2 }), { 0, 0, 0, 1 }) end),
        case("QuatInverse", function (E) C.vecNear(E.Math.QuatInverse({ 0, 0, 0, 1 }), { 0, 0, 0, 1 }) end),
        case("QuatRotate", function (E) C.vecNear(E.Math.QuatRotate({ 0, 0, 0, 1 }, { 1, 2, 3 }), { 1, 2, 3 }) end),
        case("QuatRotateAxisAngle", function (E)
            local q = E.Math.QuatRotateAxisAngle({ 0, 0, 0, 1 }, { 0, 0, 1 }, H)
            C.vecNear(E.Math.QuatRotate(q, { 1, 0, 0 }), { 0, 1, 0 })
        end),
        case("QuatLength", function (E) C.near(E.Math.QuatLength({ 0, 0, 0, 2 }), 2) end),
        case("QuatMul", function (E) C.vecNear(E.Math.QuatMul({ 0, 0, 0, 1 }, { 0, 0, 0, 1 }), { 0, 0, 0, 1 }) end),
        case("Random", function (E)
            local x = E.Math.Random()
            C.check(type(x) == "number" and x >= 0 and x <= 1, "Random() gave %s", tostring(x))
            local n = E.Math.Random(3, 5)
            C.check(math.type(n) == "integer" and n >= 3 and n <= 5, "Random(3, 5) gave %s", tostring(n))
            C.check(E.Math.Random(7) <= 7, "Random(7) above 7")
        end),
        case("Round", function (E)
            C.equal(E.Math.Round(2.5), 3)
            C.equal(E.Math.Round(-2.5), -3)
        end),
        case("Fract", function (E) C.near(E.Math.Fract(2.25), 0.25) end),
        case("Trunc", function (E) C.near(E.Math.Trunc(-2.75), -2) end),
        case("Sign", function (E) C.near(E.Math.Sign(-3), -1) end),
        case("Clamp", function (E) C.near(E.Math.Clamp(5, 0, 1), 1) end),
        case("Smoothstep", function (E) C.near(E.Math.Smoothstep(0, 1, 0.5), 0.5) end),
        case("Lerp", function (E) C.near(E.Math.Lerp(0, 10, 0.25), 2.5) end),
        case("Acos", function (E) C.near(E.Math.Acos(0), H) end),
        case("Asin", function (E) C.near(E.Math.Asin(1), H) end),
        case("Atan", function (E) C.near(E.Math.Atan(1), math.pi / 4) end),
        case("Atan2", function (E) C.near(E.Math.Atan2(1, 0), H) end),
        case("IsNaN", function (E)
            C.equal(E.Math.IsNaN(0 / 0), true)
            C.equal(E.Math.IsNaN(1), false)
        end),
        case("IsInf", function (E)
            C.equal(E.Math.IsInf(math.huge), true)
            C.equal(E.Math.IsInf(1), false)
        end),
    }
end

if Bg3mseIgtOffline then Bg3mseIgtOffline(library) end
return library
