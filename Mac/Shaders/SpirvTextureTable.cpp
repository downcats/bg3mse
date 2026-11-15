#include <stdafx.h>
#include <Shaders/ShaderInterface.h>
#include <Shaders/SpirvIdentity.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <vector>

// See TextureTableRewrite in ShaderInterface.h.
//
// What a Windows shader does with the table, in every stage looked at (the
// game's own and a mod's), and all that is recognised here:
//
//   k  = (probe.Diffuse >> 3) & 16383,  (probe.Diffuse >> 17) & 16383
//        (probe.Specular >> 0) & 65535, (probe.Specular >> 16) & 65535
//        light.Cookie >> 1
//   d  = k - 1
//   x  = d * 2,  y = x + 1                   (the probes' two)
//   cubes[x] and cubes[y], each loaded, made a sampled image and sampled at a
//   level; flat[d] the same with gradients; and x >= 0, d >= 0 as tests.
//
// The Mac game's engine writes the same members otherwise (GameInterface.h has
// where): a layer of an array where Windows has a place in the table, and for
// the specular cubes and the cookies which array of several. So k is re-read
// (the shift and the mask of the instructions that are there are changed),
// the array's number is read beside it, and each sample is made of the Mac's
// texture: the even place the colour's array, the odd one the alpha's, the
// layer d as the fourth (or third) number of where it is sampled, a specular
// cube's level less the array's number as the game's own function has it.
// Everything the stage does around that stays as it is.

namespace bg3se::mac {

namespace {

enum : uint16_t
{
    OpName = 5, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeArray = 28,
    OpTypeRuntimeArray = 29, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65,
    OpInBoundsAccessChain = 66, OpDecorate = 71, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCopyObject = 83,
    OpSampledImage = 86, OpImageSampleExplicitLod = 88, OpConvertSToF = 111, OpBitcast = 124, OpIAdd = 128, OpISub = 130,
    OpFSub = 131, OpIMul = 132, OpSGreaterThanEqual = 175, OpShiftRightLogical = 194, OpBitwiseAnd = 199,
};
constexpr uint32_t kDescriptorSet = 34, kBinding = 33, kUniformConstant = 0, kLod = 2, kGrad = 4, kDimFlat = 1, kDimCube = 3;

struct Inst
{
    uint16_t Op{ 0 };
    std::vector<uint32_t> W;        // without the first word
    int Result{ -1 };               // the place of its result id in W
    std::vector<uint32_t> Ids;      // the places of the ids it uses
};

// Which thing of the table a number stands for.
enum class What { DiffuseFirst, DiffuseSecond, SpecularFirst, SpecularSecond, Cookie };

struct Field
{
    What Is;
    uint32_t Member{ 0 };       // the value of the member it is taken from, as the instructions name it
    size_t Shift{ 0 };          // the instruction that shifts (for the cookie it is k itself)
    size_t Mask{ SIZE_MAX };    // the instruction that masks: k itself, for a probe
    uint32_t Array{ 0 };        // the id the array's number gets
};

class Rewrite
{
public:
    Rewrite(std::vector<uint32_t>& words, TextureTableRewrite const& how, std::string& why) : words_(words), how_(how), why_(why) {}

    TextureTableResult Run()
    {
        if (!Read()) return TextureTableResult::Refused;
        std::vector<uint32_t> cubes, flats;
        if (!FindTable(cubes, flats)) return TextureTableResult::Refused;
        if (cubes.empty() && flats.empty()) return TextureTableResult::NotThere;
        if (!FindFields() || !FindSamples(cubes, flats) || !Change(cubes, flats)) return TextureTableResult::Refused;
        Write();
        return TextureTableResult::Done;
    }

private:
    bool Fail(std::string const& what)
    {
        why_ = "the table of textures: " + what;
        return false;
    }

    bool Read()
    {
        if (words_.size() < 5 || words_[0] != 0x07230203) return Fail("the module cannot be read");
        bound_ = words_[3];
        for (size_t at = 5; at < words_.size();) {
            uint32_t count = words_[at] >> 16;
            if (!count || at + count > words_.size()) return Fail("the module cannot be read");
            Inst inst;
            inst.Op = (uint16_t)(words_[at] & 0xffff);
            inst.W.assign(words_.begin() + at + 1, words_.begin() + at + count);
            bool debug = inst.Op < 9 || inst.Op == 10 || inst.Op == 17 || inst.Op == 317 || inst.Op == 330;
            if (!debug && !SpirvIdOperands(inst.Op, inst.W, inst.Result, inst.Ids)) return Fail("an instruction is not one this code reads");
            if (inst.Op == OpFunction && firstFunction_ == SIZE_MAX) firstFunction_ = insts_.size();
            insts_.push_back(std::move(inst));
            at += count;
        }
        if (firstFunction_ == SIZE_MAX) return Fail("the module has no function");
        for (size_t i = 0; i < insts_.size(); i++) {
            auto const& inst = insts_[i];
            if (inst.Result >= 0) def_[inst.W[inst.Result]] = i;
            for (uint32_t place : inst.Ids) uses_[inst.W[place]].push_back(i);
            if (inst.Op == OpDecorate && inst.W.size() == 3 && inst.W[1] == kDescriptorSet) set_[inst.W[0]] = inst.W[2];
            if (inst.Op == OpDecorate && inst.W.size() == 3 && inst.W[1] == kBinding) binding_[inst.W[0]] = inst.W[2];
        }
        return true;
    }

    std::vector<uint32_t> VariablesAt(uint32_t set, uint32_t binding) const
    {
        std::vector<uint32_t> out;
        for (auto const& [id, s] : set_) {
            auto b = binding_.find(id);
            if (s == set && b != binding_.end() && b->second == binding) out.push_back(id);
        }
        return out;
    }

    Inst const* Def(uint32_t id, uint16_t op) const
    {
        auto it = def_.find(id);
        return it != def_.end() && insts_[it->second].Op == op ? &insts_[it->second] : nullptr;
    }
    std::optional<uint32_t> Constant(uint32_t id) const
    {
        auto c = Def(id, OpConstant);
        if (!c || c->W.size() != 3 || !Def(c->W[0], OpTypeInt)) return {};
        return c->W[2];
    }
    // The id a copy or a cast of whole numbers stands for.
    uint32_t Through(uint32_t id) const
    {
        for (int depth = 0; depth < 8; depth++) {
            auto it = def_.find(id);
            if (it == def_.end()) break;
            auto const& inst = insts_[it->second];
            if (inst.Op != OpCopyObject && inst.Op != OpBitcast) break;
            id = inst.W[2];
        }
        return id;
    }
    // Every instruction that uses the id, a copy or a cast of it counted as the id itself.
    std::vector<size_t> Users(uint32_t id) const
    {
        std::vector<size_t> out;
        std::vector<uint32_t> ids = { id };
        for (size_t k = 0; k < ids.size(); k++) {
            auto it = uses_.find(ids[k]);
            if (it == uses_.end()) continue;
            for (size_t user : it->second) {
                auto const& inst = insts_[user];
                if (inst.Op == OpDecorate) continue;        // says something of the id (that it is not uniform, say); no use of it
                if ((inst.Op == OpCopyObject || inst.Op == OpBitcast) && inst.W.size() == 3) ids.push_back(inst.W[1]);
                else out.push_back(user);
            }
        }
        return out;
    }

    // The two declarations of the table: unsized arrays of cubes and of flat textures, neither arrayed.
    bool FindTable(std::vector<uint32_t>& cubes, std::vector<uint32_t>& flats)
    {
        for (uint32_t id : VariablesAt(how_.Set, how_.Binding)) {
            auto variable = Def(id, OpVariable);
            auto pointer = variable ? Def(variable->W[0], OpTypePointer) : nullptr;
            auto array = pointer ? Def(pointer->W[2], OpTypeRuntimeArray) : nullptr;
            auto image = array ? Def(array->W[1], OpTypeImage) : nullptr;
            if (!image || image->W.size() < 8 || image->W[4] != 0 || image->W[5] != 0 || image->W[6] != 1) return Fail("it is not declared as an unsized array of textures");
            if (image->W[2] == kDimCube) cubes.push_back(id);
            else if (image->W[2] == kDimFlat) flats.push_back(id);
            else return Fail("it is declared as textures that are neither cubes nor flat");
            (image->W[2] == kDimCube ? cubeImage_ : flatImage_) = array->W[1];
        }
        if (cubes.size() > 1 || flats.size() > 1) return Fail("it is declared more than twice");
        if (cubes.empty() && flats.empty()) return true;
        for (uint32_t pseudo = 0; pseudo < 5; pseudo++) {
            if (!VariablesAt(how_.NewSet, how_.NewBindings[pseudo]).empty()) return Fail("the module has something where the Mac's textures are put");
        }
        return true;
    }

    // The values of one member of one element of a buffer, wherever the module takes them.
    std::vector<uint32_t> MemberValues(uint32_t set, uint32_t binding, uint32_t member) const
    {
        std::vector<uint32_t> out;
        for (uint32_t variable : VariablesAt(set, binding)) {
            auto it = uses_.find(variable);
            if (it == uses_.end()) continue;
            for (size_t user : it->second) {
                auto const& chain = insts_[user];
                if ((chain.Op != OpAccessChain && chain.Op != OpInBoundsAccessChain) || chain.W[2] != variable) continue;
                for (size_t load : Users(chain.W[1])) {
                    if (insts_[load].Op != OpLoad) continue;
                    if (chain.W.size() == 6 && Constant(chain.W[5]) == member) {
                        out.push_back(insts_[load].W[1]);
                    } else if (chain.W.size() == 5) {
                        for (size_t extract : Users(insts_[load].W[1])) {
                            auto const& e = insts_[extract];
                            if (e.Op == OpCompositeExtract && e.W.size() == 4 && e.W[3] == member) out.push_back(e.W[1]);
                        }
                    }
                }
            }
        }
        return out;
    }

    // d = k - 1 and what follows from it, for one k.
    bool Follow(uint32_t k, What what)
    {
        for (size_t user : Users(k)) {
            auto const& sub = insts_[user];
            if (sub.Op != OpISub || Through(sub.W[2]) != k || Constant(sub.W[3]) != 1u) return Fail("a place in it is worked out in a way that is not known");
            uint32_t d = sub.W[1];
            if (what == What::Cookie) {
                place_[d] = { what, false };
                for (size_t next : Users(d)) {
                    if (!IsIndexOrTest(insts_[next], d)) return Fail("a cookie's place is used for something else");
                }
                continue;
            }
            for (size_t next : Users(d)) {
                auto const& mul = insts_[next];
                if (mul.Op != OpIMul || Through(mul.W[2]) != d || Constant(mul.W[3]) != 2u) return Fail("a probe's place is worked out in a way that is not known");
                uint32_t x = mul.W[1];
                place_[x] = { what, false };
                layer_[x] = d;
                for (size_t last : Users(x)) {
                    auto const& add = insts_[last];
                    if (add.Op == OpIAdd && Through(add.W[2]) == x && Constant(add.W[3]) == 1u) {
                        uint32_t y = add.W[1];
                        place_[y] = { what, true };
                        layer_[y] = d;
                        for (size_t use : Users(y)) {
                            if (!IsIndexOrTest(insts_[use], y)) return Fail("a probe's place is used for something else (instruction " + std::to_string(insts_[use].Op) + ")");
                        }
                    } else if (!IsIndexOrTest(add, x)) {
                        return Fail("a probe's place is used for something else (instruction " + std::to_string(add.Op) + ")");
                    }
                }
            }
        }
        return true;
    }
    // An index into the table (looked at with the samples), or the test "not below nothing".
    bool IsIndexOrTest(Inst const& inst, uint32_t id) const
    {
        if ((inst.Op == OpAccessChain || inst.Op == OpInBoundsAccessChain) && inst.W.size() == 4 && Through(inst.W[3]) == id) return true;
        return inst.Op == OpSGreaterThanEqual && Through(inst.W[2]) == id && Constant(inst.W[3]) == 0u;
    }

    bool FindFields()
    {
        struct Half { uint32_t Shift; uint32_t Mask; What Is; };
        auto probe = [&](uint32_t member, std::initializer_list<Half> halves, bool flagsAtOne) {
            for (uint32_t value : MemberValues(how_.ProbesSet, how_.ProbesBinding, member)) {
                for (size_t user : Users(value)) {
                    auto const& inst = insts_[user];
                    if (Through(inst.W.size() > 2 ? inst.W[2] : 0) != value) return Fail("a probe's member is used in a way that is not known");
                    // The shape, and whether it has cubes at all: the same bits on the Mac.
                    if (flagsAtOne && inst.Op == OpBitwiseAnd && Constant(inst.W[3]) == 1u) continue;
                    if (flagsAtOne && inst.Op == OpShiftRightLogical && Constant(inst.W[3]) == 1u) continue;
                    bool known = false;
                    for (auto const& half : halves) {
                        if (inst.Op != OpShiftRightLogical || Constant(inst.W[3]) != half.Shift) continue;
                        for (size_t next : Users(inst.W[1])) {
                            auto const& mask = insts_[next];
                            if (mask.Op != OpBitwiseAnd || Through(mask.W[2]) != inst.W[1] || Constant(mask.W[3]) != half.Mask) {
                                return Fail("a probe's member is used in a way that is not known");
                            }
                            fields_.push_back({ half.Is, value, user, next });
                            if (!Follow(mask.W[1], half.Is)) return false;
                        }
                        known = true;
                    }
                    if (!known) return Fail("a probe's member is used in a way that is not known");
                }
            }
            return true;
        };
        if (!probe(how_.DiffuseMember, { { 3, 16383, What::DiffuseFirst }, { 17, 16383, What::DiffuseSecond } }, true)) return false;
        if (!probe(how_.SpecularMember, { { 0, 65535, What::SpecularFirst }, { 16, 65535, What::SpecularSecond } }, false)) return false;
        for (uint32_t value : MemberValues(how_.LightsSet, how_.LightsBinding, how_.CookieMember)) {
            for (size_t user : Users(value)) {
                auto const& inst = insts_[user];
                if (Through(inst.W.size() > 2 ? inst.W[2] : 0) != value) return Fail("a light's member is used in a way that is not known");
                if (inst.Op == OpBitwiseAnd && Constant(inst.W[3]) == 1u) continue;        // the light is pre-exposed: the same bit
                if (inst.Op != OpShiftRightLogical || Constant(inst.W[3]) != 1u) return Fail("a light's member is used in a way that is not known");
                fields_.push_back({ What::Cookie, value, user, SIZE_MAX });
                if (!Follow(inst.W[1], What::Cookie)) return false;
            }
        }
        return true;
    }

    struct Sample { size_t Chain, Load, Sampled, Take; What Is; bool Alpha; uint32_t Place; };

    bool FindSamples(std::vector<uint32_t> const& cubes, std::vector<uint32_t> const& flats)
    {
        auto one = [&](uint32_t id, uint16_t op) -> std::optional<size_t> {
            auto users = Users(id);
            if (users.size() != 1 || insts_[users[0]].Op != op) return {};
            return users[0];
        };
        for (bool cube : { true, false }) {
            for (uint32_t table : cube ? cubes : flats) {
                for (size_t user : uses_[table]) {
                    auto const& chain = insts_[user];
                    if (chain.Op == OpDecorate || chain.Op == OpName) continue;
                    if ((chain.Op != OpAccessChain && chain.Op != OpInBoundsAccessChain) || chain.W.size() != 4 || chain.W[2] != table) {
                        return Fail("it is used otherwise than by taking one texture out of it");
                    }
                    uint32_t place = Through(chain.W[3]);
                    auto known = place_.find(place);
                    if (known == place_.end() || (known->second.first == What::Cookie) == cube) return Fail("it is sampled at a place that is not a probe's or a cookie's");
                    auto load = one(chain.W[1], OpLoad);
                    auto sampled = load ? one(insts_[*load].W[1], OpSampledImage) : std::nullopt;
                    auto take = sampled ? one(insts_[*sampled].W[1], OpImageSampleExplicitLod) : std::nullopt;
                    if (!take || insts_[*take].W.size() < 5 || insts_[*take].W[2] != insts_[*sampled].W[1]) return Fail("a texture of it is used otherwise than by being sampled once");
                    auto const& t = insts_[*take];
                    // A cube at a level, a cookie with its two gradients: the image operands and nothing after them.
                    if (cube ? (t.W[4] != kLod || t.W.size() != 6) : (t.W[4] != kGrad || t.W.size() != 7)) return Fail("a texture of it is sampled in a way that is not known");
                    samples_.push_back({ user, *load, *sampled, *take, known->second.first, known->second.second, place });
                }
            }
        }
        return true;
    }

    uint32_t NewId() { return bound_++; }

    // A declaration before the functions: the one the module has, or a new one. `operands` is without the result.
    uint32_t Declare(uint16_t op, std::vector<uint32_t> const& operands, bool resultFirst)
    {
        for (auto const* list : { &insts_, &declared_ }) {
            for (auto const& inst : *list) {
                if (inst.Op != op || inst.W.size() != operands.size() + 1) continue;
                std::vector<uint32_t> rest = inst.W;
                rest.erase(rest.begin() + (resultFirst ? 0 : 1));
                if (rest == operands) return inst.W[resultFirst ? 0 : 1];
            }
        }
        Inst inst;
        inst.Op = op;
        inst.W = operands;
        uint32_t id = NewId();
        inst.W.insert(inst.W.begin() + (resultFirst ? 0 : 1), id);
        declared_.push_back(std::move(inst));
        return id;
    }
    uint32_t Whole(uint32_t type, uint32_t value) { return Declare(OpConstant, { type, value }, false); }

    void Before(size_t at, uint16_t op, std::vector<uint32_t> operands)
    {
        Inst inst;
        inst.Op = op;
        inst.W = std::move(operands);
        before_[at].push_back(std::move(inst));
    }
    void After(size_t at, uint16_t op, std::vector<uint32_t> operands)
    {
        Inst inst;
        inst.Op = op;
        inst.W = std::move(operands);
        after_[at].push_back(std::move(inst));
    }

    bool Change(std::vector<uint32_t> const& cubes, std::vector<uint32_t> const& flats)
    {
        // The members, read as the Mac game's engine writes them.
        std::map<std::pair<uint32_t, What>, uint32_t> arrayOf;
        for (auto& field : fields_) {
            auto& shift = insts_[field.Shift];
            uint32_t type = shift.W[0], shiftConstantType = Def(shift.W[3], OpConstant)->W[0];
            auto reshift = [&](uint32_t by) { shift.W[3] = Whole(shiftConstantType, by); };
            auto remask = [&](uint32_t with) {
                auto& mask = insts_[field.Mask];
                mask.W[3] = Whole(Def(mask.W[3], OpConstant)->W[0], with);
            };
            // The array's number: the member shifted and masked, right after the instruction that makes k.
            auto array = [&](uint32_t by, uint32_t with) {
                size_t at = field.Mask != SIZE_MAX ? field.Mask : field.Shift;
                uint32_t shifted = NewId(), id = NewId();
                After(at, OpShiftRightLogical, { type, shifted, shift.W[2], Whole(shiftConstantType, by) });
                After(at, OpBitwiseAnd, { type, id, shifted, Whole(type, with) });
                arrayOf[{ field.Member, field.Is }] = id;
                field.Array = id;
            };
            switch (field.Is) {
            case What::DiffuseFirst: reshift(3); remask(511); break;
            case What::DiffuseSecond: reshift(12); remask(0xfffff); break;
            case What::SpecularFirst: array(0, 3); reshift(2); remask(511); break;
            case What::SpecularSecond: array(11, 3); reshift(13); remask(0x7ffff); break;
            case What::Cookie: array(1, 7); reshift(4); break;
            }
        }

        // The Mac's textures.
        auto const* cubeImage = cubes.empty() ? nullptr : Def(cubeImage_, OpTypeImage);
        auto const* flatImage = flats.empty() ? nullptr : Def(flatImage_, OpTypeImage);
        uint32_t floatType = cubeImage ? cubeImage->W[1] : flatImage->W[1];
        if (!Def(floatType, OpTypeFloat)) return Fail("its textures do not hold floats");
        uint32_t lengthType = Declare(OpTypeInt, { 32, 0 }, true);
        auto arrayed = [&](Inst const& image) {
            return Declare(OpTypeImage, { image.W[1], image.W[2], image.W[3], 1, image.W[5], image.W[6], image.W[7] }, true);
        };
        auto variable = [&](uint32_t type, uint32_t which) {
            uint32_t pointer = Declare(OpTypePointer, { kUniformConstant, type }, true), id = NewId();
            Inst inst;
            inst.Op = OpVariable;
            inst.W = { pointer, id, kUniformConstant };
            declared_.push_back(std::move(inst));
            decorations_.push_back({ id, kDescriptorSet, how_.NewSet });
            decorations_.push_back({ id, kBinding, how_.NewBindings[which] });
            return id;
        };
        uint32_t cubeArray = 0, cubeArraySampled = 0, cubeArrayPointer = 0, diffuse[2] = {}, specular[2] = {};
        if (cubeImage) {
            cubeArray = arrayed(*cubeImage);
            cubeArraySampled = Declare(OpTypeSampledImage, { cubeArray }, true);
            cubeArrayPointer = Declare(OpTypePointer, { kUniformConstant, cubeArray }, true);
            uint32_t several = Declare(OpTypeArray, { cubeArray, Whole(lengthType, how_.SpecularArrays) }, true);
            diffuse[0] = variable(cubeArray, 0);
            diffuse[1] = variable(cubeArray, 1);
            specular[0] = variable(several, 2);
            specular[1] = variable(several, 3);
        }
        uint32_t flatArray = 0, flatArraySampled = 0, flatArrayPointer = 0, cookie = 0;
        if (flatImage) {
            flatArray = arrayed(*flatImage);
            flatArraySampled = Declare(OpTypeSampledImage, { flatArray }, true);
            flatArrayPointer = Declare(OpTypePointer, { kUniformConstant, flatArray }, true);
            cookie = variable(Declare(OpTypeArray, { flatArray, Whole(lengthType, how_.CookieArrays) }, true), 4);
        }
        uint32_t vector3 = Declare(OpTypeVector, { floatType, 3 }, true), vector4 = Declare(OpTypeVector, { floatType, 4 }, true);

        for (auto const& sample : samples_) {
            auto& chain = insts_[sample.Chain];
            auto& load = insts_[sample.Load];
            auto& sampled = insts_[sample.Sampled];
            auto& take = insts_[sample.Take];
            bool isCookie = sample.Is == What::Cookie, isDiffuse = sample.Is == What::DiffuseFirst || sample.Is == What::DiffuseSecond;
            // Which member this place was read from: the one whose k leads to it.
            uint32_t layer = isCookie ? sample.Place : layer_[sample.Place], array = 0;
            if (!isDiffuse) {
                for (auto const& field : fields_) {
                    if (field.Is != sample.Is) continue;
                    uint32_t k = insts_[field.Mask != SIZE_MAX ? field.Mask : field.Shift].W[1];
                    for (size_t user : Users(k)) {
                        if (insts_[user].Op == OpISub && insts_[user].W[1] == layer) array = field.Array;
                    }
                }
                if (!array) return Fail("which array a texture is in is not known");
            }
            uint32_t image = isCookie ? flatArray : cubeArray;
            if (isDiffuse) {
                // One array: the texture is the variable itself.
                removed_.insert(sample.Chain);
                load.W[2] = diffuse[sample.Alpha ? 1 : 0];
            } else {
                chain.W[0] = isCookie ? flatArrayPointer : cubeArrayPointer;
                chain.W[2] = isCookie ? cookie : specular[sample.Alpha ? 1 : 0];
                chain.W[3] = array;
            }
            load.W[0] = image;
            sampled.W[0] = isCookie ? flatArraySampled : cubeArraySampled;
            // Where it is sampled, and the layer after it.
            uint32_t layerAsFloat = NewId(), where = NewId();
            Before(sample.Take, OpConvertSToF, { floatType, layerAsFloat, layer });
            Before(sample.Take, OpCompositeConstruct, { isCookie ? vector3 : vector4, where, take.W[3], layerAsFloat });
            take.W[3] = where;
            if (!isCookie && !isDiffuse) {
                // The game's own function samples the array numbered a at the level less a.
                uint32_t arrayAsFloat = NewId(), level = NewId();
                Before(sample.Take, OpConvertSToF, { floatType, arrayAsFloat, array });
                Before(sample.Take, OpFSub, { floatType, level, take.W[5], arrayAsFloat });
                take.W[5] = level;
            }
        }

        // What was said of an instruction that is gone goes with it.
        for (size_t i = 0; i < insts_.size(); i++) {
            if (insts_[i].Op != OpDecorate) continue;
            auto target = def_.find(insts_[i].W[0]);
            if (target != def_.end() && removed_.contains(target->second)) removed_.insert(i);
        }
        // The table itself goes: nothing takes a texture out of it any more.
        for (size_t i = 0; i < insts_.size(); i++) {
            auto const& inst = insts_[i];
            bool table = false;
            for (auto const* list : { &cubes, &flats }) {
                for (uint32_t id : *list) {
                    table = table || (inst.Op == OpVariable && inst.W[1] == id) || ((inst.Op == OpDecorate || inst.Op == OpName) && inst.W[0] == id);
                }
            }
            if (table) removed_.insert(i);
        }
        return true;
    }

    void Write()
    {
        std::vector<uint32_t> out(words_.begin(), words_.begin() + 5);
        out[3] = bound_;
        auto put = [&](Inst const& inst) {
            out.push_back((uint32_t)(inst.W.size() + 1) << 16 | inst.Op);
            out.insert(out.end(), inst.W.begin(), inst.W.end());
        };
        // Decorations go with the module's own, before its first type; declarations right before its first function.
        size_t firstType = firstFunction_;
        for (size_t i = 0; i < firstFunction_; i++) {
            if (insts_[i].Op >= 19 && insts_[i].Op <= 33) { firstType = i; break; }
        }
        for (size_t i = 0; i < insts_.size(); i++) {
            if (i == firstType) {
                for (auto const& d : decorations_) put({ OpDecorate, { d[0], d[1], d[2] }, -1, {} });
            }
            if (i == firstFunction_) {
                for (auto const& inst : declared_) put(inst);
            }
            if (auto it = before_.find(i); it != before_.end()) for (auto const& inst : it->second) put(inst);
            if (!removed_.contains(i)) put(insts_[i]);
            if (auto it = after_.find(i); it != after_.end()) for (auto const& inst : it->second) put(inst);
        }
        words_ = std::move(out);
    }

    std::vector<uint32_t>& words_;
    TextureTableRewrite const& how_;
    std::string& why_;
    uint32_t bound_{ 0 };
    std::vector<Inst> insts_;
    size_t firstFunction_{ SIZE_MAX };
    std::map<uint32_t, size_t> def_;
    std::map<uint32_t, std::vector<size_t>> uses_;
    std::map<uint32_t, uint32_t> set_, binding_;
    uint32_t cubeImage_{ 0 }, flatImage_{ 0 };
    std::vector<Field> fields_;
    std::map<uint32_t, std::pair<What, bool>> place_;      // a place in the table: whose, and whether it is the alpha's
    std::map<uint32_t, uint32_t> layer_;                   // a probe's place: its d
    std::vector<Sample> samples_;
    std::vector<Inst> declared_;
    std::vector<std::array<uint32_t, 3>> decorations_;
    std::map<size_t, std::vector<Inst>> before_, after_;
    std::set<size_t> removed_;
};

}

TextureTableResult RewriteTextureTable(std::vector<uint32_t>& words, TextureTableRewrite const& how, std::string& why)
{
    return Rewrite(words, how, why).Run();
}

}
