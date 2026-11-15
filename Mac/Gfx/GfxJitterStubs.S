// GfxJitterStubs.S: Runs one of BG3's jitter sample loads, then lets
// gfx_jitter_sample replace the value. Reached from an island that has pushed
// x16 and x17 and put its own return point in x17. The sites sit mid-function,
// so nothing may change but the loaded register: everything C can clobber is
// kept, including the top half of v8-v15, NZCV and FPSR.

.text
.p2align 2

.macro SITE name, reg, view_from_x19_8
.globl _\name
.private_extern _\name
_\name:
    ldr     d\reg, [x9, w8, uxtw #3]
    sub     sp, sp, #672
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x17, x30, [sp, #128]
    mrs     x16, nzcv
    str     x16, [sp, #144]
    mrs     x16, fpsr
    str     x16, [sp, #152]
    stp     q0, q1, [sp, #160]
    stp     q2, q3, [sp, #192]
    stp     q4, q5, [sp, #224]
    stp     q6, q7, [sp, #256]
    stp     q8, q9, [sp, #288]
    stp     q10, q11, [sp, #320]
    stp     q12, q13, [sp, #352]
    stp     q14, q15, [sp, #384]
    stp     q16, q17, [sp, #416]
    stp     q18, q19, [sp, #448]
    stp     q20, q21, [sp, #480]
    stp     q22, q23, [sp, #512]
    stp     q24, q25, [sp, #544]
    stp     q26, q27, [sp, #576]
    stp     q28, q29, [sp, #608]
    stp     q30, q31, [sp, #640]
.if \view_from_x19_8
    ldr     x0, [x19, #8]
.else
    mov     x0, x20
.endif
    fmov    x1, d\reg
    bl      _gfx_jitter_sample
    str     x0, [sp, #(160 + \reg * 16)]
    ldp     q0, q1, [sp, #160]
    ldp     q2, q3, [sp, #192]
    ldp     q4, q5, [sp, #224]
    ldp     q6, q7, [sp, #256]
    ldp     q8, q9, [sp, #288]
    ldp     q10, q11, [sp, #320]
    ldp     q12, q13, [sp, #352]
    ldp     q14, q15, [sp, #384]
    ldp     q16, q17, [sp, #416]
    ldp     q18, q19, [sp, #448]
    ldp     q20, q21, [sp, #480]
    ldp     q22, q23, [sp, #512]
    ldp     q24, q25, [sp, #544]
    ldp     q26, q27, [sp, #576]
    ldp     q28, q29, [sp, #608]
    ldp     q30, q31, [sp, #640]
    ldr     x16, [sp, #152]
    msr     fpsr, x16
    ldr     x16, [sp, #144]
    msr     nzcv, x16
    ldp     x17, x30, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #672
    br      x17
.endm

SITE gfx_jitter_stub_projection, 3, 0
SITE gfx_jitter_stub_camera, 1, 0
SITE gfx_jitter_stub_velocity, 0, 1
