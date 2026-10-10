#pragma once

#include <windows.h>
#include <cstdint>

#include <d2d1.h>
#include <dwrite.h>

struct TimelineState {
    double position = 0.0;
    double duration = 0.0;
    double bufferedUntil = 0.0;
};

bool TimelineInitialize(
    ID2D1HwndRenderTarget* renderTarget,
    IDWriteFactory* writeFactory
);

void TimelineCleanup();

void TimelineSetState(const TimelineState& state);

void TimelineDraw(
    ID2D1HwndRenderTarget* target,
    float width,
    float height,
    bool visible,
    bool isPaused
);

bool TimelineMouseDown(float x, float y, float width, float height);
bool TimelineMouseMove(float x, float y, float width, float height);
bool TimelineMouseUp(float x, float y, float width, float height);

bool TimelineIsDragging();
bool TimelineTakeSeekRequest(double& targetSeconds);

double TimelineGetPosition();
double TimelineGetDuration();
bool TimelineHitPlayPause(float x, float y, float width, float height);