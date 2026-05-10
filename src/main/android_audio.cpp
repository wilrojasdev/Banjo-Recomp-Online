// Android audio — Phase 7 implementation.
//
// Architecture:
//
//   recompiled MIPS  --queue_samples-->  ring buffer  --Oboe pull-->  speakers
//                                          ^
//                                          | get_frames_remaining
//                                          | set_frequency (rebuilds stream)
//
// The ring buffer is fixed-capacity int16 stereo samples. Producer is the game
// thread (queue_samples). Consumer is Oboe's callback thread. We use a single
// std::mutex; for the sample sizes involved (~1.5kB per game frame at 32kHz)
// contention is negligible. A fully lock-free SPSC ring would be a Phase 9
// optimization if we measure jitter.

#include "android_audio.h"

#ifdef __ANDROID__

#include <oboe/Oboe.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <android/log.h>

#define LOG_TAG "BK64-Audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace banjo_android::audio {

namespace {

constexpr int kChannels = 2;
// Big enough to hold ~6 game frames at 32 kHz (≈ 100 ms). Tunable later.
constexpr size_t kRingFrames = 6 * 1024;
constexpr size_t kRingSamples = kRingFrames * kChannels;

class RingBuffer {
public:
    void resize(size_t frames) {
        std::lock_guard lock{mutex_};
        data_.assign(frames * kChannels, 0);
        write_idx_ = 0;
        read_idx_ = 0;
        fill_frames_ = 0;
    }

    // Push interleaved stereo int16 samples. Drops oldest data if full.
    void push(const int16_t* samples, size_t frames) {
        std::lock_guard lock{mutex_};
        if (data_.empty()) return;
        const size_t cap_frames = data_.size() / kChannels;

        for (size_t f = 0; f < frames; ++f) {
            data_[write_idx_++] = samples[f * 2 + 0];
            data_[write_idx_++] = samples[f * 2 + 1];
            if (write_idx_ >= data_.size()) write_idx_ = 0;
        }

        fill_frames_ += frames;
        if (fill_frames_ > cap_frames) {
            // Buffer overflow: advance read pointer to drop oldest frames.
            const size_t drop = fill_frames_ - cap_frames;
            read_idx_ = (read_idx_ + drop * kChannels) % data_.size();
            fill_frames_ = cap_frames;
        }
    }

    // Pull `frames` worth into `out`. If the buffer underruns, fills the rest
    // with silence. Returns frames actually drawn from the buffer (the rest is
    // silence).
    size_t pull(int16_t* out, size_t frames) {
        std::lock_guard lock{mutex_};
        if (data_.empty()) {
            std::memset(out, 0, frames * kChannels * sizeof(int16_t));
            return 0;
        }

        const size_t to_copy = (frames < fill_frames_) ? frames : fill_frames_;
        for (size_t f = 0; f < to_copy; ++f) {
            out[f * 2 + 0] = data_[read_idx_++];
            out[f * 2 + 1] = data_[read_idx_++];
            if (read_idx_ >= data_.size()) read_idx_ = 0;
        }
        if (to_copy < frames) {
            std::memset(&out[to_copy * 2], 0,
                        (frames - to_copy) * kChannels * sizeof(int16_t));
        }
        fill_frames_ -= to_copy;
        return to_copy;
    }

    size_t frames_buffered() {
        std::lock_guard lock{mutex_};
        return fill_frames_;
    }

private:
    std::mutex mutex_;
    std::vector<int16_t> data_;
    size_t write_idx_ = 0;
    size_t read_idx_ = 0;
    size_t fill_frames_ = 0;
};

class OboeCallback : public oboe::AudioStreamDataCallback {
public:
    explicit OboeCallback(RingBuffer* ring) : ring_(ring) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* /*stream*/,
                                          void* audio_data,
                                          int32_t num_frames) override {
        ring_->pull(static_cast<int16_t*>(audio_data),
                    static_cast<size_t>(num_frames));
        return oboe::DataCallbackResult::Continue;
    }

private:
    RingBuffer* ring_;
};

RingBuffer g_ring;
OboeCallback g_callback{&g_ring};
std::shared_ptr<oboe::AudioStream> g_stream;
std::atomic<uint32_t> g_sample_rate{32000};
std::mutex g_stream_mutex;

bool open_stream(int32_t sample_rate) {
    std::lock_guard lock{g_stream_mutex};

    if (g_stream != nullptr) {
        g_stream->close();
        g_stream.reset();
    }

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Shared);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(kChannels);
    builder.setSampleRate(sample_rate);
    builder.setUsage(oboe::Usage::Game);
    builder.setDataCallback(&g_callback);

    oboe::Result result = builder.openStream(g_stream);
    if (result != oboe::Result::OK) {
        LOGE("openStream(%d Hz) failed: %s", sample_rate,
             oboe::convertToText(result));
        g_stream.reset();
        return false;
    }

    result = g_stream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("requestStart failed: %s", oboe::convertToText(result));
        g_stream->close();
        g_stream.reset();
        return false;
    }

    LOGI("Oboe stream opened: %d Hz, %d channels, %s perf mode",
         sample_rate, kChannels,
         g_stream->getPerformanceMode() == oboe::PerformanceMode::LowLatency
             ? "LowLatency" : "Default");
    return true;
}

}  // anonymous namespace

bool start() {
    g_ring.resize(kRingFrames);
    return open_stream(static_cast<int32_t>(g_sample_rate.load()));
}

void stop() {
    std::lock_guard lock{g_stream_mutex};
    if (g_stream) {
        g_stream->close();
        g_stream.reset();
    }
}

namespace {

void queue_samples(int16_t* audio_data, size_t sample_count) {
    // sample_count is interleaved stereo samples, NOT frames. Confusingly the
    // ultramodern API names suggest "frames" but the implementations on the
    // desktop side push interleaved samples directly. Keep the convention.
    g_ring.push(audio_data, sample_count / kChannels);
}

size_t get_frames_remaining() {
    return g_ring.frames_buffered();
}

void set_frequency(uint32_t freq) {
    if (freq == 0) return;
    if (g_sample_rate.exchange(freq) != freq) {
        LOGI("set_frequency(%u) — reopening stream", freq);
        open_stream(static_cast<int32_t>(freq));
    }
}

}  // anonymous namespace

ultramodern::audio_callbacks_t make_audio_callbacks() {
    return ultramodern::audio_callbacks_t{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
}

}  // namespace banjo_android::audio

#endif  // __ANDROID__
