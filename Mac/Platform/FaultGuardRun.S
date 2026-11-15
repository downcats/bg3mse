// The register side of a guarded call (Platform/FaultGuard.cpp).
//
// bg3mse_guard_run(saved, fn, arg) records the caller's callee-saved state
// (x19-x28, fp, lr, sp, d8-d15) in `saved`, calls fn(arg) and returns 0.
// bg3mse_guard_resume(saved) is where the exception thread sends a thread
// whose fault a guard recovers: it reloads that state and returns 1 from the
// bg3mse_guard_run call it was recorded by, with the frames in between
// abandoned. It uses no stack of its own.
//
// Layout of `saved` (21 words): x19..x28, x29, x30, sp, d8..d15.

    .text

    .p2align 2
    .private_extern _bg3mse_guard_run
_bg3mse_guard_run:
    .cfi_startproc
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x16, sp
    str     x16, [x0, #96]
    stp     d8, d9, [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    stp     x29, x30, [sp, #-16]!
    .cfi_def_cfa_offset 16
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    mov     x29, sp
    .cfi_def_cfa w29, 16
    mov     x0, x2
    blr     x1
    mov     w0, #0
    ldp     x29, x30, [sp], #16
    .cfi_def_cfa sp, 0
    .cfi_restore w30
    .cfi_restore w29
    ret
    .cfi_endproc

    .p2align 2
    .private_extern _bg3mse_guard_resume
_bg3mse_guard_resume:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x16, [x0, #96]
    mov     sp, x16
    ldp     d8, d9, [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]
    mov     w0, #1
    ret
