/*
zig cc DDCBrightness.c -o DDCBrightness.exe -municode "-Wl,--subsystem,windows" -luser32 -lshell32 -lgdi32 -lcomctl32 -ldxva2 -ladvapi32 -Oz -s -ffunction-sections -fdata-sections "-Wl,--gc-sections"
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "advapi32.lib")

// 启用 Windows 原生控件视觉样式 (ComCtl32 v6)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_TRAYICON (WM_USER + 1)
#define IDI_TRAYICON 101
#define ID_MENU_AUTOSTART 1001
#define ID_MENU_EXIT      1002

#define REG_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APP_NAME    L"DDCBrightnessTray"
#define MAX_MONITORS 8

// Win10 Flyout 暗色方案
#define COLOR_WIN10_BG      RGB(31, 31, 31)     // #1F1F1F
#define COLOR_WIN10_TEXT    RGB(240, 240, 240)  
#define COLOR_WIN10_SUBTEXT RGB(160, 160, 160)  
#define COLOR_WIN10_SPLIT   RGB(45, 45, 45)     

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// --- 动态 API 函数指针定义 ---
typedef HRESULT (WINAPI *pfnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *pfnSetWindowTheme)(HWND, LPCWSTR, LPCWSTR);

pfnDwmSetWindowAttribute g_DwmSetWindowAttribute = NULL;
pfnSetWindowTheme g_SetWindowTheme = NULL;

void LoadDynamicAPIs() {
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        g_DwmSetWindowAttribute = (pfnDwmSetWindowAttribute)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    }
    HMODULE hTheme = LoadLibraryW(L"uxtheme.dll");
    if (hTheme) {
        g_SetWindowTheme = (pfnSetWindowTheme)GetProcAddress(hTheme, "SetWindowTheme");
    }
}

typedef struct {
    HANDLE hPhysicalMonitor;
    WCHAR description[128];
    DWORD minBrightness, curBrightness, maxBrightness;
    BOOL isSupported;
    HWND hSlider, hValLabel, hOffBtn;
} MonitorInfo;

MonitorInfo g_Monitors[MAX_MONITORS];
int g_MonitorCount = 0;
NOTIFYICONDATAW g_Nid = {0};
HWND g_hPopupWnd = NULL;
HFONT g_hFontText = NULL;
HFONT g_hFontIcon = NULL;
HBRUSH g_hbrBg = NULL;

// --- 从 DDC/CI Capabilities 字符串中获取显示器型号 ---
BOOL GetMonitorNameFromDDC(HANDLE hPhysicalMonitor, WCHAR* outName, DWORD maxLen) {
    DWORD capLen = 0;
    if (!GetCapabilitiesStringLength(hPhysicalMonitor, &capLen) || capLen == 0) {
        return FALSE;
    }

    char* capStr = (char*)malloc(capLen + 1);
    if (!capStr) return FALSE;
    memset(capStr, 0, capLen + 1);

    BOOL found = FALSE;
    if (CapabilitiesRequestAndCapabilitiesReply(hPhysicalMonitor, capStr, capLen)) {
        // 解析 DDC/CI 响应中的 "model(...)" 字段
        char* modelStart = strstr(capStr, "model(");
        if (modelStart) {
            modelStart += 6; // 跳过 "model("
            char* modelEnd = strchr(modelStart, ')');
            if (modelEnd && modelEnd > modelStart) {
                int modelLen = (int)(modelEnd - modelStart);
                if (modelLen > 0 && modelLen < (int)maxLen) {
                    MultiByteToWideChar(CP_ACP, 0, modelStart, modelLen, outName, maxLen);
                    outName[modelLen] = L'\0';
                    found = TRUE;
                }
            }
        }
    }
    free(capStr);
    return found;
}

// --- 开机自启动注册表控制 ---
BOOL IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = 0;
        LONG res = RegQueryValueExW(hKey, APP_NAME, NULL, NULL, NULL, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return FALSE;
}

void ToggleAutoStart() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_WRITE | KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (IsAutoStartEnabled()) {
            RegDeleteValueW(hKey, APP_NAME);
        } else {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE*)path, (wcslen(path) + 1) * sizeof(WCHAR));
        }
        RegCloseKey(hKey);
    }
}

// --- DDC/CI 显示器枚举 ---
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    DWORD count = 0;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) && count > 0) {
        LPPHYSICAL_MONITOR pPMs = (LPPHYSICAL_MONITOR)malloc(sizeof(PHYSICAL_MONITOR) * count);
        if (pPMs && GetPhysicalMonitorsFromHMONITOR(hMonitor, count, pPMs)) {
            for (DWORD i = 0; i < count && g_MonitorCount < MAX_MONITORS; i++) {
                g_Monitors[g_MonitorCount].hPhysicalMonitor = pPMs[i].hPhysicalMonitor;
                
                // 优先尝试从 DDC/CI 读取真实显示器型号
                WCHAR ddcName[128] = {0};
                if (GetMonitorNameFromDDC(pPMs[i].hPhysicalMonitor, ddcName, 128)) {
                    wcsncpy_s(g_Monitors[g_MonitorCount].description, 128, ddcName, _TRUNCATE);
                } else {
                    // DDC/CI 未返回名称时自动降级使用系统的设备描述
                    wcsncpy_s(g_Monitors[g_MonitorCount].description, 128, pPMs[i].szPhysicalMonitorDescription, _TRUNCATE);
                }

                DWORD minB = 0, curB = 0, maxB = 0;
                if (GetMonitorBrightness(pPMs[i].hPhysicalMonitor, &minB, &curB, &maxB)) {
                    g_Monitors[g_MonitorCount].minBrightness = minB;
                    g_Monitors[g_MonitorCount].curBrightness = curB;
                    g_Monitors[g_MonitorCount].maxBrightness = maxB;
                    g_Monitors[g_MonitorCount].isSupported = TRUE;
                } else {
                    g_Monitors[g_MonitorCount].minBrightness = 0;
                    g_Monitors[g_MonitorCount].curBrightness = 0;
                    g_Monitors[g_MonitorCount].maxBrightness = 100;
                    g_Monitors[g_MonitorCount].isSupported = FALSE;
                }
                g_MonitorCount++;
            }
            free(pPMs);
        }
    }
    return TRUE;
}

void RefreshMonitors() {
    for (int i = 0; i < g_MonitorCount; i++) {
        if (g_Monitors[i].hPhysicalMonitor) DestroyPhysicalMonitor(g_Monitors[i].hPhysicalMonitor);
    }
    g_MonitorCount = 0;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
}

// --- 构建 Win10 现代 UI ---
void RebuildUIControls(HWND hwnd) {
    HWND hChild = GetWindow(hwnd, GW_CHILD);
    while (hChild) {
        HWND hNext = GetWindow(hChild, GW_HWNDNEXT);
        DestroyWindow(hChild);
        hChild = hNext;
    }

    RefreshMonitors();

    int y = 14;
    for (int i = 0; i < g_MonitorCount; i++) {
        // 1. 显示器名称
        HWND hTitle = CreateWindowW(L"STATIC", g_Monitors[i].description,
            WS_CHILD | WS_VISIBLE | SS_LEFT, 18, y, 260, 20, hwnd, NULL, NULL, NULL);
        SendMessage(hTitle, WM_SETFONT, (WPARAM)g_hFontText, TRUE);

        // 2. 右上角 Win10 电源息屏按钮 (\uE7E8 Segoe MDL2 Power)
        HWND hOffBtn = CreateWindowW(L"BUTTON", L"\uE7E8",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
            312, y - 2, 28, 24, hwnd, (HMENU)(INT_PTR)(3000 + i), NULL, NULL);
        SendMessage(hOffBtn, WM_SETFONT, (WPARAM)g_hFontIcon, TRUE);
        if (g_SetWindowTheme) g_SetWindowTheme(hOffBtn, L"DarkMode_Explorer", NULL);
        g_Monitors[i].hOffBtn = hOffBtn;

        // 3. Win10 亮度图标 (\uE706 Segoe MDL2 Brightness)
        HWND hIcon = CreateWindowW(L"STATIC", L"\uE706",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 16, y + 30, 22, 22, hwnd, NULL, NULL, NULL);
        SendMessage(hIcon, WM_SETFONT, (WPARAM)g_hFontIcon, TRUE);

        // 4. 暗色 Trackbar 滑块
        HWND hSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            42, y + 28, 250, 24, hwnd, (HMENU)(INT_PTR)(2000 + i), NULL, NULL);
        
        if (g_SetWindowTheme) g_SetWindowTheme(hSlider, L"DarkMode_Explorer", NULL);
        SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELONG(g_Monitors[i].minBrightness, g_Monitors[i].maxBrightness));
        SendMessage(hSlider, TBM_SETPOS, TRUE, g_Monitors[i].curBrightness);
        EnableWindow(hSlider, g_Monitors[i].isSupported);
        g_Monitors[i].hSlider = hSlider;

        // 5. 亮度数值
        WCHAR valStr[16];
        swprintf_s(valStr, 16, g_Monitors[i].isSupported ? L"%d" : L"-1", g_Monitors[i].curBrightness);
        HWND hVal = CreateWindowW(L"STATIC", valStr,
            WS_CHILD | WS_VISIBLE | SS_RIGHT, 296, y + 30, 42, 20, hwnd, NULL, NULL, NULL);
        SendMessage(hVal, WM_SETFONT, (WPARAM)g_hFontText, TRUE);
        g_Monitors[i].hValLabel = hVal;

        y += 72;
    }
}

void ShowPopupWindow(HWND hwnd) {
    RebuildUIControls(hwnd);

    int windowWidth = 360;
    int windowHeight = g_MonitorCount * 72 + 12;
    if (windowHeight < 80) windowHeight = 80;

    POINT pt;
    GetCursorPos(&pt);
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    int x = pt.x - windowWidth / 2;
    int y = pt.y - windowHeight - 12;
    if (x < workArea.left) x = workArea.left + 5;
    if (x + windowWidth > workArea.right) x = workArea.right - windowWidth - 5;
    if (y < workArea.top) y = pt.y + 10;

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, windowWidth, windowHeight, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
}

// --- 窗口过程 ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        LoadDynamicAPIs();

        g_hFontText = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        
        g_hFontIcon = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

        g_hbrBg = CreateSolidBrush(COLOR_WIN10_BG);

        // 开启 Win10 DWM 暗色标题栏/深色模式支持
        if (g_DwmSetWindowAttribute) {
            BOOL darkMode = TRUE;
            g_DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= 3000 && id < 3000 + g_MonitorCount) {
            int idx = id - 3000;
            if (g_Monitors[idx].isSupported) {
                SetVCPFeature(g_Monitors[idx].hPhysicalMonitor, 0xD6, 0x04);
            }
            SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2, SMTO_ABORTIFHUNG, 100, NULL);
            ShowWindow(hwnd, SW_HIDE);
        }
        break;
    }

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else ShowPopupWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_MENU_AUTOSTART, L"开机自启动");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_MENU_EXIT, L"退出");

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);

            if (cmd == ID_MENU_AUTOSTART) ToggleAutoStart();
            else if (cmd == ID_MENU_EXIT) PostQuitMessage(0);
        }
        break;

    case WM_HSCROLL: {
        HWND hSlider = (HWND)lParam;
        for (int i = 0; i < g_MonitorCount; i++) {
            if (g_Monitors[i].hSlider == hSlider && g_Monitors[i].isSupported) {
                DWORD pos = (DWORD)SendMessage(hSlider, TBM_GETPOS, 0, 0);
                g_Monitors[i].curBrightness = pos;
                SetMonitorBrightness(g_Monitors[i].hPhysicalMonitor, pos);

                WCHAR valStr[16];
                swprintf_s(valStr, 16, L"%d", pos);
                SetWindowTextW(g_Monitors[i].hValLabel, valStr);
                break;
            }
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COLOR_WIN10_TEXT);
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)g_hbrBg;
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COLOR_WIN10_SUBTEXT);
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)g_hbrBg;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hbrBg);

        HPEN hPen = CreatePen(PS_SOLID, 1, COLOR_WIN10_SPLIT);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        for (int i = 1; i < g_MonitorCount; i++) {
            int y = i * 72;
            MoveToEx(hdc, 16, y, NULL);
            LineTo(hdc, rc.right - 16, y);
        }
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        return 1;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);
        break;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_Nid);
        if (g_hFontText) DeleteObject(g_hFontText);
        if (g_hFontIcon) DeleteObject(g_hFontIcon);
        if (g_hbrBg) DeleteObject(g_hbrBg);
        for (int i = 0; i < g_MonitorCount; i++) {
            if (g_Monitors[i].hPhysicalMonitor) DestroyPhysicalMonitor(g_Monitors[i].hPhysicalMonitor);
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    const WCHAR CLASS_NAME[] = L"DDCBrightnessWin10FlyoutClass";
    WNDCLASSW wc = {0};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hPopupWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"Brightness Control",
        WS_POPUP,
        0, 0, 360, 160,
        NULL, NULL, hInstance, NULL
    );

    g_Nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_Nid.hWnd = g_hPopupWnd;
    g_Nid.uID = IDI_TRAYICON;
    g_Nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_Nid.uCallbackMessage = WM_TRAYICON;
    g_Nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_Nid.szTip, 128, L"显示器 DDC/CI 亮度调节");
    Shell_NotifyIconW(NIM_ADD, &g_Nid);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}