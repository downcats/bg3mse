// The game's side of the hook tests (Mac/Tests/HookTests.cpp): a caller that
// passes registers and a stack block exactly as the test set them, and a
// function that records what arrives, in assembly so that nothing between the
// two decides how an argument travels. The function is on a page of its own
// section, which a hook's write swaps alone.

    .text
    .p2align 2

// ht_call(target, registers, stack, results): calls target with x0 to x8
// taken from registers[0..8] and the 0x60 bytes at `stack` at the callee's
// [sp], and stores the x0 that comes back in results[0].
    .globl _ht_call
_ht_call:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x3
    sub sp, sp, #0x60
    ldp x9, x10, [x2]
    stp x9, x10, [sp]
    ldp x9, x10, [x2, #0x10]
    stp x9, x10, [sp, #0x10]
    ldp x9, x10, [x2, #0x20]
    stp x9, x10, [sp, #0x20]
    ldp x9, x10, [x2, #0x30]
    stp x9, x10, [sp, #0x30]
    ldp x9, x10, [x2, #0x40]
    stp x9, x10, [sp, #0x40]
    ldp x9, x10, [x2, #0x50]
    stp x9, x10, [sp, #0x50]
    mov x16, x0
    mov x17, x1
    ldp x0, x1, [x17]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
    ldr x8, [x17, #64]
    blr x16
    str x0, [x19]
    add sp, sp, #0x60
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

    .section __TEXT,__hooktests,regular,pure_instructions
    .p2align 14

// A stand-in for a hooked game function: records x0 to x8 in ht_seen and the
// 0x60 bytes at its [sp] in ht_stack, counts the call, and returns ht_return
// in x0.
    .globl _ht_record
_ht_record:
    adrp x16, _ht_seen@PAGE
    add x16, x16, _ht_seen@PAGEOFF
    stp x0, x1, [x16]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    str x8, [x16, #64]
    adrp x16, _ht_stack@PAGE
    add x16, x16, _ht_stack@PAGEOFF
    ldp x9, x10, [sp]
    stp x9, x10, [x16]
    ldp x9, x10, [sp, #0x10]
    stp x9, x10, [x16, #0x10]
    ldp x9, x10, [sp, #0x20]
    stp x9, x10, [x16, #0x20]
    ldp x9, x10, [sp, #0x30]
    stp x9, x10, [x16, #0x30]
    ldp x9, x10, [sp, #0x40]
    stp x9, x10, [x16, #0x40]
    ldp x9, x10, [sp, #0x50]
    stp x9, x10, [x16, #0x50]
    adrp x16, _ht_calls@PAGE
    add x16, x16, _ht_calls@PAGEOFF
    ldr x17, [x16]
    add x17, x17, #1
    str x17, [x16]
    adrp x16, _ht_return@PAGE
    add x16, x16, _ht_return@PAGEOFF
    ldr x0, [x16]
    ret
