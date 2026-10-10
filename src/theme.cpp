
#include "theme.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>

bool LoadTheme(const char* path, Theme& theme) {
    char fullPath[MAX_PATH] = {};

    if (GetFullPathNameA(path, MAX_PATH, fullPath, nullptr) == 0)
        return false;

    char rText[32] = {};
    char gText[32] = {};
    char bText[32] = {};

    if (GetPrivateProfileStringA("theme", "background_r", "", rText,
            sizeof(rText), fullPath) == 0 ||
        GetPrivateProfileStringA("theme", "background_g", "", gText,
            sizeof(gText), fullPath) == 0 ||
        GetPrivateProfileStringA("theme", "background_b", "", bText,
            sizeof(bText), fullPath) == 0)
        return false;

    char* end = nullptr;

    long r = std::strtol(rText, &end, 10);
    if (end == rText || *end != '\0' || r < 0 || r > 255)
        return false;

    long g = std::strtol(gText, &end, 10);
    if (end == gText || *end != '\0' || g < 0 || g > 255)
        return false;

    long b = std::strtol(bText, &end, 10);
    if (end == bText || *end != '\0' || b < 0 || b > 255)
        return false;

    theme.backgroundR = static_cast<int>(r);
    theme.backgroundG = static_cast<int>(g);
    theme.backgroundB = static_cast<int>(b);

    return true;
}
