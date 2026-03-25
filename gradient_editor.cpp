// gradient_editor.cpp - Full WinAPI Gradient Editor (Lab + Homework)
// Build: g++ gradient_editor.cpp resource.res -o gradient_editor.exe -lgdi32 -lcomdlg32 -lcomctl32 -mwindows
// Maciej Welpa

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES
#define NOMINMAX        // prevent windows.h from defining min/max macros

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <cassert>

using std::min;
using std::max;

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

#include "resource.h"

// ============================================================
// Constants
// ============================================================
static const int DEF_W = 600;
static const int DEF_H = 400;
static const int MIN_W = 400;
static const int MIN_H = 300;
static const int MARGIN = 5;
static const int STRIP_H = 50;
static const int STRIP_PAD = 8;   // vertical padding inside strip for handle visibility
static const int HANDLE_R = 8;   // color-stop handle radius
static const int CP_R = 10;  // canvas control-point radius

static const float DRAG_THRESH = 4.0f; // pixels before drag mode activates

// Custom window messages
#define WM_STOPS_CHANGED  (WM_USER + 10)   // strip → main: stops updated
#define WM_PICK_COLOR     (WM_USER + 11)   // strip → main: request custom color picker for stop index

// ============================================================
// Data Structures
// ============================================================
struct ColorStop {
    float    pos;   // [0..1]
    COLORREF color;
    bool operator<(const ColorStop& o) const { return pos < o.pos; }
};

enum GradientMode { GM_LINEAR = 0, GM_RADIAL = 1 };

struct Vec2 { float x, y; };

struct AppState {
    std::vector<ColorStop> stops;
    GradientMode           mode;
    Vec2                   startPt; // normalised [0..1] in canvas
    Vec2                   endPt;

    AppState() { reset(); }

    void reset() {
        stops = { {0.0f, RGB(0,0,0)}, {1.0f, RGB(255,255,255)} };
        mode = GM_LINEAR;
        startPt = { 0.0f, 0.5f };
        endPt = { 1.0f, 0.5f };
    }
};

static AppState   g_app;
static HINSTANCE  g_hInst;
static HWND       g_hwndMain, g_hwndCanvas, g_hwndStrip;
static COLORREF   g_custColors[16] = {};

// ============================================================
// Color Utilities
// ============================================================
struct HSV { float h, s, v; };

static HSV RGBtoHSV(COLORREF c) {
    float r = GetRValue(c) / 255.f, g = GetGValue(c) / 255.f, b = GetBValue(c) / 255.f;
    float mx = (r > g ? (r > b ? r : b) : (g > b ? g : b));
    float mn = (r < g ? (r < b ? r : b) : (g < b ? g : b));
    float d = mx - mn;
    HSV hsv{ 0, 0, mx };
    if (mx > 0) hsv.s = d / mx;
    if (d > 1e-6f) {
        if (mx == r) hsv.h = 60.f * fmodf((g - b) / d, 6.f);
        else if (mx == g) hsv.h = 60.f * ((b - r) / d + 2.f);
        else              hsv.h = 60.f * ((r - g) / d + 4.f);
        if (hsv.h < 0) hsv.h += 360.f;
    }
    return hsv;
}

static COLORREF HSVtoRGB(float h, float s, float v) {
    if (s <= 0) { int x = (int)(v * 255); return RGB(x, x, x); }
    h = fmodf(h, 360.f); if (h < 0) h += 360.f;
    float f = h / 60.f; int i = (int)f; f -= i;
    float p = v * (1 - s), q = v * (1 - s * f), t_ = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i % 6) {
    case 0: r = v; g = t_; b = p; break;
    case 1: r = q; g = v;  b = p; break;
    case 2: r = p; g = v;  b = t_; break;
    case 3: r = p; g = q;  b = v; break;
    case 4: r = t_; g = p;  b = v; break;
    default:r = v; g = p;  b = q; break;
    }
    return RGB((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

// ============================================================
// Gradient Interpolation
// ============================================================
static COLORREF InterpStops(float t, const std::vector<ColorStop>& s) {
    if (s.empty()) return 0;
    if (t <= s.front().pos) return s.front().color;
    if (t >= s.back().pos)  return s.back().color;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        if (t >= s[i].pos && t <= s[i + 1].pos) {
            float range = s[i + 1].pos - s[i].pos;
            float f = (range > 1e-6f) ? (t - s[i].pos) / range : 0.f;
            int r = (int)(GetRValue(s[i].color) * (1 - f) + GetRValue(s[i + 1].color) * f + .5f);
            int g = (int)(GetGValue(s[i].color) * (1 - f) + GetGValue(s[i + 1].color) * f + .5f);
            int b = (int)(GetBValue(s[i].color) * (1 - f) + GetBValue(s[i + 1].color) * f + .5f);
            return RGB(max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)));
        }
    }
    return s.back().color;
}

static std::vector<ColorStop> SortedStops() {
    auto v = g_app.stops;
    std::sort(v.begin(), v.end());
    return v;
}

// ============================================================
// DIB Gradient Rendering
// ============================================================
static inline DWORD CRefToDIB(COLORREF c) {
    return (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
}

static void RenderGradientDIB(DWORD* px, int w, int h) {
    auto stops = SortedStops();
    if (stops.empty()) { memset(px, 0, w * h * 4); return; }

    if (g_app.mode == GM_LINEAR) {
        float sx = g_app.startPt.x * w, sy = g_app.startPt.y * h;
        float ex = g_app.endPt.x * w, ey = g_app.endPt.y * h;
        float dx = ex - sx, dy = ey - sy, len2 = dx * dx + dy * dy;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float t = (len2 > 1e-6f) ? ((x - sx) * dx + (y - sy) * dy) / len2 : 0.5f;
                t = max(0.f, min(1.f, t));
                px[y * w + x] = CRefToDIB(InterpStops(t, stops));
            }
    }
    else {
        float cx = g_app.startPt.x * w, cy = g_app.startPt.y * h;
        float ex = g_app.endPt.x * w, ey = g_app.endPt.y * h;
        float R = sqrtf((ex - cx) * (ex - cx) + (ey - cy) * (ey - cy));
        if (R < 1) R = 1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float d = sqrtf((float)(x - cx) * (x - cx) + (float)(y - cy) * (y - cy));
                float t = min(1.f, d / R);
                px[y * w + x] = CRefToDIB(InterpStops(t, stops));
            }
    }
}

// Helper: paint gradient into a DC using StretchDIBits
static void PaintGradientDC(HDC hdc, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::vector<DWORD> buf((size_t)w * h);
    RenderGradientDIB(buf.data(), w, h);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, x, y, w, h, 0, 0, 0, h, buf.data(), &bmi, DIB_RGB_COLORS);
}

// ============================================================
// GIMP-Style Color Picker
// ============================================================
// Forward declared; implemented later
static COLORREF ShowGimpColorPicker(HWND parent, COLORREF initial);

// ============================================================
// BMP Export
// ============================================================
static bool ExportBMP(HWND hwnd, int w, int h) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"BMP Files\0*.bmp\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bmp";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileName(&ofn)) return false;

    std::vector<DWORD> buf((size_t)w * h);
    RenderGradientDIB(buf.data(), w, h);

    // Convert top-down DWORD array to bottom-up BGR rows
    int rowBytes = w * 3;
    int rowPadded = (rowBytes + 3) & ~3;
    std::vector<BYTE> bmpData((size_t)rowPadded * h, 0);
    for (int y = 0; y < h; y++) {
        BYTE* row = bmpData.data() + (size_t)(h - 1 - y) * rowPadded;
        for (int x = 0; x < w; x++) {
            DWORD d = buf[(size_t)y * w + x];
            row[x * 3 + 0] = (BYTE)(d & 0xFF);          // B
            row[x * 3 + 1] = (BYTE)((d >> 8) & 0xFF);  // G
            row[x * 3 + 2] = (BYTE)((d >> 16) & 0xFF);  // R
        }
    }

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + (DWORD)bmpData.size();

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h; // bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)bmpData.size();

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((char*)&bfh, sizeof(bfh));
    f.write((char*)&bih, sizeof(bih));
    f.write((char*)bmpData.data(), bmpData.size());
    return true;
}

// ============================================================
// CSV Save / Load
// ============================================================
static bool SaveCSV(HWND hwnd) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"CSV Files\0*.csv\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileName(&ofn)) return false;

    std::wofstream f(path);
    if (!f) return false;
    for (auto& s : SortedStops()) {
        wchar_t line[64];
        swprintf(line, 64, L"%.6f,#%02X%02X%02X\n",
            s.pos, GetRValue(s.color), GetGValue(s.color), GetBValue(s.color));
        f << line;
    }
    return true;
}

static bool LoadCSV(HWND hwnd) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"CSV Files\0*.csv\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileName(&ofn)) return false;

    std::wifstream f(path);
    if (!f) return false;

    std::vector<ColorStop> newStops;
    std::wstring line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        size_t comma = line.find(L',');
        if (comma == std::wstring::npos) continue;
        try {
            float pos = std::stof(line.substr(0, comma));
            std::wstring hex = line.substr(comma + 1);
            // strip whitespace/newline
            while (!hex.empty() && (hex.back() == L'\r' || hex.back() == L'\n' || hex.back() == L' ')) hex.pop_back();
            if (hex.empty()) continue;
            if (hex[0] == L'#') hex = hex.substr(1);
            if (hex.size() != 6) continue;
            unsigned int rgb = (unsigned int)std::stoul(hex, nullptr, 16);
            COLORREF col = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
            newStops.push_back({ max(0.f, min(1.f, pos)), col });
        }
        catch (...) { continue; }
    }
    if (newStops.size() < 2) {
        MessageBox(hwnd, L"CSV must contain at least 2 stops.", L"Load Error", MB_ICONWARNING);
        return false;
    }
    g_app.stops = newStops;
    return true;
}

// ============================================================
// Gradient Strip Control
// ============================================================
struct StripState {
    int  hoverIdx = -1;   // which handle is hovered
    int  dragIdx = -1;   // which handle is being dragged
    bool dragging = false; // are we in drag mode
    int  dragStartX = 0;
    int  clickX = 0;
    int  selIdx = -1;   // selected handle
};

static StripState g_strip;

static int StripHitTest(HWND hwnd, int mx) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    for (int i = 0; i < (int)g_app.stops.size(); i++) {
        int hx = (int)(g_app.stops[i].pos * w);
        int hy = rc.bottom - STRIP_PAD;
        if (abs(mx - hx) <= HANDLE_R + 2)
            return i;
    }
    return -1;
}

static void DrawStripHandle(HDC hdc, int cx, int cy, COLORREF col, bool hover, bool selected) {
    // Triangle pointing upward
    POINT pts[3] = {
        {cx, cy - HANDLE_R - 2},
        {cx - HANDLE_R, cy + 4},
        {cx + HANDLE_R, cy + 4}
    };
    // Outer black border
    HBRUSH hbr = CreateSolidBrush(col);
    HPEN   hpOuter = CreatePen(PS_SOLID, hover || selected ? 3 : 1, hover ? RGB(255, 220, 0) : RGB(0, 0, 0));
    SelectObject(hdc, hpOuter);
    SelectObject(hdc, hbr);
    Polygon(hdc, pts, 3);
    DeleteObject(hpOuter);
    DeleteObject(hbr);
    // White inner border for visibility
    HPEN hpInner = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HBRUSH hbr2 = CreateSolidBrush(col);
    SelectObject(hdc, hpInner);
    SelectObject(hdc, hbr2);
    POINT pts2[3] = {
        {cx,      cy - HANDLE_R + 1},
        {cx - HANDLE_R + 2, cy + 2},
        {cx + HANDLE_R - 2, cy + 2}
    };
    Polygon(hdc, pts2, 3);
    DeleteObject(hpInner);
    DeleteObject(hbr2);
}

static LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // handled in WM_PAINT

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcReal = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;

        // Double buffering
        HDC memDC = CreateCompatibleDC(hdcReal);
        HBITMAP memBmp = CreateCompatibleBitmap(hdcReal, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // Fill background with system button face
        HBRUSH hbg = GetSysColorBrush(COLOR_BTNFACE);
        FillRect(memDC, &rc, hbg);

        // Gradient strip area (with vertical padding for handles)
        int stripTop = STRIP_PAD;
        int stripBot = h - STRIP_PAD - HANDLE_R * 2 - 4;
        if (stripBot > stripTop) {
            // Render gradient strip
            int sw = w, sh = stripBot - stripTop;
            std::vector<DWORD> buf((size_t)sw * sh);
            // Use a horizontal linear gradient for strip display
            auto stops = SortedStops();
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++) {
                    float t = (sw > 1) ? (float)x / (sw - 1) : 0.f;
                    buf[(size_t)y * sw + x] = CRefToDIB(InterpStops(t, stops));
                }
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = sw;
            bmi.bmiHeader.biHeight = -sh;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(memDC, 0, stripTop, sw, sh, 0, 0, 0, sh, buf.data(), &bmi, DIB_RGB_COLORS);
            // Border around strip
            HPEN hpBorder = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
            HBRUSH hbNull = (HBRUSH)GetStockObject(NULL_BRUSH);
            SelectObject(memDC, hpBorder);
            SelectObject(memDC, hbNull);
            Rectangle(memDC, 0, stripTop, sw, stripBot);
            DeleteObject(hpBorder);
        }

        // Draw handles
        int hy = h - STRIP_PAD - 4;
        for (int i = 0; i < (int)g_app.stops.size(); i++) {
            int hx = (int)(g_app.stops[i].pos * w);
            DrawStripHandle(memDC, hx, hy, g_app.stops[i].color,
                i == g_strip.hoverIdx, i == g_strip.selIdx);
        }

        BitBlt(hdcReal, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam);
        int hit = StripHitTest(hwnd, mx);
        if (hit != g_strip.hoverIdx) {
            g_strip.hoverIdx = hit;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        // Drag logic
        if (g_strip.dragIdx >= 0 && (wParam & MK_LBUTTON)) {
            int dx = abs(mx - g_strip.clickX);
            if (!g_strip.dragging && dx > (int)DRAG_THRESH)
                g_strip.dragging = true;
            if (g_strip.dragging) {
                RECT rc; GetClientRect(hwnd, &rc);
                int w = rc.right;
                float newPos = (w > 0) ? (float)mx / w : 0.f;
                newPos = max(0.f, min(1.f, newPos));
                g_app.stops[g_strip.dragIdx].pos = newPos;
                InvalidateRect(hwnd, NULL, FALSE);
                InvalidateRect(GetParent(hwnd), NULL, FALSE);
                SendMessage(GetParent(hwnd), WM_STOPS_CHANGED, 0, 0);
            }
        }
        // Track mouse leave
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_strip.hoverIdx = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int hit = StripHitTest(hwnd, mx);
        if (hit >= 0) {
            g_strip.dragIdx = hit;
            g_strip.selIdx = hit;
            g_strip.clickX = mx;
            g_strip.dragging = false;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx = GET_X_LPARAM(lParam);
        ReleaseCapture();
        if (g_strip.dragIdx >= 0 && !g_strip.dragging) {
            // It was a click (not a drag) → open color picker
            int idx = g_strip.dragIdx;
            COLORREF newCol = ShowGimpColorPicker(GetParent(hwnd), g_app.stops[idx].color);
            g_app.stops[idx].color = newCol;
            InvalidateRect(hwnd, NULL, FALSE);
            InvalidateRect(GetParent(hwnd), NULL, FALSE);
            SendMessage(GetParent(hwnd), WM_STOPS_CHANGED, 0, 0);
        }
        else if (g_strip.dragging) {
            SendMessage(GetParent(hwnd), WM_STOPS_CHANGED, 0, 0);
        }
        g_strip.dragIdx = -1;
        g_strip.dragging = false;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        // Add new stop at clicked position
        int mx = GET_X_LPARAM(lParam);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right;
        float pos = (w > 0) ? (float)mx / w : 0.f;
        pos = max(0.f, min(1.f, pos));
        // Interpolate color at that position
        COLORREF col = InterpStops(pos, SortedStops());
        g_app.stops.push_back({ pos, col });
        InvalidateRect(hwnd, NULL, FALSE);
        InvalidateRect(GetParent(hwnd), NULL, FALSE);
        SendMessage(GetParent(hwnd), WM_STOPS_CHANGED, 0, 0);
        return 0;
    }

    case WM_RBUTTONUP: {
        int mx = GET_X_LPARAM(lParam);
        int hit = StripHitTest(hwnd, mx);
        if (hit >= 0 && g_app.stops.size() > 2) {
            g_app.stops.erase(g_app.stops.begin() + hit);
            if (g_strip.selIdx == hit) g_strip.selIdx = -1;
            InvalidateRect(hwnd, NULL, FALSE);
            InvalidateRect(GetParent(hwnd), NULL, FALSE);
            SendMessage(GetParent(hwnd), WM_STOPS_CHANGED, 0, 0);
        }
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================
// Canvas Control (gradient display + start/end point handles)
// ============================================================
struct CanvasState {
    int  dragPt = -1;  // 0=start, 1=end
    int  hoverPt = -1;
    bool dragging = false;
};

static CanvasState g_canvas;

static void DrawControlPoint(HDC hdc, int cx, int cy, bool isStart, bool hover) {
    // Double-border circle: outer black, inner white, fill with point color
    int r = CP_R;
    COLORREF fillCol = isStart ? RGB(50, 255, 50) : RGB(255, 80, 80);
    if (hover) r = CP_R + 2;

    // Outer black
    HPEN hpB = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
    HBRUSH hbF = CreateSolidBrush(fillCol);
    SelectObject(hdc, hpB);
    SelectObject(hdc, hbF);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    DeleteObject(hpB);
    DeleteObject(hbF);

    // Inner white ring
    HPEN hpW = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HBRUSH hbF2 = CreateSolidBrush(fillCol);
    SelectObject(hdc, hpW);
    SelectObject(hdc, hbF2);
    int ri = r - 3;
    Ellipse(hdc, cx - ri, cy - ri, cx + ri, cy + ri);
    DeleteObject(hpW);
    DeleteObject(hbF2);

    // Label
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    const wchar_t* lbl = isStart ? L"S" : L"E";
    RECT tr = { cx - ri, cy - ri, cx + ri, cy + ri };
    DrawText(hdc, lbl, 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcReal = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;

        HDC memDC = CreateCompatibleDC(hdcReal);
        HBITMAP memBmp = CreateCompatibleBitmap(hdcReal, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // Render gradient
        PaintGradientDC(memDC, 0, 0, w, h);

        // Draw start/end control points
        int sx = (int)(g_app.startPt.x * w);
        int sy = (int)(g_app.startPt.y * h);
        int ex = (int)(g_app.endPt.x * w);
        int ey = (int)(g_app.endPt.y * h);

        // Draw connecting line for linear mode
        if (g_app.mode == GM_LINEAR) {
            HPEN hpLine = CreatePen(PS_DOT, 1, RGB(200, 200, 200));
            SelectObject(memDC, hpLine);
            MoveToEx(memDC, sx, sy, NULL);
            LineTo(memDC, ex, ey);
            DeleteObject(hpLine);
        }
        else {
            // Draw radius circle for radial mode
            float R = sqrtf((float)(ex - sx) * (ex - sx) + (float)(ey - sy) * (ey - sy));
            HPEN hpCirc = CreatePen(PS_DOT, 1, RGB(200, 200, 200));
            HBRUSH hbNull = (HBRUSH)GetStockObject(NULL_BRUSH);
            SelectObject(memDC, hpCirc);
            SelectObject(memDC, hbNull);
            Ellipse(memDC, (int)(sx - R), (int)(sy - R), (int)(sx + R), (int)(sy + R));
            DeleteObject(hpCirc);
        }

        DrawControlPoint(memDC, sx, sy, true, g_canvas.hoverPt == 0);
        DrawControlPoint(memDC, ex, ey, false, g_canvas.hoverPt == 1);

        BitBlt(hdcReal, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        int sx = (int)(g_app.startPt.x * w), sy = (int)(g_app.startPt.y * h);
        int ex_ = (int)(g_app.endPt.x * w), ey_ = (int)(g_app.endPt.y * h);
        int dS = (mx - sx) * (mx - sx) + (my - sy) * (my - sy);
        int dE = (mx - ex_) * (mx - ex_) + (my - ey_) * (my - ey_);
        int threshold = (CP_R + 4) * (CP_R + 4);
        if (dS <= threshold) {
            g_canvas.dragPt = 0; g_canvas.dragging = true; SetCapture(hwnd);
        }
        else if (dE <= threshold) {
            g_canvas.dragPt = 1; g_canvas.dragging = true; SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        if (w <= 0 || h <= 0) return 0;

        if (g_canvas.dragging && g_canvas.dragPt >= 0) {
            float nx = max(0.f, min(1.f, (float)mx / w));
            float ny = max(0.f, min(1.f, (float)my / h));
            if (g_canvas.dragPt == 0) { g_app.startPt = { nx, ny }; }
            else { g_app.endPt = { nx, ny }; }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        // Hover detection
        int sx = (int)(g_app.startPt.x * w), sy = (int)(g_app.startPt.y * h);
        int ex_ = (int)(g_app.endPt.x * w), ey_ = (int)(g_app.endPt.y * h);
        int dS = (mx - sx) * (mx - sx) + (my - sy) * (my - sy);
        int dE = (mx - ex_) * (mx - ex_) + (my - ey_) * (my - ey_);
        int t2 = (CP_R + 4) * (CP_R + 4);
        int newHover = (dS <= t2) ? 0 : (dE <= t2) ? 1 : -1;
        if (newHover != g_canvas.hoverPt) {
            g_canvas.hoverPt = newHover;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_LBUTTONUP:
        g_canvas.dragging = false;
        g_canvas.dragPt = -1;
        ReleaseCapture();
        return 0;

    case WM_MOUSELEAVE:
        g_canvas.hoverPt = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================
// GIMP-Style Color Picker Dialog
// ============================================================
// Picker dialog state
struct PickerState {
    float h = 0, s = 1, v = 1;  // HSV (h: 0..360, s,v: 0..1)
    COLORREF current = RGB(255, 0, 0);
    COLORREF original = RGB(255, 0, 0);
    bool ok = false;
    bool pickingScreen = false;
    HHOOK llHook = NULL;
    HWND hwnd = NULL;
    // interaction
    bool draggingRing = false;
    bool draggingTri = false;
    // slider update guard
    bool updating = false;
    // modal loop control
    bool modalDone = false;
};

static PickerState g_picker;

// Picker layout constants
static const int PKR_CX = 150;  // center X of ring/triangle
static const int PKR_CY = 155;  // center Y
static const int PKR_ROUT = 120;  // ring outer radius
static const int PKR_RIN = 92;   // ring inner radius
static const int PKR_RTRI = 86;   // triangle circumradius

// IDs for picker controls
#define IDC_PKR_SLIDER_H   2001
#define IDC_PKR_SLIDER_S   2002
#define IDC_PKR_SLIDER_V   2003
#define IDC_PKR_SLIDER_R   2004
#define IDC_PKR_SLIDER_G   2005
#define IDC_PKR_SLIDER_B   2006
#define IDC_PKR_EDIT_H     2011
#define IDC_PKR_EDIT_S     2012
#define IDC_PKR_EDIT_V     2013
#define IDC_PKR_EDIT_R     2014
#define IDC_PKR_EDIT_G     2015
#define IDC_PKR_EDIT_B     2016
#define IDC_PKR_SWATCH_CUR 2020
#define IDC_PKR_SWATCH_OLD 2021
#define IDC_PKR_BTN_RESET  2022
#define IDC_PKR_BTN_PICK   2023
#define IDC_PKR_CANVAS     2024
#define WM_SCREENPICK_COLOR (WM_USER+50)
#define WM_SCREENPICK_UNHOOK (WM_USER+51)

// Helper: get triangle vertex positions
static void GetTriVertices(float hAngle, POINT verts[3]) {
    // v0 = pure color, v1 = white, v2 = black
    for (int i = 0; i < 3; i++) {
        float a = hAngle + (float)i * (2.f * (float)M_PI / 3.f);
        verts[i].x = PKR_CX + (LONG)(PKR_RTRI * cosf(a));
        verts[i].y = PKR_CY + (LONG)(PKR_RTRI * sinf(a));
    }
}

// Barycentric coordinates for point P in triangle (v0,v1,v2)
static bool Bary(POINT p, POINT v0, POINT v1, POINT v2, float& a, float& b, float& c) {
    float d0x = (float)(v1.x - v0.x), d0y = (float)(v1.y - v0.y);
    float d1x = (float)(v2.x - v0.x), d1y = (float)(v2.y - v0.y);
    float denom = d0x * d1y - d1x * d0y;
    if (fabsf(denom) < 1e-6f) { a = b = c = 1.f / 3; return false; }
    float px = (float)(p.x - v0.x), py = (float)(p.y - v0.y);
    b = (px * d1y - d1x * py) / denom;
    c = (d0x * py - px * d0y) / denom;
    a = 1.f - b - c;
    return (a >= 0 && b >= 0 && c >= 0);
}

static void PickerUpdateFromHSV(HWND dlg) {
    if (g_picker.updating) return;
    g_picker.updating = true;
    g_picker.current = HSVtoRGB(g_picker.h, g_picker.s, g_picker.v);

    // Update HSV sliders
    SendDlgItemMessage(dlg, IDC_PKR_SLIDER_H, TBM_SETPOS, TRUE, (LPARAM)(int)g_picker.h);
    SendDlgItemMessage(dlg, IDC_PKR_SLIDER_S, TBM_SETPOS, TRUE, (LPARAM)(int)(g_picker.s * 100));
    SendDlgItemMessage(dlg, IDC_PKR_SLIDER_V, TBM_SETPOS, TRUE, (LPARAM)(int)(g_picker.v * 100));
    // Update RGB sliders
    SendDlgItemMessage(dlg, IDC_PKR_SLIDER_R, TBM_SETPOS, TRUE, GetRValue(g_picker.current));
    SendDlgItemMessage(dlg, IDC_PKR_SLIDER_G, TBM_SETPOS, TRUE, GetGValue(g_picker.current));
    SendDlgItemMessage(dlg, IDC_PKR_SLIDER_B, TBM_SETPOS, TRUE, GetBValue(g_picker.current));
    // Update edit boxes
    wchar_t buf[16];
    swprintf(buf, 16, L"%.1f", g_picker.h);     SetDlgItemText(dlg, IDC_PKR_EDIT_H, buf);
    swprintf(buf, 16, L"%.2f", g_picker.s);     SetDlgItemText(dlg, IDC_PKR_EDIT_S, buf);
    swprintf(buf, 16, L"%.2f", g_picker.v);     SetDlgItemText(dlg, IDC_PKR_EDIT_V, buf);
    swprintf(buf, 16, L"%d", GetRValue(g_picker.current)); SetDlgItemText(dlg, IDC_PKR_EDIT_R, buf);
    swprintf(buf, 16, L"%d", GetGValue(g_picker.current)); SetDlgItemText(dlg, IDC_PKR_EDIT_G, buf);
    swprintf(buf, 16, L"%d", GetBValue(g_picker.current)); SetDlgItemText(dlg, IDC_PKR_EDIT_B, buf);

    InvalidateRect(GetDlgItem(dlg, IDC_PKR_CANVAS), NULL, FALSE);
    InvalidateRect(GetDlgItem(dlg, IDC_PKR_SWATCH_CUR), NULL, FALSE);
    g_picker.updating = false;
}

// Render the hue ring + SV triangle into a DC
static void DrawPickerCanvas(HDC hdc, int cw, int ch) {
    // Background
    RECT rc = { 0,0,cw,ch };
    HBRUSH hbg = GetSysColorBrush(COLOR_BTNFACE);
    FillRect(hdc, &rc, hbg);

    float hAngle = g_picker.h * (float)M_PI / 180.f;

    // Draw hue ring pixel-by-pixel using DIB
    std::vector<DWORD> buf((size_t)cw * ch, CRefToDIB(GetSysColor(COLOR_BTNFACE)));
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            float dx = (float)(x - PKR_CX);
            float dy = (float)(y - PKR_CY);
            float r = sqrtf(dx * dx + dy * dy);
            if (r >= PKR_RIN && r <= PKR_ROUT) {
                float a = atan2f(dy, dx); // -pi..pi
                float hue = (a < 0 ? a + 2.f * (float)M_PI : a) * 180.f / (float)M_PI;
                buf[(size_t)y * cw + x] = CRefToDIB(HSVtoRGB(hue, 1.f, 1.f));
            }
        }
    }

    // Draw SV triangle
    POINT verts[3];
    GetTriVertices(hAngle, verts);
    COLORREF pureColor = HSVtoRGB(g_picker.h, 1.f, 1.f);
    // Rasterize triangle into buf
    int mnx = verts[0].x; if (verts[1].x < mnx)mnx = verts[1].x; if (verts[2].x < mnx)mnx = verts[2].x;
    int mxx = verts[0].x; if (verts[1].x > mxx)mxx = verts[1].x; if (verts[2].x > mxx)mxx = verts[2].x;
    int mny = verts[0].y; if (verts[1].y < mny)mny = verts[1].y; if (verts[2].y < mny)mny = verts[2].y;
    int mxy = verts[0].y; if (verts[1].y > mxy)mxy = verts[1].y; if (verts[2].y > mxy)mxy = verts[2].y;
    mnx = max(mnx, 0); mxx = min(mxx, cw - 1);
    mny = max(mny, 0); mxy = min(mxy, ch - 1);
    for (int y = mny; y <= mxy; y++) {
        for (int x = mnx; x <= mxx; x++) {
            POINT p = { x,y };
            float a, b, c;
            if (Bary(p, verts[0], verts[1], verts[2], a, b, c)) {
                // v0=pure color, v1=white, v2=black
                int R = (int)(a * GetRValue(pureColor) + b * 255.f + c * 0.f + .5f);
                int G = (int)(a * GetGValue(pureColor) + b * 255.f + c * 0.f + .5f);
                int B_ = (int)(a * GetBValue(pureColor) + b * 255.f + c * 0.f + .5f);
                R = max(0, min(255, R)); G = max(0, min(255, G)); B_ = max(0, min(255, B_));
                buf[(size_t)y * cw + x] = (DWORD)((R << 16) | (G << 8) | B_);
            }
        }
    }

    // Blit buf to HDC
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw; bmi.bmiHeader.biHeight = -ch;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, 0, 0, cw, ch, 0, 0, 0, ch, buf.data(), &bmi, DIB_RGB_COLORS);

    // Draw triangle border
    HPEN hpTri = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
    HBRUSH hbNull = (HBRUSH)GetStockObject(NULL_BRUSH);
    SelectObject(hdc, hpTri);
    SelectObject(hdc, hbNull);
    Polygon(hdc, verts, 3);
    DeleteObject(hpTri);

    // Draw hue ring selection marker
    HPEN hpMark = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(hdc, hpMark);
    float markerR = (PKR_RIN + PKR_ROUT) / 2.f;
    int mx_ = (int)(PKR_CX + markerR * cosf(hAngle));
    int my_ = (int)(PKR_CY + markerR * sinf(hAngle));
    Ellipse(hdc, mx_ - 5, my_ - 5, mx_ + 5, my_ + 5);
    DeleteObject(hpMark);

    // Draw SV triangle current-color marker
    // Position from HSV: a=S*V, b=(1-S)*V, c=(1-V)
    float ac = g_picker.s * g_picker.v;
    float bc = (1.f - g_picker.s) * g_picker.v;
    float cc = 1.f - g_picker.v;
    float mx2 = ac * verts[0].x + bc * verts[1].x + cc * verts[2].x;
    float my2 = ac * verts[0].y + bc * verts[1].y + cc * verts[2].y;
    HPEN hpMk2 = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
    HPEN hpMk3 = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    SelectObject(hdc, hpMk2);
    HBRUSH hbNull2 = (HBRUSH)GetStockObject(NULL_BRUSH);
    SelectObject(hdc, hbNull2);
    Ellipse(hdc, (int)mx2 - 6, (int)my2 - 6, (int)mx2 + 6, (int)my2 + 6);
    SelectObject(hdc, hpMk3);
    Ellipse(hdc, (int)mx2 - 4, (int)my2 - 4, (int)mx2 + 4, (int)my2 + 4);
    DeleteObject(hpMk2); DeleteObject(hpMk3);
}

// Low-level mouse hook for screen picker
static LRESULT CALLBACK LLMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_picker.pickingScreen) {
        MSLLHOOKSTRUCT* mhs = (MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_LBUTTONDOWN) {
            HDC hdc = GetDC(NULL);
            COLORREF col = GetPixel(hdc, mhs->pt.x, mhs->pt.y);
            ReleaseDC(NULL, hdc);
            PostMessage(g_picker.hwnd, WM_SCREENPICK_COLOR, col, 0);
            PostMessage(g_picker.hwnd, WM_SCREENPICK_UNHOOK, 0, 0);
            g_picker.pickingScreen = false;
            return 1; // consume click
        }
        else if (wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN) {
            // Cancel picking
            PostMessage(g_picker.hwnd, WM_SCREENPICK_UNHOOK, 0, 0);
            g_picker.pickingScreen = false;
        }
    }
    return CallNextHookEx(g_picker.llHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK PickerCanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static LRESULT CALLBACK PickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_picker.hwnd = hwnd;
        // Right panel: sliders + edits
        int lx = 320, sy0 = 10, sh = 22, gap = 28;
        const wchar_t* labels[] = { L"H:", L"S:", L"V:", L"R:", L"G:", L"B:" };
        int slider_ids[] = { IDC_PKR_SLIDER_H, IDC_PKR_SLIDER_S, IDC_PKR_SLIDER_V,
                             IDC_PKR_SLIDER_R, IDC_PKR_SLIDER_G, IDC_PKR_SLIDER_B };
        int edit_ids[] = { IDC_PKR_EDIT_H,   IDC_PKR_EDIT_S,   IDC_PKR_EDIT_V,
                             IDC_PKR_EDIT_R,    IDC_PKR_EDIT_G,   IDC_PKR_EDIT_B };
        int maxvals[] = { 359, 100, 100, 255, 255, 255 };

        for (int i = 0; i < 6; i++) {
            int y = sy0 + i * gap;
            CreateWindowEx(0, L"STATIC", labels[i], WS_CHILD | WS_VISIBLE | SS_RIGHT,
                lx, y + 3, 18, 18, hwnd, NULL, g_hInst, NULL);
            HWND hSlider = CreateWindowEx(0, TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                lx + 22, y, 130, sh, hwnd, (HMENU)(size_t)slider_ids[i], g_hInst, NULL);
            SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELONG(0, maxvals[i]));
            CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | ES_RIGHT,
                lx + 156, y, 45, sh, hwnd, (HMENU)(size_t)edit_ids[i], g_hInst, NULL);
        }

        // Swatches
        int sw_y = sy0 + 6 * gap + 8;
        CreateWindowEx(WS_EX_STATICEDGE, L"STATIC", L"Current", WS_CHILD | WS_VISIBLE | SS_CENTER,
            lx, sw_y, 100, 14, hwnd, NULL, g_hInst, NULL);
        CreateWindowEx(WS_EX_STATICEDGE, L"STATIC", L"Old", WS_CHILD | WS_VISIBLE | SS_CENTER,
            lx + 105, sw_y, 100, 14, hwnd, NULL, g_hInst, NULL);
        // Current swatch (owner-draw via WM_CTLCOLORSTATIC)
        CreateWindowEx(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            lx, sw_y + 16, 100, 40, hwnd, (HMENU)IDC_PKR_SWATCH_CUR, g_hInst, NULL);
        CreateWindowEx(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            lx + 105, sw_y + 16, 100, 40, hwnd, (HMENU)IDC_PKR_SWATCH_OLD, g_hInst, NULL);

        // Buttons
        int btn_y = sw_y + 62;
        CreateWindowEx(0, L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            lx, btn_y, 95, 26, hwnd, (HMENU)IDC_PKR_BTN_RESET, g_hInst, NULL);
        CreateWindowEx(0, L"BUTTON", L"Pick Screen", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            lx + 100, btn_y, 100, 26, hwnd, (HMENU)IDC_PKR_BTN_PICK, g_hInst, NULL);

        // OK / Cancel
        int ok_y = btn_y + 34;
        CreateWindowEx(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            lx, ok_y, 95, 28, hwnd, (HMENU)IDOK, g_hInst, NULL);
        CreateWindowEx(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            lx + 100, ok_y, 95, 28, hwnd, (HMENU)IDCANCEL, g_hInst, NULL);

        // Canvas child (left side – hue ring + triangle)
        WNDCLASSEX wcCanvas = { sizeof(wcCanvas) };
        wcCanvas.lpfnWndProc = PickerCanvasProc;
        wcCanvas.hInstance = g_hInst;
        wcCanvas.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcCanvas.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wcCanvas.lpszClassName = L"PickerCanvas";
        RegisterClassEx(&wcCanvas);
        CreateWindowEx(0, L"PickerCanvas", L"", WS_CHILD | WS_VISIBLE,
            5, 5, PKR_CX * 2 + 5, PKR_CY * 2, hwnd, (HMENU)IDC_PKR_CANVAS, g_hInst, NULL);

        PickerUpdateFromHSV(hwnd);
        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        COLORREF col = (dis->CtlID == IDC_PKR_SWATCH_CUR) ? g_picker.current : g_picker.original;
        HBRUSH hb = CreateSolidBrush(col);
        FillRect(dis->hDC, &dis->rcItem, hb);
        DeleteObject(hb);
        return TRUE;
    }

    case WM_HSCROLL: {
        if (g_picker.updating) return 0;
        HWND hSlider = (HWND)lParam;
        int id = GetDlgCtrlID(hSlider);
        int pos = (int)SendMessage(hSlider, TBM_GETPOS, 0, 0);
        COLORREF cur = g_picker.current;
        if (id == IDC_PKR_SLIDER_H) { g_picker.h = (float)pos; }
        else if (id == IDC_PKR_SLIDER_S) { g_picker.s = pos / 100.f; }
        else if (id == IDC_PKR_SLIDER_V) { g_picker.v = pos / 100.f; }
        else if (id == IDC_PKR_SLIDER_R || id == IDC_PKR_SLIDER_G || id == IDC_PKR_SLIDER_B) {
            int R = (int)SendDlgItemMessage(hwnd, IDC_PKR_SLIDER_R, TBM_GETPOS, 0, 0);
            int G = (int)SendDlgItemMessage(hwnd, IDC_PKR_SLIDER_G, TBM_GETPOS, 0, 0);
            int B = (int)SendDlgItemMessage(hwnd, IDC_PKR_SLIDER_B, TBM_GETPOS, 0, 0);
            cur = RGB(R, G, B);
            HSV hsv = RGBtoHSV(cur);
            g_picker.h = hsv.h; g_picker.s = hsv.s; g_picker.v = hsv.v;
        }
        PickerUpdateFromHSV(hwnd);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int notif = HIWORD(wParam);
        if (id == IDOK) {
            g_picker.ok = true;
            if (g_picker.llHook) { UnhookWindowsHookEx(g_picker.llHook); g_picker.llHook = NULL; }
            DestroyWindow(hwnd); return 0;
        }
        if (id == IDCANCEL) {
            g_picker.ok = false;
            if (g_picker.llHook) { UnhookWindowsHookEx(g_picker.llHook); g_picker.llHook = NULL; }
            DestroyWindow(hwnd); return 0;
        }
        if (id == IDC_PKR_BTN_RESET) {
            COLORREF old = g_picker.original;
            HSV hsv = RGBtoHSV(old);
            g_picker.h = hsv.h; g_picker.s = hsv.s; g_picker.v = hsv.v;
            PickerUpdateFromHSV(hwnd);
            return 0;
        }
        if (id == IDC_PKR_BTN_PICK) {
            g_picker.pickingScreen = true;
            g_picker.llHook = SetWindowsHookEx(WH_MOUSE_LL, LLMouseProc, g_hInst, 0);
            SetCursor(LoadCursor(NULL, IDC_CROSS));
            return 0;
        }
        // Edit box changes (EN_KILLFOCUS)
        if (notif == EN_KILLFOCUS && !g_picker.updating) {
            wchar_t buf[32];
            if (id == IDC_PKR_EDIT_H) {
                GetDlgItemText(hwnd, IDC_PKR_EDIT_H, buf, 32);
                g_picker.h = max(0.f, min(359.f, (float)wcstod(buf, nullptr)));
                PickerUpdateFromHSV(hwnd);
            }
            else if (id == IDC_PKR_EDIT_S) {
                GetDlgItemText(hwnd, IDC_PKR_EDIT_S, buf, 32);
                g_picker.s = max(0.f, min(1.f, (float)wcstod(buf, nullptr)));
                PickerUpdateFromHSV(hwnd);
            }
            else if (id == IDC_PKR_EDIT_V) {
                GetDlgItemText(hwnd, IDC_PKR_EDIT_V, buf, 32);
                g_picker.v = max(0.f, min(1.f, (float)wcstod(buf, nullptr)));
                PickerUpdateFromHSV(hwnd);
            }
            else if (id == IDC_PKR_EDIT_R || id == IDC_PKR_EDIT_G || id == IDC_PKR_EDIT_B) {
                int R = GetDlgItemInt(hwnd, IDC_PKR_EDIT_R, NULL, FALSE);
                int G = GetDlgItemInt(hwnd, IDC_PKR_EDIT_G, NULL, FALSE);
                int B = GetDlgItemInt(hwnd, IDC_PKR_EDIT_B, NULL, FALSE);
                COLORREF col = RGB(max(0, min(255, R)), max(0, min(255, G)), max(0, min(255, B)));
                HSV hsv = RGBtoHSV(col);
                g_picker.h = hsv.h; g_picker.s = hsv.s; g_picker.v = hsv.v;
                PickerUpdateFromHSV(hwnd);
            }
        }
        return 0;
    }

    case WM_SCREENPICK_COLOR: {
        COLORREF col = (COLORREF)wParam;
        HSV hsv = RGBtoHSV(col);
        g_picker.h = hsv.h; g_picker.s = hsv.s; g_picker.v = hsv.v;
        PickerUpdateFromHSV(hwnd);
        return 0;
    }
    case WM_SCREENPICK_UNHOOK:
        if (g_picker.llHook) { UnhookWindowsHookEx(g_picker.llHook); g_picker.llHook = NULL; }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return 0;

    case WM_DESTROY:
        g_picker.hwnd = NULL;
        g_picker.modalDone = true;  // signal nested loop to exit
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Canvas sub-window for picker (handles mouse on ring/triangle)
static LRESULT CALLBACK PickerCanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int cw = rc.right, ch = rc.bottom;
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
        HBITMAP old = (HBITMAP)SelectObject(memDC, bmp);
        DrawPickerCanvas(memDC, cw, ch);
        BitBlt(hdc, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, old); DeleteObject(bmp); DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float dx = (float)(GET_X_LPARAM(lp) - PKR_CX);
        float dy = (float)(GET_Y_LPARAM(lp) - PKR_CY);
        float r = sqrtf(dx * dx + dy * dy);
        if (r >= PKR_RIN && r <= PKR_ROUT) {
            g_picker.draggingRing = true;
            SetCapture(hwnd);
            float a = atan2f(dy, dx); if (a < 0) a += 2.f * (float)M_PI;
            g_picker.h = a * 180.f / (float)M_PI;
            PickerUpdateFromHSV(GetParent(hwnd));
        }
        else {
            float hAngle = g_picker.h * (float)M_PI / 180.f;
            POINT verts[3]; GetTriVertices(hAngle, verts);
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            float a, b, c;
            if (Bary(p, verts[0], verts[1], verts[2], a, b, c)) {
                // S=a/(a+b), V=a+b (clamp)
                float V = min(1.f, max(0.f, a + b));
                float S = (V > 1e-6f) ? min(1.f, max(0.f, a / V)) : 0.f;
                g_picker.s = S; g_picker.v = V;
                PickerUpdateFromHSV(GetParent(hwnd));
                g_picker.draggingTri = true;
                SetCapture(hwnd);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_picker.draggingRing && !g_picker.draggingTri) return 0;
        float dx = (float)(GET_X_LPARAM(lp) - PKR_CX);
        float dy = (float)(GET_Y_LPARAM(lp) - PKR_CY);
        if (g_picker.draggingRing) {
            float a = atan2f(dy, dx); if (a < 0) a += 2.f * (float)M_PI;
            g_picker.h = a * 180.f / (float)M_PI;
        }
        else {
            float hAngle = g_picker.h * (float)M_PI / 180.f;
            POINT verts[3]; GetTriVertices(hAngle, verts);
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            float a, b, c;
            Bary(p, verts[0], verts[1], verts[2], a, b, c);
            // Clamp to triangle
            if (a < 0) { float s = a; a = 0; b -= s / 2; c -= s / 2; }
            if (b < 0) { float s = b; b = 0; a -= s / 2; c -= s / 2; }
            if (c < 0) { float s = c; c = 0; a -= s / 2; b -= s / 2; }
            float tot = a + b + c; if (tot > 1e-6f) { a /= tot; b /= tot; c /= tot; }
            float V = min(1.f, max(0.f, a + b));
            float S = (V > 1e-6f) ? min(1.f, max(0.f, a / V)) : 0.f;
            g_picker.s = S; g_picker.v = V;
        }
        PickerUpdateFromHSV(GetParent(hwnd));
        return 0;
    }
    case WM_LBUTTONUP:
        g_picker.draggingRing = g_picker.draggingTri = false;
        ReleaseCapture();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static COLORREF ShowGimpColorPicker(HWND parent, COLORREF initial) {
    // Initialize picker state
    HSV hsv = RGBtoHSV(initial);
    g_picker.h = hsv.h; g_picker.s = hsv.s; g_picker.v = hsv.v;
    g_picker.current = initial;
    g_picker.original = initial;
    g_picker.ok = false;
    g_picker.pickingScreen = false;
    g_picker.draggingRing = false;
    g_picker.draggingTri = false;
    g_picker.llHook = NULL;
    g_picker.modalDone = false;

    // Register window class (harmless if already registered)
    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc = PickerWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"GimpColorPicker";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        L"GimpColorPicker", L"Color Picker",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 560, 390,
        parent, NULL, g_hInst, NULL);

    // Center relative to parent
    RECT pr; GetWindowRect(parent, &pr);
    RECT wr; GetWindowRect(hwnd, &wr);
    int wx = pr.left + (pr.right - pr.left - (wr.right - wr.left)) / 2;
    int wy = pr.top + (pr.bottom - pr.top - (wr.bottom - wr.top)) / 2;
    SetWindowPos(hwnd, NULL, wx, wy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Nested modal message loop
    MSG msg;
    while (!g_picker.modalDone) {
        BOOL r = GetMessage(&msg, NULL, 0, 0);
        if (r == 0) {
            // WM_QUIT received – re-post so the outer loop also exits
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (r < 0) break; // error
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    return g_picker.ok ? g_picker.current : initial;
}

// ============================================================
// Main Window
// ============================================================
static void UpdateLayout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    // Canvas: fills from top, leaves room for strip at bottom
    int canvasBottom = ch - MARGIN - STRIP_H - MARGIN;
    if (canvasBottom < MARGIN) canvasBottom = MARGIN;
    SetWindowPos(g_hwndCanvas, NULL,
        MARGIN, MARGIN,
        cw - 2 * MARGIN,
        canvasBottom - MARGIN,
        SWP_NOZORDER);

    // Gradient strip: below canvas
    SetWindowPos(g_hwndStrip, NULL,
        MARGIN, canvasBottom,
        cw - 2 * MARGIN, STRIP_H,
        SWP_NOZORDER);
}

static void UpdateMenuCheck(HWND hwnd) {
    HMENU hMenu = GetMenu(hwnd);
    HMENU hGrad = GetSubMenu(hMenu, 1); // "Gradient" submenu
    CheckMenuItem(hGrad, ID_GRADIENT_LINEAR, g_app.mode == GM_LINEAR ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hGrad, ID_GRADIENT_RADIAL, g_app.mode == GM_RADIAL ? MF_CHECKED : MF_UNCHECKED);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            UpdateLayout(hwnd);
            InvalidateRect(g_hwndCanvas, NULL, FALSE);
            InvalidateRect(g_hwndStrip, NULL, FALSE);
        }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        RECT r = { 0,0,MIN_W,MIN_H };
        AdjustWindowRect(&r, (DWORD)GetWindowLong(hwnd, GWL_STYLE), TRUE);
        mmi->ptMinTrackSize = { r.right - r.left, r.bottom - r.top };
        return 0;
    }

    case WM_ERASEBKGND: {
        // Paint margins with system color to avoid artifacts
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH hbr = GetSysColorBrush(COLOR_BTNFACE);
        FillRect(hdc, &rc, hbr);
        return 1;
    }

    case WM_STOPS_CHANGED:
        InvalidateRect(g_hwndCanvas, NULL, FALSE);
        InvalidateRect(g_hwndStrip, NULL, FALSE);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case ID_GRADIENT_RESET:
            g_app.reset();
            InvalidateRect(g_hwndCanvas, NULL, FALSE);
            InvalidateRect(g_hwndStrip, NULL, FALSE);
            UpdateMenuCheck(hwnd);
            return 0;
        case ID_GRADIENT_LINEAR:
            g_app.mode = GM_LINEAR;
            g_app.startPt = { 0.f,0.5f }; g_app.endPt = { 1.f,0.5f };
            InvalidateRect(g_hwndCanvas, NULL, FALSE);
            UpdateMenuCheck(hwnd);
            return 0;
        case ID_GRADIENT_RADIAL:
            g_app.mode = GM_RADIAL;
            g_app.startPt = { 0.5f,0.5f }; g_app.endPt = { 1.f,0.5f };
            InvalidateRect(g_hwndCanvas, NULL, FALSE);
            UpdateMenuCheck(hwnd);
            return 0;
        case ID_FILE_EXPORT_BMP: {
            RECT rc; GetClientRect(g_hwndCanvas, &rc);
            if (!ExportBMP(hwnd, rc.right, rc.bottom))
                if (CommDlgExtendedError())
                    MessageBox(hwnd, L"Export failed.", L"Error", MB_ICONERROR);
            return 0;
        }
        case ID_FILE_SAVE_CSV:
            SaveCSV(hwnd);
            return 0;
        case ID_FILE_LOAD_CSV:
            if (LoadCSV(hwnd)) {
                InvalidateRect(g_hwndCanvas, NULL, FALSE);
                InvalidateRect(g_hwndStrip, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================
// WinMain
// ============================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInst;
    InitCommonControls();

    // Register canvas class
    WNDCLASSEX wcCanvas = { sizeof(wcCanvas) };
    wcCanvas.lpfnWndProc = CanvasProc;
    wcCanvas.hInstance = hInst;
    wcCanvas.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcCanvas.lpszClassName = L"GradientCanvas";
    wcCanvas.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcCanvas.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassEx(&wcCanvas);

    // Register strip class
    WNDCLASSEX wcStrip = { sizeof(wcStrip) };
    wcStrip.lpfnWndProc = StripProc;
    wcStrip.hInstance = hInst;
    wcStrip.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcStrip.lpszClassName = L"GradientStrip";
    wcStrip.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcStrip.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassEx(&wcStrip);

    // Register main window class
    WNDCLASSEX wcMain = { sizeof(wcMain) };
    wcMain.lpfnWndProc = MainWndProc;
    wcMain.hInstance = hInst;
    wcMain.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcMain.lpszClassName = L"GradientEditor";
    wcMain.style = CS_HREDRAW | CS_VREDRAW;
    wcMain.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    // Load custom icon from resource
    wcMain.hIcon = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wcMain.hIconSm = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    RegisterClassEx(&wcMain);

    // Calculate window size from desired client size
    RECT r = { 0, 0, DEF_W, DEF_H };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, TRUE); // TRUE because we have a menu

    // Create main window
    g_hwndMain = CreateWindowEx(0,
        L"GradientEditor", L"Gradient Editor",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, LoadMenu(hInst, MAKEINTRESOURCE(IDR_MENU)),
        hInst, NULL);
    if (!g_hwndMain) return 1;

    // Create child windows (positioned by UpdateLayout in WM_SIZE)
    g_hwndCanvas = CreateWindowEx(0, L"GradientCanvas", L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, g_hwndMain, NULL, hInst, NULL);
    g_hwndStrip = CreateWindowEx(WS_EX_STATICEDGE, L"GradientStrip", L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, g_hwndMain, NULL, hInst, NULL);

    // Load accelerators
    HACCEL hAccel = LoadAccelerators(hInst, MAKEINTRESOURCE(IDR_ACCEL));

    // Initial layout
    UpdateLayout(g_hwndMain);
    UpdateMenuCheck(g_hwndMain);

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!TranslateAccelerator(g_hwndMain, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
