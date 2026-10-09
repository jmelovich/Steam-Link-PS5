/*
 * steamlink-ps5 - VideoDec2 decode + CPU NV12->RGBA. See video.hpp.
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
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

    // Two RGBA output buffers, so the presenter reads one while we write the other.
    std::uint32_t *rgba[2] = {nullptr, nullptr};
    std::uint32_t rgba_capacity = 0;
    int rgba_front = -1;
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
    std::uint64_t serial = 0;
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
    g.rgba_front = -1;
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
    g.config.max_width = 1920;
    g.config.max_height = 1088;
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

    // RGBA output buffers (visible size; the stream picks the height, e.g. 804).
    g.frame_width = width;
    g.frame_height = height;
    g.rgba_capacity = width * height;
    for (auto *&buffer : g.rgba)
    {
        buffer = static_cast<std::uint32_t *>(std::malloc(g.rgba_capacity * sizeof(std::uint32_t)));
        if (!buffer)
        {
            slp::log("video: rgba alloc failed");
            return false;
        }
    }
    g.rgba_front = -1;
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

bool submit(const std::uint8_t *access_unit, std::size_t length, bool keyframe)
{
    std::lock_guard lock{g.mutex};
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
    const std::uint32_t width = decoded_width < g.frame_width ? decoded_width : g.frame_width;
    const std::uint32_t height = decoded_height < g.frame_height ? decoded_height : g.frame_height;

    static int first = 0;
    if (first++ == 0)
        slp::log("video: first picture %ux%u pitch=%u coded_h=%u format=0x%x", output.width,
                 output.height, pitch, output.height, output.frame_format);

    const int back = g.rgba_front == 0 ? 1 : 0;
    convert_nv12(nv12, pitch, decoded_height, width, height, g.rgba[back]);
    g.rgba_front = back;
    g.frame_width = width;
    g.frame_height = height;
    ++g.serial;
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
    if (g.rgba_front < 0)
        return false;
    out.rgba = g.rgba[g.rgba_front];
    out.width = g.frame_width;
    out.height = g.frame_height;
    out.serial = g.serial;
    return true;
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
