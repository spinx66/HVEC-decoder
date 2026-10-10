#include "ui.h"
#include "timeline.h"
#include "video.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dxgiformat.h>
#include <algorithm>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

static ID2D1Factory* factory = nullptr;
static ID2D1HwndRenderTarget* target = nullptr;
static ID2D1Bitmap* videoBitmap = nullptr;

static IDWriteFactory* textFactory = nullptr;
static IDWriteTextFormat* textFormat = nullptr;
static IDWriteTextFormat* centerTextFormat = nullptr;

static ID2D1SolidColorBrush* backgroundBrush = nullptr;
static ID2D1LinearGradientBrush* panelGradientBrush = nullptr;
static ID2D1SolidColorBrush* textBrush = nullptr;

static HWND mainWindow = nullptr;
static bool controlsVisible = true;

bool CreateUi(HWND window) {
    mainWindow = window;

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        &factory
    );

    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&textFactory)
    );

    if (FAILED(hr)) return false;

    hr = textFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        18.0f,
        L"en-us",
        &textFormat
    );

    if (FAILED(hr)) return false;

    hr = textFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        20.0f,
        L"en-us",
        &centerTextFormat
    );

    if (FAILED(hr)) return false;

    centerTextFormat->SetTextAlignment(
        DWRITE_TEXT_ALIGNMENT_CENTER
    );

    centerTextFormat->SetParagraphAlignment(
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER
    );

    RECT rect{};
    GetClientRect(window, &rect);

    const UINT initialWidth = static_cast<UINT>(
        std::max<LONG>(1, rect.right - rect.left)
    );

    const UINT initialHeight = static_cast<UINT>(
        std::max<LONG>(1, rect.bottom - rect.top)
    );

    hr = factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            window,
            D2D1::SizeU(initialWidth, initialHeight)
        ),
        &target
    );

    if (FAILED(hr)) return false;

    if (FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.055f, 0.063f, 0.082f),
            &backgroundBrush)) ||
        FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.91f, 0.93f, 0.97f),
            &textBrush))) {
        return false;
    }

    // Create 70% transparent black gradient for top panel
    ID2D1GradientStopCollection* gradientStops = nullptr;
    D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.70f) },
        { 1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.40f) }
    };

    hr = target->CreateGradientStopCollection(
        stops,
        2,
        &gradientStops
    );

    if (FAILED(hr)) return false;

    hr = target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0, 0),
            D2D1::Point2F(0, 64)
        ),
        gradientStops,
        &panelGradientBrush
    );

    gradientStops->Release();

    if (FAILED(hr)) return false;

    if (!TimelineInitialize(target, textFactory)) {
        return false;
    }

    controlsVisible = true;
    return true;
}

void ResizeUi(UINT width, UINT height) {
    if (!target || width == 0 || height == 0) {
        return;
    }

    const HRESULT hr = target->Resize(
        D2D1::SizeU(width, height)
    );

    if (SUCCEEDED(hr) && mainWindow) {
        InvalidateRect(mainWindow, nullptr, FALSE);
    }
}

void ClearVideoFrame() {
    if (videoBitmap) {
        videoBitmap->Release();
        videoBitmap = nullptr;
    }

    controlsVisible = true;

    if (mainWindow) {
        KillTimer(mainWindow, UI_CONTROLS_TIMER_ID);
        InvalidateRect(mainWindow, nullptr, FALSE);
    }
}

bool SetVideoFrame(
    const uint8_t* pixels,
    UINT width,
    UINT height,
    UINT stride
) {
    if (!target || !pixels ||
        width == 0 ||
        height == 0 ||
        static_cast<unsigned long long>(stride) <
            static_cast<unsigned long long>(width) * 4ULL) {
        return false;
    }

    const bool firstFrame = (videoBitmap == nullptr);

    const D2D1_BITMAP_PROPERTIES properties =
        D2D1::BitmapProperties(
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE
            )
        );

    if (videoBitmap) {
        const D2D1_SIZE_U oldSize =
            videoBitmap->GetPixelSize();

        if (oldSize.width == width &&
            oldSize.height == height) {
            return SUCCEEDED(
                videoBitmap->CopyFromMemory(
                    nullptr,
                    pixels,
                    stride
                )
            );
        }
    }

    ID2D1Bitmap* newBitmap = nullptr;

    const HRESULT hr = target->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels,
        stride,
        properties,
        &newBitmap
    );

    if (FAILED(hr)) {
        return false;
    }

    if (videoBitmap) {
        videoBitmap->Release();
    }

    videoBitmap = newBitmap;

    if (firstFrame && mainWindow) {
        controlsVisible = true;

        SetTimer(
            mainWindow,
            UI_CONTROLS_TIMER_ID,
            2200,
            nullptr
        );

        InvalidateRect(mainWindow, nullptr, FALSE);
    }

    return true;
}

void UiMouseMoved(HWND window) {
    if (!videoBitmap) {
        if (!controlsVisible) {
            controlsVisible = true;
            InvalidateRect(window, nullptr, FALSE);
        }

        KillTimer(window, UI_CONTROLS_TIMER_ID);
        return;
    }

    if (!controlsVisible) {
        controlsVisible = true;
        InvalidateRect(window, nullptr, FALSE);
    }

    SetTimer(
        window,
        UI_CONTROLS_TIMER_ID,
        2200,
        nullptr
    );
}

void UiControlsTimer(HWND window) {
    if (!videoBitmap) {
        KillTimer(window, UI_CONTROLS_TIMER_ID);
        controlsVisible = true;
        return;
    }

    POINT cursor{};

    if (GetCursorPos(&cursor)) {
        ScreenToClient(window, &cursor);

        RECT rect{};
        GetClientRect(window, &rect);

        const bool insideWindow =
            cursor.x >= 0 &&
            cursor.x < rect.right &&
            cursor.y >= 0 &&
            cursor.y < rect.bottom;

        const bool overTopControls =
            cursor.y >= 0 && cursor.y < 64;

        const bool overTimeline =
            cursor.y >= rect.bottom - 90 && cursor.y <= rect.bottom;

        if (insideWindow && (overTopControls || overTimeline || TimelineIsDragging() || IsVideoPaused())) {
            SetTimer(
                window,
                UI_CONTROLS_TIMER_ID,
                600,
                nullptr
            );

            return;
        }
    }

    KillTimer(window, UI_CONTROLS_TIMER_ID);

    if (controlsVisible && !TimelineIsDragging() && !IsVideoPaused()) {
        controlsVisible = false;
        InvalidateRect(window, nullptr, FALSE);
    }
}

void DrawUi() {
    if (!target) return;

    const D2D1_SIZE_F size = target->GetSize();

    const float width = size.width;
    const float height = size.height;

    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    target->BeginDraw();

    target->Clear(
        D2D1::ColorF(0.055f, 0.063f, 0.082f)
    );

    const D2D1_RECT_F videoRect =
        D2D1::RectF(0, 0, width, height);

    target->FillRectangle(
        videoRect,
        backgroundBrush
    );

    if (videoBitmap) {
        const D2D1_SIZE_F imageSize =
            videoBitmap->GetSize();

        const float availableWidth =
            videoRect.right - videoRect.left;

        const float availableHeight =
            videoRect.bottom - videoRect.top;

        const float scaleX =
            availableWidth / imageSize.width;

        const float scaleY =
            availableHeight / imageSize.height;

        const float scale =
            (scaleX < scaleY) ? scaleX : scaleY;

        const float drawWidth =
            imageSize.width * scale;

        const float drawHeight =
            imageSize.height * scale;

        const float left =
            (availableWidth - drawWidth) / 2.0f;

        const float top =
            (availableHeight - drawHeight) / 2.0f;

        const D2D1_RECT_F destination =
            D2D1::RectF(
                left,
                top,
                left + drawWidth,
                top + drawHeight
            );

        target->DrawBitmap(
            videoBitmap,
            destination,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
        );
    } else {
        target->DrawText(
            L"Drag & Drop a Video",
            20,
            centerTextFormat,
            videoRect,
            textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL
        );
    }

    const bool showControls = controlsVisible || TimelineIsDragging() || IsVideoPaused();

    if (showControls) {
        target->FillRectangle(
            D2D1::RectF(0, 0, width, 64),
            panelGradientBrush
        );

        target->DrawText(
            L"SPINX PLAYER",
            12,
            textFormat,
            D2D1::RectF(24, 18, 260, 48),
            textBrush
        );
    }

    TimelineDraw(target, width, height, showControls, IsVideoPaused());

    const HRESULT hr = target->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET) {
        // Device loss recovery
    }
}

void CleanupUi() {
    if (mainWindow) {
        KillTimer(mainWindow, UI_CONTROLS_TIMER_ID);
    }

    TimelineCleanup();

    if (videoBitmap) videoBitmap->Release();

    if (textBrush) textBrush->Release();
    if (panelGradientBrush) panelGradientBrush->Release();
    if (backgroundBrush) backgroundBrush->Release();

    if (target) target->Release();

    if (centerTextFormat) centerTextFormat->Release();
    if (textFormat) textFormat->Release();

    if (textFactory) textFactory->Release();
    if (factory) factory->Release();

    videoBitmap = nullptr;
    textBrush = nullptr;
    panelGradientBrush = nullptr;
    backgroundBrush = nullptr;
    target = nullptr;
    centerTextFormat = nullptr;
    textFormat = nullptr;
    textFactory = nullptr;
    factory = nullptr;
    mainWindow = nullptr;
    controlsVisible = true;
}