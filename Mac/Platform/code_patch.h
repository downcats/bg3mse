/**
 * bg3mse - code writes (Platform/CodeWriter.h), jump pads (Platform/Islands.h)
 * and entry hooks (Platform/EntryHook.h) for C callers: the camera module.
 *
 * Each function returns true (or the address) when it did what it says;
 * otherwise nothing was changed and the reason is written to why (why_len
 * bytes, may be 0).
 */

#ifndef BG3MSE_CODE_PATCH_H
#define BG3MSE_CODE_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One aligned instruction word; safe with any number of threads executing it. */
bool bg3mse_code_write_word(void *at, uint32_t word, char *why, size_t why_len);

/**
 * A jump pad (ldr x16, #8; br x16; .quad target) within branch range of near,
 * so that one b or bl there reaches target anywhere. Never freed.
 */
void *bg3mse_jump_pad(void *near, void *target, char *why, size_t why_len);

/**
 * An entry hook on the function at target, function_bytes long, prepared and
 * committed: *original is set to the trampoline before the patch goes live.
 * Returns the hook, or NULL when refused.
 */
void *bg3mse_entry_hook(void *target, size_t function_bytes, void *replacement, void **original,
                        char *why, size_t why_len);

#ifdef __cplusplus
}
#endif

#endif /* BG3MSE_CODE_PATCH_H */
