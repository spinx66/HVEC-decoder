
#include "video.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

using Clock = std::chrono::steady_clock;

static std::atomic<bool> stopRequested{false};
static std::thread decoderThread;

static std::mutex frameMutex;
static std::vector<uint8_t> latestFrame;
static int latestWidth = 0;
static int latestHeight = 0;
static int latestStride = 0;
static bool frameMessagePending = false;
static std::string lastVideoError;

struct FormatCloser {
    void operator()(AVFormatContext* p) const {
        if (p) avformat_close_input(&p);
    }
};

struct CodecCloser {
    void operator()(AVCodecContext* p) const {
        if (p) avcodec_free_context(&p);
    }
};

struct FrameCloser {
    void operator()(AVFrame* p) const {
        if (p) av_frame_free(&p);
    }
};

struct PacketCloser {
    void operator()(AVPacket* p) const {
        if (p) av_packet_free(&p);
    }
};

struct SwsCloser {
    void operator()(SwsContext* p) const {
        if (p) sws_freeContext(p);
    }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;
using SwsPtr = std::unique_ptr<SwsContext, SwsCloser>;

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

        // Coalesce notifications instead of queuing every frame.
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
            "Could not open video:\n" + FfmpegError(result)
        );
        return false;
    }

    FormatPtr format(rawFormat);

    result = avformat_find_stream_info(format.get(), nullptr);

    if (result < 0) {
        ReportError(
            window,
            "Could not read video information:\n" +
            FfmpegError(result)
        );
        return false;
    }

    const AVCodec* decoder = nullptr;

    int videoStream = av_find_best_stream(
        format.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        &decoder,
        0
    );

    if (videoStream < 0 || !decoder) {
        ReportError(window, "No supported video stream was found.");
        return false;
    }

    CodecPtr codec(avcodec_alloc_context3(decoder));

    if (!codec) {
        ReportError(window, "Could not allocate the video decoder.");
        return false;
    }

    result = avcodec_parameters_to_context(
        codec.get(),
        format->streams[videoStream]->codecpar
    );

    if (result < 0) {
        ReportError(
            window,
            "Could not configure the decoder:\n" +
            FfmpegError(result)
        );
        return false;
    }

    result = avcodec_open2(codec.get(), decoder, nullptr);

    if (result < 0) {
        ReportError(
            window,
            "Could not start the decoder:\n" +
            FfmpegError(result)
        );
        return false;
    }

    FramePtr frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());

    if (!frame || !packet) {
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

    auto receiveFrames = [&]() -> bool {
        while (!stopRequested.load()) {
            int ret = avcodec_receive_frame(
                codec.get(),
                frame.get()
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

            const int width = frame->width;
            const int height = frame->height;

            if (width <= 0 || height <= 0 ||
                static_cast<size_t>(width) >
                    static_cast<size_t>(
                        std::numeric_limits<int>::max() / 4
                    ) ||
                static_cast<size_t>(width) * 4 >
                    std::numeric_limits<size_t>::max() /
                    static_cast<size_t>(height)) {

                av_frame_unref(frame.get());
                ReportError(window, "Invalid video frame dimensions.");
                return false;
            }

            const int stride = width * 4;

            SwsContext* updated = sws_getCachedContext(
                scaler.release(),
                width,
                height,
                static_cast<AVPixelFormat>(frame->format),
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
                av_frame_unref(frame.get());
                ReportError(window, "Could not initialize pixel conversion.");
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

            int rows = sws_scale(
                scaler.get(),
                frame->data,
                frame->linesize,
                0,
                height,
                destination,
                destinationStride
            );

            av_frame_unref(frame.get());

            if (rows != height) {
                ReportError(window, "Could not convert a video frame.");
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

            // Basic constant-frame-rate playback pacing.
            nextFrameTime += frameDuration;

            auto now = Clock::now();

            if (nextFrameTime > now) {
                while (!stopRequested.load() &&
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

    while (!stopRequested.load()) {
        result = av_read_frame(format.get(), packet.get());

        if (result < 0) {
            break;
        }

        if (packet->stream_index == videoStream) {
            result = avcodec_send_packet(codec.get(), packet.get());

            if (result == AVERROR(EAGAIN)) {
                // Drain pending frames and retry this packet.
                if (!receiveFrames()) {
                    av_packet_unref(packet.get());
                    return false;
                }

                result = avcodec_send_packet(codec.get(), packet.get());
            }

            if (result < 0) {
                av_packet_unref(packet.get());

                ReportError(
                    window,
                    "Could not submit a video packet:\n" +
                    FfmpegError(result)
                );
                return false;
            }

            if (!receiveFrames()) {
                av_packet_unref(packet.get());
                return false;
            }
        }

        av_packet_unref(packet.get());
    }

    if (stopRequested.load()) {
        return true;
    }

    // Flush delayed frames at end of file.
    result = avcodec_send_packet(codec.get(), nullptr);

    if (result == AVERROR(EAGAIN)) {
        if (!receiveFrames()) {
            return false;
        }

        result = avcodec_send_packet(codec.get(), nullptr);
    }

    if (result >= 0 || result == AVERROR_EOF) {
        receiveFrames();
    }

    // The final frame remains displayed at end of file.
    return true;
}

void StopVideo() {
    stopRequested.store(true);

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
