
#include "ui.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dxgiformat.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

static ID2D1Factory* factory = nullptr;
static ID2D1HwndRenderTarget* target = nullptr;
static ID2D1Bitmap* videoBitmap = nullptr;

static IDWriteFactory* textFactory = nullptr;
static IDWriteTextFormat* textFormat = nullptr;
static IDWriteTextFormat* centerTextFormat = nullptr;

static ID2D1SolidColorBrush* backgroundBrush = nullptr;
static ID2D1SolidColorBrush* panelBrush = nullptr;
static ID2D1SolidColorBrush* textBrush = nullptr;
static ID2D1SolidColorBrush* accentBrush = nullptr;

static HWND mainWindow = nullptr;

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

    hr = factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            window,
            D2D1::SizeU(
                static_cast<UINT>(rect.right - rect.left),
                static_cast<UINT>(rect.bottom - rect.top)
            )
        ),
        &target
    );

    if (FAILED(hr)) return false;

    if (FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.055f, 0.063f, 0.082f),
            &backgroundBrush)) ||
        FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.10f, 0.115f, 0.145f),
            &panelBrush)) ||
        FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.91f, 0.93f, 0.97f),
            &textBrush)) ||
        FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.52f, 0.62f, 1.0f),
            &accentBrush))) {
        return false;
    }

    return true;
}

void ResizeUi(UINT width, UINT height) {
    if (target && width > 0 && height > 0) {
        HRESULT hr = target->Resize(
            D2D1::SizeU(width, height)
        );

        if (SUCCEEDED(hr) && mainWindow) {
            InvalidateRect(mainWindow, nullptr, FALSE);
        }
    }
}

void ClearVideoFrame() {
    if (videoBitmap) {
        videoBitmap->Release();
        videoBitmap = nullptr;
    }

    if (mainWindow) {
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
        width == 0 || height == 0 || stride < width * 4ULL) {
        return false;
    }

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

    HRESULT hr = target->CreateBitmap(
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
    return true;
}

void DrawUi() {
    if (!target) return;

    const D2D1_SIZE_F size = target->GetSize();
    const float width = size.width;
    const float height = size.height;

    target->BeginDraw();

    target->Clear(
        D2D1::ColorF(0.055f, 0.063f, 0.082f)
    );

    // Top bar.
    target->FillRectangle(
        D2D1::RectF(0, 0, width, 64),
        panelBrush
    );

    target->DrawText(
        L"SPINX PLAYER",
        12,
        textFormat,
        D2D1::RectF(24, 18, 260, 48),
        textBrush
    );

    // Video surface.
    const D2D1_RECT_F videoRect = D2D1::RectF(
        24, 84, width - 24, height - 100
    );

    target->FillRectangle(videoRect, backgroundBrush);

    if (videoBitmap) {
        const D2D1_SIZE_F imageSize = videoBitmap->GetSize();

        const float availableWidth =
            videoRect.right - videoRect.left;

        const float availableHeight =
            videoRect.bottom - videoRect.top;

        const float scaleX = availableWidth / imageSize.width;
        const float scaleY = availableHeight / imageSize.height;
        const float scale = (scaleX < scaleY) ? scaleX : scaleY;

        const float drawWidth = imageSize.width * scale;
        const float drawHeight = imageSize.height * scale;

        const float left =
            videoRect.left + (availableWidth - drawWidth) / 2.0f;

        const float top =
            videoRect.top + (availableHeight - drawHeight) / 2.0f;

        const D2D1_RECT_F destination = D2D1::RectF(
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

    // Bottom controls.
    target->FillRectangle(
        D2D1::RectF(0, height - 76, width, height),
        panelBrush
    );

    // Timeline.
    const float timelineY = height - 54;

    target->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(
                24, timelineY,
                width - 24, timelineY + 4
            ),
            2, 2
        ),
        panelBrush
    );

    target->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(
                24, timelineY,
                24 + (width - 48) * 0.28f,
                timelineY + 4
            ),
            2, 2
        ),
        accentBrush
    );

    // Placeholder time labels.
    target->DrawText(
        L"00:00",
        5,
        textFormat,
        D2D1::RectF(
            24, height - 39,
            100, height - 12
        ),
        textBrush
    );

    target->DrawText(
        L"00:00",
        5,
        textFormat,
        D2D1::RectF(
            width - 100, height - 39,
            width - 24, height - 12
        ),
        textBrush
    );

    // Play triangle.
    ID2D1PathGeometry* geometry = nullptr;
    ID2D1GeometrySink* sink = nullptr;

    if (factory &&
        SUCCEEDED(factory->CreatePathGeometry(&geometry))) {

        if (SUCCEEDED(geometry->Open(&sink))) {
            sink->BeginFigure(
                D2D1::Point2F(width / 2 - 6, height - 38),
                D2D1_FIGURE_BEGIN_FILLED
            );

            sink->AddLine(
                D2D1::Point2F(width / 2 - 6, height - 18)
            );

            sink->AddLine(
                D2D1::Point2F(width / 2 + 10, height - 28)
            );

            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();

            target->FillGeometry(geometry, textBrush);
            sink->Release();
        }

        geometry->Release();
    }

    HRESULT hr = target->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET) {
        // Full Direct2D device-loss recovery is a later improvement.
    }
}

void CleanupUi() {
    if (videoBitmap) videoBitmap->Release();

    if (accentBrush) accentBrush->Release();
    if (textBrush) textBrush->Release();
    if (panelBrush) panelBrush->Release();
    if (backgroundBrush) backgroundBrush->Release();

    if (target) target->Release();

    if (centerTextFormat) centerTextFormat->Release();
    if (textFormat) textFormat->Release();

    if (textFactory) textFactory->Release();
    if (factory) factory->Release();

    videoBitmap = nullptr;

    accentBrush = nullptr;
    textBrush = nullptr;
    panelBrush = nullptr;
    backgroundBrush = nullptr;

    target = nullptr;

    centerTextFormat = nullptr;
    textFormat = nullptr;

    textFactory = nullptr;
    factory = nullptr;

    mainWindow = nullptr;
}
