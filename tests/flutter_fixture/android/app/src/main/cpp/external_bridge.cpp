#include <atomic>
#include <cstdint>

namespace {
std::atomic<uint64_t> g_external_callback{0};
}

extern "C" __attribute__((visibility("default"))) void dartplant_external_bridge_register(
    uint64_t callback) {
    g_external_callback.store(callback, std::memory_order_release);
}

extern "C" uint64_t dartplant_external_bridge_bootstrap_impl(uint64_t api_data, uint64_t callback,
                                                             uint64_t thr, uint64_t pp,
                                                             uint64_t heap_bits,
                                                             uint64_t null_value) {
    if (api_data == 0 || callback == 0) return 0;
    using Callback = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    return reinterpret_cast<Callback>(callback)(api_data, thr, pp, heap_bits, null_value);
}

extern "C" uint64_t dartplant_external_bridge_bootstrap_from_registers(uint64_t api_data,
                                                                       uint64_t null_value,
                                                                       uint64_t thr, uint64_t pp,
                                                                       uint64_t heap_bits) {
    const uint64_t callback = g_external_callback.load(std::memory_order_acquire);
    return dartplant_external_bridge_bootstrap_impl(api_data, callback, thr, pp, heap_bits,
                                                    null_value);
}

#if defined(__aarch64__)
// The FFI call is made on the live Dart mutator; x22/x26/x27/x28 are the
// source-verified Flutter ARM64 VM register contract, NOT a Java-thread
// process sample. Pass them straight through without inspecting moving heap.
extern "C" __attribute__((naked, visibility("default"))) uint64_t
dartplant_external_bridge_bootstrap(uint64_t) {
    __asm__ volatile(
        "mov x1, x22\n"
        "mov x2, x26\n"
        "mov x3, x27\n"
        "mov x4, x28\n"
        "b dartplant_external_bridge_bootstrap_from_registers\n");
}
#else
extern "C"
    __attribute__((visibility("default"))) uint64_t dartplant_external_bridge_bootstrap(uint64_t) {
    return 0;
}
#endif
