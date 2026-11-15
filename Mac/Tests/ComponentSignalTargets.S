// Stand-ins for the component signal tests.
//
// The four senders are the macOS game's
// ecs::ComponentOps<T>::SendComponentAttachedSignal and
// SendComponentDetachedSignal (game build 4.1.1.7631656), instruction for
// instruction, in both forms: they are what calls a subscriber in the game,
// and the tests call Norbyte's subscribers through them. They have no operand
// that depends on where they are, so the check of the game's code
// (Mac/Binding/ComponentSignals.h) takes them for the game's.
//
//     sender(ops, begin, end)
//
// x0: the component operations (the callbacks at +0x10, the world at +0x20);
// x1, x2: iterators of a paged array of sixteen-byte entries (the pages at
// +0, the index at +8, the page shift at +0xc), an entry being an entity's
// handle and its component, or for the boxed form the address of where its
// component's address is. For each entry, each connection of the signal
// (0x48 bytes; `connections` and `count` say where the callbacks keep the
// array) is called through the first pointer of what its first pointer
// points to: its storage in x0, the handle in x1, the world in x2, the
// component in x3.

    .text

.macro SENDER name, connections, count, boxed
    .globl \name
    .p2align 2
\name:
    sub     sp, sp, #0x70
    stp     x28, x27, [sp, #0x10]
    stp     x26, x25, [sp, #0x20]
    stp     x24, x23, [sp, #0x30]
    stp     x22, x21, [sp, #0x40]
    stp     x20, x19, [sp, #0x50]
    stp     x29, x30, [sp, #0x60]
    add     x29, sp, #0x60
    ldr     w24, [x1, #0x8]
    ldr     w8, [x2, #0x8]
    cmp     w24, w8
    b.ne    1f
0:
    ldp     x29, x30, [sp, #0x60]
    ldp     x20, x19, [sp, #0x50]
    ldp     x22, x21, [sp, #0x40]
    ldp     x24, x23, [sp, #0x30]
    ldp     x26, x25, [sp, #0x20]
    ldp     x28, x27, [sp, #0x10]
    add     sp, sp, #0x70
    ret
1:
    mov     x19, x2
    mov     x20, x0
    ldr     x9, [x1]
    str     x9, [sp, #0x8]
    ldrh    w26, [x1, #0xc]
    mov     w9, #-0x1
    lsl     w9, w9, w26
    mvn     w27, w9
    b       3f
2:
    add     w24, w24, #0x1
    cmp     w24, w8
    b.eq    0b
3:
    ldr     x9, [x20, #0x10]
    ldr     x28, [x9, #\connections]
    ldrsw   x9, [x9, #\count]
    cmp     x28, #0x0
    ccmp    w9, #0x0, #0x4, ne
    b.eq    2b
    asr     w8, w24, w26
    ldr     x10, [sp, #0x8]
    ldr     x8, [x10, w8, sxtw #3]
    and     w10, w24, w27
    add     x8, x8, w10, sxtw #4
.if \boxed
    ldp     x21, x10, [x8]
    ldr     x22, [x10]
.else
    ldp     x22, x21, [x8]
.endif
    ldr     x23, [x20, #0x20]
    mov     w8, #0x48
    madd    x25, x9, x8, x28
4:
    add     x0, x28, #0x8
    ldr     x8, [x28], #0x48
    ldr     x8, [x8]
.if \boxed
    mov     x1, x21
    mov     x2, x23
    mov     x3, x22
.else
    mov     x1, x22
    mov     x2, x23
    mov     x3, x21
.endif
    blr     x8
    cmp     x28, x25
    b.ne    4b
    ldr     w8, [x19, #0x8]
    b       2b
.endm

    SENDER _cst_send_attached, 0x10, 0x1c, 0
    SENDER _cst_send_detached, 0x28, 0x34, 0
    SENDER _cst_send_attached_boxed, 0x10, 0x1c, 1
    SENDER _cst_send_detached_boxed, 0x28, 0x34, 1

// A sender that is not the game's: the attached one as a game would have it
// that passed the entity by the address of a copy (the handle and the world
// stored on the stack, their address in x1, the component in x2), which is
// the call Norbyte's Windows declaration is written for.
    .globl _cst_send_attached_by_address
    .p2align 2
_cst_send_attached_by_address:
    sub     sp, sp, #0x80
    stp     x28, x27, [sp, #0x20]
    stp     x26, x25, [sp, #0x30]
    stp     x24, x23, [sp, #0x40]
    stp     x22, x21, [sp, #0x50]
    stp     x20, x19, [sp, #0x60]
    stp     x29, x30, [sp, #0x70]
    add     x29, sp, #0x70
    ldr     w24, [x1, #0x8]
    ldr     w8, [x2, #0x8]
    cmp     w24, w8
    b.ne    1f
0:
    ldp     x29, x30, [sp, #0x70]
    ldp     x20, x19, [sp, #0x60]
    ldp     x22, x21, [sp, #0x50]
    ldp     x24, x23, [sp, #0x40]
    ldp     x26, x25, [sp, #0x30]
    ldp     x28, x27, [sp, #0x20]
    add     sp, sp, #0x80
    ret
1:
    mov     x19, x2
    mov     x20, x0
    ldr     x9, [x1]
    str     x9, [sp, #0x8]
    ldrh    w26, [x1, #0xc]
    mov     w9, #-0x1
    lsl     w9, w9, w26
    mvn     w27, w9
    b       3f
2:
    add     w24, w24, #0x1
    cmp     w24, w8
    b.eq    0b
3:
    ldr     x9, [x20, #0x10]
    ldr     x28, [x9, #0x10]
    ldrsw   x9, [x9, #0x1c]
    cmp     x28, #0x0
    ccmp    w9, #0x0, #0x4, ne
    b.eq    2b
    asr     w8, w24, w26
    ldr     x10, [sp, #0x8]
    ldr     x8, [x10, w8, sxtw #3]
    and     w10, w24, w27
    add     x8, x8, w10, sxtw #4
    ldp     x22, x21, [x8]
    ldr     x23, [x20, #0x20]
    mov     w8, #0x48
    madd    x25, x9, x8, x28
4:
    stp     x22, x23, [sp, #0x10]
    add     x0, x28, #0x8
    ldr     x8, [x28], #0x48
    ldr     x8, [x8]
    add     x1, sp, #0x10
    mov     x2, x21
    blr     x8
    cmp     x28, x25
    b.ne    4b
    ldr     w8, [x19, #0x8]
    b       2b

// A subscriber of the game's, as far as a call shows it: records x0 to x3 in
// cst_seen and counts the call.
    .globl _cst_record
    .p2align 2
_cst_record:
    adrp    x16, _cst_seen@PAGE
    add     x16, x16, _cst_seen@PAGEOFF
    stp     x0, x1, [x16]
    stp     x2, x3, [x16, #16]
    adrp    x16, _cst_calls@PAGE
    add     x16, x16, _cst_calls@PAGEOFF
    ldr     x17, [x16]
    add     x17, x17, #1
    str     x17, [x16]
    ret
