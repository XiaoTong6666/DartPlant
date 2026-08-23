#include "standalone_dobby_host.h"

#include <dobby.h>

namespace {

int Hook(void* target, void* replacement, void** backup) {
    return DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(replacement),
                     reinterpret_cast<dobby_dummy_func_t*>(backup));
}

int Unhook(void* target) { return DobbyDestroy(target); }

const DartPlantNativeApiEntries kEntries = {
    .version = 2,
    .hook_func = Hook,
    .unhook_func = Unhook,
};

}  // namespace

const DartPlantNativeApiEntries* dartplant_fixture_standalone_dobby_host() { return &kEntries; }
