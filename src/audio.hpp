/*
 * Steam-Link-PS5 - Stream audio: Opus packets in, sceAudioOut out.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace slp::audio
{
// Open the console's audio port for a 48 kHz stereo stream and start the output
// thread. Returns false (and logs) if the port cannot be opened.
bool start(std::uint32_t frequency, std::uint32_t channels);

// One Opus packet from the host. Decoded and queued; a full queue drops the
// oldest audio rather than delaying the stream.
void submit(const std::uint8_t *packet, std::size_t length);

void stop();
} // namespace slp::audio
