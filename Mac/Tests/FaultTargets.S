// Functions that fault in a known way, for Mac/Tests/FaultTests.cpp. Written
// in assembly so the compiler cannot turn them into anything else.

    .text

// Reads 8 bytes at address 0x10.
    .p2align 2
    .globl _fault_test_read_null
_fault_test_read_null:
    mov     x8, #0x10
    ldr     x0, [x8]
    ret

// Stores zero at the address in x0.
    .p2align 2
    .globl _fault_test_write
_fault_test_write:
    str     xzr, [x0]
    ret

// An undefined instruction.
    .p2align 2
    .globl _fault_test_undefined
_fault_test_undefined:
    udf     #0
    ret

// brk #1, which __builtin_trap() compiles to.
    .p2align 2
    .globl _fault_test_trap
_fault_test_trap:
    brk     #1
    ret

// Calls itself until the stack runs out, 1 KiB a frame.
    .p2align 2
    .globl _fault_test_recurse
_fault_test_recurse:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #1024
    str     xzr, [sp]
    bl      _fault_test_recurse
    add     sp, sp, #1024
    ldp     x29, x30, [sp], #16
    ret
