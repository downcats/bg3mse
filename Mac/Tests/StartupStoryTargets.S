// The story library's side of the story debugger test
// (Mac/Tests/StartupStoryDebugger.cpp): the functions of the library that
// Norbyte's story layer hooks, as functions of this image's own that his
// hooks go on the way they go on the library's. Each passes its arguments on
// to the test's handler of that function (st_handlers) and returns what it
// returns. They are on a page of their own section, which a hook's write
// swaps alone.

    .section __TEXT,__storytests,regular,pure_instructions
    .p2align 14

.macro STAND_IN name, index
    .globl \name
    .p2align 2
\name:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    adrp x16, _st_handlers@PAGE
    add x16, x16, _st_handlers@PAGEOFF
    ldr x16, [x16, #(\index * 8)]
    blr x16
    ldp x29, x30, [sp], #16
    ret
.endm

// COsiris::RegisterDIVFunctions(TOsirisInitFunction*)
STAND_IN _st_register_div_functions, 0
// COsiris::Load(COsiSmartBuf&)
STAND_IN _st_load, 1
// COsiris::InitGame()
STAND_IN _st_init_game, 2
// COsiris::DeleteAllData(bool)
STAND_IN _st_delete_all_data, 3
// COsiFunctionParseData::Fire(): a rule's or a goal's action is carried out
STAND_IN _st_fire, 4
