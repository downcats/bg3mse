// A small dylib for the function extent and hook tests, written in assembly
// so that its functions lie exactly in this order. It is linked twice: with
// its function starts table, and without it and without local symbols, so
// that _lib_local is a function only the former knows of.

    .text
    .p2align 2

// x0 + 1
    .globl _lib_add_one
_lib_add_one:
    add x0, x0, #1
    ret

// 7; local, so it has no symbol in the copy linked without local symbols.
_lib_local:
    mov x0, #7
    ret

// x0 * 2
    .globl _lib_twice
_lib_twice:
    lsl x0, x0, #1
    ret

// lib_local() + x0
    .globl _lib_add_local
_lib_add_local:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    bl _lib_local
    add x0, x0, x9
    ldp x29, x30, [sp], #16
    ret

    .data
    .p2align 3
    .globl _lib_data
_lib_data:
    .quad 5

    .subsections_via_symbols
