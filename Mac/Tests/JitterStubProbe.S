// JitterStubProbe.S: Enters a jitter stub the way the island does, with every
// register holding a known value, and records what comes back.
//
// void gfx_probe_stub(void (*stub)(void), const uint64_t *table, uint64_t view_x20,
//                     uint64_t *saved_regs /* x0-x15, nzcv, fpsr, x19, x20 */,
//                     __uint128_t *saved_v /* v0-v31 */);

.text
.p2align 2
.globl _gfx_probe_stub
_gfx_probe_stub:
    stp     x29, x30, [sp, #-112]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     x3, x4, [sp, #96]
    stp     d8, d9, [sp, #-16]!
    stp     d10, d11, [sp, #-16]!
    stp     d12, d13, [sp, #-16]!
    stp     d14, d15, [sp, #-16]!

    mov     x16, x0                  // the stub
    mov     x9, x1                   // the table
    mov     w8, #1                   // its second entry
    mov     x20, x2                  // the view
    // For the velocity stub, [x19, #8] is the view.
    sub     sp, sp, #16
    str     x2, [sp, #8]
    mov     x19, sp

    // A distinct pattern in every vector, in both halves.
    .macro FILLV n
    mov     x10, #(0x1000 + \n)
    mov     v\n\().d[0], x10
    mov     x10, #(0x2000 + \n)
    mov     v\n\().d[1], x10
    .endm
    FILLV 0
    FILLV 1
    FILLV 2
    FILLV 3
    FILLV 4
    FILLV 5
    FILLV 6
    FILLV 7
    FILLV 8
    FILLV 9
    FILLV 10
    FILLV 11
    FILLV 12
    FILLV 13
    FILLV 14
    FILLV 15
    FILLV 16
    FILLV 17
    FILLV 18
    FILLV 19
    FILLV 20
    FILLV 21
    FILLV 22
    FILLV 23
    FILLV 24
    FILLV 25
    FILLV 26
    FILLV 27
    FILLV 28
    FILLV 29
    FILLV 30
    FILLV 31

    mov     x0, #100
    mov     x1, #101
    mov     x2, #102
    mov     x3, #103
    mov     x4, #104
    mov     x5, #105
    mov     x6, #106
    mov     x7, #107
    mov     x10, #110
    mov     x11, #111
    mov     x12, #112
    mov     x13, #113
    mov     x14, #114
    mov     x15, #115
    mov     x17, #0x20000000          // Z,C flags: Z=0x40000000 C=0x20000000
    msr     nzcv, x17
    adr     x17, 1f
    br      x16
1:
    // Everything comes back here untouched, but for the loaded d register.
    stp     x16, x17, [sp, #-16]!     // free two registers without flags
    mrs     x17, nzcv
    str     x17, [sp, #-16]!
    ldr     x17, [sp, #16 + 16 + 16 + 0]  // unused; keep offsets honest below
    ldr     x17, [sp]
    add     x16, sp, #32              // past the two pushes
    add     x16, x16, #16             // past the view slot
    add     x16, x16, #64             // past d8-d15
    ldp     x16, x17, [x16, #96]      // saved_regs, saved_v (x3, x4 above)
    stp     x0, x1, [x16, #0]
    stp     x2, x3, [x16, #16]
    stp     x4, x5, [x16, #32]
    stp     x6, x7, [x16, #48]
    stp     x8, x9, [x16, #64]
    stp     x10, x11, [x16, #80]
    stp     x12, x13, [x16, #96]
    stp     x14, x15, [x16, #112]
    ldr     x0, [sp]                  // nzcv
    str     x0, [x16, #128]
    mrs     x0, fpsr
    str     x0, [x16, #136]
    stp     x19, x20, [x16, #144]
    stp     q0, q1, [x17, #0]
    stp     q2, q3, [x17, #32]
    stp     q4, q5, [x17, #64]
    stp     q6, q7, [x17, #96]
    stp     q8, q9, [x17, #128]
    stp     q10, q11, [x17, #160]
    stp     q12, q13, [x17, #192]
    stp     q14, q15, [x17, #224]
    stp     q16, q17, [x17, #256]
    stp     q18, q19, [x17, #288]
    stp     q20, q21, [x17, #320]
    stp     q22, q23, [x17, #352]
    stp     q24, q25, [x17, #384]
    stp     q26, q27, [x17, #416]
    stp     q28, q29, [x17, #448]
    stp     q30, q31, [x17, #480]
    add     sp, sp, #32               // the two pushes
    add     sp, sp, #16               // the view slot
    ldp     d14, d15, [sp], #16
    ldp     d12, d13, [sp], #16
    ldp     d10, d11, [sp], #16
    ldp     d8, d9, [sp], #16
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #112
    ret
