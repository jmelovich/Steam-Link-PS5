/*
 * Steam-Link-PS5 - Opus -> sceAudioOut. See audio.hpp.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sceAudioOutOutput() blocks until the previous grain has played, so a thread
 * feeds it 256-frame grains from a ring the decoder fills. Silence when the ring
 * is empty (network hiccup); drop the oldest when it overflows (we are behind).
 * Port and format constants are the ones ProsperoLight and rpcs3-PS5 use.
 */

#include "audio.hpp"
#include "app_log.hpp"

#include <opus.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <pthread.h>

extern "C"
{
    std::int32_t sceAudioOutInit();
    std::int32_t sceAudioOutOpen(std::int32_t user, std::int32_t type, std::int32_t index,
                                 std::uint32_t grain, std::uint32_t frequency, std::uint32_t format);
    std::int32_t sceAudioOutOutput(std::int32_t handle, const void *buffer);
    std::int32_t sceAudioOutClose(std::int32_t handle);
}

namespace slp::audio
{
namespace
{
constexpr std::int32_t k_user_system = 0xff;
constexpr std::int32_t k_port_main = 0;
constexpr std::uint32_t k_grain = 256;                 // frames per sceAudioOutOutput
constexpr std::uint32_t k_format_s16_stereo = 1;
constexpr std::uint32_t k_already_initialized = 0x8026000e;
constexpr unsigned k_channels = 2;
constexpr std::size_t k_ring_frames = 48000 / 5;       // 200 ms of backlog at most
constexpr int k_max_opus_frame = 5760;                 // 120 ms at 48 kHz

struct State
{
    std::mutex mutex;
    OpusDecoder *decoder = nullptr;
    std::int32_t port = -1;
    pthread_t thread{};
    bool thread_running = false;
    std::atomic<bool> stop{false};

    // Interleaved S16 stereo ring.
    std::int16_t ring[k_ring_frames * k_channels];
    std::size_t head = 0;   // next frame to play
    std::size_t count = 0;  // frames queued
    std::uint64_t dropped = 0;
    std::uint64_t underruns = 0;
    std::uint64_t played = 0;
};
State g;

void ring_push(const std::int16_t *pcm, std::size_t frames)
{
    std::lock_guard lock{g.mutex};
    if (frames > k_ring_frames)
    {
        pcm += (frames - k_ring_frames) * k_channels;
        frames = k_ring_frames;
    }
    if (g.count + frames > k_ring_frames)
    {
        const std::size_t drop = g.count + frames - k_ring_frames;
        g.head = (g.head + drop) % k_ring_frames;
        g.count -= drop;
        g.dropped += drop;
    }
    std::size_t tail = (g.head + g.count) % k_ring_frames;
    for (std::size_t i = 0; i < frames; ++i)
    {
        g.ring[tail * k_channels] = pcm[i * k_channels];
        g.ring[tail * k_channels + 1] = pcm[i * k_channels + 1];
        tail = (tail + 1) % k_ring_frames;
    }
    g.count += frames;
}

void *output_thread(void *)
{
    std::int16_t grain[k_grain * k_channels];
    while (!g.stop.load())
    {
        {
            std::lock_guard lock{g.mutex};
            if (g.count >= k_grain)
            {
                for (std::size_t i = 0; i < k_grain; ++i)
                {
                    grain[i * k_channels] = g.ring[g.head * k_channels];
                    grain[i * k_channels + 1] = g.ring[g.head * k_channels + 1];
                    g.head = (g.head + 1) % k_ring_frames;
                }
                g.count -= k_grain;
                g.played += k_grain;
            }
            else
            {
                std::memset(grain, 0, sizeof grain);
                ++g.underruns;
            }
        }
        // Blocks for one grain (~5.3 ms): this is the thread's pacing.
        if (sceAudioOutOutput(g.port, grain) < 0)
            break;
    }
    sceAudioOutOutput(g.port, nullptr); // drain
    return nullptr;
}
} // namespace

bool start(std::uint32_t frequency, std::uint32_t channels)
{
    stop();
    if (frequency != 48000 || channels != k_channels)
    {
        slp::log("audio: unsupported format %u Hz %u ch (want 48000 Hz stereo)", frequency, channels);
        return false;
    }
    int error = OPUS_OK;
    g.decoder = opus_decoder_create(48000, k_channels, &error);
    if (!g.decoder || error != OPUS_OK)
    {
        slp::log("audio: opus_decoder_create failed: %s", opus_strerror(error));
        return false;
    }
    const std::int32_t init = sceAudioOutInit();
    if (init < 0 && static_cast<std::uint32_t>(init) != k_already_initialized)
    {
        slp::log("audio: sceAudioOutInit failed 0x%x", static_cast<unsigned>(init));
        return false;
    }
    g.port = sceAudioOutOpen(k_user_system, k_port_main, 0, k_grain, 48000, k_format_s16_stereo);
    if (g.port < 0)
    {
        slp::log("audio: sceAudioOutOpen failed 0x%x", static_cast<unsigned>(g.port));
        g.port = -1;
        return false;
    }
    g.head = g.count = 0;
    g.dropped = g.underruns = g.played = 0;
    g.stop = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    g.thread_running = pthread_create(&g.thread, &attr, output_thread, nullptr) == 0;
    pthread_attr_destroy(&attr);
    slp::log("audio: port %d open, 48 kHz stereo, grain %u", g.port, k_grain);
    return g.thread_running;
}

void submit(const std::uint8_t *packet, std::size_t length)
{
    if (!g.decoder || !packet || length == 0)
        return;
    static std::int16_t pcm[k_max_opus_frame * k_channels];
    const int frames = opus_decode(g.decoder, packet, static_cast<opus_int32>(length), pcm, k_max_opus_frame, 0);
    if (frames < 0)
    {
        static int logged = 0;
        if (logged++ < 5)
            slp::log("audio: opus_decode: %s", opus_strerror(frames));
        return;
    }
    ring_push(pcm, static_cast<std::size_t>(frames));
}

void stop()
{
    if (g.thread_running)
    {
        g.stop = true;
        pthread_join(g.thread, nullptr);
        g.thread_running = false;
        slp::log("audio: stopped; played %llu frames, dropped %llu, underruns %llu",
                 static_cast<unsigned long long>(g.played), static_cast<unsigned long long>(g.dropped),
                 static_cast<unsigned long long>(g.underruns));
    }
    if (g.port >= 0)
    {
        sceAudioOutClose(g.port);
        g.port = -1;
    }
    if (g.decoder)
    {
        opus_decoder_destroy(g.decoder);
        g.decoder = nullptr;
    }
}
} // namespace slp::audio
