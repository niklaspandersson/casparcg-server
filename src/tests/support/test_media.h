#pragma once

#include <core/frame/frame.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace caspar { namespace test {

// Generated test media. Every frame carries its index as a barcode, and the audio is a ramp of
// sample numbers, so a test checks exactly which frame and which samples came out.
//
// Barcode: 18 cells of equal width across the full picture width, from the top to height / 8.
// Cell 0 is white and cell 1 black (markers), cells 2..17 hold the index, most significant bit
// first, white = 1. White is luma 235 and black luma 16 (video range); the rest of the picture is
// mid grey. Each cell is read at its centre, so the stamp survives lossy codecs and scaling.
//
// Audio: sample n of the clip holds (n << 8) | channel in every channel, as signed 32-bit.

constexpr int max_stamped_index = 0xFFFF;

struct clip_options
{
    std::wstring format       = L"720p5000"; // size, frame rate and scan from a channel format name
    int          frames       = 50;
    std::string  codec        = "ffv1"; // any FFmpeg encoder name, e.g. "mpeg4", "libx264", "prores_ks"
    std::string  pixel_format = "yuv420p";
    std::string  extension    = "mov"; // picks the container. MKV rounds timestamps to whole ms,
                                       // so audio isn't sample-accurate after a seek
    bool        audio          = true;
    int         audio_channels = 2;
    std::string audio_codec    = "pcm_s32le";
    std::string name           = "clip";
};

// Encodes a clip into output_dir() and returns its path.
std::filesystem::path make_clip(const clip_options& options = {});

// The same, to a path of the caller's choosing (`name` is ignored; the extension picks the
// container). The caspar_make_clip tool uses it for end-to-end tests.
void write_clip(const std::filesystem::path& path, const clip_options& options);

// Writes the barcode for `index` into an 8-bit luma plane.
void stamp_index(std::uint8_t* luma, int width, int height, int linesize, int index);

// Reads the barcode from an 8-bit luma plane, or nothing if there is no readable stamp.
std::optional<int> read_index(const std::uint8_t* luma, int width, int height, int linesize);

// Reads the barcode from a frame in any of the mixer's pixel formats, 8 or 16 bits.
std::optional<int> read_index(const core::const_frame& frame);

// Sample numbers of the audio ramp in channel 0 of interleaved samples with `channels` channels.
std::vector<std::int64_t> ramp_samples(const std::int32_t* samples, std::size_t count, int channels);
std::vector<std::int64_t> ramp_samples(const core::const_frame& frame, int channels);

struct decoded_clip
{
    std::vector<std::optional<int>> indices;       // per decoded video frame
    std::vector<std::int64_t>       audio_samples; // ramp sample numbers, channel 0
    int                             audio_channels = 0;
};

// Decodes a file written by make_clip() or by a consumer.
decoded_clip decode_clip(const std::filesystem::path& path);

}} // namespace caspar::test
