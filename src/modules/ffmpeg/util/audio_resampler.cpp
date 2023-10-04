#include "audio_resampler.h"
#include "av_assert.h"

extern "C" {
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace caspar { namespace ffmpeg {

AudioResampler::AudioResampler(int64_t sample_rate, AVSampleFormat in_sample_fmt)
{
    AVChannelLayout channel_layout = AV_CHANNEL_LAYOUT_7POINT1;

    SwrContext* raw_ctx = nullptr;
    FF(swr_alloc_set_opts2(&raw_ctx,
                           &channel_layout,
                           AV_SAMPLE_FMT_S32,
                           sample_rate,
                           &channel_layout,
                           in_sample_fmt,
                           sample_rate,
                           0,
                           nullptr));

    ctx = std::shared_ptr<SwrContext>(raw_ctx, [](SwrContext* ptr) { swr_free(&ptr); });

    FF_RET(swr_init(ctx.get()), "swr_init");
}

caspar::array<int32_t> AudioResampler::convert(int frames, const void** src)
{
    auto result = caspar::array<int32_t>(frames * 8 * sizeof(int32_t));
    auto ptr    = result.data();
    auto ret    = swr_convert(ctx.get(), (uint8_t**)&ptr, frames, reinterpret_cast<const uint8_t**>(src), frames);

    return result;
}

}}; // namespace caspar::ffmpeg