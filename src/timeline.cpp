#include "timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
    ID2D1SolidColorBrush* trackBrush = nullptr;
    ID2D1SolidColorBrush* bufferBrush = nullptr;
    ID2D1SolidColorBrush* progressBrush = nullptr;
    ID2D1SolidColorBrush* thumbBrush = nullptr;
    ID2D1SolidColorBrush* textBrush = nullptr;
    ID2D1SolidColorBrush* hoverBrush = nullptr;

    IDWriteTextFormat* timeFormat = nullptr;

    TimelineState state{};

    bool dragging = false;
    bool pendingSeek = false;
    double requestedSeek = 0.0;
    double dragPosition = 0.0;

    constexpr float SIDE_MARGIN = 28.0f;
    constexpr float TIMELINE_HEIGHT = 76.0f;
    constexpr float TRACK_HEIGHT = 4.0f;
    constexpr float THUMB_RADIUS = 6.0f;

    double ClampTime(double value) {
        if (!std::isfinite(value) || value < 0.0)
            return 0.0;

        if (state.duration > 0.0)
            return std::min(value, state.duration);

        return 0.0;
    }

    bool ValidDuration() {
        return std::isfinite(state.duration) &&
               state.duration > 0.0;
    }

    float TrackLeft(float width) {
        return SIDE_MARGIN;
    }

    float TrackRight(float width) {
        return std::max(SIDE_MARGIN + 1.0f,
                        width - SIDE_MARGIN);
    }

    float TrackY(float height) {
        return height - 43.0f;
    }

    float TimeToX(double seconds, float width) {
        const float left = TrackLeft(width);
        const float right = TrackRight(width);

        if (!ValidDuration())
            return left;

        const double fraction =
            ClampTime(seconds) / state.duration;

        return left + static_cast<float>(fraction) *
                      (right - left);
    }

    double XToTime(float x, float width) {
        if (!ValidDuration())
            return 0.0;

        const float left = TrackLeft(width);
        const float right = TrackRight(width);
        const float usable = right - left;

        if (usable <= 0.0f)
            return 0.0;

        const double fraction = std::clamp(
            static_cast<double>((x - left) / usable),
            0.0,
            1.0
        );

        return fraction * state.duration;
    }

    bool HitTrack(float x, float y, float width, float height) {
        const float left = TrackLeft(width) - 8.0f;
        const float right = TrackRight(width) + 8.0f;
        const float centerY = TrackY(height);

        return x >= left && x <= right &&
               y >= centerY - 22.0f &&
               y <= height;
    }

    void FormatTime(double seconds, wchar_t* output, size_t size) {
        if (!std::isfinite(seconds) || seconds < 0.0) {
            swprintf_s(output, size, L"--:--");
            return;
        }

        const auto total = static_cast<unsigned long long>(
            seconds
        );

        const auto hours = total / 3600;
        const auto minutes = (total / 60) % 60;
        const auto secs = total % 60;

        if (hours > 0) {
            swprintf_s(
                output, size, L"%llu:%02llu:%02llu",
                hours, minutes, secs
            );
        } else {
            swprintf_s(
                output, size, L"%02llu:%02llu",
                minutes, secs
            );
        }
    }

    void DrawText(
        ID2D1HwndRenderTarget* target,
        const wchar_t* text,
        float left,
        float top,
        float right,
        float bottom
    ) {
        if (!target || !timeFormat || !textBrush)
            return;

        target->DrawText(
            text,
            static_cast<UINT32>(wcslen(text)),
            timeFormat,
            D2D1::RectF(left, top, right, bottom),
            textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL
        );
    }
}

bool TimelineInitialize(
    ID2D1HwndRenderTarget* renderTarget,
    IDWriteFactory* writeFactory
) {
    if (!renderTarget || !writeFactory)
        return false;

    HRESULT hr = writeFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"en-us",
        &timeFormat
    );

    if (FAILED(hr))
        return false;

    const auto createBrush = [&](D2D1_COLOR_F color,
                                 ID2D1SolidColorBrush** brush) {
        return SUCCEEDED(
            renderTarget->CreateSolidColorBrush(color, brush)
        );
    };

    if (!createBrush(
            D2D1::ColorF(0.28f, 0.30f, 0.34f, 0.90f),
            &trackBrush) ||
        !createBrush(
            D2D1::ColorF(0.57f, 0.60f, 0.65f, 0.85f),
            &bufferBrush) ||
        !createBrush(
            D2D1::ColorF(1.0f, 0.25f, 0.29f),
            &progressBrush) ||
        !createBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f),
            &thumbBrush) ||
        !createBrush(
            D2D1::ColorF(0.91f, 0.93f, 0.97f),
            &textBrush) ||
        !createBrush(
            D2D1::ColorF(0.12f, 0.13f, 0.16f, 0.70f),
            &hoverBrush)) {
        TimelineCleanup();
        return false;
    }

    return true;
}

void TimelineCleanup() {
    if (hoverBrush) hoverBrush->Release();
    if (textBrush) textBrush->Release();
    if (thumbBrush) thumbBrush->Release();
    if (progressBrush) progressBrush->Release();
    if (bufferBrush) bufferBrush->Release();
    if (trackBrush) trackBrush->Release();
    if (timeFormat) timeFormat->Release();

    hoverBrush = nullptr;
    textBrush = nullptr;
    thumbBrush = nullptr;
    progressBrush = nullptr;
    bufferBrush = nullptr;
    trackBrush = nullptr;
    timeFormat = nullptr;

    dragging = false;
    pendingSeek = false;
}

void TimelineSetState(const TimelineState& newState) {
    state.duration =
        std::isfinite(newState.duration) &&
        newState.duration > 0.0
            ? newState.duration : 0.0;

    state.position = ClampTime(newState.position);
    state.bufferedUntil = ClampTime(newState.bufferedUntil);
}

void TimelineDraw(
    ID2D1HwndRenderTarget* target,
    float width,
    float height,
    bool visible,
    bool isPaused
) {
    if (!target || !visible ||
        !trackBrush || !progressBrush || !textBrush)
        return;

    const float panelTop = height - TIMELINE_HEIGHT;

    target->FillRectangle(
        D2D1::RectF(0.0f, panelTop, width, height),
        hoverBrush
    );

    const float left = TrackLeft(width);
    const float right = TrackRight(width);
    const float y = TrackY(height);

    // Base track.
    target->FillRectangle(
        D2D1::RectF(
            left, y - TRACK_HEIGHT / 2.0f,
            right, y + TRACK_HEIGHT / 2.0f
        ),
        trackBrush
    );

    if (ValidDuration()) {
        const float bufferedX =
            TimeToX(state.bufferedUntil, width);

        target->FillRectangle(
            D2D1::RectF(
                left, y - TRACK_HEIGHT / 2.0f,
                bufferedX, y + TRACK_HEIGHT / 2.0f
            ),
            bufferBrush
        );

        const double shownPosition =
            dragging ? dragPosition : state.position;

        const float positionX =
            TimeToX(shownPosition, width);

        target->FillRectangle(
            D2D1::RectF(
                left, y - TRACK_HEIGHT / 2.0f,
                positionX, y + TRACK_HEIGHT / 2.0f
            ),
            progressBrush
        );

        target->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(positionX, y),
                THUMB_RADIUS,
                THUMB_RADIUS
            ),
            thumbBrush
        );

        // Draw Play/Pause button at bottom middle
        const float centerX = width / 2.0f;
        const float centerY = height - 21.0f;

        if (isPaused) {
            target->FillRectangle(D2D1::RectF(centerX - 5.0f, centerY - 7.0f, centerX - 1.0f, centerY + 7.0f), textBrush);
            target->FillRectangle(D2D1::RectF(centerX - 1.0f, centerY - 5.0f, centerX + 3.0f, centerY + 5.0f), textBrush);
            target->FillRectangle(D2D1::RectF(centerX + 3.0f, centerY - 3.0f, centerX + 7.0f, centerY + 3.0f), textBrush);
        } else {
            target->FillRectangle(
                D2D1::RectF(centerX - 5.0f, centerY - 7.0f, centerX - 1.0f, centerY + 7.0f),
                textBrush
            );
            target->FillRectangle(
                D2D1::RectF(centerX + 2.0f, centerY - 7.0f, centerX + 6.0f, centerY + 7.0f),
                textBrush
            );
        }
    }

    wchar_t current[32]{};
    wchar_t total[32]{};

    FormatTime(
        dragging ? dragPosition : state.position,
        current, sizeof(current) / sizeof(current[0])
    );

    FormatTime(
        state.duration,
        total, sizeof(total) / sizeof(total[0])
    );

    DrawText(
        target, current,
        left, height - 31.0f,
        left + 90.0f, height - 10.0f
    );

    DrawText(
        target, total,
        std::max(left + 100.0f, right - 90.0f),
        height - 31.0f,
        right, height - 10.0f
    );

    if (!ValidDuration()) {
        DrawText(
            target, L"Loading media information...",
            left, height - 59.0f,
            right, height - 43.0f
        );
    }
}

bool TimelineMouseDown(
    float x, float y, float width, float height
) {
    if (!HitTrack(x, y, width, height))
        return false;

    if (!ValidDuration())
        return true;

    dragging = true;
    dragPosition = XToTime(x, width);
    SetCapture(GetActiveWindow());
    return true;
}

bool TimelineMouseMove(
    float x, float y, float width, float height
) {
    if (!dragging)
        return false;

    dragPosition = XToTime(x, width);
    return true;
}

bool TimelineMouseUp(
    float x, float y, float width, float height
) {
    if (!dragging)
        return false;

    dragPosition = XToTime(x, width);
    requestedSeek = dragPosition;
    pendingSeek = true;
    dragging = false;

    if (GetCapture())
        ReleaseCapture();

    return true;
}

bool TimelineIsDragging() {
    return dragging;
}

bool TimelineTakeSeekRequest(double& targetSeconds) {
    if (!pendingSeek)
        return false;

    targetSeconds = requestedSeek;
    pendingSeek = false;
    return true;
}

double TimelineGetPosition() {
    return dragging ? dragPosition : state.position;
}

double TimelineGetDuration() {
    return state.duration;
}

bool TimelineHitPlayPause(float x, float y, float width, float height) {
    const float centerX = width / 2.0f;
    const float panelTop = height - TIMELINE_HEIGHT;
    return x >= centerX - 20.0f && x <= centerX + 20.0f &&
           y >= panelTop + 24.0f && y <= panelTop + 64.0f;
}