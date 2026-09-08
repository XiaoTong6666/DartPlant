// Copyright (C) 2026 XiaoTong6666
// Licensed under the Apache License, Version 2.0 (the "License").

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

static void fail_closed(void) {
    static const char message[] = "DARTPLANT_ARM64_UUID_SHIM random source failure\n";
    const ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1U);
    (void) ignored;
    _exit(126);
}

static void fill_random(uint8_t* out, size_t size) {
    size_t offset = 0U;
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fail_closed();
    }
    while (offset < size) {
        const ssize_t count = read(fd, out + offset, size - offset);
        if (count > 0) {
            offset += (size_t) count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        (void) close(fd);
        fail_closed();
    }
    (void) close(fd);
}

__attribute__((visibility("default"))) void uuid_generate(uint8_t out[16]) {
    static int reported = 0;
    fill_random(out, 16U);

    // RFC 4122 / RFC 9562 UUIDv4: version 4 and RFC variant bits.
    out[6] = (uint8_t) ((out[6] & 0x0FU) | 0x40U);
    out[8] = (uint8_t) ((out[8] & 0x3FU) | 0x80U);

    if (__atomic_exchange_n(&reported, 1, __ATOMIC_RELAXED) == 0) {
        static const char message[] = "DARTPLANT_ARM64_UUID_SHIM uuid_generate interposed\n";
        const ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1U);
        (void) ignored;
    }
}
