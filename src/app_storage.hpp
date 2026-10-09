/*
 * steamlink-ps5 - Where the app keeps its files, and the client identity.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace slp
{
// The pairing identity Steam remembers: a device id and the 32-byte secret the
// device token is derived from. Generated once, then persisted.
struct Identity
{
    std::uint64_t device_id;
    std::uint8_t secret_key[32];
};

// Picks the data directory (/data/steamlink-ps5 when ShadowMount+ mounts /data into
// the sandbox, otherwise /download0), opens the log, loads or creates the identity.
// Call first thing in main, on the main thread.
void storage_init();
const char *data_dir();
const Identity &identity();
// Console entropy (sysctl kern.rng_pseudo). Not for keys that leave the console.
void random_bytes(void *destination, std::size_t length);

// Pairing negotiates a 32-byte secret per host (Steam's PIN-masked key exchange);
// streaming from that host needs the same key again. Keyed by the host's client id.
bool host_key_load(std::uint64_t host_client_id, std::uint8_t key[32]);
void host_key_save(std::uint64_t host_client_id, const std::uint8_t key[32]);
} // namespace slp
