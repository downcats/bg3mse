#pragma once

#include <GameDefinitions/Base/Base.h>

BEGIN_SE()

struct EntityManager;

struct LevelDesc : public ProtectedGameObject<LevelDesc>
{
    [[bg3::hidden]] void* VMT;
    FixedString LevelName;
    FixedString UniqueKey;
    Array<Path> Paths;
    uint8_t Type;
};

struct LevelMetaData : public ProtectedGameObject<LevelMetaData>
{
    [[bg3::hidden]] void* VMT;
    AABound Bounds;
    FixedString LevelTemplateBoundTrigger;
    FixedString Atmosphere;
    FixedString Lighting;
    bool VisualStreamingEnabled;
    bool IsPlatformLevel;
    float CombatCameraFadeDistance;
};

struct [[bg3::hidden]] LevelLoadState
{
    int RefCount;
    int State;
    void* LevelDataLoader;
};

struct LevelDataManager : public ProtectedGameObject<LevelDataManager>
{
    [[bg3::hidden]] void* VMT;
    HashMap<FixedString, LevelData*> Levels;
};

#if !defined(_MSC_VER)
// The first template of each name (LevelData::TrackedTemplates), in a map
// kept for the level data and the thread that asks: what a script holds of
// it stays that level's, and what it writes there does not reach the game.
inline HashMap<FixedString, GameObjectTemplate*>* FirstTemplates(void const* owner, HashMap<FixedString, Array<GameObjectTemplate*>> const& lists)
{
    thread_local std::unordered_map<void const*, HashMap<FixedString, GameObjectTemplate*>> maps;
    auto& first = maps[owner];
    first.clear();
    for (uint32_t i = 0; i < lists.size(); i++) {
        auto const& templates = lists.values()[i];
        if (templates.size() > 0) first.set(lists.keys()[i], templates[0]);
    }
    return &first;
}

#endif
struct LevelData : public ProtectedGameObject<LevelData>
{
    LevelDesc* LevelDesc;
    LevelMetaData* Meta;
    [[bg3::hidden]] LevelDataManager* LevelDataManager;
#if defined(_MSC_VER)
    HashMap<FixedString, GameObjectTemplate*> TrackedTemplates;
#else
    // The macOS game keeps an array of templates for each name
    // (Mac/Layout/GameGlobals.h). A script written for Windows reads
    // TrackedTemplates here too, as the first template of each name; the
    // arrays themselves are TrackedTemplateLists.
    HashMap<FixedString, Array<GameObjectTemplate*>> TrackedTemplateLists;
    //# P_STAND_IN_GETTER(TrackedTemplates, GetTrackedTemplates)
    HashMap<FixedString, GameObjectTemplate*>* GetTrackedTemplates() const
    {
        return FirstTemplates(this, TrackedTemplateLists);
    }
#endif
    [[bg3::hidden]] CRITICAL_SECTION CriticalSection;
    [[bg3::hidden]] LegacyRefMap<FixedString, LevelLoadState> LoadStates;
    uint32_t State;
    HashSet<FixedString> Dependencies;
};


struct [[bg3::hidden]] LevelBase : public ProtectedGameObject<LevelBase>
{
    void* VMT;
    LevelManager* LevelManager;
    void* LevelAllocator;
    LevelData* LevelData;
    LocalTemplateManager* LocalTemplateManager;
    Scene* Scene;
    phx::PhysicsSceneBase* PhysicsScene;
    FixedString LevelGuid;
};

struct ActivePersistentLevelTemplate
{
    FixedString SubLevelName;
    FixedString LevelInstanceID;
    EntityHandle Entity;
    uint64_t field_10{ 0 };
    AABound Bound;
};

struct TemplateTrace
{
    FixedString field_0;
    FixedString field_4;
};

struct [[bg3::hidden]] LSLevel : public LevelBase
{
    EntityHandle Root;
    uint32_t field_48;
    TrackedCompactSet<ActivePersistentLevelTemplate> ActiveLevelTemplates;
    Array<TemplateTrace> LocalTemplateTraces;
    // void* LayerManager;
    uint32_t LoadState;
};


struct [[bg3::hidden]] EoCLevel : public LSLevel
{
    void* EntityManager;
    AiGrid* AiGrid;
    void* CoverManager;
};

struct [[bg3::hidden]] LevelManagerBase
{
    uint64_t Unknown[0x10];
};

struct [[bg3::hidden]] LevelManager : public ProtectedGameObject<LevelManager>
{
    void* VMT;
    LevelManagerBase b;
    LocalTemplateManager* LocalTemplateManager;
    EoCLevel* CurrentLevel;
    LegacyMap<FixedString, EoCLevel*> Levels;
    void* field_B0;
};

END_SE()

BEGIN_NS(esv)

struct [[bg3::hidden]] Level : public EoCLevel
{
    void* SightManager;
    void* CharacterManager;
    void* ItemManager;
    SurfaceManager* SurfaceManager;
    void* GameActionManager;
    void* EffectManager;
    void* ShroudManager;
    void* LightingOverrides;
    void* AtmosphereOverrides;
    void* EnvironmentalStatusManager;
    void* StaticPhysicsManager;
    LevelCacheTemplateManager* CacheTemplateManager;
};

struct [[bg3::hidden]] LevelManager : public bg3se::LevelManager
{
    Array<void*> field_B8;
    ecs::EntityWorld* EntityWorld;
};

END_NS()

BEGIN_NS(ecl)

struct [[bg3::hidden]] Level : public EoCLevel
{
    // TODO
};

struct [[bg3::hidden]] LevelManager : public bg3se::LevelManager
{
};

END_NS()
