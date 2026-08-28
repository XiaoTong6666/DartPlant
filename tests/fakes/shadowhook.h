#ifndef DARTPLANT_TEST_FAKE_SHADOWHOOK_H_
#define DARTPLANT_TEST_FAKE_SHADOWHOOK_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHADOWHOOK_MODE_SHARED = 0,
    SHADOWHOOK_MODE_UNIQUE = 1,
    SHADOWHOOK_MODE_MULTI = 2,
} shadowhook_mode_t;

#define SHADOWHOOK_HOOK_DEFAULT 0
#define SHADOWHOOK_HOOK_WITH_SHARED_MODE 1
#define SHADOWHOOK_HOOK_WITH_UNIQUE_MODE 2
#define SHADOWHOOK_HOOK_WITH_MULTI_MODE 4
#define SHADOWHOOK_HOOK_RECORD 8

int shadowhook_init(shadowhook_mode_t default_mode, bool debuggable);
void* shadowhook_hook_func_addr_2(void* func_addr, void* new_addr, void** orig_addr, uint32_t flags,
                                  ...);
int shadowhook_unhook(void* stub);

#ifdef __cplusplus
}
#endif

#endif  // DARTPLANT_TEST_FAKE_SHADOWHOOK_H_
