#include "audio.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <exception>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 SAMPLE_RATE = 48000;
constexpr UINT32 CHANNELS = 2;

// Keep at most two seconds of queued audio.
constexpr std::size_t MAX_QUEUED_FRAMES = SAMPLE_RATE * 2;

std::mutex stateMutex;
std::mutex queueMutex;
std::condition_variable queueChanged;

std::deque<float> pcmQueue;

std::thread audioThread;
std::atomic<bool> running{false};
std::atomic<bool> stopping{false};

std::string lastError;

void SetError(const char* message) {
    std::lock_guard<std::mutex> lock(stateMutex);
    lastError = message ? message : "Unknown audio error.";
}

void SetHRESULTError(const char* operation, HRESULT hr) {
    char message[256]{};

    std::snprintf(
        message,
        sizeof(message),
        "%s failed (HRESULT 0x%08lX).",
        operation,
        static_cast<unsigned long>(hr)
    );

    SetError(message);
}

void AudioWorker() {
    HRESULT comResult = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    const bool uninitializeCOM = SUCCEEDED(comResult);

    if (FAILED(comResult)) {
        SetHRESULTError("CoInitializeEx", comResult);
        running.store(false);
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(enumerator.GetAddressOf())
    );

    if (FAILED(hr)) {
        SetHRESULTError("Creating audio device enumerator", hr);
        goto cleanup;
    }

    {
        ComPtr<IMMDevice> device;

        hr = enumerator->GetDefaultAudioEndpoint(
            eRender,
            eConsole,
            device.GetAddressOf()
        );

        if (FAILED(hr)) {
            SetHRESULTError("Finding default audio device", hr);
            goto cleanup;
        }

        ComPtr<IAudioClient> audioClient;

        hr = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(
                audioClient.GetAddressOf()
            )
        );

        if (FAILED(hr)) {
            SetHRESULTError("Activating audio client", hr);
            goto cleanup;
        }

        // Request stereo IEEE 32-bit float PCM.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = CHANNELS;
        format.nSamplesPerSec = SAMPLE_RATE;
        format.wBitsPerSample = 32;
        format.nBlockAlign = CHANNELS * sizeof(float);
        format.nAvgBytesPerSec =
            SAMPLE_RATE * format.nBlockAlign;
        format.cbSize = 0;

        WAVEFORMATEX* closestFormat = nullptr;

        hr = audioClient->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            &format,
            &closestFormat
        );

        if (closestFormat) {
            CoTaskMemFree(closestFormat);
            closestFormat = nullptr;
        }

        if (hr != S_OK) {
            // Try the Windows mix format if float 48 kHz
            // stereo is not directly supported.
            WAVEFORMATEX* mixFormat = nullptr;

            hr = audioClient->GetMixFormat(&mixFormat);

            if (FAILED(hr) || !mixFormat) {
                SetError(
                    "Could not determine the Windows audio format."
                );
                goto cleanup;
            }

            // The current version expects float PCM.
            // We reject incompatible formats rather than
            // interpreting the samples incorrectly.
            const bool compatible =
                mixFormat->nChannels == CHANNELS &&
                mixFormat->nSamplesPerSec == SAMPLE_RATE &&
                mixFormat->wBitsPerSample == 32 &&
                (
                    mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (
                        mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        mixFormat->cbSize >= 22 &&
                        IsEqualGUID(
							reinterpret_cast<WAVEFORMATEXTENSIBLE*>(
								mixFormat
							)->SubFormat,
							GUID{
								0x00000003, 0x0000, 0x0010,
								{0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}
							}
						)
                    )
                );

            if (!compatible) {
                CoTaskMemFree(mixFormat);
                SetError(
                    "The Windows audio format is incompatible "
                    "with stereo 48 kHz float PCM."
                );
                goto cleanup;
            }

            hr = audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                0,
                1000000, // 100 ms in 100-nanosecond units
                0,
                mixFormat,
                nullptr
            );

            CoTaskMemFree(mixFormat);

            if (FAILED(hr)) {
                SetHRESULTError("Initializing audio output", hr);
                goto cleanup;
            }
        } else {
            hr = audioClient->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
					AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
				1000000,
				0,
				&format,
				nullptr
			);

            if (FAILED(hr)) {
                SetHRESULTError("Initializing audio output", hr);
                goto cleanup;
            }
        }

        UINT32 bufferFrames = 0;

        hr = audioClient->GetBufferSize(&bufferFrames);

        if (FAILED(hr) || bufferFrames == 0) {
            SetError("Could not get the audio output buffer size.");
            goto cleanup;
        }

        ComPtr<IAudioRenderClient> renderClient;

        hr = audioClient->GetService(
            IID_PPV_ARGS(renderClient.GetAddressOf())
        );

        if (FAILED(hr)) {
            SetHRESULTError("Getting audio render client", hr);
            goto cleanup;
        }

        // Start with silence so playback can begin before
        // FFmpeg has decoded the first audio packet.
        hr = audioClient->Start();

        if (FAILED(hr)) {
            SetHRESULTError("Starting audio output", hr);
            goto cleanup;
        }

        running.store(true);

        while (!stopping.load()) {
            UINT32 padding = 0;

            hr = audioClient->GetCurrentPadding(&padding);

            if (FAILED(hr)) {
                SetHRESULTError("Checking audio buffer", hr);
                break;
            }

            const UINT32 available =
                bufferFrames > padding
                    ? bufferFrames - padding
                    : 0;

            if (available == 0) {
                std::unique_lock<std::mutex> lock(queueMutex);

                queueChanged.wait_for(
                    lock,
                    std::chrono::milliseconds(5),
                    [] {
                        return stopping.load() || !pcmQueue.empty();
                    }
                );

                continue;
            }

            BYTE* output = nullptr;

            hr = renderClient->GetBuffer(
                available,
                &output
            );

            if (FAILED(hr)) {
                SetHRESULTError("Requesting audio buffer", hr);
                break;
            }

            const std::size_t requestedSamples =
                static_cast<std::size_t>(available) * CHANNELS;

            std::size_t copiedSamples = 0;

            {
                std::lock_guard<std::mutex> lock(queueMutex);

                copiedSamples = std::min(
                    requestedSamples,
                    pcmQueue.size()
                );

                copiedSamples -= copiedSamples % CHANNELS;

                float* destination =
                    reinterpret_cast<float*>(output);

                for (std::size_t i = 0; i < copiedSamples; ++i) {
                    destination[i] = pcmQueue.front();
                    pcmQueue.pop_front();
                }
            }

            // Fill any underrun with silence.
            if (copiedSamples < requestedSamples) {
                std::memset(
                    output + copiedSamples * sizeof(float),
                    0,
                    (requestedSamples - copiedSamples) * sizeof(float)
                );
            }

            hr = renderClient->ReleaseBuffer(
                available,
                0
            );

            if (FAILED(hr)) {
                SetHRESULTError("Submitting audio buffer", hr);
                break;
            }

            queueChanged.notify_all();
        }

        audioClient->Stop();
    }

cleanup:
    running.store(false);

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pcmQueue.clear();
    }

    queueChanged.notify_all();

    if (uninitializeCOM) {
        CoUninitialize();
    }
}

} // namespace

bool AudioStart() {
    AudioStop();

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lastError.clear();
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pcmQueue.clear();
    }

    stopping.store(false);
    running.store(false);

    try {
        audioThread = std::thread(AudioWorker);
    } catch (const std::exception& e) {
        SetError(e.what());
        return false;
    }

    // The worker initializes COM and WASAPI asynchronously.
    // Give it a short opportunity to report startup errors.
    for (int i = 0; i < 100; ++i) {
        if (running.load()) {
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);

            if (!lastError.empty()) {
                if (audioThread.joinable()) {
                    audioThread.join();
                }
                return false;
            }
        }

        if (!audioThread.joinable()) {
            return false;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(5)
        );
    }

    // Startup is still in progress; the caller can check
    // AudioIsRunning() after a short delay.
    return true;
}

bool AudioSubmitPCM(
    const float* samples,
    std::size_t frames
) {
    if (!samples || frames == 0 ||
        !running.load() || stopping.load()) {
        return false;
    }

    // Avoid integer overflow and excessive queue growth.
    if (frames > MAX_QUEUED_FRAMES) {
        return false;
    }

    std::lock_guard<std::mutex> lock(queueMutex);

    const std::size_t queuedFrames =
        pcmQueue.size() / CHANNELS;

    if (frames > MAX_QUEUED_FRAMES - queuedFrames) {
        return false;
    }

    const std::size_t sampleCount = frames * CHANNELS;

    pcmQueue.insert(
        pcmQueue.end(),
        samples,
        samples + sampleCount
    );

    queueChanged.notify_one();
    return true;
}

void AudioClearQueue() {
    std::lock_guard<std::mutex> lock(queueMutex);
    pcmQueue.clear();
    queueChanged.notify_all();
}

void AudioStop() {
    stopping.store(true);
    queueChanged.notify_all();

    if (audioThread.joinable()) {
        audioThread.join();
    }

    running.store(false);
    stopping.store(false);

    std::lock_guard<std::mutex> lock(queueMutex);
    pcmQueue.clear();
}

std::string AudioGetError() {
    std::lock_guard<std::mutex> lock(stateMutex);
    return lastError;
}

bool AudioIsRunning() {
    return running.load();
}
