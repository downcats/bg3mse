#pragma once

#include <cstddef>

#include "GameResourceManager.h"

// Members of the game's structures that the engine table's operand checks
// (Engine/Entries) and the call adapters (Binding/CallAdapters.h) name by
// offset, where the structure is also one Norbyte declares and his code
// reaches the member through his declaration.
//
// The table and the adapters state each offset with the constant here, and
// the resolver compares it with the game's code. Norbyte's declaration is
// asserted to put the member at the same constant
// (Layout/DefinitionChecks.cpp, Layout/OsirisDefinitionChecks.cpp), so a
// declaration of his that disagrees with the game's operand does not build.

namespace bg3se::mac {

// ls::_StringView<char>: LSStringView, the characters and then the length.
constexpr size_t kStringViewLength = 0x08;

// ls::Path (GameDefinitions/FileReader.h): the last byte of its text, a
// string. The text is in the path itself unless that byte is negative, then
// behind the pointer the path starts with.
constexpr size_t kGamePathTextFlag = 0x0f;

// ls::TextureAtlasManager: TextureAtlasMap (GameDefinitions/Resources.h).
constexpr size_t kAtlasMapItemCount = 0x08;
constexpr size_t kIconMapHashSize = 0x18, kIconMapHashTable = 0x20, kIconMapItemCount = 0x28;

// eoc::Passives: stats::PassivePrototypeManager (GameDefinitions/Stats/Prototype.h), its map and a node of it.
constexpr size_t kPassivesMapHashSize = 0x0c, kPassivesMapHashTable = 0x10;
constexpr size_t kPassiveNodeKey = 0x08;

// ls::Function<F>: Norbyte's Function (CoreLib/Base/BaseFunction.h). A
// pointer to the object's own methods, null while it holds nothing; the
// three methods; then what the function captured.
constexpr size_t kFunctionMethods = 0x00;
constexpr size_t kFunctionCall = 0x08, kFunctionCopy = 0x10, kFunctionMoveDestroy = 0x18;
constexpr size_t kFunctionCaptures = 0x20;
constexpr size_t kFunctionSize = 0x40;

// ecl::StatusVFX's subclasses (GameDefinitions/Status.h): the effect handler
// of StatusVFXEffect, StatusVFXApplyEffect and StatusVFXBeam (the beam's two
// flags follow it), that of ManagedStatusVFX, and StatusVFXBoostMaterial's
// Created.
constexpr size_t kStatusVfxEffectHandler = 0x18, kStatusVfxBeamFlags = 0x138;
constexpr size_t kManagedStatusVfxEffectHandler = 0x28;
constexpr size_t kStatusVfxBoostMaterialCreated = 0x20;

// ls::GlobalTemplateManager (GameDefinitions/RootTemplates.h): its two banks.
constexpr size_t kTemplateManagerBanks = 0x20;

// ls::Visual (GameDefinitions/Render.h): BlendShapeBase, 16 bits.
constexpr size_t kVisualBlendShapeBase = 0x102;

// eoc::AiGrid (GameDefinitions/Ai.h): PathMap, and a node of it.
constexpr size_t kAiGridPathBuckets = 0xf4;     // 32-bit count
constexpr size_t kAiGridPathTable = 0xf8;       // the buckets: pointers to nodes
constexpr size_t kAiPathNodeNext = 0x0, kAiPathNodeId = 0x8, kAiPathNodePath = 0x10;

// ls::Resource, ls::LoadableResource and ls::TextureResource (GameDefinitions/Resources.h).
constexpr size_t kGameResourceSourceFile = 0x08, kGameResourceGuid = 0x18;
constexpr size_t kLoadableResourceState = 0x20;
constexpr size_t kTextureResourceStreaming = 0x34, kTextureResourceSRGB = 0x35, kTextureResourceType = 0x36;

// ls::InputRawChange (GameDefinitions/Input.h): Value.State.
constexpr size_t kInputChangeState = 0x0c;

}
