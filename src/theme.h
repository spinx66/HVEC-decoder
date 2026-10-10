
#pragma once

struct Theme {
    int backgroundR = 9;
    int backgroundG = 12;
    int backgroundB = 18;
};

bool LoadTheme(const char* path, Theme& theme);
