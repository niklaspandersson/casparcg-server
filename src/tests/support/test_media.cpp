#include "test_media.h"
#include "output_dir.h"

#include <common/utf.h>

#include <core/frame/pixel_format.h>
#include <core/video_format.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>

namespace caspar { namespace test {

namespace {

constexpr int cell_count = 18; // two markers + 16 bits
constexpr int white      = 235;
constexpr int black      = 16;
constexpr int grey       = 128;

[[noreturn]] void fail(const std::string& what, int err = 0)
{
    if (err == 0)
        throw std::runtime_error(what);
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, text, sizeof(text));
    throw std::runtime_error(what + ": " + text);
}

void check(int err, const std::string& what)
{
    if (err < 0)
        fail(what, err);
}

struct deleters
{
    void operator()(AVFormatContext* ctx) const
    {
        if (ctx->iformat) {
            avformat_close_input(&ctx);
        } else {
            if (ctx->pb)
                avio_closep(&ctx->pb);
            avformat_free_context(ctx);
        }
    }
    void operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
    void operator()(SwsContext* ctx) const { sws_freeContext(ctx); }
    void operator()(SwrContext* ctx) const { swr_free(&ctx); }
};

template <typename T>
using av_ptr = std::unique_ptr<T, deleters>;

// The barcode, read through a luma(x, y) sampler that returns 0..255.
template <typename Luma>
std::optional<int> read_cells(int width, int height, Luma&& luma)
{
    if (width < cell_count * 2 || height < 16)
        return {};

    auto cell = [&](int n) {
        // Average a block at the centre of the cell, half its width and height.
        int cell_width = width / cell_count;
        int x0         = n * cell_width + cell_width / 4;
        int y0         = height / 8 / 4;
        int sum = 0, count = 0;
        for (int y = y0; y < y0 + std::max(1, height / 16); ++y) {
            for (int x = x0; x < x0 + std::max(1, cell_width / 2); ++x) {
                sum += luma(x, y);
                ++count;
            }
        }
        return sum / count;
    };

    int marker_white = cell(0);
    int marker_black = cell(1);
    if (marker_white - marker_black < 64)
        return {};

    int threshold = (marker_white + marker_black) / 2;
    int index     = 0;
    for (int n = 2; n < cell_count; ++n)
        index = (index << 1) | (cell(n) > threshold ? 1 : 0);
    return index;
}

std::int32_t ramp_value(std::int64_t sample, int channel) { return static_cast<std::int32_t>((sample << 8) | channel); }

struct encoder
{
    av_ptr<AVCodecContext> ctx;
    AVStream*              stream = nullptr;
};

void write_packets(AVFormatContext* format, encoder& enc, AVFrame* frame)
{
    check(avcodec_send_frame(enc.ctx.get(), frame), "avcodec_send_frame");
    av_ptr<AVPacket> packet(av_packet_alloc());
    while (true) {
        int err = avcodec_receive_packet(enc.ctx.get(), packet.get());
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return;
        check(err, "avcodec_receive_packet");
        // Some encoders leave the duration out; MOV's edit list then drops the last frame.
        if (packet->duration == 0 && enc.ctx->codec_type == AVMEDIA_TYPE_VIDEO)
            packet->duration = 1;
        av_packet_rescale_ts(packet.get(), enc.ctx->time_base, enc.stream->time_base);
        packet->stream_index = enc.stream->index;
        check(av_interleaved_write_frame(format, packet.get()), "av_interleaved_write_frame");
    }
}

encoder
open_encoder(AVFormatContext* format, const std::string& name, const std::function<void(AVCodecContext*)>& setup)
{
    auto codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec)
        fail("FFmpeg has no encoder named " + name);

    encoder enc;
    enc.ctx.reset(avcodec_alloc_context3(codec));
    setup(enc.ctx.get());
    if (format->oformat->flags & AVFMT_GLOBALHEADER)
        enc.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(enc.ctx.get(), codec, nullptr), "avcodec_open2 " + name);

    enc.stream = avformat_new_stream(format, nullptr);
    check(avcodec_parameters_from_context(enc.stream->codecpar, enc.ctx.get()), "avcodec_parameters_from_context");
    enc.stream->time_base = enc.ctx->time_base;
    return enc;
}

} // namespace

void stamp_index(std::uint8_t* luma, int width, int height, int linesize, int index)
{
    if (index < 0 || index > max_stamped_index)
        fail("index out of the stamp's range: " + std::to_string(index));

    int cell_width = width / cell_count;
    for (int y = 0; y < height / 8; ++y) {
        auto row = luma + static_cast<std::ptrdiff_t>(y) * linesize;
        for (int x = 0; x < width; ++x) {
            int n = x / cell_width;
            int value;
            if (n == 0)
                value = white;
            else if (n == 1)
                value = black;
            else if (n < cell_count)
                value = (index >> (cell_count - 1 - n)) & 1 ? white : black;
            else
                value = grey; // the remainder when width isn't a multiple of the cell count
            row[x] = static_cast<std::uint8_t>(value);
        }
    }
}

std::optional<int> read_index(const std::uint8_t* luma, int width, int height, int linesize)
{
    return read_cells(width, height, [&](int x, int y) { return luma[static_cast<std::ptrdiff_t>(y) * linesize + x]; });
}

std::optional<int> read_index(const core::const_frame& frame)
{
    if (!frame)
        return {};

    auto& desc  = frame.pixel_format_desc();
    auto& plane = desc.planes.at(0);
    auto  data  = frame.image_data(0).data();
    int   shift = 0; // to 8 bits
    switch (plane.depth) {
        case common::bit_depth::bit8:
            break;
        case common::bit_depth::bit10:
            shift = 2;
            break;
        case common::bit_depth::bit12:
            shift = 4;
            break;
        case common::bit_depth::bit16:
            shift = 8;
            break;
    }
    int bytes = plane.depth == common::bit_depth::bit8 ? 1 : 2;

    auto component = [&](int x, int y, int c) -> int {
        auto p = data + static_cast<std::ptrdiff_t>(y) * plane.linesize + (x * plane.stride + c) * bytes;
        return bytes == 1 ? *p : (p[0] | p[1] << 8) >> shift;
    };

    // The components averaged into a grey level, per format. G alone is enough for planar RGB.
    std::vector<int> components;
    switch (desc.format) {
        case core::pixel_format::gray:
        case core::pixel_format::luma:
        case core::pixel_format::ycbcr:
        case core::pixel_format::ycbcra:
        case core::pixel_format::gbrp:
        case core::pixel_format::gbrap:
            components = {0};
            break;
        case core::pixel_format::bgra:
        case core::pixel_format::rgba:
        case core::pixel_format::bgr:
        case core::pixel_format::rgb:
            components = {0, 1, 2};
            break;
        case core::pixel_format::argb:
        case core::pixel_format::abgr:
            components = {1, 2, 3};
            break;
        default:
            fail("read_index: unsupported pixel format " + std::to_string(static_cast<int>(desc.format)));
    }

    return read_cells(plane.width, plane.height, [&](int x, int y) {
        int sum = 0;
        for (int c : components)
            sum += component(x, y, c);
        return sum / static_cast<int>(components.size());
    });
}

std::vector<std::int64_t> ramp_samples(const std::int32_t* samples, std::size_t count, int channels)
{
    std::vector<std::int64_t> result;
    for (std::size_t n = 0; n + channels <= count; n += channels)
        result.push_back(samples[n] >> 8);
    return result;
}

std::vector<std::int64_t> ramp_samples(const core::const_frame& frame, int channels)
{
    auto& audio = frame.audio_data();
    return ramp_samples(audio.data(), audio.size(), channels);
}

std::filesystem::path make_clip(const clip_options& options)
{
    auto format_desc = core::video_format_repository().find(options.format);
    if (format_desc.format == core::video_format::invalid)
        fail("make_clip: unknown format " + u8(options.format));

    auto path = output_dir() / (options.name + "." + options.extension);

    AVFormatContext* raw_format = nullptr;
    check(avformat_alloc_output_context2(&raw_format, nullptr, nullptr, path.string().c_str()),
          "avformat_alloc_output_context2");
    av_ptr<AVFormatContext> format(raw_format);

    const auto rate       = AVRational{format_desc.framerate.numerator(), format_desc.framerate.denominator()};
    const bool interlaced = format_desc.field_count == 2;
    const auto pix_fmt    = av_get_pix_fmt(options.pixel_format.c_str());
    if (pix_fmt == AV_PIX_FMT_NONE)
        fail("make_clip: unknown pixel format " + options.pixel_format);

    auto video = open_encoder(format.get(), options.codec, [&](AVCodecContext* ctx) {
        ctx->width     = format_desc.width;
        ctx->height    = format_desc.height;
        ctx->pix_fmt   = pix_fmt;
        ctx->time_base = av_inv_q(rate);
        ctx->framerate = rate;
        ctx->gop_size  = 12;
        if (interlaced) {
            ctx->field_order = AV_FIELD_TT;
            ctx->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
        }
    });

    std::optional<encoder> audio;
    if (options.audio) {
        // Try packed, then planar 32-bit samples; the ramp needs all 32 bits.
        for (auto sample_fmt : {AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S32P}) {
            try {
                audio = open_encoder(format.get(), options.audio_codec, [&](AVCodecContext* ctx) {
                    av_channel_layout_default(&ctx->ch_layout, options.audio_channels);
                    ctx->sample_fmt  = sample_fmt;
                    ctx->sample_rate = format_desc.audio_sample_rate;
                    ctx->time_base   = AVRational{1, format_desc.audio_sample_rate};
                });
                break;
            } catch (std::runtime_error&) {
                if (sample_fmt == AV_SAMPLE_FMT_S32P)
                    throw;
            }
        }
    }

    check(avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE), "avio_open " + path.string());
    check(avformat_write_header(format.get(), nullptr), "avformat_write_header");

    // Frames are drawn in yuv420p and converted when the encoder wants something else.
    av_ptr<AVFrame> source(av_frame_alloc());
    source->format = AV_PIX_FMT_YUV420P;
    source->width  = format_desc.width;
    source->height = format_desc.height;
    check(av_frame_get_buffer(source.get(), 0), "av_frame_get_buffer");

    av_ptr<SwsContext> scaler;
    av_ptr<AVFrame>    converted;
    if (pix_fmt != AV_PIX_FMT_YUV420P) {
        scaler.reset(sws_getContext(format_desc.width,
                                    format_desc.height,
                                    AV_PIX_FMT_YUV420P,
                                    format_desc.width,
                                    format_desc.height,
                                    pix_fmt,
                                    SWS_POINT,
                                    nullptr,
                                    nullptr,
                                    nullptr));
        converted.reset(av_frame_alloc());
        converted->format = pix_fmt;
        converted->width  = format_desc.width;
        converted->height = format_desc.height;
        check(av_frame_get_buffer(converted.get(), 0), "av_frame_get_buffer");
    }

    const std::int64_t total_samples =
        av_rescale_q(options.frames, av_inv_q(rate), AVRational{1, format_desc.audio_sample_rate});
    std::int64_t samples_written = 0;

    for (int n = 0; n < options.frames; ++n) {
        check(av_frame_make_writable(source.get()), "av_frame_make_writable");
        for (int y = 0; y < source->height; ++y)
            std::fill_n(source->data[0] + static_cast<std::ptrdiff_t>(y) * source->linesize[0], source->width, grey);
        for (int p = 1; p < 3; ++p) {
            for (int y = 0; y < source->height / 2; ++y)
                std::fill_n(
                    source->data[p] + static_cast<std::ptrdiff_t>(y) * source->linesize[p], source->width / 2, 128);
        }
        stamp_index(source->data[0], source->width, source->height, source->linesize[0], n);

        AVFrame* frame = source.get();
        if (scaler) {
            check(av_frame_make_writable(converted.get()), "av_frame_make_writable");
            sws_scale(
                scaler.get(), source->data, source->linesize, 0, source->height, converted->data, converted->linesize);
            frame = converted.get();
        }
        frame->pts      = n;
        frame->duration = 1;
        if (interlaced)
            frame->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
        write_packets(format.get(), video, frame);

        // Audio up to the end of this frame, so the muxer interleaves it with the video.
        if (audio) {
            auto ctx = audio->ctx.get();
            auto end = av_rescale_q(n + 1, av_inv_q(rate), AVRational{1, ctx->sample_rate});
            while (samples_written < std::min(end, total_samples)) {
                int chunk = ctx->frame_size > 0 ? ctx->frame_size : 1024;
                chunk     = static_cast<int>(std::min<std::int64_t>(chunk, total_samples - samples_written));

                av_ptr<AVFrame> samples(av_frame_alloc());
                samples->format      = ctx->sample_fmt;
                samples->nb_samples  = chunk;
                samples->sample_rate = ctx->sample_rate;
                check(av_channel_layout_copy(&samples->ch_layout, &ctx->ch_layout), "av_channel_layout_copy");
                check(av_frame_get_buffer(samples.get(), 0), "av_frame_get_buffer");

                int channels = ctx->ch_layout.nb_channels;
                for (int s = 0; s < chunk; ++s) {
                    for (int c = 0; c < channels; ++c) {
                        auto value = ramp_value(samples_written + s, c);
                        if (ctx->sample_fmt == AV_SAMPLE_FMT_S32)
                            reinterpret_cast<std::int32_t*>(samples->data[0])[s * channels + c] = value;
                        else
                            reinterpret_cast<std::int32_t*>(samples->extended_data[c])[s] = value;
                    }
                }
                samples->pts      = samples_written;
                samples->duration = chunk;
                samples_written += chunk;
                write_packets(format.get(), *audio, samples.get());
            }
        }
    }

    write_packets(format.get(), video, nullptr);
    if (audio)
        write_packets(format.get(), *audio, nullptr);
    check(av_write_trailer(format.get()), "av_write_trailer");

    return path;
}

decoded_clip decode_clip(const std::filesystem::path& path)
{
    AVFormatContext* raw_format = nullptr;
    check(avformat_open_input(&raw_format, path.string().c_str(), nullptr, nullptr),
          "avformat_open_input " + path.string());
    av_ptr<AVFormatContext> format(raw_format);
    check(avformat_find_stream_info(format.get(), nullptr), "avformat_find_stream_info");

    auto open_decoder = [&](AVMediaType type, int& index) {
        av_ptr<AVCodecContext> ctx;
        index = av_find_best_stream(format.get(), type, -1, -1, nullptr, 0);
        if (index < 0)
            return ctx;
        auto stream = format->streams[index];
        auto codec  = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec)
            fail("no decoder for stream " + std::to_string(index) + " in " + path.string());
        ctx.reset(avcodec_alloc_context3(codec));
        check(avcodec_parameters_to_context(ctx.get(), stream->codecpar), "avcodec_parameters_to_context");
        check(avcodec_open2(ctx.get(), codec, nullptr), "avcodec_open2");
        return ctx;
    };

    int  video_index = -1, audio_index = -1;
    auto video = open_decoder(AVMEDIA_TYPE_VIDEO, video_index);
    auto audio = open_decoder(AVMEDIA_TYPE_AUDIO, audio_index);

    decoded_clip              result;
    av_ptr<SwsContext>        to_grey;
    std::vector<std::uint8_t> grey_plane;
    av_ptr<SwrContext>        to_s32;

    auto on_video = [&](AVFrame* frame) {
        to_grey.reset(sws_getCachedContext(to_grey.release(),
                                           frame->width,
                                           frame->height,
                                           static_cast<AVPixelFormat>(frame->format),
                                           frame->width,
                                           frame->height,
                                           AV_PIX_FMT_GRAY8,
                                           SWS_POINT,
                                           nullptr,
                                           nullptr,
                                           nullptr));
        grey_plane.resize(static_cast<std::size_t>(frame->width) * frame->height);
        std::uint8_t* dst[4]      = {grey_plane.data()};
        int           linesize[4] = {frame->width};
        sws_scale(to_grey.get(), frame->data, frame->linesize, 0, frame->height, dst, linesize);
        result.indices.push_back(read_index(grey_plane.data(), frame->width, frame->height, frame->width));
    };

    auto on_audio = [&](AVFrame* frame) {
        int channels = frame->ch_layout.nb_channels;
        if (!to_s32) {
            SwrContext* swr = nullptr;
            check(swr_alloc_set_opts2(&swr,
                                      &frame->ch_layout,
                                      AV_SAMPLE_FMT_S32,
                                      frame->sample_rate,
                                      &frame->ch_layout,
                                      static_cast<AVSampleFormat>(frame->format),
                                      frame->sample_rate,
                                      0,
                                      nullptr),
                  "swr_alloc_set_opts2");
            to_s32.reset(swr);
            check(swr_init(swr), "swr_init");
            result.audio_channels = channels;
        }
        std::vector<std::int32_t> samples(static_cast<std::size_t>(frame->nb_samples) * channels);
        auto                      out       = reinterpret_cast<std::uint8_t*>(samples.data());
        int                       converted = swr_convert(to_s32.get(),
                                    &out,
                                    frame->nb_samples,
                                    const_cast<const std::uint8_t**>(frame->extended_data),
                                    frame->nb_samples);
        check(converted, "swr_convert");
        auto ramp = ramp_samples(samples.data(), static_cast<std::size_t>(converted) * channels, channels);
        result.audio_samples.insert(result.audio_samples.end(), ramp.begin(), ramp.end());
    };

    auto drain = [&](AVCodecContext* ctx, auto&& on_frame) {
        av_ptr<AVFrame> frame(av_frame_alloc());
        while (avcodec_receive_frame(ctx, frame.get()) >= 0) {
            on_frame(frame.get());
            av_frame_unref(frame.get());
        }
    };

    av_ptr<AVPacket> packet(av_packet_alloc());
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (video && packet->stream_index == video_index) {
            check(avcodec_send_packet(video.get(), packet.get()), "avcodec_send_packet");
            drain(video.get(), on_video);
        } else if (audio && packet->stream_index == audio_index) {
            check(avcodec_send_packet(audio.get(), packet.get()), "avcodec_send_packet");
            drain(audio.get(), on_audio);
        }
        av_packet_unref(packet.get());
    }
    if (video) {
        avcodec_send_packet(video.get(), nullptr);
        drain(video.get(), on_video);
    }
    if (audio) {
        avcodec_send_packet(audio.get(), nullptr);
        drain(audio.get(), on_audio);
    }

    return result;
}

}} // namespace caspar::test
