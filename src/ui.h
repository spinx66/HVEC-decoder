
#pragma once

#include <windows.h>
#include <cstdint>

bool CreateUi(HWND window);
void ResizeUi(UINT width, UINT height);
void DrawUi();
void CleanupUi();

void ClearVideoFrame();

bool SetVideoFrame(
    const uint8_t* pixels,
    UINT width,
    UINT height,
    UINT stride
);
