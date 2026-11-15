#include <stdafx.h>
#include <GameDefinitions/Symbols.h>

#include <Binding/Layouts.h>

// What leads Norbyte's code to the objects of each game class whose virtual
// functions the slot check covers (Binding/Layouts.h, Layout/VirtualSlots.h).

BEGIN_NS(mac::binding)

namespace {

static_assert(std::is_same_v<decltype(StaticSymbols::ecl__EoCClient), ecl::EoCClient**>);
static_assert(std::is_same_v<decltype(StaticSymbols::esv__EoCServer), esv::EoCServer**>);
static_assert(std::is_same_v<decltype(StaticSymbols::gRPGStats), stats::RPGStats**>);

constexpr VirtualDependent kVirtualDependents[] = {
    // The visitor the game hands the extender when a save is written or
    // read: the persistent variables, user and mod variables and timers are
    // visited through it from the hook on the Osiris variable helper's visit.
    { "ObjectVisitor", "esv__OsirisVariableHelper__SavegameVisit", nullptr },
    // What his code reaches from everywhere through game objects it reads
    // without a test (components, effects, the entity replication, resources,
    // templates, statuses, the controllers of characters, surfaces,
    // timelines), mostly where it makes a Lua object of the right subclass:
    // start-up refuses.
    { "MoveableObject", nullptr, nullptr, false, true },
    { "BasicModel", nullptr, nullptr, false, true },
    { "Model", nullptr, nullptr, false, true },
    { "ecs::FieldTracker", nullptr, nullptr, false, true },
    { "resource::Resource", nullptr, nullptr, false, true },
    { "resource::LoadableResource", nullptr, nullptr, false, true },
    { "resource::PhysicsObject", nullptr, nullptr, false, true },
    { "GameObjectTemplate", nullptr, nullptr, false, true },
    { "aspk::Component", nullptr, nullptr, false, true },
    { "aspk::Property", nullptr, nullptr, false, true },
    { "aspk::KeyFrameData", nullptr, nullptr, false, true },
    { "TextKeyTypeProperties", nullptr, nullptr, false, true },
    { "gn::GenomeNodeTemplate", nullptr, nullptr, false, true },
    { "gn::TGenomeSet", nullptr, nullptr, false, true },
    { "esv::ActionState", nullptr, nullptr, false, true },
    { "esv::BehaviourState", nullptr, nullptr, false, true },
    { "esv::MovementState", nullptr, nullptr, false, true },
    { "esv::SteeringState", nullptr, nullptr, false, true },
    { "esv::SurfaceAction", nullptr, nullptr, false, true },
    { "esv::Status", nullptr, nullptr, false, true },
    { "ecl::Status", nullptr, nullptr, false, true },
    { "spell_cast::BaseTarget", nullptr, nullptr, false, true },
    { "spell_cast::InitialTarget", nullptr, nullptr, false, true },
    // NoesisGUI's objects, reached through the game UI and its components.
    { "Noesis::BaseObject", nullptr, nullptr, false, true },
    { "Noesis::BaseRefCounted", nullptr, nullptr, false, true },
    { "Noesis::Visual", nullptr, nullptr, false, true },
    { "Noesis::FrameworkElement", nullptr, nullptr, false, true },
    { "Noesis::TypeProperty", nullptr, nullptr, false, true },
    { "Noesis::BoxedValue", nullptr, nullptr, false, true },
    { "Noesis::BaseCommand", nullptr, nullptr, false, true },
    { "Noesis::ICommand", nullptr, nullptr, false, true },
    { "Noesis::INotifyPropertyChanged", nullptr, nullptr, false, true },
    // The handlers of routed events: his code adds them to an element it
    // was given by Ext.UI, whose roots are shut without these.
    { "Noesis::Delegate", "Noesis__Reflection__Data", nullptr },
    { "Noesis::Delegate", "Noesis__SymbolManager__Data", nullptr },
    // The input manager's listeners: Ext.Input.GetInputManager hands the
    // manager that keeps them to Lua.
    { "InputEventListener", "ls__gInputManager", nullptr },
    // The camera controller, through the camera component's member; the
    // client's input controller and its character tasks, through the client
    // character's.
    { "rf::CameraController", nullptr, "ls::CameraComponent" },
    { "ecl::InputController", nullptr, "ecl::Character" },
    { "ecl::CharacterTask", nullptr, "ecl::Character" },
    { "ecl::CharacterMoveTask", nullptr, "ecl::Character" },
    // The template cache and the expression manager are read through their
    // members without a test (RootTemplates.inl, LuaGet.inl).
    { "CacheTemplateManagerBase", nullptr, nullptr, false, true },
    { "DynamicStatsExpressionManager", nullptr, nullptr, false, true },
    // The GUID resource banks, through the resource manager Ext.StaticData
    // tests for.
    { "resource::GuidResourceBankBase", "eoc__gGuidResourceManager", nullptr },
    // The effect handler's helper, through the client's effect handler system.
    { "ecl::effect::HandlerSystemHelper", nullptr, "ecl::effect::HandlerSystem" },
    // The functors, his and the game's, are kept in the stats object.
    { "stats::Functor", "gRPGStats", nullptr },
    // The sound manager is reached through the resource manager's member,
    // the physics scene through the current level: his code asks where it
    // takes them (ClientAudio.inl, Level.inl).
    { "SoundManager", nullptr, nullptr, true },
    { "phx::PhysicsSceneBase", nullptr, nullptr, true },
    // The network: the peers, their protocols, the messages and the
    // serializer are reached through the client and server objects.
    { "net::AbstractPeerBase", "ecl__EoCClient", nullptr },
    { "net::AbstractPeerBase", "esv__EoCServer", nullptr },
    { "net::Protocol", "ecl__EoCClient", nullptr },
    { "net::Protocol", "esv__EoCServer", nullptr },
    { "net::Message", "ecl__EoCClient", nullptr },
    { "net::Message", "esv__EoCServer", nullptr },
    { "net::BitstreamSerializer", "ecl__EoCClient", nullptr },
    { "net::BitstreamSerializer", "esv__EoCServer", nullptr },
    // The stats object's managers.
    { "stats::CNamedElementManager", "gRPGStats", nullptr },
    // The channels of a timeline's material and reflection parameters, read
    // through the effects' and the client timeline system's timelines.
    { "aspk::TLMaterialComponent::Parameter", nullptr, "ls::EffectComponent" },
    { "aspk::TLMaterialComponent::Parameter", nullptr, "ecl::TimelineSystem" },
    { "aspk::TimelineActorPropertiesReflectionComponent::Parameter", nullptr, "ls::EffectComponent" },
    { "aspk::TimelineActorPropertiesReflectionComponent::Parameter", nullptr, "ecl::TimelineSystem" },
};

}

std::span<VirtualDependent const> VirtualDependents()
{
    return kVirtualDependents;
}

END_NS()
