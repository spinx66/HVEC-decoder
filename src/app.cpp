#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "theme.h"
#include "ui.h"
#include "video.h"
#include "timeline.h"

Theme theme;

static void HandleDroppedFile(HWND window, HDROP drop) {
    const UINT count = DragQueryFileW(
        drop, 0xFFFFFFFF, nullptr, 0
    );

    if (count > 0) {
        const UINT length = DragQueryFileW(
            drop, 0, nullptr, 0
        );

        if (length > 0) {
            std::vector<wchar_t> widePath(
                static_cast<size_t>(length) + 1
            );

            if (DragQueryFileW(
                    drop, 0, widePath.data(),
                    static_cast<UINT>(widePath.size())
                ) > 0) {

                const int required = WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    widePath.data(),
                    -1,
                    nullptr,
                    0,
                    nullptr,
                    nullptr
                );

                if (required > 0) {
                    std::vector<char> utf8Path(
                        static_cast<size_t>(required)
                    );

                    if (WideCharToMultiByte(
                            CP_UTF8,
                            0,
                            widePath.data(),
                            -1,
                            utf8Path.data(),
                            required,
                            nullptr,
                            nullptr
                        ) > 0) {

                        ClearVideoFrame();
                        StartVideo(window, utf8Path.data());
                    }
                }
            }
        }
    }

    DragFinish(drop);
}

LRESULT CALLBACK WindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
) {
    switch (message) {
        case WM_SIZE:
            ResizeUi(
                static_cast<UINT>(LOWORD(lParam)),
                static_cast<UINT>(HIWORD(lParam))
            );
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(window, &ps);
            DrawUi();
            EndPaint(window, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_DROPFILES:
            HandleDroppedFile(
                window,
                reinterpret_cast<HDROP>(wParam)
            );
            return 0;

        case WM_APP_VIDEO_FRAME: {
            std::vector<uint8_t> pixels;
            int width = 0;
            int height = 0;
            int stride = 0;

            if (TakeLatestFrame(
                    pixels, width, height, stride)) {

                if (width > 0 && height > 0 && stride > 0) {
                    if (SetVideoFrame(
                            pixels.data(),
                            static_cast<UINT>(width),
                            static_cast<UINT>(height),
                            static_cast<UINT>(stride))) {
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
            }

            return 0;
        }

        case WM_MOUSEMOVE: {
            UiMouseMoved(window);

            const float x = static_cast<float>(
                GET_X_LPARAM(lParam)
            );

            const float y = static_cast<float>(
                GET_Y_LPARAM(lParam)
            );

            RECT rect{};
            GetClientRect(window, &rect);

            if (TimelineMouseMove(
                    x, y,
                    static_cast<float>(rect.right),
                    static_cast<float>(rect.bottom))) {
                InvalidateRect(window, nullptr, FALSE);
            }

            return 0;
        }

        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(
                GET_X_LPARAM(lParam)
            );

            const float y = static_cast<float>(
                GET_Y_LPARAM(lParam)
            );

            RECT rect{};
            GetClientRect(window, &rect);

            if (TimelineHitPlayPause(x, y, static_cast<float>(rect.right), static_cast<float>(rect.bottom))) {
                ToggleVideoPause();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            if (TimelineMouseDown(
                    x, y,
                    static_cast<float>(rect.right),
                    static_cast<float>(rect.bottom))) {
                SetFocus(window);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            break;
        }

        case WM_LBUTTONUP: {
            const float x = static_cast<float>(
                GET_X_LPARAM(lParam)
            );

            const float y = static_cast<float>(
                GET_Y_LPARAM(lParam)
            );

            RECT rect{};
            GetClientRect(window, &rect);

            if (TimelineMouseUp(
                    x, y,
                    static_cast<float>(rect.right),
                    static_cast<float>(rect.bottom))) {

                double targetSeconds = 0.0;

                if (TimelineTakeSeekRequest(targetSeconds)) {
                    SeekVideo(targetSeconds);
                }

                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            break;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_LEFT) {
                double target = std::max(0.0, TimelineGetPosition() - 30.0);
                SeekVideo(target);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            } else if (wParam == VK_RIGHT) {
                double target = std::min(TimelineGetDuration(), TimelineGetPosition() + 30.0);
                SeekVideo(target);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            } else if (wParam == VK_SPACE) {
                ToggleVideoPause();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_TIMER:
            if (wParam == UI_CONTROLS_TIMER_ID) {
                UiControlsTimer(window);
                return 0;
            }
            break;

        case WM_APP_VIDEO_ERROR: {
            char error[1024]{};
            GetVideoError(error, sizeof(error));

            MessageBoxA(
                window,
                error,
                "Spinx Player - Playback Error",
                MB_OK | MB_ICONERROR
            );

            return 0;
        }

        case WM_DESTROY:
            StopVideo();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcA(
        window, message, wParam, lParam
    );
}

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE,
    LPSTR,
    int show
) {
    char exePath[MAX_PATH]{};

    const DWORD pathLength = GetModuleFileNameA(
        nullptr, exePath, MAX_PATH
    );

    if (pathLength == 0 || pathLength >= MAX_PATH) {
        MessageBoxA(
            nullptr,
            "Could not determine the executable path.",
            "Spinx Player",
            MB_ICONERROR
        );
        return 1;
    }

    char* lastSlash = strrchr(exePath, '\\');

    if (!lastSlash) {
        MessageBoxA(
            nullptr,
            "Could not resolve the theme path.",
            "Spinx Player",
            MB_ICONERROR
        );
        return 1;
    }

    *(lastSlash + 1) = '\0';

    if (strlen(exePath) + strlen("theme.ini") >= MAX_PATH) {
        MessageBoxA(
            nullptr,
            "The theme path is too long.",
            "Spinx Player",
            MB_ICONERROR
        );
        return 1;
    }

    strcat_s(exePath, MAX_PATH, "theme.ini");

    if (!LoadTheme(exePath, theme)) {
        MessageBoxA(
            nullptr,
            exePath,
            "Could not load theme.ini",
            MB_ICONERROR
        );
        return 1;
    }

    WNDCLASSA wc{};

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = "SpinxPlayerWindow";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    if (!RegisterClassA(&wc)) {
        MessageBoxA(
            nullptr,
            "Could not register the window class.",
            "Spinx Player",
            MB_ICONERROR
        );
        return 1;
    }

    HWND window = CreateWindowExA(
        0,
        wc.lpszClassName,
        "Spinx Player",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        540,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!window) {
        MessageBoxA(
            nullptr,
            "Could not create the player window.",
            "Spinx Player",
            MB_ICONERROR
        );
        return 1;
    }

    if (!CreateUi(window)) {
        MessageBoxA(
            window,
            "Could not initialize the Direct2D interface.",
            "Spinx Player",
            MB_ICONERROR
        );

        CleanupUi();
        DestroyWindow(window);
        return 1;
    }

    DragAcceptFiles(window, TRUE);

    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    int result = 0;

    while ((result = GetMessageA(
        &message, nullptr, 0, 0
    )) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    StopVideo();
    CleanupUi();

    if (result == -1) {
        MessageBoxA(
            nullptr,
            "The Windows message loop failed.",
            "Spinx Player",
            MB_ICONERROR
        );
        return 1;
    }

    return static_cast<int>(message.wParam);
}