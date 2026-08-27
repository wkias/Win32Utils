#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>

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

typedef struct {
    HANDLE hPhysicalMonitor;
    WCHAR description[128];
    DWORD minBrightness, curBrightness, maxBrightness;
    BOOL isSupported;
    HWND hSlider, hValLabel;
} MonitorInfo;

MonitorInfo g_Monitors[MAX_MONITORS];
int g_MonitorCount = 0;
NOTIFYICONDATAW g_Nid = {0};
HWND g_hPopupWnd = NULL;
HFONT g_hFont = NULL;

// --- 开机自启动 ---
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

// --- 显示器枚举 ---
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    DWORD count = 0;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) && count > 0) {
        LPPHYSICAL_MONITOR pPMs = (LPPHYSICAL_MONITOR)malloc(sizeof(PHYSICAL_MONITOR) * count);
        if (pPMs && GetPhysicalMonitorsFromHMONITOR(hMonitor, count, pPMs)) {
            for (DWORD i = 0; i < count && g_MonitorCount < MAX_MONITORS; i++) {
                g_Monitors[g_MonitorCount].hPhysicalMonitor = pPMs[i].hPhysicalMonitor;
                wcsncpy_s(g_Monitors[g_MonitorCount].description, 128, pPMs[i].szPhysicalMonitorDescription, _TRUNCATE);

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

// --- 构建原生 UI ---
void RebuildUIControls(HWND hwnd) {
    HWND hChild = GetWindow(hwnd, GW_CHILD);
    while (hChild) {
        HWND hNext = GetWindow(hChild, GW_HWNDNEXT);
        DestroyWindow(hChild);
        hChild = hNext;
    }

    RefreshMonitors();

    int y = 12;
    for (int i = 0; i < g_MonitorCount; i++) {
        // 显示器名称
        HWND hTitle = CreateWindowW(L"STATIC", g_Monitors[i].description,
            WS_CHILD | WS_VISIBLE | SS_LEFT, 15, y, 220, 20, hwnd, NULL, NULL, NULL);
        SendMessage(hTitle, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 原生息屏按钮
        HWND hOffBtn = CreateWindowW(L"BUTTON", L"息屏",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, y - 2, 50, 24, hwnd, (HMENU)(INT_PTR)(3000 + i), NULL, NULL);
        SendMessage(hOffBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 亮度滑块 (Win10/11 原生样式)
        HWND hSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 15, y + 26, 275, 24, hwnd, (HMENU)(INT_PTR)(2000 + i), NULL, NULL);
        SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELONG(g_Monitors[i].minBrightness, g_Monitors[i].maxBrightness));
        SendMessage(hSlider, TBM_SETPOS, TRUE, g_Monitors[i].curBrightness);
        EnableWindow(hSlider, g_Monitors[i].isSupported);
        g_Monitors[i].hSlider = hSlider;

        // 数值文本
        WCHAR valStr[16];
        swprintf_s(valStr, 16, g_Monitors[i].isSupported ? L"%d" : L"-1", g_Monitors[i].curBrightness);
        HWND hVal = CreateWindowW(L"STATIC", valStr,
            WS_CHILD | WS_VISIBLE | SS_RIGHT, 295, y + 28, 35, 20, hwnd, NULL, NULL, NULL);
        SendMessage(hVal, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        g_Monitors[i].hValLabel = hVal;

        y += 65;
    }
}

void ShowPopupWindow(HWND hwnd) {
    RebuildUIControls(hwnd);

    int windowWidth = 350;
    int windowHeight = g_MonitorCount * 65 + 15;
    if (windowHeight < 80) windowHeight = 80;

    POINT pt;
    GetCursorPos(&pt);
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    int x = pt.x - windowWidth / 2;
    int y = pt.y - windowHeight - 10;
    if (x < workArea.left) x = workArea.left + 5;
    if (x + windowWidth > workArea.right) x = workArea.right - windowWidth - 5;
    if (y < workArea.top) y = pt.y + 10;

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, windowWidth, windowHeight, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
}

// --- 窗口消息循环 ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        break;

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

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);
        break;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_Nid);
        if (g_hFont) DeleteObject(g_hFont);
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

    const WCHAR CLASS_NAME[] = L"DDCBrightnessNativePopup";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // 使用系统原生窗口背景
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hPopupWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"Brightness Control",
        WS_POPUP | WS_DLGFRAME,
        0, 0, 350, 150,
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