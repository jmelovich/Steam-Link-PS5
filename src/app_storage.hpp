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

// User settings, a key=value file under the data directory.
struct Settings
{
    std::uint32_t max_height = 1080;   // 1080 or 720: the resolution asked of the host
    bool hevc = false;                 // offer HEVC to the host
    std::uint32_t audio_channels = 2;  // 2 or 6
    std::uint64_t last_host = 0;       // client id of the PC streamed from last
    bool auto_connect = true;          // connect to last_host without asking
    std::uint32_t bitrate_kbps = 0;    // cap asked of the host in kbit/s; 0 = AUTO (let Steam/host decide)
    std::uint32_t quality = 1;         // 0 balanced, 1 fast, 2 beautiful
    std::uint32_t jitter_frames = 2;   // decoded frames buffered before display (0-4); absorbs
                                       // arrival jitter so it does not become on-screen judder.
                                       // 0 = present newest immediately (lowest latency, more stutter).
};
Settings &settings();
void settings_save();
} // namespace slp
