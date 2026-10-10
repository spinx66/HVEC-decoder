
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <vector>

constexpr UINT WM_APP_VIDEO_FRAME = WM_APP + 1;
constexpr UINT WM_APP_VIDEO_ERROR = WM_APP + 2;

void StartVideo(HWND window, const char* filePath);
void StopVideo();

bool TakeLatestFrame(
    std::vector<uint8_t>& pixels,
    int& width,
    int& height,
    int& stride
);

void GetVideoError(char* buffer, size_t bufferSize);
