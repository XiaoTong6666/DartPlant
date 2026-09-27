// Controlled physical mapping lifecycle, separate from the Flutter loader.
// This proves RX mapping disappearance and same-VA replacement, NOT that the
// Flutter AOT image is dlclosed or that DartPlant's owner tree was rebound.
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/runtime_image_set.h"

namespace {
constexpr char kTag[] = "DartPlantModule";
constexpr uint32_t kFirstCode[] = {0xd2800020U, 0xd65f03c0U};
constexpr uint32_t kSecondCode[] = {0xd2800040U, 0xd65f03c0U};

int MakeImage(const char* name, const uint32_t (&code)[2], size_t page_size) {
    int fd = static_cast<int>(syscall(__NR_memfd_create, name, MFD_CLOEXEC | MFD_EXEC));
    if (fd < 0 && errno == EINVAL) {
        // MFD_EXEC was added after the kernel shipped on some Android 14
        // devices. Older kernels create executable memfds by default; use
        // that documented legacy contract rather than demanding a new flag.
        // The actual PROT_EXEC mapping must still succeed below.
        fd = static_cast<int>(syscall(__NR_memfd_create, name, MFD_CLOEXEC));
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, static_cast<off_t>(page_size)) != 0 ||
        pwrite(fd, code, sizeof(code), 0) != static_cast<ssize_t>(sizeof(code))) {
        close(fd);
        return -1;
    }
    return fd;
}

void Record(bool passed, const char* stage, uintptr_t address, bool removed, bool same_va,
            bool distinct_inode, bool owner_reconciled = false) {
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "DARTPLANT_HOST {event=physical_mapping_control, state=%s, "
        "stage=%s, mapping_removed=%u, same_va=%u, "
        "different_inode=%u, owner_reconciled=%u, executable=1, dart_owner_bound=0, "
        "address=%p, errno=%d}",
        passed ? "pass" : "fail", stage, static_cast<unsigned>(removed),
        static_cast<unsigned>(same_va), static_cast<unsigned>(distinct_inode),
        static_cast<unsigned>(owner_reconciled), reinterpret_cast<void*>(address), errno);
}

bool ProvePhysicalOwnerReincarnation(uintptr_t address, size_t page_size) {
    const auto make_module = [&](const char* build_id) {
        dartplant::ModuleImage module;
        module.name = "libdartplant-map-probe.so";
        module.path = "/proc/self/fd/dartplant-map-probe";
        module.build_id = build_id;
        module.load_bias = address;
        module.executable_ranges.push_back({
            .start = address,
            .end = address + page_size,
            .file_offset = 0,
            .virtual_address = 0,
            .file_size = page_size,
        });
        return module;
    };
    const auto make_snapshot = [&](const dartplant::ModuleImage& module) {
        dartplant::FlutterSnapshotSource snapshot;
        snapshot.module_name = module.name;
        snapshot.module_path = module.path;
        snapshot.module_build_id = module.build_id;
        snapshot.snapshot_hash = "controlled-physical-probe-not-flutter-aot";
        snapshot.snapshot_features = "arm64 product compressed-pointers";
        snapshot.profile_name = "controlled-map-probe";
        snapshot.isolate_instructions_va = 0;
        snapshot.isolate_instructions_size = page_size;
        snapshot.isolate_instructions_runtime = address;
        snapshot.compressed_pointers = true;
        return snapshot;
    };
    const auto old_module = make_module("physical-v1");
    const auto new_module = make_module("physical-v2");
    dartplant::RuntimeImageSet published;
    std::string error;
    if (!published.SetRoot(old_module, make_snapshot(old_module), 1, &error)) return false;
    published.BindOwnerEpochs(11, 13);
    published.ActivateAll();
    const auto* old_image = published.Root();
    if (old_image == nullptr) return false;
    const auto old_owner = old_image->OwnerIdentity();
    const auto old_id = old_image->id;
    const auto old_epoch = old_image->incarnation_epoch;
    dartplant::RuntimeImageSet staged;
    if (!staged.SetRoot(new_module, make_snapshot(new_module), 2, &error) ||
        !staged.ReconcileOwnershipFrom(published)) {
        return false;
    }
    staged.BindOwnerEpochs(11, 13);
    staged.ActivateAll();
    const auto* fresh = staged.Root();
    if (fresh == nullptr) return false;
    const auto fresh_owner = fresh->OwnerIdentity();
    return fresh->id == old_id && fresh->incarnation_epoch != old_epoch &&
           fresh_owner.image_id == old_owner.image_id &&
           fresh_owner.image_incarnation_epoch != old_owner.image_incarnation_epoch &&
           fresh_owner.runtime_generation != old_owner.runtime_generation &&
           !staged.SameIdentity(published) && !staged.ContainsIdentity(*old_image) &&
           staged.FindById(old_id) == fresh && fresh->ContainsRuntimeRange(address, 4);
}
}  // namespace

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_physical_mapping_control() {
    const long system_page = sysconf(_SC_PAGESIZE);
    if (system_page <= 0) return 0;
    const size_t page_size = static_cast<size_t>(system_page);
    const int first_fd = MakeImage("dartplant-first-image", kFirstCode, page_size);
    const int second_fd = MakeImage("dartplant-second-image", kSecondCode, page_size);
    if (first_fd < 0 || second_fd < 0) {
        if (first_fd >= 0) close(first_fd);
        if (second_fd >= 0) close(second_fd);
        Record(false, "create_image", 0, false, false, false);
        return 0;
    }
    struct stat first_stat{};
    struct stat second_stat{};
    const bool distinct_inode =
        fstat(first_fd, &first_stat) == 0 && fstat(second_fd, &second_stat) == 0 &&
        (first_stat.st_dev != second_stat.st_dev || first_stat.st_ino != second_stat.st_ino);
    void* first = mmap(nullptr, page_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, first_fd, 0);
    if (first == MAP_FAILED) {
        Record(false, "first_rx_map", 0, false, false, distinct_inode);
        close(first_fd);
        close(second_fd);
        return 0;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(first);
    const bool first_code_matches = std::memcmp(first, kFirstCode, sizeof(kFirstCode)) == 0;
    const bool unmap_ok = munmap(first, page_size) == 0;
    unsigned char resident = 0;
    errno = 0;
    const bool removed = unmap_ok && mincore(first, page_size, &resident) == -1 && errno == ENOMEM;
    void* second = MAP_FAILED;
    if (removed) {
        second = mmap(first, page_size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED_NOREPLACE,
                      second_fd, 0);
    }
    const bool same_va = second != MAP_FAILED && second == first;
    const bool second_code_matches = same_va &&
                                     std::memcmp(second, kSecondCode, sizeof(kSecondCode)) == 0 &&
                                     std::memcmp(second, kFirstCode, sizeof(kFirstCode)) != 0;
    const bool owner_reconciled =
        second_code_matches && ProvePhysicalOwnerReincarnation(address, page_size);
    const bool passed = distinct_inode && first_code_matches && removed && same_va &&
                        second_code_matches && owner_reconciled;
    Record(passed, passed ? "complete" : "remap_or_fingerprint", address, removed, same_va,
           distinct_inode, owner_reconciled);
    if (second != MAP_FAILED) munmap(second, page_size);
    close(first_fd);
    close(second_fd);
    return passed ? 1 : 0;
}
