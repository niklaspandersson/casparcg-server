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
 * Author: Robert Nagy, ronag89@gmail.com
 *         Core Audio implementation for macOS - Phase 11
 */

#include "coreaudio_consumer.h"

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/log.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/utf.h>

#include <core/consumer/frame_consumer.h>
#include <core/consumer/channel_info.h>
#include <core/frame/frame.h>
#include <core/video_format.h>

#include <modules/ffmpeg/defines.h>

#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>

extern "C" {
#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

namespace caspar { namespace oal {

namespace {

// Enumerate available Core Audio output devices
std::vector<std::pair<AudioDeviceID, std::wstring>> enumerate_audio_devices()
{
    std::vector<std::pair<AudioDeviceID, std::wstring>> devices;

    AudioObjectPropertyAddress prop_addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 data_size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &prop_addr, 0, nullptr, &data_size);

    if (status != noErr) {
        CASPAR_LOG(warning) << L"Failed to get Core Audio device count";
        return devices;
    }

    size_t device_count = data_size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> device_ids(device_count);

    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &prop_addr, 0, nullptr,
        &data_size, device_ids.data());

    if (status != noErr) {
        CASPAR_LOG(warning) << L"Failed to enumerate Core Audio devices";
        return devices;
    }

    CASPAR_LOG(info) << L"------- Core Audio Device List -----";

    for (AudioDeviceID device_id : device_ids) {
        // Check if device has output streams
        prop_addr.mSelector = kAudioDevicePropertyStreams;
        prop_addr.mScope = kAudioDevicePropertyScopeOutput;

        UInt32 stream_size = 0;
        status = AudioObjectGetPropertyDataSize(
            device_id, &prop_addr, 0, nullptr, &stream_size);

        if (status != noErr || stream_size == 0) {
            continue; // No output streams, skip this device
        }

        // Get device name
        prop_addr.mSelector = kAudioDevicePropertyDeviceNameCFString;
        prop_addr.mScope = kAudioObjectPropertyScopeGlobal;

        CFStringRef name_ref = nullptr;
        data_size = sizeof(name_ref);
        status = AudioObjectGetPropertyData(
            device_id, &prop_addr, 0, nullptr, &data_size, &name_ref);

        if (status == noErr && name_ref) {
            char name_buf[256];
            CFStringGetCString(name_ref, name_buf, sizeof(name_buf),
                              kCFStringEncodingUTF8);
            CFRelease(name_ref);

            std::wstring device_name = u16(std::string(name_buf));
            CASPAR_LOG(info) << device_name;
            devices.emplace_back(device_id, device_name);
        }
    }

    CASPAR_LOG(info) << L"------- Core Audio Devices List done -----";

    return devices;
}

// Find device by name
AudioDeviceID find_device_by_name(const std::wstring& target_name)
{
    auto devices = enumerate_audio_devices();

    std::string short_target = u8(target_name);
    boost::algorithm::erase_all(short_target, " ");

    for (const auto& [device_id, name] : devices) {
        std::string short_name = u8(name);
        boost::algorithm::erase_all(short_name, " ");

        if (boost::iequals(short_target, short_name)) {
            CASPAR_LOG(debug) << L"Found specified Core Audio output device: " << name;
            return device_id;
        }
    }

    CASPAR_LOG(warning) << L"Failed to find specified Core Audio output device. "
                        << L"Using system default device";
    return kAudioObjectUnknown;
}

} // anonymous namespace

struct coreaudio_consumer : public core::frame_consumer
{
    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       perf_timer_;
    int                                 channel_index_ = -1;

    core::video_format_desc format_desc_;

    AudioQueueRef                        audio_queue_ = nullptr;
    std::vector<AudioQueueBufferRef>     buffers_;
    bool                                 started_  = false;
    int                                  duration_ = 1920;
    std::wstring                         device_name_;

    std::shared_ptr<SwrContext> swr_;

    // Thread-safe queue for audio data between executor and AudioQueue callback
    tbb::concurrent_bounded_queue<std::vector<int16_t>> audio_buffer_;

    std::atomic<bool> is_running_{true};

    executor executor_{L"coreaudio_consumer"};

  public:
    explicit coreaudio_consumer()
    {
        // Get default device name from configuration
        device_name_ = env::properties().get(L"configuration.system-audio.producer.default-device-name", L"");

        // Enumerate devices at startup
        enumerate_audio_devices();

        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        diagnostics::register_graph(graph_);

        // Set buffer capacity to prevent unbounded growth
        audio_buffer_.set_capacity(16);
    }

    ~coreaudio_consumer() override
    {
        is_running_ = false;

        executor_.invoke([this] {
            if (audio_queue_) {
                AudioQueueStop(audio_queue_, true);  // Immediate stop

                for (auto& buffer : buffers_) {
                    if (buffer) {
                        AudioQueueFreeBuffer(audio_queue_, buffer);
                    }
                }
                buffers_.clear();

                AudioQueueDispose(audio_queue_, true);
                audio_queue_ = nullptr;
            }
        });
    }

    // frame consumer

    void initialize(const core::video_format_desc& format_desc, const core::channel_info& channel_info, int port_index) override
    {
        format_desc_   = format_desc;
        channel_index_ = channel_info.index;
        graph_->set_text(print());

        executor_.begin_invoke([this] {
            duration_ = *std::min_element(format_desc_.audio_cadence.begin(), format_desc_.audio_cadence.end());

            // Setup AudioQueue with stereo int16 format
            AudioStreamBasicDescription format = {};
            format.mSampleRate = format_desc_.audio_sample_rate;
            format.mFormatID = kAudioFormatLinearPCM;
            format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
            format.mBitsPerChannel = 16;
            format.mChannelsPerFrame = 2;  // Stereo output
            format.mBytesPerFrame = format.mChannelsPerFrame * (format.mBitsPerChannel / 8);
            format.mFramesPerPacket = 1;
            format.mBytesPerPacket = format.mBytesPerFrame;

            OSStatus status = AudioQueueNewOutput(
                &format,
                audio_queue_callback,
                this,
                nullptr,  // Run loop (nullptr = internal)
                nullptr,  // Run loop mode
                0,        // Flags
                &audio_queue_
            );

            if (status != noErr) {
                CASPAR_THROW_EXCEPTION(invalid_operation()
                    << msg_info("Failed to create Core Audio AudioQueue. Error: " + std::to_string(status)));
            }

            // Set output device if specified
            if (!device_name_.empty()) {
                AudioDeviceID device_id = find_device_by_name(device_name_);
                if (device_id != kAudioObjectUnknown) {
                    status = AudioQueueSetProperty(
                        audio_queue_,
                        kAudioQueueProperty_CurrentDevice,
                        &device_id,
                        sizeof(device_id)
                    );

                    if (status != noErr) {
                        CASPAR_LOG(warning) << L"Failed to set Core Audio output device";
                    }
                }
            }

            // Allocate 8 buffers (matching OpenAL pattern)
            int buffer_size = duration_ * 2 * sizeof(int16_t);  // stereo int16
            buffers_.resize(8);

            for (size_t i = 0; i < buffers_.size(); ++i) {
                status = AudioQueueAllocateBuffer(audio_queue_, buffer_size, &buffers_[i]);
                if (status != noErr) {
                    CASPAR_THROW_EXCEPTION(invalid_operation()
                        << msg_info("Failed to allocate Core Audio buffer"));
                }
            }

            CASPAR_LOG(info) << L"Core Audio consumer initialized: "
                             << format_desc_.audio_sample_rate << L"Hz, "
                             << duration_ << L" samples/buffer, "
                             << buffers_.size() << L" buffers";
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        executor_.begin_invoke([=] {
            auto dst         = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* ptr) { av_frame_free(&ptr); });
            dst->format      = AV_SAMPLE_FMT_S16;
            dst->sample_rate = format_desc_.audio_sample_rate;
#if FFMPEG_NEW_CHANNEL_LAYOUT
            av_channel_layout_default(&dst->ch_layout, 2);
#else
            dst->channels       = 2;
            dst->channel_layout = av_get_default_channel_layout(dst->channels);
#endif
            dst->nb_samples = duration_;
            if (av_frame_get_buffer(dst.get(), 32) < 0) {
                CASPAR_THROW_EXCEPTION(invalid_argument() << msg_info("Failed to allocate audio frame buffer"));
            }
            std::memset(dst->extended_data[0], 0, dst->linesize[0]);

            if (!started_) {
                // Prime all buffers with silence and start playback
                for (auto& buffer : buffers_) {
                    buffer->mAudioDataByteSize = duration_ * 2 * sizeof(int16_t);
                    std::memset(buffer->mAudioData, 0, buffer->mAudioDataByteSize);
                    AudioQueueEnqueueBuffer(audio_queue_, buffer, 0, nullptr);
                }

                OSStatus status = AudioQueueStart(audio_queue_, nullptr);
                if (status != noErr) {
                    CASPAR_LOG(error) << L"Failed to start Core Audio playback: " << status;
                }
                started_ = true;

                return;
            }

            auto src         = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* ptr) { av_frame_free(&ptr); });
            src->format      = AV_SAMPLE_FMT_S32;
            src->sample_rate = format_desc_.audio_sample_rate;
#if FFMPEG_NEW_CHANNEL_LAYOUT
            av_channel_layout_default(&src->ch_layout, format_desc_.audio_channels);
#else
            src->channels       = format_desc_.audio_channels;
            src->channel_layout = av_get_default_channel_layout(src->channels);
#endif
            src->nb_samples       = static_cast<int>(frame.audio_data().size() / format_desc_.audio_channels);
            src->extended_data[0] = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(frame.audio_data().data()));
            src->linesize[0]      = static_cast<int>(frame.audio_data().size() * sizeof(int32_t));

            if (!swr_) {
                swr_.reset(swr_alloc(), [](SwrContext* ptr) { swr_free(&ptr); });
                if (!swr_) {
                    CASPAR_THROW_EXCEPTION(bad_alloc());
                }
                if (swr_config_frame(swr_.get(), dst.get(), src.get()) < 0) {
                    CASPAR_THROW_EXCEPTION(invalid_argument() << msg_info("Failed to configure SwrContext"));
                }
                if (swr_init(swr_.get()) < 0) {
                    CASPAR_THROW_EXCEPTION(invalid_argument() << msg_info("Failed to initialize SwrContext"));
                }
            }

            if (swr_convert_frame(swr_.get(), nullptr, src.get()) < 0) {
                CASPAR_THROW_EXCEPTION(invalid_argument() << msg_info("Failed to convert audio frame"));
            }

            // Extract converted audio and push to buffer queue
            while (swr_get_delay(swr_.get(), format_desc_.audio_sample_rate) >= dst->nb_samples) {
                if (swr_convert_frame(swr_.get(), dst.get(), nullptr) != 0) {
                    break;
                }

                std::vector<int16_t> audio_data(duration_ * 2);  // stereo
                std::memcpy(audio_data.data(), dst->extended_data[0], duration_ * 2 * sizeof(int16_t));

                if (!audio_buffer_.try_push(std::move(audio_data))) {
                    graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
                }
            }

            graph_->set_value("tick-time", perf_timer_.elapsed() * format_desc_.fps * 0.5);
            perf_timer_.restart();
        });

        return make_ready_future(true);
    }

    std::wstring print() const override
    {
        return L"coreaudio[" + std::to_wstring(channel_index_) + L"|" + format_desc_.name + L"]";
    }

    std::wstring name() const override { return L"system-audio"; }

    bool has_synchronization_clock() const override { return false; }

    int index() const override { return 500; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["audio/device"] = device_name_.empty() ? L"default" : device_name_;
        return state;
    }

private:
    // AudioQueue callback - called from Core Audio thread when a buffer is consumed
    static void audio_queue_callback(void* user_data,
                                     AudioQueueRef queue,
                                     AudioQueueBufferRef buffer)
    {
        auto* self = static_cast<coreaudio_consumer*>(user_data);

        if (!self->is_running_) {
            return;
        }

        std::vector<int16_t> audio_data;

        if (self->audio_buffer_.try_pop(audio_data)) {
            // Copy audio data to buffer
            size_t byte_size = audio_data.size() * sizeof(int16_t);
            std::memcpy(buffer->mAudioData, audio_data.data(), byte_size);
            buffer->mAudioDataByteSize = static_cast<UInt32>(byte_size);
        } else {
            // Underrun - fill with silence
            buffer->mAudioDataByteSize = self->duration_ * 2 * sizeof(int16_t);
            std::memset(buffer->mAudioData, 0, buffer->mAudioDataByteSize);
            self->graph_->set_tag(diagnostics::tag_severity::WARNING, "late-frame");
        }

        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer_ca(const std::vector<std::wstring>&     params,
                                                          const core::video_format_repository& format_repository,
                                                          const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                          const core::channel_info& channel_info)
{
    if (params.empty() || !boost::iequals(params.at(0), L"AUDIO"))
        return core::frame_consumer::empty();

    return spl::make_shared<coreaudio_consumer>();
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer_ca(const boost::property_tree::wptree&                      ptree,
                                  const core::video_format_repository&                     format_repository,
                                  const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                  const core::channel_info&                                channel_info)
{
    return spl::make_shared<coreaudio_consumer>();
}

}} // namespace caspar::oal
