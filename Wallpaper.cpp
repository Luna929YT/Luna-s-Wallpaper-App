#define UNICODE
#define _UNICODE

#include <windows.h>
#include <gdiplus.h>
#include <filesystem>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <memory>
#include <map>
#include <functional>
#include <algorithm>

using namespace std;
using namespace Gdiplus;

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// -------------------------------------------------
// Globals
// -------------------------------------------------
HWND hwndMain = nullptr;
bool visible = true;
bool paused = false;
bool fullscreen = false;
bool uiHidden = false;
RECT windowedRect{};
mt19937 rng((unsigned)chrono::steady_clock::now().time_since_epoch().count());

// Menu ID mapping
map<UINT, struct WallpaperGroup*> menuIdToGroup;
UINT nextMenuId = 1000;

// -------------------------------------------------
// Wallpaper groups
// -------------------------------------------------
struct WallpaperGroup {
    wstring name;
    vector<wstring> images;
    map<wstring, unique_ptr<WallpaperGroup>> subgroups;

    void CollectImages(vector<wstring>& outImages) {
        outImages.insert(outImages.end(), images.begin(), images.end());
        for (auto& [_, sub] : subgroups)
            sub->CollectImages(outImages);
    }
};

unique_ptr<WallpaperGroup> rootGroup;
vector<wstring> currentSet;
size_t currentIndex = 0;

// -------------------------------------------------
// Bitmaps
// -------------------------------------------------
unique_ptr<Bitmap> wallpaperBmp;
unique_ptr<Bitmap> btnShowBmp;
unique_ptr<Bitmap> btnHideBmp;
unique_ptr<Bitmap> btnPlayBmp;
unique_ptr<Bitmap> btnPauseBmp;
unique_ptr<Bitmap> btnNextBmp;
unique_ptr<Bitmap> btnPrevBmp;
unique_ptr<Bitmap> btnUiToggleBmp;
unique_ptr<Bitmap> btnDropdownBmp;

// -------------------------------------------------
// Buttons
// -------------------------------------------------
struct Button {
    RECT rect;
    Bitmap* image;
    function<void()> onClick;
};
vector<Button> buttons;

// -------------------------------------------------
// Helpers
// -------------------------------------------------
Rect CalcAspectFit(Bitmap* bmp, const RECT& rc) {
    const REAL imgW = (REAL)bmp->GetWidth();
    const REAL imgH = (REAL)bmp->GetHeight();
    const REAL wndW = (REAL)rc.right;
    const REAL wndH = (REAL)rc.bottom;

    const REAL imgAspect = imgW / imgH;
    const REAL wndAspect = wndW / wndH;

    REAL drawW, drawH, x = 0, y = 0;
    if (imgAspect > wndAspect) {
        drawW = wndW;
        drawH = wndW / imgAspect;
        y = (wndH - drawH) * 0.5f;
    } else {
        drawH = wndH;
        drawW = wndH * imgAspect;
        x = (wndW - drawW) * 0.5f;
    }
    return Rect((INT)x, (INT)y, (INT)drawW, (INT)drawH);
}

// -------------------------------------------------
// Load folders recursively
// -------------------------------------------------
void LoadGroupRecursive(WallpaperGroup* group, const wstring& path) {
    for (auto& f : filesystem::directory_iterator(path)) {
        if (f.is_directory()) {
            auto sub = make_unique<WallpaperGroup>();
            sub->name = f.path().filename().wstring();
            LoadGroupRecursive(sub.get(), f.path().wstring());
            group->subgroups[sub->name] = move(sub);
        } else if (f.is_regular_file()) {
            auto ext = f.path().extension().wstring();
            if (ext == L".bmp" || ext == L".jpg" || ext == L".png" || ext == L".webp")
                group->images.push_back(f.path().wstring());
        }
    }
}

// -------------------------------------------------
// Populate current set
// -------------------------------------------------
void PopulateCurrentSet(WallpaperGroup* group) {
    currentSet.clear();
    group->CollectImages(currentSet);

    // Remove duplicates
    sort(currentSet.begin(), currentSet.end());
    currentSet.erase(unique(currentSet.begin(), currentSet.end()), currentSet.end());

    if (!currentSet.empty())
        currentIndex = rng() % currentSet.size();

    wallpaperBmp = make_unique<Bitmap>(currentSet[currentIndex].c_str());
    InvalidateRect(hwndMain, nullptr, TRUE);
}

// -------------------------------------------------
// Draw UI
// -------------------------------------------------
void DrawUI(HDC hdc) {
    if (!wallpaperBmp) return;
    Graphics g(hdc);

    RECT rc;
    GetClientRect(hwndMain, &rc);
    int targetW = rc.right - rc.left;
    int targetH = rc.bottom - rc.top;

    UINT imgW = wallpaperBmp->GetWidth();
    UINT imgH = wallpaperBmp->GetHeight();

    double scaleX = (double)targetW / imgW;
    double scaleY = (double)targetH / imgH;
    double scale = max(scaleX, scaleY);

    int drawW = static_cast<int>(imgW * scale);
    int drawH = static_cast<int>(imgH * scale);

    int offsetX = (targetW - drawW) / 2;
    int offsetY = (targetH - drawH) / 2;

    g.DrawImage(wallpaperBmp.get(), Rect(offsetX, offsetY, drawW, drawH));

    if (!uiHidden) {
        for (auto& b : buttons) {
            if (b.image)
                g.DrawImage(b.image, Rect(b.rect.left, b.rect.top,
                                          b.rect.right - b.rect.left,
                                          b.rect.bottom - b.rect.top));
        }
    }
}

// -------------------------------------------------
// Dropdown menu population
// -------------------------------------------------
void PopulateDropdownMenu(HMENU hMenu, WallpaperGroup* group) {
    for (auto& [name, sub] : group->subgroups) {
        if (!sub->subgroups.empty()) {
            HMENU subMenu = CreatePopupMenu();
            PopulateDropdownMenu(subMenu, sub.get());
            AppendMenu(hMenu, MF_POPUP, (UINT_PTR)subMenu, name.c_str());
        } else {
            UINT id = nextMenuId++;
            menuIdToGroup[id] = sub.get();
            AppendMenu(hMenu, MF_STRING, id, name.c_str());
        }
    }
}

// -------------------------------------------------
// Setup Buttons
// -------------------------------------------------
void SetupButtons() {
    RECT rc;
    GetClientRect(hwndMain, &rc);
    int w = 50, h = 40, m = 10;
    buttons.clear();

    // UI toggle
    buttons.push_back({ { rc.right - w - m, m, rc.right - m, m + h }, btnUiToggleBmp.get(),
        [&]() { uiHidden = !uiHidden; SetupButtons(); InvalidateRect(hwndMain, nullptr, TRUE); } });

    if (uiHidden) return;

    // Visibility
    buttons.push_back({ { m, m, m + w, m + h }, visible ? btnHideBmp.get() : btnShowBmp.get(),
        [&]() { visible = !visible; SetupButtons(); InvalidateRect(hwndMain, nullptr, TRUE); } });

    // Play/Pause
    buttons.push_back({ { m + w + m, m, m + w * 2 + m, m + h }, paused ? btnPlayBmp.get() : btnPauseBmp.get(),
        [&]() { paused = !paused; SetupButtons(); InvalidateRect(hwndMain, nullptr, TRUE); } });

    // Previous
    buttons.push_back({ { m, rc.bottom - h - m, m + w, rc.bottom - m }, btnPrevBmp.get(),
        [&]() {
            if (!currentSet.empty()) {
                currentIndex = (currentIndex + currentSet.size() - 1) % currentSet.size();
                wallpaperBmp = make_unique<Bitmap>(currentSet[currentIndex].c_str());
                InvalidateRect(hwndMain, nullptr, TRUE);
            }
        } });

    // Next
    buttons.push_back({ { rc.right - w - m, rc.bottom - h - m, rc.right - m, rc.bottom - m }, btnNextBmp.get(),
        [&]() {
            if (!currentSet.empty()) {
                currentIndex = (currentIndex + 1) % currentSet.size();
                wallpaperBmp = make_unique<Bitmap>(currentSet[currentIndex].c_str());
                InvalidateRect(hwndMain, nullptr, TRUE);
            }
        } });

    // Dropdown menu
    buttons.push_back({ { m, 60, m + w, 60 + h }, btnDropdownBmp.get(),
        [&]() {
            if (!rootGroup) return;

            HMENU hMenu = CreatePopupMenu();
            menuIdToGroup.clear();
            nextMenuId = 1000;
            PopulateDropdownMenu(hMenu, rootGroup.get());

            POINT pt = { buttons.back().rect.left, buttons.back().rect.bottom };
            ClientToScreen(hwndMain, &pt);
            TrackPopupMenu(hMenu,
                TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hwndMain, nullptr);

            DestroyMenu(hMenu);
        } });
}

// -------------------------------------------------
// Fullscreen toggle
// -------------------------------------------------
void ToggleFullscreen() {
    DWORD style = GetWindowLong(hwndMain, GWL_STYLE);

    if (!fullscreen) {
        GetWindowRect(hwndMain, &windowedRect);
        SetWindowLong(hwndMain, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hwndMain, MONITOR_DEFAULTTOPRIMARY), &mi);

        SetWindowPos(hwndMain, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);
        fullscreen = true;
    } else {
        SetWindowLong(hwndMain, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwndMain, HWND_TOP,
            windowedRect.left, windowedRect.top,
            windowedRect.right - windowedRect.left,
            windowedRect.bottom - windowedRect.top,
            SWP_FRAMECHANGED);
        fullscreen = false;
    }

    SetupButtons();
    InvalidateRect(hwndMain, nullptr, TRUE);
}

// -------------------------------------------------
// Window Proc
// -------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawUI(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        SetupButtons();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_TIMER:
        if (!paused && !currentSet.empty()) {
            currentIndex = (currentIndex + 1) % currentSet.size();
            wallpaperBmp = make_unique<Bitmap>(currentSet[currentIndex].c_str());
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        for (auto& b : buttons)
            if (PtInRect(&b.rect, pt))
                b.onClick();
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        auto it = menuIdToGroup.find(id);
        if (it != menuIdToGroup.end()) {
            PopulateCurrentSet(it->second);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_F11) ToggleFullscreen();
        if (wParam == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// -------------------------------------------------
// Main
// -------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    GdiplusStartupInput gsi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gsi, nullptr);

    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WallpaperEngineWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    hwndMain = CreateWindowEx(
        0, wc.lpszClassName, L"Wallpaper Engine",
        WS_OVERLAPPEDWINDOW,
        100, 100, 800, 600,
        nullptr, nullptr, hInst, nullptr
    );

    ShowWindow(hwndMain, SW_SHOW);

    // Load folder structure
    rootGroup = make_unique<WallpaperGroup>();
    LoadGroupRecursive(rootGroup.get(), L"C:\\Users\\DeclanLynch\\Desktop\\wallpaper app\\Wallpapers");

    // Load all images initially
    PopulateCurrentSet(rootGroup.get());

    // Load button bitmaps
    btnShowBmp        = make_unique<Bitmap>(L"assets/show.png");
    btnHideBmp        = make_unique<Bitmap>(L"assets/hide.png");
    btnPlayBmp        = make_unique<Bitmap>(L"assets/play.png");
    btnPauseBmp       = make_unique<Bitmap>(L"assets/pause.png");
    btnNextBmp        = make_unique<Bitmap>(L"assets/next.png");
    btnPrevBmp        = make_unique<Bitmap>(L"assets/prev.png");
    btnUiToggleBmp    = make_unique<Bitmap>(L"assets/ui.png");
    btnDropdownBmp    = make_unique<Bitmap>(L"assets/dropdown.png");

    SetupButtons();
    SetTimer(hwndMain, 1, 5000, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(token);
    return 0;
}
