// Stand-ins and callers for the binding tests, in assembly so that what a
// function receives in each register can be recorded as it arrives and what a
// caller passes is exactly what the test set.

    .text
    .p2align 2

// A stand-in for a game function: records x0 to x8 in bt_seen, counts the
// call, and returns bt_return[0] in x0 and bt_return[1] in x1.
    .globl _bt_record
_bt_record:
    adrp x16, _bt_seen@PAGE
    add x16, x16, _bt_seen@PAGEOFF
    stp x0, x1, [x16]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    str x8, [x16, #64]
    adrp x16, _bt_calls@PAGE
    add x16, x16, _bt_calls@PAGEOFF
    ldr x17, [x16]
    add x17, x17, #1
    str x17, [x16]
    adrp x16, _bt_return@PAGE
    add x16, x16, _bt_return@PAGEOFF
    ldp x0, x1, [x16]
    ret

// The same for a function that builds its result at the address in x8:
// bt_return[0] is stored there.
    .globl _bt_record_indirect
_bt_record_indirect:
    adrp x16, _bt_seen@PAGE
    add x16, x16, _bt_seen@PAGEOFF
    stp x0, x1, [x16]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    str x8, [x16, #64]
    adrp x16, _bt_calls@PAGE
    add x16, x16, _bt_calls@PAGEOFF
    ldr x17, [x16]
    add x17, x17, #1
    str x17, [x16]
    adrp x16, _bt_return@PAGE
    add x16, x16, _bt_return@PAGEOFF
    ldr x17, [x16]
    str x17, [x8]
    ret

// bt_call(target, registers, results): calls target with x0 to x8 taken from
// registers[0..8] and stores what comes back in x0 and x1.
    .globl _bt_call
_bt_call:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x2
    mov x16, x0
    mov x17, x1
    ldp x0, x1, [x17]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
    ldr x8, [x17, #64]
    blr x16
    stp x0, x1, [x19]
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

// A function with one call in it, for the call site tests, on a page of its
// own section so that a write of it swaps nothing else.
    .section __TEXT,__binding,regular,pure_instructions
    .p2align 14
    .globl _bt_host
_bt_host:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    .globl _bt_host_call
_bt_host_call:
    bl _bt_callee
    .globl _bt_host_not_a_call
_bt_host_not_a_call:
    add w0, w0, #100
    ldp x29, x30, [sp], #16
    ret

    .globl _bt_callee
_bt_callee:
    add w0, w0, #1
    ret

    .globl _bt_other_callee
_bt_other_callee:
    add w0, w0, #2
    ret
    .p2align 14

// Two functions that say 1, for the code patch tests: the first instruction
// of each is the one replaced.
    .globl _bt_says_one
_bt_says_one:
    mov w0, #1
    ret
    .globl _bt_says_one_too
_bt_says_one_too:
    mov w0, #1
    ret
