#include "video.h"
#include "audio.h"
#include "timeline.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

using Clock = std::chrono::steady_clock;

static std::atomic<bool> stopRequested{false};
static std::atomic<bool> paused{false};
static std::thread decoderThread;

static std::atomic<bool> seekRequested{false};
static std::atomic<double> seekTargetSeconds{0.0};

static std::mutex frameMutex;
static std::vector<uint8_t> latestFrame;
static int latestWidth = 0;
static int latestHeight = 0;
static int latestStride = 0;
static bool frameMessagePending = false;
static std::string lastVideoError;

struct FormatCloser {
    void operator()(AVFormatContext* p) const {
        if (p) {
            avformat_close_input(&p);
        }
    }
};

struct CodecCloser {
    void operator()(AVCodecContext* p) const {
        if (p) {
            avcodec_free_context(&p);
        }
    }
};

struct FrameCloser {
    void operator()(AVFrame* p) const {
        if (p) {
            av_frame_free(&p);
        }
    }
};

struct PacketCloser {
    void operator()(AVPacket* p) const {
        if (p) {
            av_packet_free(&p);
        }
    }
};

struct SwsCloser {
    void operator()(SwsContext* p) const {
        if (p) {
            sws_freeContext(p);
        }
    }
};

struct SwrCloser {
    void operator()(SwrContext* p) const {
        if (p) {
            swr_free(&p);
        }
    }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;
using SwsPtr = std::unique_ptr<SwsContext, SwsCloser>;
using SwrPtr = std::unique_ptr<SwrContext, SwrCloser>;

static std::string FfmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

static void ReportError(HWND window, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        lastVideoError = error;
    }

    if (IsWindow(window)) {
        PostMessageA(window, WM_APP_VIDEO_ERROR, 0, 0);
    }
}

static void PublishFrame(
    HWND window,
    std::vector<uint8_t>&& pixels,
    int width,
    int height,
    int stride
) {
    bool postNotification = false;

    {
        std::lock_guard<std::mutex> lock(frameMutex);

        latestFrame = std::move(pixels);
        latestWidth = width;
        latestHeight = height;
        latestStride = stride;

        if (!frameMessagePending) {
            frameMessagePending = true;
            postNotification = true;
        }
    }

    if (postNotification &&
        !PostMessageA(window, WM_APP_VIDEO_FRAME, 0, 0)) {
        std::lock_guard<std::mutex> lock(frameMutex);
        frameMessagePending = false;
    }
}

bool TakeLatestFrame(
    std::vector<uint8_t>& pixels,
    int& width,
    int& height,
    int& stride
) {
    std::lock_guard<std::mutex> lock(frameMutex);

    frameMessagePending = false;

    if (latestFrame.empty()) {
        return false;
    }

    pixels.swap(latestFrame);

    width = latestWidth;
    height = latestHeight;
    stride = latestStride;

    return true;
}

void GetVideoError(char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(frameMutex);

    std::strncpy(
        buffer,
        lastVideoError.c_str(),
        bufferSize - 1
    );

    buffer[bufferSize - 1] = '\0';
}

struct AudioStopGuard {
    bool active = false;

    ~AudioStopGuard() {
        if (active) {
            AudioStop();
        }
    }
};

static bool DecodeVideo(HWND window, const std::string& filePath) {
    AVFormatContext* rawFormat = nullptr;

    int result = avformat_open_input(
        &rawFormat,
        filePath.c_str(),
        nullptr,
        nullptr
    );

    if (result < 0) {
        ReportError(
            window,
            "Could not open media file:\n" + FfmpegError(result)
        );
        return false;
    }

    FormatPtr format(rawFormat);

    result = avformat_find_stream_info(format.get(), nullptr);

    if (result < 0) {
        ReportError(
            window,
            "Could not read media information:\n" +
            FfmpegError(result)
        );
        return false;
    }

    const AVCodec* videoDecoder = nullptr;

    const int videoStream = av_find_best_stream(
        format.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        &videoDecoder,
        0
    );

    if (videoStream < 0 || !videoDecoder) {
        ReportError(window, "No supported video stream was found.");
        return false;
    }

    double fileDuration = (format->duration != AV_NOPTS_VALUE)
        ? static_cast<double>(format->duration) / AV_TIME_BASE
        : 0.0;

    if (fileDuration <= 0.0 && format->streams[videoStream]->duration != AV_NOPTS_VALUE) {
        fileDuration = format->streams[videoStream]->duration * av_q2d(format->streams[videoStream]->time_base);
    }

    CodecPtr videoCodec(avcodec_alloc_context3(videoDecoder));

    if (!videoCodec) {
        ReportError(window, "Could not allocate the video decoder.");
        return false;
    }

    result = avcodec_parameters_to_context(
        videoCodec.get(),
        format->streams[videoStream]->codecpar
    );

    if (result < 0) {
        ReportError(
            window,
            "Could not configure the video decoder:\n" +
            FfmpegError(result)
        );
        return false;
    }

    result = avcodec_open2(
        videoCodec.get(),
        videoDecoder,
        nullptr
    );

    if (result < 0) {
        ReportError(
            window,
            "Could not start the video decoder:\n" +
            FfmpegError(result)
        );
        return false;
    }

    const AVCodec* audioDecoder = nullptr;

    const int audioStream = av_find_best_stream(
        format.get(),
        AVMEDIA_TYPE_AUDIO,
        -1,
        -1,
        &audioDecoder,
        0
    );

    CodecPtr audioCodec(nullptr);
    SwrPtr resampler(nullptr);

    AudioStopGuard audioOutput;

    bool audioEnabled = false;
    uint64_t acceptedAudioFrames = 0;
    Clock::time_point audioStartTime = Clock::now();

    if (audioStream >= 0 && audioDecoder) {
        audioCodec.reset(avcodec_alloc_context3(audioDecoder));

        if (!audioCodec) {
            result = AVERROR(ENOMEM);
        } else {
            result = avcodec_parameters_to_context(
                audioCodec.get(),
                format->streams[audioStream]->codecpar
            );

            if (result >= 0) {
                result = avcodec_open2(
                    audioCodec.get(),
                    audioDecoder,
                    nullptr
                );
            }
        }

        if (result >= 0) {
            AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
            AVChannelLayout inputLayout{};

            if (audioCodec->ch_layout.nb_channels > 0) {
                result = av_channel_layout_copy(
                    &inputLayout,
                    &audioCodec->ch_layout
                );
            } else {
                const int channelCount =
                    format->streams[audioStream]
                        ->codecpar->ch_layout.nb_channels;

                if (channelCount > 0) {
                    av_channel_layout_default(
                        &inputLayout,
                        channelCount
                    );
                    result = 0;
                } else {
                    result = AVERROR(EINVAL);
                }
            }

            if (result >= 0 &&
                audioCodec->sample_rate > 0 &&
                audioCodec->sample_fmt != AV_SAMPLE_FMT_NONE) {

                SwrContext* rawResampler = nullptr;

                result = swr_alloc_set_opts2(
                    &rawResampler,
                    &outputLayout,
                    AV_SAMPLE_FMT_FLT,
                    48000,
                    &inputLayout,
                    audioCodec->sample_fmt,
                    audioCodec->sample_rate,
                    0,
                    nullptr
                );

                if (rawResampler) {
                    resampler.reset(rawResampler);
                }

                if (result >= 0 && resampler) {
                    result = swr_init(resampler.get());
                } else if (result >= 0) {
                    result = AVERROR(ENOMEM);
                }
            } else if (result >= 0) {
                result = AVERROR(EINVAL);
            }

            av_channel_layout_uninit(&inputLayout);
            av_channel_layout_uninit(&outputLayout);
        }

        if (result >= 0 && resampler) {
            audioEnabled = AudioStart();

            if (audioEnabled) {
                audioOutput.active = true;
                audioStartTime = Clock::now();
            } else {
                std::string message = AudioGetError();

                ReportError(
                    window,
                    "Audio output could not start:\n" +
                    (message.empty()
                        ? std::string("Unknown WASAPI error.")
                        : message)
                );
            }
        }
    }

    FramePtr videoFrame(av_frame_alloc());
    FramePtr audioFrame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());

    if (!videoFrame || !audioFrame || !packet) {
        ReportError(window, "Could not allocate decoding buffers.");
        return false;
    }

    SwsPtr scaler(nullptr);

    AVRational rate = av_guess_frame_rate(
        format.get(),
        format->streams[videoStream],
        nullptr
    );

    double fps = (rate.num > 0 && rate.den > 0)
        ? av_q2d(rate)
        : 25.0;

    if (fps < 1.0 || fps > 120.0) {
        fps = 25.0;
    }

    const auto frameDuration =
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / fps)
        );

    auto nextFrameTime = Clock::now();

    auto receiveVideoFrames = [&]() -> bool {
        while (!stopRequested.load()) {
            while (paused.load() && !stopRequested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                nextFrameTime = Clock::now(); // Reset pacing when paused
            }

            const int ret = avcodec_receive_frame(
                videoCodec.get(),
                videoFrame.get()
            );

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }

            if (ret < 0) {
                ReportError(
                    window,
                    "Video decoding failed:\n" + FfmpegError(ret)
                );
                return false;
            }

            const int width = videoFrame->width;
            const int height = videoFrame->height;

            if (width <= 0 || height <= 0 ||
                width > std::numeric_limits<int>::max() / 4 ||
                static_cast<size_t>(width) * 4 >
                    std::numeric_limits<size_t>::max() /
                    static_cast<size_t>(height)) {

                av_frame_unref(videoFrame.get());

                ReportError(window, "Invalid video frame dimensions.");
                return false;
            }

            const int stride = width * 4;

            SwsContext* updated = sws_getCachedContext(
                scaler.release(),
                width,
                height,
                static_cast<AVPixelFormat>(videoFrame->format),
                width,
                height,
                AV_PIX_FMT_BGRA,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );

            scaler.reset(updated);

            if (!scaler) {
                av_frame_unref(videoFrame.get());

                ReportError(
                    window,
                    "Could not initialize pixel conversion."
                );
                return false;
            }

            std::vector<uint8_t> pixels(
                static_cast<size_t>(stride) *
                static_cast<size_t>(height)
            );

            uint8_t* destination[4] = {
                pixels.data(), nullptr, nullptr, nullptr
            };

            int destinationStride[4] = {
                stride, 0, 0, 0
            };

            const int rows = sws_scale(
                scaler.get(),
                videoFrame->data,
                videoFrame->linesize,
                0,
                height,
                destination,
                destinationStride
            );

            double currentPts = 0.0;
            if (videoFrame->pts != AV_NOPTS_VALUE) {
                currentPts = videoFrame->pts * av_q2d(format->streams[videoStream]->time_base);
            }

            av_frame_unref(videoFrame.get());

            if (rows != height) {
                ReportError(
                    window,
                    "Could not convert a video frame."
                );
                return false;
            }

            if (stopRequested.load()) {
                return false;
            }

            PublishFrame(
                window,
                std::move(pixels),
                width,
                height,
                stride
            );

            TimelineState timelineState{};
            timelineState.position = currentPts;
            timelineState.duration = fileDuration;
            timelineState.bufferedUntil = fileDuration;
            TimelineSetState(timelineState);

            nextFrameTime += frameDuration;

            const auto now = Clock::now();

            if (nextFrameTime > now) {
                while (!stopRequested.load() && !paused.load() &&
                       Clock::now() < nextFrameTime) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2)
                    );
                }
            } else if (now - nextFrameTime > frameDuration * 3) {
                nextFrameTime = Clock::now();
            }
        }

        return false;
    };

    auto receiveAudioFrames = [&]() -> bool {
        if (!audioEnabled) {
            return true;
        }

        while (!stopRequested.load()) {
            while (paused.load() && !stopRequested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            const int ret = avcodec_receive_frame(
                audioCodec.get(),
                audioFrame.get()
            );

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }

            if (ret < 0) {
                ReportError(
                    window,
                    "Audio decoding failed:\n" + FfmpegError(ret)
                );
                return false;
            }

            const int capacity = swr_get_out_samples(
                resampler.get(),
                audioFrame->nb_samples
            );

            if (capacity < 0) {
                av_frame_unref(audioFrame.get());
                return false;
            }

            if (capacity == 0) {
                av_frame_unref(audioFrame.get());
                continue;
            }

            std::vector<float> samples(
                static_cast<size_t>(capacity) * 2
            );

            uint8_t* outputPlanes[1] = {
                reinterpret_cast<uint8_t*>(samples.data())
            };

            const uint8_t** inputPlanes =
                const_cast<const uint8_t**>(
                    audioFrame->extended_data
                );

            const int converted = swr_convert(
                resampler.get(),
                outputPlanes,
                capacity,
                inputPlanes,
                audioFrame->nb_samples
            );

            av_frame_unref(audioFrame.get());

            if (converted < 0) {
                return false;
            }

            if (converted > 0) {
                const bool accepted = AudioSubmitPCM(
                    samples.data(),
                    static_cast<size_t>(converted)
                );

                if (accepted) {
                    acceptedAudioFrames +=
                        static_cast<uint64_t>(converted);
                }
            }
        }

        return false;
    };

    while (!stopRequested.load()) {
        while (paused.load() && !stopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (seekRequested.load()) {
            double targetSec = seekTargetSeconds.load();
            int64_t timestamp = static_cast<int64_t>(targetSec / av_q2d(format->streams[videoStream]->time_base));
            
            av_seek_frame(format.get(), videoStream, timestamp, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(videoCodec.get());
            if (audioCodec) {
                avcodec_flush_buffers(audioCodec.get());
            }
            
            AudioClearQueue();
            
            seekRequested.store(false);
        }

        result = av_read_frame(format.get(), packet.get());

        if (result < 0) {
            break;
        }

        if (packet->stream_index == videoStream) {
            result = avcodec_send_packet(
                videoCodec.get(),
                packet.get()
            );

            if (result == AVERROR(EAGAIN)) {
                if (!receiveVideoFrames()) {
                    av_packet_unref(packet.get());
                    return false;
                }

                result = avcodec_send_packet(
                    videoCodec.get(),
                    packet.get()
                );
            }

            if (result < 0) {
                av_packet_unref(packet.get());
                return false;
            }

            if (!receiveVideoFrames()) {
                av_packet_unref(packet.get());
                return false;
            }
        } else if (audioEnabled &&
                   packet->stream_index == audioStream) {

            result = avcodec_send_packet(
                audioCodec.get(),
                packet.get()
            );

            if (result == AVERROR(EAGAIN)) {
                if (!receiveAudioFrames()) {
                    av_packet_unref(packet.get());
                    return false;
                }

                result = avcodec_send_packet(
                    audioCodec.get(),
                    packet.get()
                );
            }

            if (result < 0) {
                av_packet_unref(packet.get());
                return false;
            }

            if (!receiveAudioFrames()) {
                av_packet_unref(packet.get());
                return false;
            }
        }

        av_packet_unref(packet.get());
    }

    return true;
}

void SeekVideo(double seconds) {
    seekTargetSeconds.store(seconds);
    seekRequested.store(true);
}

void ToggleVideoPause() {
    paused.store(!paused.load());
}

bool IsVideoPaused() {
    return paused.load();
}

void StopVideo() {
    stopRequested.store(true);
    paused.store(false);

    if (decoderThread.joinable()) {
        decoderThread.join();
    }
}

void StartVideo(HWND window, const char* filePath) {
    if (!filePath || !*filePath || !IsWindow(window)) {
        return;
    }

    StopVideo();

    {
        std::lock_guard<std::mutex> lock(frameMutex);

        latestFrame.clear();
        latestWidth = 0;
        latestHeight = 0;
        latestStride = 0;
        frameMessagePending = false;
        lastVideoError.clear();
    }

    stopRequested.store(false);
    paused.store(false);
    seekRequested.store(false);

    decoderThread = std::thread(
        [window, path = std::string(filePath)]() {
            try {
                DecodeVideo(window, path);
            } catch (const std::exception& error) {
                ReportError(
                    window,
                    std::string("Playback error:\n") + error.what()
                );
            } catch (...) {
                ReportError(
                    window,
                    "An unexpected playback error occurred."
                );
            }
        }
    );
}