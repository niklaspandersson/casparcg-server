/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Julian Waller, julian@superfly.tv
 */

#include "../StdAfx.h"

#include "frame.h"

#include <common/memshfl.h>

#include <tbb/parallel_for.h>
#include <tbb/scalable_allocator.h>

namespace caspar { namespace decklink {

std::vector<float>   bt709{0.2126, 0.7152, 0.0722, -0.1146, -0.3854, 0.5, 0.5, -0.4542, -0.0458};
std::vector<int32_t> create_int_matrix(const std::vector<float>& matrix)
{
    static const float LumaRangeWidth   = 876.f * (1024.f / 1023.f); // 876;
    static const float ChromaRangeWidth = 896.f * (1024.f / 1023.f); // 896;

    std::vector<float> color_matrix_f(matrix);

    color_matrix_f[0] *= LumaRangeWidth;
    color_matrix_f[1] *= LumaRangeWidth;
    color_matrix_f[2] *= LumaRangeWidth;

    color_matrix_f[3] *= ChromaRangeWidth;
    color_matrix_f[4] *= ChromaRangeWidth;
    color_matrix_f[5] *= ChromaRangeWidth;
    color_matrix_f[6] *= ChromaRangeWidth;
    color_matrix_f[7] *= ChromaRangeWidth;
    color_matrix_f[8] *= ChromaRangeWidth;

    std::vector<int32_t> int_matrix(color_matrix_f.size());

    transform(color_matrix_f.cbegin(), color_matrix_f.cend(), int_matrix.begin(), [](const float& f) {
        return (int32_t)round(f * 1024.f);
    });

    return int_matrix;
};

BMDPixelFormat get_pixel_format(bool hdr) { return hdr ? bmdFormat10BitYUV : bmdFormat8BitBGRA; }

int get_row_bytes(BMDPixelFormat pix_fmt, int width)
{
    switch (pix_fmt) {
        case bmdFormat10BitYUV:
            return ((width + 47) / 48) * 128;
        case bmdFormat10BitRGBXLE:
            return ((width + 63) / 64) * 256;
        default:
            break;
    }

    return width * 4;
}

std::shared_ptr<void> allocate_frame_data(const core::video_format_desc& format_desc, BMDPixelFormat pix_fmt)
{
    auto alignment = 256;
    auto size      = get_row_bytes(pix_fmt, format_desc.width) * format_desc.height;
    return create_aligned_buffer(size, alignment);
}

std::shared_ptr<void> convert_to_key_only(const std::shared_ptr<void>& image_data, std::size_t byte_count)
{
    auto key_data = create_aligned_buffer(byte_count);

    aligned_memshfl(key_data.get(), image_data.get(), byte_count, 0x0F0F0F0F, 0x0B0B0B0B, 0x07070707, 0x03030303);

    return key_data;
}

void convert_frame(const core::video_format_desc& channel_format_desc,
                   const core::video_format_desc& decklink_format_desc,
                   const port_configuration&      config,
                   std::shared_ptr<void>&         image_data,
                   bool                           topField,
                   const core::const_frame&       frame,
                   bool                           hdr)
{
    // No point copying an empty frame
    if (!frame)
        return;

    int firstLine = topField ? 0 : 1;

    if (channel_format_desc.format == decklink_format_desc.format && config.src_x == 0 && config.src_y == 0 &&
        config.region_w == 0 && config.region_h == 0 && config.dest_x == 0 && config.dest_y == 0) {
        // Fast path

        if (hdr) {
            auto color_matrix = create_int_matrix(bt709);

            // Pack R16G16B16A16 as v210
            const int NUM_THREADS     = 8;
            auto      rows_per_thread = decklink_format_desc.height / NUM_THREADS;
            size_t    byte_count_line = get_row_bytes(bmdFormat10BitYUV, decklink_format_desc.width);
            int       fullspeed_x     = decklink_format_desc.width / 48;
            tbb::parallel_for(0, NUM_THREADS, [&](int i) {
                auto    end       = (i + 1) * rows_per_thread;
                // __m128i luma_mult = _mm_set_epi16(4, 1, 16, 4, 1, 16, 0, 0);
                // __m128i luma_shuf = _mm_set_epi8(-1, 0, 1, -1, 2, 3, 4, 5, -1, 6, 7, -1, 8, 9, 10, 11);
                __m128i luma_mult = _mm_set_epi16(0, 0, 16, 1, 4, 16, 1, 4);
                __m128i luma_shuf = _mm_set_epi8(11, 10, 9, 8, -1, 7, 6, -1, 5, 4, 3, 2, -1, 1, 0, -1);

                // __m128i chroma_mult = _mm_set_epi16(1, 16, 4, 1, 16, 4, 0, 0);
                // __m128i chroma_shuf = _mm_set_epi8(0, 1, 2, 3, -1, 4, 5, -1, 6, 7, 8, 9, -1, 10, 11, -1);

                __m256i zero     = _mm256_setzero_si256();
                __m256i y_offset = _mm256_set1_epi32(64 << 20);
                __m256i c_offset = _mm256_set1_epi32(1025 << 19);
                __m128i yc_ctmp = _mm_set_epi32(0, color_matrix[2], color_matrix[1], color_matrix[0]);
                __m128i cb_ctmp = _mm_set_epi32(0, color_matrix[8], color_matrix[7], color_matrix[6]);
                __m128i cr_ctmp = _mm_set_epi32(0, color_matrix[5], color_matrix[4], color_matrix[3]);

                __m256i y_coeff  = _mm256_set_m128i(yc_ctmp, yc_ctmp);
                __m256i cb_coeff = _mm256_set_m128i(cb_ctmp, cb_ctmp);
                __m256i cr_coeff = _mm256_set_m128i(cr_ctmp, cr_ctmp);
                for (int y = firstLine + i * rows_per_thread; y < end; y += decklink_format_desc.field_count) {
                    auto dest = reinterpret_cast<uint32_t*>(image_data.get()) + (long long)y * byte_count_line / 4;
                    __m128i* v210_dest = reinterpret_cast<__m128i*>(dest);

                    for (int x = 0; x < fullspeed_x; x++) {
                        auto src = reinterpret_cast<const uint16_t*>(
                            frame.image_data(0).data() + ((long long)y * decklink_format_desc.width + x * 48) * 8);


                        // Load pixels
                        const __m256i* pixeldata = reinterpret_cast<const __m256i*>(src);

                        __m256i luma[6];
                        __m256i chroma[6];

                        for (int p = 0; p < 6; p++) {
                            __m256i p0123 = _mm256_load_si256(pixeldata + p * 2);
                            __m256i p4567 = _mm256_load_si256(pixeldata + p * 2 + 1);

                            // shift down to 10 bit precision
                            p0123 = _mm256_srli_epi16(p0123, 6);    //r0 g0 b0 a0 r1 g1 b1 a1 r2 g2 b2 a2 r3 g3 b3 a3
                            p4567 = _mm256_srli_epi16(p4567, 6);

                            // unpack 16 bit values to 32 bit registers, padding with zeros
                            __m256i pixel_pairs[4];
                            pixel_pairs[0] = _mm256_unpacklo_epi16(p0123, zero); // pixels 0 2  //r0 0 g0 0 b0 0 a0 0 r2 0 g2 0 b2 0 a2 0
                            pixel_pairs[1] = _mm256_unpackhi_epi16(p0123, zero); // pixels 1 3  //r1 0 g1 0 b1 0 a1 0 r3 0 g3 0 b3 0 a3 0
                            pixel_pairs[2] = _mm256_unpacklo_epi16(p4567, zero); // pixels 4 6
                            pixel_pairs[3] = _mm256_unpackhi_epi16(p4567, zero); // pixels 5 7

                            /* COMPUTE LUMA */
                            {
                                // Multiply by y-coefficients
                                __m256i y4[4];
                                for (int m = 0; m < 4; m++) {
                                    y4[m] = _mm256_mullo_epi32(pixel_pairs[m], y_coeff);
                                }

                                // sum products
                                __m256i y2_sum0123    = _mm256_hadd_epi32(y4[0], y4[1]);
                                __m256i y2_sum4567    = _mm256_hadd_epi32(y4[2], y4[3]);
                                __m256i y_sum01452367 = _mm256_hadd_epi32(y2_sum0123, y2_sum4567);
                                auto    sum           = _mm256_add_epi32(y_sum01452367, y_offset);
                                luma[p]               = _mm256_srli_epi32(sum,
                                                            20); // add offset and shift down to 10 bit precision
                            }

                            /* COMPUTE CHROMA */
                            {
                                // Multiply by cb-coefficients
                                __m256i cbcr4[4]; // 0 = cb02, 1 = cr02, 2 = cb46, 3 = cr46
                                for (int m = 0; m < 2; m++) {
                                    cbcr4[m * 2]     = _mm256_mullo_epi32(pixel_pairs[m * 2], cb_coeff);
                                    cbcr4[m * 2 + 1] = _mm256_mullo_epi32(pixel_pairs[m * 2], cr_coeff);
                                }
                                //for (int m = 0; m < 2; m++) {
                                //    cbcr4[m * 2]     = _mm256_sign_epi32(cbcr4[m * 2], cb_coeff);
                                //    cbcr4[m * 2 + 1] = _mm256_sign_epi32(cbcr4[m * 2 + 1], cr_coeff);
                                //}
                                // sum products
                                __m256i cbcr_02    = _mm256_hadd_epi32(cbcr4[0], cbcr4[1]);
                                __m256i cbcr_46    = _mm256_hadd_epi32(cbcr4[2], cbcr4[3]);
                                __m256i cbcr_sum_0426 = _mm256_hadd_epi32(cbcr_02, cbcr_46);
                                auto    sum           = _mm256_add_epi32(cbcr_sum_0426, c_offset);  // add offset 
                                chroma[p]             = _mm256_srli_epi32(sum, 20); // and shift down to 10 bit precision
                            }
                        }

                        /*-- pack v210 --*/

                        // luma layout =    y0  y1  y4  y5  y2  y3  y6  y7
                        // chroma layout = cb0 cr0 cb4 cr4 cb2 cr2 cb6 cr6

                        __m256i luma_16bit[3];
                        __m256i chroma_16bit[3];
                        __m256i offsets = _mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0);// (0, 4, 1, 5, 2, 6, 3, 7);
                        for (int m = 0; m < 3; m++) {
                            auto y16 =
                                _mm256_packus_epi32(luma[m * 2], luma[m * 2 + 1]); // layout 0 1   4 5   8 9   12 13   2
                                                                                   // 3   6 7   10 11   14 15
                            auto cbcr16 = _mm256_packus_epi32(chroma[m * 2],
                                                                  chroma[m * 2 + 1]); // cbcr0 cbcr4 cbcr8 cbcr12
                                                                                      // cbcr2 cbcr6 cbcr10 cbcr14
                            luma_16bit[m] = _mm256_permutevar8x32_epi32(
                                y16,
                                offsets); // layout 0 1   2 3   4 5   6 7   8 9   10 11   12 13   14 15
                            chroma_16bit[m] = _mm256_permutevar8x32_epi32(
                                cbcr16,
                                offsets); // cbcr0 cbcr2 cbcr4 cbcr6   cbcr8 cbcr10 cbcr12 cbcr14
                        }

                        __m128i chroma_mult = _mm_set_epi16(0, 0, 4, 16, 1, 4, 16, 1);
                        __m128i chroma_shuf = _mm_set_epi8(-1, 11, 10, -1, 9, 8, 7, 6, -1, 5, 4, -1, 3, 2, 1, 0);

                        uint16_t* luma_ptr   = reinterpret_cast<uint16_t*>(luma_16bit);
                        uint16_t* chroma_ptr = reinterpret_cast<uint16_t*>(chroma_16bit);
                        for (int m = 0; m < 8; ++m) {
                            __m128i  l      = _mm_loadu_si128(reinterpret_cast<__m128i*>(luma_ptr));
                            __m128i  c    = _mm_loadu_si128(reinterpret_cast<__m128i*>(chroma_ptr));
                            __m128i luma_packed   = _mm_mullo_epi16(l, luma_mult);
                            __m128i chroma_packed = _mm_mullo_epi16(c, chroma_mult);

                            luma_packed   = _mm_shuffle_epi8(luma_packed, luma_shuf);
                            chroma_packed = _mm_shuffle_epi8(chroma_packed, chroma_shuf);

                            auto res = _mm_or_si128(luma_packed, chroma_packed);
                            _mm_store_si128(v210_dest++, res);

                            luma_ptr += 6;
                            chroma_ptr += 6;
                        }
                    }
                }
            });

        } else {
            size_t byte_count_line = (size_t)decklink_format_desc.width * 4;
            for (int y = firstLine; y < decklink_format_desc.height; y += decklink_format_desc.field_count) {
                std::memcpy(reinterpret_cast<char*>(image_data.get()) + (long long)y * byte_count_line,
                            frame.image_data(0).data() + (long long)y * byte_count_line,
                            byte_count_line);
            }
        }
    } else {
        // Take a sub-region

        // TODO: Add support for hdr frames

        // Some repetetive numbers
        size_t byte_count_dest_line  = (size_t)decklink_format_desc.width * 4;
        size_t byte_count_src_line   = (size_t)channel_format_desc.width * 4;
        size_t byte_offset_src_line  = std::max(0, (config.src_x * 4));
        size_t byte_offset_dest_line = std::max(0, (config.dest_x * 4));
        int    y_skip_src_lines      = std::max(0, config.src_y);
        int    y_skip_dest_lines     = std::max(0, config.dest_y);

        size_t byte_copy_per_line =
            std::min(byte_count_src_line - byte_offset_src_line, byte_count_dest_line - byte_offset_dest_line);
        if (config.region_w > 0) // If the user chose a width, respect that
            byte_copy_per_line = std::min(byte_copy_per_line, (size_t)config.region_w * 4);

        size_t byte_pad_end_of_line =
            std::max(((size_t)decklink_format_desc.width * 4) - byte_copy_per_line - byte_offset_dest_line, (size_t)0);

        int copy_line_count =
            std::min(channel_format_desc.height - y_skip_src_lines, decklink_format_desc.height - y_skip_dest_lines);
        if (config.region_h > 0) // If the user chose a height, respect that
            copy_line_count = std::min(copy_line_count, config.region_h);

        int max_y_content = std::min(y_skip_dest_lines + copy_line_count, channel_format_desc.height);

        for (int y = firstLine; y < y_skip_dest_lines; y += decklink_format_desc.field_count) {
            // Fill the line with black
            std::memset(
                reinterpret_cast<char*>(image_data.get()) + (byte_count_dest_line * y), 0, byte_count_dest_line);
        }

        int firstFillLine = y_skip_dest_lines;
        if (decklink_format_desc.field_count != 1 && firstFillLine % 2 != firstLine)
            firstFillLine += 1;
        for (int y = firstFillLine; y < max_y_content; y += decklink_format_desc.field_count) {
            auto line_start_ptr   = reinterpret_cast<char*>(image_data.get()) + (long long)y * byte_count_dest_line;
            auto line_content_ptr = line_start_ptr + byte_offset_dest_line; // Future

            // Fill the start with black
            if (byte_offset_dest_line > 0) {
                std::memset(line_start_ptr, 0, byte_offset_dest_line);
            }

            // Copy the pixels
            std::memcpy(line_content_ptr,
                        frame.image_data(0).data() + (long long)(y + y_skip_src_lines) * byte_count_src_line +
                            byte_offset_src_line,
                        byte_copy_per_line);

            // Fill the end with black
            if (byte_pad_end_of_line > 0) {
                std::memset(line_content_ptr + byte_copy_per_line, 0, byte_pad_end_of_line);
            }
        }

        // Calculate the first line number to fill with black
        if (decklink_format_desc.field_count != 1 && max_y_content % 2 != firstLine)
            max_y_content += 1;
        for (int y = max_y_content; y < decklink_format_desc.height; y += decklink_format_desc.field_count) {
            // Fill the line with black
            std::memset(
                reinterpret_cast<char*>(image_data.get()) + (byte_count_dest_line * y), 0, byte_count_dest_line);
        }
    }
}

std::shared_ptr<void> convert_frame_for_port(const core::video_format_desc& channel_format_desc,
                                             const core::video_format_desc& decklink_format_desc,
                                             const port_configuration&      config,
                                             const core::const_frame&       frame1,
                                             const core::const_frame&       frame2,
                                             BMDFieldDominance              field_dominance,
                                             bool                           hdr)
{
    std::shared_ptr<void> image_data =
        allocate_frame_data(decklink_format_desc, hdr ? bmdFormat10BitRGBXLE : bmdFormat8BitBGRA);

    if (field_dominance != bmdProgressiveFrame) {
        convert_frame(channel_format_desc,
                      decklink_format_desc,
                      config,
                      image_data,
                      field_dominance == bmdUpperFieldFirst,
                      frame1,
                      hdr);

        convert_frame(channel_format_desc,
                      decklink_format_desc,
                      config,
                      image_data,
                      field_dominance != bmdUpperFieldFirst,
                      frame2,
                      hdr);

    } else {
        convert_frame(channel_format_desc, decklink_format_desc, config, image_data, true, frame1, hdr);
    }

    if (config.key_only) {
        image_data = convert_to_key_only(image_data, decklink_format_desc.size);
    }

    return image_data;
}

}} // namespace caspar::decklink
