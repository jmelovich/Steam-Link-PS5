/*
 * Steam-Link-PS5 - VideoDec2 decode + CPU NV12->RGBA. See video.hpp.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The decoder setup follows ProsperoLight's proven sequence (GPL-3.0-or-later):
 * load module 207, query and allocate compute memory, create a compute queue,
 * query and allocate the decoder's three memory regions, create the decoder at
 * pipeline depth one, and allocate fixed access-unit and frame pools from direct
 * memory. The ABI structs are theirs. Presentation is ours: a CPU colour convert
 * rather than AGC.
 */

#include "video.hpp"
#include "app_log.hpp"
#include "app_storage.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <unistd.h>

namespace slp::video
{
namespace
{
// ---- VideoDec2 ABI (ProsperoLight) -----------------------------------------
struct videodec2_decoder_config
{
    std::uint64_t size;
    std::uint32_t resource_type, codec_type, profile, max_level;
    std::int32_t max_width, max_height, max_dpb_frames;
    std::uint32_t pipeline_depth;
    std::uint64_t compute_queue, cpu_affinity;
    std::int32_t cpu_priority;
    std::uint32_t optimize_progressive, check_memory_type, reserved;
};
struct videodec2_decoder_memory
{
    std::uint64_t size, cpu_size;
    void *cpu;
    std::uint64_t gpu_size;
    void *gpu;
    std::uint64_t cpu_gpu_size;
    void *cpu_gpu;
    std::uint64_t max_frame_size;
    std::uint32_t frame_alignment, reserved;
};
struct videodec2_compute_config
{
    std::uint64_t size;
    std::uint16_t pipe_id, queue_id;
    std::uint8_t check_memory_type, reserved0;
    std::uint16_t reserved1;
};
struct videodec2_compute_memory
{
    std::uint64_t size, cpu_gpu_size;
    void *cpu_gpu;
};
struct videodec2_input
{
    std::uint64_t size;
    void *au;
    std::uint64_t au_size, pts, dts, attached;
};
struct videodec2_frame
{
    std::uint64_t size;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t accepted, reserved;
};
struct videodec2_output
{
    std::uint64_t size;
    std::uint8_t valid, error, picture_count, padding;
    std::uint32_t codec, width, pitch, height, reserved;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t frame_format, pitch_bytes;
};

extern "C"
{
    int sceSysmoduleLoadModule(std::uint32_t id);
    int sceKernelAllocateDirectMemory(std::int64_t start, std::int64_t end, std::size_t len,
                                      std::size_t align, int type, std::int64_t *physical);
    int sceKernelMapDirectMemory(void **address, std::size_t len, int protection, int flags,
                                 std::int64_t physical, std::size_t align);
    int sceKernelMunmap(void *address, std::size_t len);
    int sceKernelReleaseDirectMemory(std::int64_t start, std::size_t len);
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelMapNamedFlexibleMemory(void **address, std::size_t len, int protection, int flags,
                                        const char *name);
    int sceKernelReleaseFlexibleMemory(void *address, std::size_t len);

    std::int32_t sceVideodec2QueryComputeMemoryInfo(videodec2_compute_memory *memory);
    std::int32_t sceVideodec2AllocateComputeQueue(const videodec2_compute_config *config,
                                                  const videodec2_compute_memory *memory,
                                                  void **queue);
    std::int32_t sceVideodec2ReleaseComputeQueue(void *queue);
    std::int32_t sceVideodec2QueryDecoderMemoryInfo(const videodec2_decoder_config *config,
                                                    videodec2_decoder_memory *memory);
    std::int32_t sceVideodec2CreateDecoder(const videodec2_decoder_config *config,
                                           const videodec2_decoder_memory *memory, void **decoder);
    std::int32_t sceVideodec2DeleteDecoder(void *decoder);
    std::int32_t sceVideodec2Reset(void *decoder);
    std::int32_t sceVideodec2Decode(void *decoder, videodec2_input *input, videodec2_frame *frame,
                                    videodec2_output *output);
    std::int32_t sceVideodec2Flush(void *decoder, videodec2_frame *frame, videodec2_output *output);
}

// ---- small direct-memory allocator -----------------------------------------
constexpr std::size_t k_align_16k = 16 * 1024;
constexpr std::size_t k_input_slot = 0x800000;   // 8 MiB per access unit (ProsperoLight)
constexpr unsigned k_input_slots = 4;
constexpr unsigned k_frame_slots = 6;            // depth-one needs only a couple; headroom
constexpr std::uint64_t k_decoder_cpu_mask = 0x3fu;
constexpr std::int32_t k_decoder_priority = 700;

std::size_t align16k(std::size_t value)
{
    return (value + k_align_16k - 1) & ~(k_align_16k - 1);
}

// Direct-memory types. GARLIC (GPU) is what the decoder's work buffers want;
// ONION is CPU-coherent, so the CPU reads the GPU-written frame without a flush.
constexpr int k_mem_garlic = 12;
constexpr int k_mem_onion = 0;

struct DirectRegion
{
    void *address = nullptr;
    std::int64_t physical = -1;
    std::size_t length = 0;

    bool allocate(std::size_t bytes, int protection, std::int64_t limit, int memory_type = k_mem_garlic)
    {
        length = bytes;
        if (sceKernelAllocateDirectMemory(0, limit, bytes, k_align_16k, memory_type, &physical) != 0)
            return false;
        if (sceKernelMapDirectMemory(&address, bytes, protection, 0, physical, k_align_16k) != 0)
        {
            sceKernelReleaseDirectMemory(physical, bytes);
            physical = -1;
            return false;
        }
        return true;
    }
    void release()
    {
        if (address)
            sceKernelMunmap(address, length);
        if (physical >= 0)
            sceKernelReleaseDirectMemory(physical, length);
        address = nullptr;
        physical = -1;
        length = 0;
    }
};

// Protection bits: 0x1 CPU read, 0x2 CPU write, 0x10 GPU read, 0x20 GPU write.
constexpr int k_prot_gpu_rw = 0x32;             // GPU RW + CPU write (compute/decoder scratch)
constexpr int k_prot_cpu_read = 0x33;           // + CPU read, so we can convert the NV12

struct Decoder
{
    std::mutex mutex;
    void *decoder = nullptr;
    void *compute_queue = nullptr;
    DirectRegion compute;
    DirectRegion gpu;
    DirectRegion cpu_gpu;
    void *cpu_scratch = nullptr;
    std::size_t cpu_scratch_len = 0;
    DirectRegion input_pool;
    DirectRegion frame_pool;
    std::size_t input_slot_size = 0;
    std::size_t frame_slot_size = 0;
    unsigned input_sequence = 0;
    unsigned frame_sequence = 0;
    bool await_keyframe = true;
    bool needs_reset = false;
    videodec2_decoder_config config{};

    // RGBA output buffers form a small jitter buffer: decoded frames are queued (fifo)
    // and the presenter pops one per vblank, paced, so network/decode timing jitter does
    // not become on-screen judder. jitter_target frames are held before display; a larger
    // value absorbs more jitter at the cost of latency. Buffers not in the fifo, not being
    // presented and not mid-convert are free for the next decode.
    static constexpr int kMaxRgba = 8;
    std::uint32_t *rgba[kMaxRgba] = {};
    std::uint32_t rgba_capacity = 0;
    // Buffers are sized to the DECODER's maximum, not the first frame's size, so the host
    // raising the resolution mid-stream still fits (the picture is packed at its own width,
    // so a smaller frame in a bigger buffer is fine).
    std::uint32_t rgba_max_w = 0;
    std::uint32_t rgba_max_h = 0;
    int n_rgba = 0;                   // buffers actually allocated (jitter_target + headroom)
    int fifo[kMaxRgba] = {};          // ready-frame buffer indices, oldest at [0]
    std::uint64_t fifo_serial[kMaxRgba] = {};
    // Per-frame size: the queue can hold frames from before and after a resolution change.
    std::uint32_t fifo_w[kMaxRgba] = {};
    std::uint32_t fifo_h[kMaxRgba] = {};
    int fifo_count = 0;
    int presenting = -1;              // buffer currently on screen (held until the next pop)
    int converting = -1;              // buffer a decode is converting into (reserved, off-limits)
    std::uint64_t present_serial = 0; // serial currently presented (for the draw dedup)
    std::uint32_t present_w = 0;      // size of the frame currently presented
    std::uint32_t present_h = 0;
    // Best-known VISIBLE size, from the negotiated video config. The decoder reports the
    // CODED size (visible rounded up to the macroblock grid, e.g. 804 -> 816); we crop that
    // padding back to this. Re-learned from the decoder if the host changes resolution
    // mid-stream without ihslib re-issuing the config.
    std::uint32_t vis_w = 0;
    std::uint32_t vis_h = 0;
    std::uint32_t jitter_target = 2;  // frames to buffer before display; 0 = present newest now
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
    std::uint64_t serial = 0;         // newest decoded serial
};

Decoder g;

void release_all()
{
    if (g.decoder)
    {
        sceVideodec2DeleteDecoder(g.decoder);
        g.decoder = nullptr;
    }
    if (g.compute_queue)
    {
        sceVideodec2ReleaseComputeQueue(g.compute_queue);
        g.compute_queue = nullptr;
    }
    g.gpu.release();
    g.cpu_gpu.release();
    g.input_pool.release();
    g.frame_pool.release();
    g.compute.release();
    if (g.cpu_scratch)
    {
        sceKernelReleaseFlexibleMemory(g.cpu_scratch, g.cpu_scratch_len);
        sceKernelMunmap(g.cpu_scratch, g.cpu_scratch_len);
        g.cpu_scratch = nullptr;
    }
    for (auto *&buffer : g.rgba)
    {
        std::free(buffer);
        buffer = nullptr;
    }
    g.rgba_capacity = 0;
    g.rgba_max_w = 0;
    g.rgba_max_h = 0;
    g.n_rgba = 0;
    g.fifo_count = 0;
    g.presenting = -1;
    g.converting = -1;
    g.present_serial = 0;
    g.present_w = 0;
    g.present_h = 0;
    g.vis_w = 0;
    g.vis_h = 0;
}

// BT.601 limited-range NV12 -> RGBA8888 (Steam captures BT.601 limited). One
// picture at a time; the slow part, but correct and dependency-free.
void convert_nv12(const std::uint8_t *nv12, std::uint32_t pitch, std::uint32_t coded_height,
                  std::uint32_t width, std::uint32_t height, std::uint32_t *rgba)
{
    const std::uint8_t *y_plane = nv12;
    const std::uint8_t *uv_plane = nv12 + static_cast<std::size_t>(pitch) * coded_height;
    for (std::uint32_t row = 0; row < height; ++row)
    {
        const std::uint8_t *y_row = y_plane + static_cast<std::size_t>(row) * pitch;
        const std::uint8_t *uv_row = uv_plane + static_cast<std::size_t>(row / 2) * pitch;
        std::uint32_t *out = rgba + static_cast<std::size_t>(row) * width;
        for (std::uint32_t col = 0; col < width; ++col)
        {
            const int c = static_cast<int>(y_row[col]) - 16;
            const int d = static_cast<int>(uv_row[(col & ~1u)]) - 128;
            const int e = static_cast<int>(uv_row[(col & ~1u) + 1]) - 128;
            auto clip = [](int v) -> std::uint32_t {
                return static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            const std::uint32_t r = clip((298 * c + 409 * e + 128) >> 8);
            const std::uint32_t gg = clip((298 * c - 100 * d - 208 * e + 128) >> 8);
            const std::uint32_t b = clip((298 * c + 516 * d + 128) >> 8);
            // The VideoOut buffer is little-endian RGBA: red in the low byte.
            out[col] = 0xff000000u | (b << 16) | (gg << 8) | r;
        }
    }
}
// ---- convert thread pool -----------------------------------------------------
// One frame converts ~1.5 M pixels; split across workers it stays well under a
// frame period on the console's Zen 2 cores. Workers are persistent; a job is a
// generation bump, each worker takes its band of rows.
constexpr int k_workers = 4;

struct ConvertJob
{
    const std::uint8_t *nv12;
    std::uint32_t pitch, coded_height, width, height;
    std::uint32_t *rgba;
};

struct Pool
{
    pthread_t threads[k_workers]{};
    bool started = false;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
    pthread_cond_t done = PTHREAD_COND_INITIALIZER;
    std::uint64_t generation = 0;
    int pending = 0;
    bool quit = false;
    ConvertJob job{};
};
Pool pool;

void convert_band(const ConvertJob &job, int worker)
{
    const std::uint32_t band = (job.height + k_workers - 1) / k_workers;
    const std::uint32_t first = band * static_cast<std::uint32_t>(worker);
    if (first >= job.height)
        return;
    const std::uint32_t rows = first + band > job.height ? job.height - first : band;
    // Same maths as convert_nv12, over rows [first, first + rows).
    const std::uint8_t *y_plane = job.nv12;
    const std::uint8_t *uv_plane = job.nv12 + static_cast<std::size_t>(job.pitch) * job.coded_height;
    for (std::uint32_t row = first; row < first + rows; ++row)
    {
        const std::uint8_t *y_row = y_plane + static_cast<std::size_t>(row) * job.pitch;
        const std::uint8_t *uv_row = uv_plane + static_cast<std::size_t>(row / 2) * job.pitch;
        std::uint32_t *out = job.rgba + static_cast<std::size_t>(row) * job.width;
        for (std::uint32_t col = 0; col < job.width; ++col)
        {
            const int c = static_cast<int>(y_row[col]) - 16;
            const int d = static_cast<int>(uv_row[(col & ~1u)]) - 128;
            const int e = static_cast<int>(uv_row[(col & ~1u) + 1]) - 128;
            auto clip = [](int v) -> std::uint32_t {
                return static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            const std::uint32_t r = clip((298 * c + 409 * e + 128) >> 8);
            const std::uint32_t gg = clip((298 * c - 100 * d - 208 * e + 128) >> 8);
            const std::uint32_t b = clip((298 * c + 516 * d + 128) >> 8);
            out[col] = 0xff000000u | (b << 16) | (gg << 8) | r;
        }
    }
}

void *worker_main(void *arg)
{
    const int worker = static_cast<int>(reinterpret_cast<std::intptr_t>(arg));
    std::uint64_t seen = 0;
    for (;;)
    {
        pthread_mutex_lock(&pool.mutex);
        while (!pool.quit && pool.generation == seen)
            pthread_cond_wait(&pool.wake, &pool.mutex);
        if (pool.quit)
        {
            pthread_mutex_unlock(&pool.mutex);
            return nullptr;
        }
        seen = pool.generation;
        const ConvertJob job = pool.job;
        pthread_mutex_unlock(&pool.mutex);
        convert_band(job, worker);
        pthread_mutex_lock(&pool.mutex);
        if (--pool.pending == 0)
            pthread_cond_broadcast(&pool.done);
        pthread_mutex_unlock(&pool.mutex);
    }
}

void pool_start()
{
    if (pool.started)
        return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    for (int i = 0; i < k_workers; ++i)
        pthread_create(&pool.threads[i], &attr, worker_main, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
    pthread_attr_destroy(&attr);
    pool.started = true;
}

void convert_parallel(const ConvertJob &job)
{
    if (!pool.started)
    {
        convert_nv12(job.nv12, job.pitch, job.coded_height, job.width, job.height, job.rgba);
        return;
    }
    pthread_mutex_lock(&pool.mutex);
    pool.job = job;
    pool.pending = k_workers;
    ++pool.generation;
    pthread_cond_broadcast(&pool.wake);
    while (pool.pending > 0)
        pthread_cond_wait(&pool.done, &pool.mutex);
    pthread_mutex_unlock(&pool.mutex);
}
} // namespace

void prepare_modules()
{
    const int result = sceSysmoduleLoadModule(207);
    slp::log("video: load module 207 -> 0x%x", static_cast<unsigned>(result));
}

bool start(std::uint32_t width, std::uint32_t height, Codec codec, const std::uint8_t *,
           std::size_t codec_data_len)
{
    std::lock_guard lock{g.mutex};
    release_all();

    const std::int64_t limit = static_cast<std::int64_t>(sceKernelGetDirectMemorySize());

    // Compute memory and queue.
    videodec2_compute_memory compute_memory{};
    compute_memory.size = sizeof(compute_memory);
    std::int32_t result = sceVideodec2QueryComputeMemoryInfo(&compute_memory);
    if (result != 0)
    {
        slp::log("video: compute query failed 0x%x", static_cast<unsigned>(result));
        return false;
    }
    if (!g.compute.allocate(align16k(compute_memory.cpu_gpu_size), k_prot_cpu_read, limit))
    {
        slp::log("video: compute alloc failed (%llu bytes)",
                 static_cast<unsigned long long>(compute_memory.cpu_gpu_size));
        return false;
    }
    compute_memory.cpu_gpu = g.compute.address;
    compute_memory.cpu_gpu_size = g.compute.length;
    videodec2_compute_config compute_config{};
    compute_config.size = sizeof(compute_config);
    result = sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory, &g.compute_queue);
    if (result != 0)
    {
        slp::log("video: compute queue failed 0x%x", static_cast<unsigned>(result));
        return false;
    }

    // Decoder config for this mode. H.264 High / HEVC Main, 8-bit, up to 1088 lines.
    const bool hevc = codec == Codec::hevc;
    g.config = {};
    g.config.size = sizeof(g.config);
    g.config.resource_type = 1;
    g.config.codec_type = hevc ? 2u : 1u;             // VideoDec2 codec ids (ProsperoLight modes)
    g.config.profile = hevc ? 0x000ee049u : 100u;     // HEVC Main / H.264 High
    g.config.max_level = hevc ? 153u : 51u;
    // Size the decoder to the actual stream, not a fixed 1920x1088: the host can be an
    // ultrawide or a 1440p/4K display, and anything wider than the configured max was
    // cropped before our scaler ever saw it. Keep >= 1920x1088 for headroom and cap at
    // 4096x2176 (H.264 level 5.1 / HEVC 5.1). The display scaler fits whatever size to 1080p.
    auto align16 = [](std::uint32_t v) { return (v + 15u) & ~15u; };
    std::uint32_t mw = width < 1920 ? 1920 : (width > 4096 ? 4096 : width);
    std::uint32_t mh = height < 1088 ? 1088 : (height > 2176 ? 2176 : height);
    g.config.max_width = align16(mw);
    g.config.max_height = align16(mh);
    g.config.max_dpb_frames = 4;
    g.config.pipeline_depth = 1;
    g.config.compute_queue = reinterpret_cast<std::uint64_t>(g.compute_queue);
    g.config.cpu_affinity = k_decoder_cpu_mask;
    g.config.cpu_priority = k_decoder_priority;
    g.config.optimize_progressive = 1;

    videodec2_decoder_memory memory{};
    memory.size = sizeof(memory);
    result = sceVideodec2QueryDecoderMemoryInfo(&g.config, &memory);
    if (result != 0)
    {
        slp::log("video: decoder memory query failed 0x%x", static_cast<unsigned>(result));
        return false;
    }
    slp::log("video: decoder memory cpu=%llu gpu=%llu shared=%llu frame=%llu align=%u",
             static_cast<unsigned long long>(memory.cpu_size),
             static_cast<unsigned long long>(memory.gpu_size),
             static_cast<unsigned long long>(memory.cpu_gpu_size),
             static_cast<unsigned long long>(memory.max_frame_size), memory.frame_alignment);

    g.cpu_scratch_len = align16k(memory.cpu_size);
    if (sceKernelMapNamedFlexibleMemory(&g.cpu_scratch, g.cpu_scratch_len, 0x03, 0,
                                        "SteamVdecCpu") != 0)
    {
        slp::log("video: cpu scratch map failed (%zu bytes)", g.cpu_scratch_len);
        return false;
    }
    memory.cpu = g.cpu_scratch;
    if (!g.gpu.allocate(align16k(memory.gpu_size), k_prot_gpu_rw, limit))
    {
        slp::log("video: gpu region alloc failed");
        return false;
    }
    memory.gpu = g.gpu.address;
    memory.gpu_size = g.gpu.length;
    if (memory.cpu_gpu_size != 0)
    {
        if (!g.cpu_gpu.allocate(align16k(memory.cpu_gpu_size), k_prot_cpu_read, limit))
        {
            slp::log("video: cpu_gpu region alloc failed");
            return false;
        }
        memory.cpu_gpu = g.cpu_gpu.address;
        memory.cpu_gpu_size = g.cpu_gpu.length;
    }
    result = sceVideodec2CreateDecoder(&g.config, &memory, &g.decoder);
    if (result != 0)
    {
        slp::log("video: create decoder failed 0x%x", static_cast<unsigned>(result));
        return false;
    }

    // Access-unit and frame pools. The frame pool is CPU-readable so we can convert.
    g.input_slot_size = k_input_slot;
    g.frame_slot_size = align16k(memory.max_frame_size);
    // The frame pool is ONION so the CPU reads the decoder's output coherently
    // (GARLIC would need an explicit GPU cache flush we have no primitive for).
    if (!g.input_pool.allocate(g.input_slot_size * k_input_slots, k_prot_gpu_rw, limit) ||
        !g.frame_pool.allocate(g.frame_slot_size * k_frame_slots, k_prot_cpu_read, limit, k_mem_onion))
    {
        slp::log("video: pool alloc failed");
        return false;
    }
    result = sceVideodec2Reset(g.decoder);
    if (result != 0)
    {
        slp::log("video: reset failed 0x%x", static_cast<unsigned>(result));
        return false;
    }

    // RGBA output buffers (visible size; the stream picks the height, e.g. 804). The jitter
    // buffer needs jitter_target queued + 1 presenting + 1 converting + 1 free, so allocate
    // jitter_target + 3 buffers (clamped to the array size).
    g.jitter_target = slp::settings().jitter_frames;
    if (g.jitter_target > 4)
        g.jitter_target = 4;
    g.n_rgba = static_cast<int>(g.jitter_target) + 3;
    if (g.n_rgba > Decoder::kMaxRgba)
        g.n_rgba = Decoder::kMaxRgba;
    // Size the buffers to the decoder's maximum, NOT this first frame. The host can raise the
    // resolution mid-stream (launch a game at 1280x536, then switch the display to 1920x1080)
    // without ihslib re-issuing the video-start config, and the decoder will then hand us a
    // bigger picture. Sizing to the first frame meant such a frame had to be clamped, which
    // showed as the top-left corner cropped and blown up. Each picture is packed at its own
    // width, so a smaller frame living in a larger buffer costs nothing but the memory.
    g.rgba_max_w = static_cast<std::uint32_t>(g.config.max_width);
    g.rgba_max_h = static_cast<std::uint32_t>(g.config.max_height);
    g.vis_w = width;
    g.vis_h = height;
    g.frame_width = width;
    g.frame_height = height;
    g.rgba_capacity = g.rgba_max_w * g.rgba_max_h;
    for (int i = 0; i < g.n_rgba; ++i)
    {
        g.rgba[i] = static_cast<std::uint32_t *>(std::malloc(g.rgba_capacity * sizeof(std::uint32_t)));
        if (!g.rgba[i])
        {
            slp::log("video: rgba alloc failed");
            return false;
        }
    }
    g.fifo_count = 0;
    g.presenting = -1;
    g.converting = -1;
    g.present_serial = 0;
    slp::log("video: jitter buffer %u frames (%d rgba buffers)", g.jitter_target, g.n_rgba);
    pool_start();
    g.serial = 0;
    g.input_sequence = 0;
    g.frame_sequence = 0;
    g.await_keyframe = true;
    g.needs_reset = false;
    slp::log("video: decoder ready %ux%u %s (frame slot %zu bytes)", width, height,
             hevc ? "hevc" : "h264", g.frame_slot_size);
    (void)codec_data_len;
    return true;
}

// --- jitter-buffer helpers; call with g.mutex held --------------------------------------
// A buffer index free to decode into: not queued, not on screen, not mid-convert.
int reserve_free_locked()
{
    for (int i = 0; i < g.n_rgba; ++i)
    {
        if (i == g.presenting || i == g.converting)
            continue;
        bool queued = false;
        for (int j = 0; j < g.fifo_count; ++j)
            if (g.fifo[j] == i)
            {
                queued = true;
                break;
            }
        if (!queued)
            return i;
    }
    return -1;
}
void fifo_pop_front_locked()
{
    for (int j = 0; j < g.fifo_count - 1; ++j)
    {
        g.fifo[j] = g.fifo[j + 1];
        g.fifo_serial[j] = g.fifo_serial[j + 1];
        g.fifo_w[j] = g.fifo_w[j + 1];
        g.fifo_h[j] = g.fifo_h[j + 1];
    }
    if (g.fifo_count > 0)
        --g.fifo_count;
}

bool submit(const std::uint8_t *access_unit, std::size_t length, bool keyframe)
{
    std::unique_lock lock{g.mutex};
    if (!g.decoder || !access_unit || length == 0 || length > g.input_slot_size)
        return false;
    if (g.await_keyframe)
    {
        if (!keyframe)
            return false;
        g.await_keyframe = false;
    }
    if (g.needs_reset)
    {
        if (sceVideodec2Reset(g.decoder) != 0)
        {
            g.await_keyframe = true;
            return false;
        }
        g.needs_reset = false;
    }
    g.needs_reset = true; // cleared only on a clean decode

    std::uint8_t *input_slot = static_cast<std::uint8_t *>(g.input_pool.address) +
                               static_cast<std::size_t>(g.input_sequence++ % k_input_slots) *
                                   g.input_slot_size;
    std::memcpy(input_slot, access_unit, length);

    std::uint8_t *frame_slot = static_cast<std::uint8_t *>(g.frame_pool.address) +
                               static_cast<std::size_t>(g.frame_sequence++ % k_frame_slots) *
                                   g.frame_slot_size;

    videodec2_input input{};
    input.size = sizeof(input);
    input.au = input_slot;
    input.au_size = length;
    input.pts = 0;
    input.dts = UINT64_MAX;
    videodec2_frame frame{};
    frame.size = sizeof(frame);
    frame.buffer = frame_slot;
    frame.buffer_size = g.frame_slot_size;
    videodec2_output output{};
    output.size = sizeof(output);

    std::int32_t result = sceVideodec2Decode(g.decoder, &input, &frame, &output);
    if (result != 0 || output.error)
    {
        static int logged = 0;
        if (logged++ < 8)
            slp::log("video: decode rc=0x%x err=%u", static_cast<unsigned>(result), output.error);
        g.await_keyframe = true;
        return false;
    }
    if (!output.valid)
    {
        // Depth one: the picture is ready after a flush into the same buffer.
        std::memset(&output, 0, sizeof(output));
        output.size = sizeof(output);
        result = sceVideodec2Flush(g.decoder, &frame, &output);
        if (result != 0 || output.error || !output.valid)
        {
            static int logged = 0;
            if (logged++ < 8)
                slp::log("video: flush rc=0x%x err=%u valid=%u", static_cast<unsigned>(result),
                         output.error, output.valid);
            g.await_keyframe = true;
            return false;
        }
    }
    g.needs_reset = false;

    const auto *nv12 = static_cast<const std::uint8_t *>(output.buffer);
    const std::uint32_t pitch = output.pitch_bytes ? output.pitch_bytes : output.pitch;
    const std::uint32_t decoded_width = output.width ? output.width : g.frame_width;
    const std::uint32_t decoded_height = output.height ? output.height : g.frame_height;
    // The decoder hands us the CODED size: the visible size rounded up to the macroblock grid
    // (e.g. 1920x804 arrives as 1920x816). Crop that padding back to the negotiated visible
    // size. But if the picture differs by MORE than one macroblock, the host changed resolution
    // mid-stream and ihslib did not re-issue the video config, so the negotiated size is stale:
    // re-learn it from the decoder instead of cropping the new picture to the old size. That
    // stale-size crop was the "cropped in" bug - it showed the top-left corner blown up, and
    // because the old code wrote the clamped value back as the new bound it ratcheted down and
    // never recovered.
    if (decoded_width > g.vis_w + 15u || decoded_height > g.vis_h + 15u ||
        decoded_width + 15u < g.vis_w || decoded_height + 15u < g.vis_h)
    {
        slp::log("video: stream size changed to %ux%u (was %ux%u), rescaling", decoded_width,
                 decoded_height, g.vis_w, g.vis_h);
        g.vis_w = decoded_width;
        g.vis_h = decoded_height;
    }
    std::uint32_t width = decoded_width < g.vis_w ? decoded_width : g.vis_w;
    std::uint32_t height = decoded_height < g.vis_h ? decoded_height : g.vis_h;
    if (width > g.rgba_max_w || height > g.rgba_max_h)
    {
        // A real overflow: the stream exceeded the decoder's max, so the session must be
        // re-negotiated to show it whole. Worth shouting about (once per new size).
        static std::uint32_t warned_w = 0, warned_h = 0;
        if (width != warned_w || height != warned_h)
        {
            warned_w = width;
            warned_h = height;
            slp::log("video: decoded %ux%u exceeds decoder max %ux%u, cropping", width, height,
                     g.rgba_max_w, g.rgba_max_h);
        }
        if (width > g.rgba_max_w)
            width = g.rgba_max_w;
        if (height > g.rgba_max_h)
            height = g.rgba_max_h;
    }

    static int first = 0;
    if (first++ == 0)
        slp::log("video: first picture %ux%u pitch=%u coded_h=%u format=0x%x", output.width,
                 output.height, pitch, output.height, output.frame_format);

    // Reserve a free buffer to convert into; if every buffer is busy, drop the oldest
    // queued frame to free one (the presenter is draining slower than we decode).
    int back = reserve_free_locked();
    if (back < 0)
    {
        back = g.fifo[0];
        fifo_pop_front_locked();
    }
    g.converting = back;
    const std::uint32_t w = width, h = height;
    // Convert OUTSIDE the lock: the CPU NV12->RGBA is the slow part, and holding g.mutex
    // across it would block the presenter's latest()/release() every frame -> missed
    // vblanks -> judder. `back` is reserved (converting) and the decoder's frame slot is
    // stable until the next decode (submit is single-threaded), so the convert needs no lock.
    lock.unlock();
    convert_parallel({nv12, pitch, decoded_height, w, h, g.rgba[back]});
    lock.lock();
    g.converting = -1;
    g.frame_width = w;
    g.frame_height = h;
    ++g.serial;
    g.fifo[g.fifo_count] = back;
    g.fifo_serial[g.fifo_count] = g.serial;
    g.fifo_w[g.fifo_count] = w;
    g.fifo_h[g.fifo_count] = h;
    ++g.fifo_count;
    // Cap latency: never buffer much beyond the target (catch up by dropping the oldest).
    while (g.fifo_count > static_cast<int>(g.jitter_target) + 2)
        fifo_pop_front_locked();
    return true;
}

void stop()
{
    std::lock_guard lock{g.mutex};
    release_all();
}

bool latest(Frame &out)
{
    std::lock_guard lock{g.mutex};
    if (g.jitter_target == 0)
    {
        // Lowest latency: jump to the newest ready frame, discard any older queued ones.
        while (g.fifo_count > 1)
            fifo_pop_front_locked();
        if (g.fifo_count == 1)
        {
            g.presenting = g.fifo[0];
            g.present_serial = g.fifo_serial[0];
            g.present_w = g.fifo_w[0];
            g.present_h = g.fifo_h[0];
            fifo_pop_front_locked();
        }
    }
    else
    {
        // Paced jitter buffer: advance only while MORE than the target depth is queued, so
        // the buffer keeps ~jitter_target frames in hand and the display advances at the rate
        // frames actually arrive, whatever the vblank rate is. A late frame draws from the
        // buffer instead of repeating the last one; a burst fills the buffer instead of being
        // dropped. This also makes a source slower than the display correct for free: a 60 fps
        // stream on a 120 Hz output simply holds each frame for two vblanks (frame doubling)
        // instead of thrashing between hold-and-refill, which is what a "one frame per vblank"
        // rule would have done.
        if (g.fifo_count > static_cast<int>(g.jitter_target))
        {
            g.presenting = g.fifo[0];
            g.present_serial = g.fifo_serial[0];
            g.present_w = g.fifo_w[0];
            g.present_h = g.fifo_h[0];
            fifo_pop_front_locked();
        }
    }
    if (g.presenting < 0)
        return false; // nothing decoded yet (or still priming the very first frames)
    out.rgba = g.rgba[g.presenting];
    // The size of the frame actually on screen, not the newest decode: after a mid-stream
    // resolution change the queue briefly holds both sizes, and scaling a queued old frame
    // with the new frame's dimensions would skew/crop it.
    out.width = g.present_w;
    out.height = g.present_h;
    out.serial = g.present_serial;
    return true;
}

void release(const Frame &)
{
    // The on-screen buffer stays reserved as g.presenting until the next latest() swaps it,
    // so there is nothing to free here.
}

bool self_test(const char *annexb_path)
{
    const int fd = open(annexb_path, O_RDONLY);
    if (fd < 0)
    {
        slp::log("self-test: cannot open %s", annexb_path);
        return false;
    }
    static std::uint8_t clip[256 * 1024];
    const long total = read(fd, clip, sizeof clip);
    close(fd);
    if (total <= 0)
    {
        slp::log("self-test: empty clip");
        return false;
    }

    // The first access unit: from the start through the first coded picture, i.e.
    // up to the second VCL NAL (slice type 1 or 5). Annex-B start codes are 3 or 4
    // bytes; the NAL type is the low 5 bits of the byte after the code.
    auto is_start = [&](long i, int &code_len) -> bool {
        if (i + 3 < total && clip[i] == 0 && clip[i + 1] == 0 && clip[i + 2] == 0 && clip[i + 3] == 1)
        {
            code_len = 4;
            return true;
        }
        if (i + 2 < total && clip[i] == 0 && clip[i + 1] == 0 && clip[i + 2] == 1)
        {
            code_len = 3;
            return true;
        }
        return false;
    };
    long au_end = total;
    int vcl_seen = 0;
    for (long i = 0; i + 2 < total;)
    {
        int code_len = 0;
        if (is_start(i, code_len))
        {
            const int nal_type = clip[i + code_len] & 0x1f;
            if (nal_type == 1 || nal_type == 5)
            {
                if (++vcl_seen == 2)
                {
                    au_end = i;
                    break;
                }
            }
            i += code_len;
        }
        else
        {
            ++i;
        }
    }

    slp::log("self-test: clip %ld bytes, first access unit %ld bytes", total, au_end);
    if (!start(1920, 1080, Codec::h264, nullptr, 0))
        return false;
    const bool ok = submit(clip, static_cast<std::size_t>(au_end), true);
    slp::log("self-test: decode %s", ok ? "produced a picture" : "failed");
    return ok;
}
} // namespace slp::video
