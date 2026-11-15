// Functions that fault in a known way, for Mac/Tests/CrashTests.cpp.

    .text

// Reads 8 bytes at address 0x10, with 0xdeadbeef in x9. The read is the
// fourth instruction: 12 bytes in.
    .p2align 2
    .globl _crash_test_read_null
_crash_test_read_null:
    movz    x9, #0xbeef
    movk    x9, #0xdead, lsl #16
    mov     x8, #0x10
    ldr     x0, [x8]
    ret

// Calls itself until the stack runs out, 1 KiB a frame.
    .p2align 2
    .globl _crash_test_recurse
_crash_test_recurse:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #1024
    str     xzr, [sp]
    bl      _crash_test_recurse
    add     sp, sp, #1024
    ldp     x29, x30, [sp], #16
    ret

// crash_test_read_null with known values in two vector registers: every
// other byte 0xff in q0, and 0x5a in every byte of q31.
    .p2align 2
    .globl _crash_test_read_null_vectors
_crash_test_read_null_vectors:
    movi    v0.2d, #0xff00ff00ff00ff00
    movi    v31.16b, #0x5a
    b       _crash_test_read_null

// Sets the frame pointer to x0 and then adds 1 to the 64-bit counter at x1,
// again and again: a thread whose frame-pointer chain is whatever x0 is, for
// the crash reporter's walk of the other threads. Never returns.
    .p2align 2
    .globl _crash_test_spin_with_fp
_crash_test_spin_with_fp:
    mov     x29, x0
1:
    ldr     x2, [x1]
    add     x2, x2, #1
    str     x2, [x1]
    b       1b
