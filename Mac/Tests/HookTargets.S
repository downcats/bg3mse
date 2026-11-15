// Functions for the entry hook tests, written in assembly so that their first
// instructions are exactly these. Each is followed by a symbol marking its end.

    .text
    .p2align 2

// Returns hook_data: the first instruction is an adrp.
    .globl _hook_adrp_first, _hook_adrp_first_end
_hook_adrp_first:
    adrp x8, _hook_data@PAGE
    ldr w0, [x8, _hook_data@PAGEOFF]
    ret
_hook_adrp_first_end:

// Returns 3 for a zero argument, 2 otherwise: the first instruction is a cbz.
    .globl _hook_cbz_first, _hook_cbz_first_end
_hook_cbz_first:
    cbz x0, 1f
    mov w0, #2
    ret
1:  mov w0, #3
    ret
_hook_cbz_first_end:

// Returns a literal of its own: the first instruction is an ldr (literal).
    .globl _hook_literal_first, _hook_literal_first_end
_hook_literal_first:
    ldr x0, 2f
    ret
    .p2align 3
2:  .quad 0x1122334455667788
_hook_literal_first_end:

// Counts its argument down: its entry is also its loop head.
    .globl _hook_loop_head, _hook_loop_head_end
_hook_loop_head:
    sub x0, x0, #1
    cbnz x0, _hook_loop_head
    ret
_hook_loop_head_end:

// The stress test's function, on the page its callers' loop runs from.
    .section __TEXT,__hookstress,regular,pure_instructions
    .p2align 2
    .globl _hook_stress_target, _hook_stress_target_end
_hook_stress_target:
    mov w8, #1
    mov w0, #1234
    ret
_hook_stress_target_end:

    .data
    .p2align 2
    .globl _hook_data
_hook_data:
    .long 4242
