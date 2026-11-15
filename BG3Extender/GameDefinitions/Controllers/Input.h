#pragma once

#include <GameDefinitions/Controllers/Controller.h>
#include <GameDefinitions/Controllers/CharacterTask.h>

BEGIN_NS(ecl)

struct InputController : public BaseController
{
#if defined(_MSC_VER)
    virtual uint16_t& OnInputEvent(uint16_t& result, input::InputEvent const& e) = 0;
    virtual bool IsActive() = 0;
    virtual CharacterTask* GetCharacterTask(uint32_t type) = 0;
    virtual bool TryForceClearPreviewTask() = 0;
    virtual bool SetRunningTask(CharacterTask*, bool forceClear) = 0;
    virtual bool IsDragging() = 0;
    virtual EntityHandle& FetchPressedObject(EntityHandle&) = 0;
    virtual void InitializeTasks() = 0;
#else
    // The macOS game's: the result of an input event and the pressed object's
    // entity handle come back in a register (Mac/Layout/DeclaredVirtuals.cpp).
    virtual uint16_t OnInputEvent(input::InputEvent const& e) = 0;
    virtual bool IsActive() const = 0;
    virtual CharacterTask* GetCharacterTask(uint32_t type) = 0;
    virtual bool TryForceClearPreviewTask() = 0;
    virtual bool SetRunningTask(CharacterTask*, bool forceClear) = 0;
    virtual bool IsDragging() = 0;
    virtual uint64_t FetchPressedObject() = 0;
    virtual void InitializeTasks() = 0;
#endif

    bool Active;
    bool field_19;
    float MouseHoldTime;
    bool IsInSelectorMode_M;
    bool IsMousePressed;
    bool IsMouseHold;
    bool IsMouseHoldMoving;
    glm::vec2 MousePos;
    Array<CharacterTask*> Tasks;
    EntityHandle DragDropEntity;
    InputControllerFlags Flags;
    CharacterTask* RunningTask;
    CharacterTask* PreviewTask;
    InputControllerRequestFlags RequestFlags;
    bool IsNewTaskStarted;
    [[bg3::hidden]] uint32_t FallSync[8];
    [[bg3::hidden]] UnknownFunction SignalConnections;
#if 0 // Editor only
    [[bg3::hidden]] __int64 field_C0;
#endif
    CursorType UICursorRunning;
    CursorType UICursorPreview;
    [[bg3::hidden]] void* OptionManager;
};

END_NS()
