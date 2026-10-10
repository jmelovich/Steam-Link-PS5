/*
 * Steam-Link-PS5 - Hardware video: VideoDec2 decode, then a CPU NV12->RGBA convert
 * into a frame the text/VideoOut path can show.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ProsperoLight presents the decoder's NV12 surface through AGC without a copy.
 * That needs the ps5-opengl SDK and ~1800 lines of GPU register programming. To
 * keep the build a plain `make`, we instead convert on the CPU into RGBA and let
 * the existing VideoOut path scan it out. Slower, but no extra dependency; AGC can
 * come later (M6).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace slp::video
{
// Load the video-decoder system module (207). Call once, in main, before the
// data directory is opened: VideoDec2 lazily loads codec PRXs and needs the
// title's original filesystem namespace.
void prepare_modules();

// Steam's video codec ids (ihslib IHS_StreamVideoCodec).
enum class Codec
{
    h264 = 4,
    hevc = 5,
};

// Create the decoder for a stream. codec_data is the Annex-B SPS/PPS, if the host
// sent any. Returns false and logs why on failure; the session keeps running
// without a picture. Safe to call again for a new stream (recreates the decoder).
bool start(std::uint32_t width, std::uint32_t height, Codec codec, const std::uint8_t *codec_data,
           std::size_t codec_data_len);

// Decode one Annex-B access unit. Returns true when a picture was produced and
// converted; the presenter then sees it through latest(). keyframe marks an IDR.
bool submit(const std::uint8_t *access_unit, std::size_t length, bool keyframe);

// Tear the decoder down (end of stream).
void stop();

// The newest decoded picture, as RGBA8888. Returns false until the first one.
// The buffer is held for the presenter until release(); the decoder writes the
// other buffers meanwhile (triple buffering), so there is no tearing.
struct Frame
{
    const std::uint32_t *rgba;
    std::uint32_t width;
    std::uint32_t height;
    std::uint64_t serial;
};
bool latest(Frame &out);
void release(const Frame &frame);

// Decode one keyframe from a packaged Annex-B H.264 clip and present it, so the
// decode/convert/present path can be checked with a known picture, independent of
// the network. Returns true if a picture was produced.
bool self_test(const char *annexb_path);
} // namespace slp::video
