// A dylib of its own for Mac/Tests/CrashTests.cpp: code that is in no image
// of the crash reporter's, as the game's code is not.

    .text

// crash_foreign_call(pair): calls pair[0](pair[1]) and returns 0. It can be a
// thread's start routine, which makes a thread none of whose frames are the
// test's.
    .p2align 2
    .globl _crash_foreign_call
_crash_foreign_call:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    ldr     x16, [x0]
    ldr     x0, [x0, #8]
    blr     x16
    mov     x0, #0
    ldp     x29, x30, [sp], #16
    ret

// Reads 8 bytes at address 0x10.
    .p2align 2
    .globl _crash_foreign_read_null
_crash_foreign_read_null:
    mov     x8, #0x10
    ldr     x0, [x8]
    ret
