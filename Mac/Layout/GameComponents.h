#pragma once

#include <cstddef>

// Members of six component objects, of a timeline component and of the network peer, where the Mac
// game's own code shows them (Layout/GameShapes.cpp says which code).
// Norbyte's declarations are asserted to give these offsets
// (Layout/DefinitionChecks.cpp). The sizes of the components are compared
// with the game's registrations separately (Layout/ComponentRegistry.h);
// these say which member is where.

namespace bg3se::mac {

// esv::SoundVolumeTrigger, esv::TimelineSceneTrigger, ecl::TimelineSceneTrigger:
// the entity right after the trigger's base.
constexpr size_t kTriggerEntity = 0xa8;

// esv::Projectile: eight handles right after the vtable pointer, then
// positions, then the hit.
constexpr size_t kProjectileEntity = 0x8;
constexpr size_t kProjectileHit = 0x70;

// ls::Construction: two arrays, then the instance's id.
constexpr size_t kConstructionInstanceId = 0x20;

// eoc::SyncedTimelineActorControlComponent: two ids, two bytes, and a word
// after two bytes of padding.
constexpr size_t kActorControlTimeline = 0x10;
constexpr size_t kActorControlSlot = 0x20;
constexpr size_t kActorControlListener = 0x21;
constexpr size_t kActorControlWord = 0x24;
constexpr size_t kActorControlSize = 0x28;

// eoc::character_creation::CompanionDefinitionComponent: a definition of
// 0x138 bytes, then the icon's version and whether there is one.
constexpr size_t kCompanionIconVersion = 0x138;
constexpr size_t kCompanionHasIcon = 0x13c;

// ls::TLShot (aspk::TLShotComponent), a timeline component: the base's last
// two bytes, the shot's first member in the padding after them, a byte
// after the three camera ids, and the map that ends the object.
constexpr size_t kTimelineComponentLastBytes = 0x60;
constexpr size_t kShotSwitchInterval = 0x64;
constexpr size_t kShotLogicEnabled = 0xb8;
constexpr size_t kShotPrefetchData = 0xf8;
constexpr size_t kShotSize = 0x138;

// net::AbstractPeer: the message factory, the list of protocols and the map
// of them by id.
constexpr size_t kPeerMessageFactory = 0x1f8;
constexpr size_t kPeerProtocols = 0x2d0;        // array: pointer, capacity at +8, size at +0xc
constexpr size_t kPeerProtocolMap = 0x2e0;      // hash map: buckets, their count at +8, next at +0x10, keys at +0x20

// net::MessageFactory: the pools by message id, and the lock.
constexpr size_t kFactoryPools = 0x8;           // the array's buffer; the array object begins at 0
constexpr size_t kFactoryLock = 0x20;

// net::MessagePool: the message it makes copies of, how many at a time, the
// free messages and those handed out.
constexpr size_t kPoolTemplate = 0x0;
constexpr size_t kPoolGrowSize = 0x8;
constexpr size_t kPoolMessages = 0x10;          // queue: buffer, capacity at +8, count at +0xc, read index at +0x10, write index at +0x14
constexpr size_t kPoolLeased = 0x28;            // array: pointer, capacity at +8, size at +0xc
constexpr size_t kPoolSize = 0x38;

// net::Message: the id and reliability, the priority, two bytes, the
// timestamp and the two words after it; and its CreateInstance slot.
constexpr size_t kMessageId = 0x8;
constexpr size_t kMessagePriority = 0x10;
constexpr size_t kMessageOrdering = 0x14;
constexpr size_t kMessageTimestamp = 0x18;
constexpr size_t kMessageSize = 0x28;
constexpr size_t kMessageCreateSlot = 0x20;
constexpr uint32_t kMessageDefaultReliability = 4;
constexpr uint32_t kMessageDefaultPriority = 1;
constexpr uint32_t kClientConnectMessageId = 6;

// net::BitStreamSerializer (a net::MessageVisitor): non-zero when writing,
// the stream, and the slots of its write and read functions.
constexpr size_t kSerializerIsWriting = 0x8;
constexpr size_t kSerializerStream = 0x10;
constexpr size_t kSerializerWriteSlot = 0x10;
constexpr size_t kSerializerReadSlot = 0x18;

}
