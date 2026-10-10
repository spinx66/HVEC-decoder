
#pragma once

#include <windows.h>
#include <cstdint>

inline constexpr UINT_PTR UI_CONTROLS_TIMER_ID = 0x501;

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

void UiMouseMoved(HWND window);
void UiControlsTimer(HWND window);
