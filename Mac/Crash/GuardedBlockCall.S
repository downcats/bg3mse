// The frame a guarded block is called through, and the registers of a thread
// that reports on itself (Crash/CrashReporter.cpp).
//
// bg3mse_guarded_block_call(fn, arg) calls fn(arg). Its frame has a
// personality routine, bg3mse_guarded_block_personality, and no handler or
// cleanup: the unwinder asks the routine about every exception that is about
// to pass through the frame, first while it still searches for a handler,
// with nothing unwound yet. That is where a C++ exception leaving a guarded
// block is seen.

    .text

    .p2align 2
    .private_extern _bg3mse_guarded_block_call
_bg3mse_guarded_block_call:
    .cfi_startproc
    // 155: an indirect, pc-relative, 4-byte pointer to the routine.
    .cfi_personality 155, _bg3mse_guarded_block_personality
    stp     x29, x30, [sp, #-16]!
    .cfi_def_cfa_offset 16
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    mov     x29, sp
    .cfi_def_cfa w29, 16
    mov     x16, x0
    mov     x0, x1
    blr     x16
    ldp     x29, x30, [sp], #16
    .cfi_def_cfa sp, 0
    .cfi_restore w30
    .cfi_restore w29
    ret
    .cfi_endproc

// bg3mse_crash_capture_state(arm_thread_state64_t* out) records the caller's
// registers as they are at the call: x0..x28, fp and sp as they stand, pc as
// the address the call returns to, and lr as 0 (the caller's return address
// is in its frame record, where a walk of the stack finds it).
    .p2align 2
    .private_extern _bg3mse_crash_capture_state
_bg3mse_crash_capture_state:
    stp     x0, x1, [x0, #0]
    stp     x2, x3, [x0, #16]
    stp     x4, x5, [x0, #32]
    stp     x6, x7, [x0, #48]
    stp     x8, x9, [x0, #64]
    stp     x10, x11, [x0, #80]
    stp     x12, x13, [x0, #96]
    stp     x14, x15, [x0, #112]
    stp     x16, x17, [x0, #128]
    stp     x18, x19, [x0, #144]
    stp     x20, x21, [x0, #160]
    stp     x22, x23, [x0, #176]
    stp     x24, x25, [x0, #192]
    stp     x26, x27, [x0, #208]
    stp     x28, x29, [x0, #224]
    str     xzr, [x0, #240]
    mov     x16, sp
    str     x16, [x0, #248]
    str     x30, [x0, #256]
    str     xzr, [x0, #264]
    ret
