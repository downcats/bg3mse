// Functions for the Detours tests, written in assembly so that their first
// instructions and their order are exactly these. DetoursTests.cpp's stress
// caller starts the section, on a page boundary, so all of it lies on one page.

    .section __TEXT,__detours,regular,pure_instructions
    .p2align 2

// x0 + 10
    .globl _dt_add_ten
_dt_add_ten:
    add w0, w0, #10
    ret

// 20 and 21: two functions on one page.
    .globl _dt_twenty
_dt_twenty:
    mov w0, #20
    ret

    .globl _dt_twenty_one
_dt_twenty_one:
    mov w0, #21
    ret

// 30, 31 and 32: for the refusals and the failed commits.
    .globl _dt_thirty
_dt_thirty:
    mov w0, #30
    ret

    .globl _dt_thirty_one
_dt_thirty_one:
    mov w0, #31
    ret

    .globl _dt_thirty_two
_dt_thirty_two:
    mov w0, #32
    ret

// Its first instruction is a call, which the planner refuses. Never called.
    .globl _dt_calls_first
_dt_calls_first:
    bl _dt_twenty
    brk #0

// 1234: the function the stress test's threads call while it is hooked.
    .globl _dt_stress
_dt_stress:
    mov w8, #1
    mov w0, #1234
    ret

// 40 and 41, each starting a page of its own; nothing else lies on the
// second page, which the failed-commit test makes non-executable for a moment.
    .section __TEXT,__detours_far,regular,pure_instructions
    .p2align 14
    .globl _dt_forty
_dt_forty:
    mov w0, #40
    ret

    .p2align 14
    .globl _dt_forty_one
_dt_forty_one:
    mov w0, #41
    ret
    .space 16384 - 8

    .subsections_via_symbols
