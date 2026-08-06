//zig cc -target x86_64-windows-gnu -Oz -s -nodefaultlibs start.c -o start.exe -e EntryPoint -lkernel32 -luser32 -lgdi32 -ladvapi32 -lcomdlg32 "-Wl,/subsystem:windows"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>

#define ID_EDIT_PATH  101
#define ID_BTN_BROWSE 102
#define ID_BTN_ADD    103
#define ID_BTN_DEL    104
#define ID_EDIT_ARGS  105

#define REG_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static HWND g_hEditPath = NULL;
static HWND g_hEditArgs = NULL;
static HFONT g_hFont = NULL;

// 防 -nostdlib 找不到 memset
void* memset(void* dest, int c, size_t count) {
    char* bytes = (char*)dest;
    while (count--) *bytes++ = (char)c;
    return dest;
}

// 开启高分屏 DPI 感知
void EnableHighDPI(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *pfnSetProcessDpiAwarenessContext)(HANDLE);
        pfnSetProcessDpiAwarenessContext setDpiContext = 
            (pfnSetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiContext) {
            setDpiContext((HANDLE)-4);
            return;
        }
    }
    SetProcessDPIAware();
}

// 提取可执行文件名
const WCHAR* GetFileNameFromPath(const WCHAR* path) {
    const WCHAR* last = path;
    for (const WCHAR* p = path; *p; p++) {
        if (*p == L'\\' || *p == L'/') last = p + 1;
    }
    return last;
}

// 浏览按钮动作
void OnBrowse(HWND hWnd) {
    WCHAR szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(g_hEditPath, szFile);
    }
}

// 添加开机自启 (支持启动参数与去重)
void OnAddRun(HWND hWnd) {
    WCHAR szPath[MAX_PATH] = {0};
    WCHAR szArgs[MAX_PATH] = {0};
    GetWindowTextW(g_hEditPath, szPath, MAX_PATH);
    GetWindowTextW(g_hEditArgs, szArgs, MAX_PATH);

    if (lstrlenW(szPath) == 0) {
        MessageBoxW(hWnd, L"请先选择需要配置的可执行文件！", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    const WCHAR* szName = GetFileNameFromPath(szPath);
    HKEY hKey;

    // 1. 检查注册表是否已存在该项
    LONG lRes = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey);
    if (lRes == ERROR_SUCCESS) {
        lRes = RegQueryValueExW(hKey, szName, NULL, NULL, NULL, NULL);
        RegCloseKey(hKey);
        if (lRes == ERROR_SUCCESS) {
            MessageBoxW(hWnd, L"该程序已存在于开机启动项中，无需重复添加！", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    // 2. 组装命令路径（若填了参数，拼接为: "C:\path\app.exe" --arg1 -arg2）
    WCHAR szCmd[MAX_PATH * 2 + 10] = {0};
    if (lstrlenW(szArgs) > 0) {
        wsprintfW(szCmd, L"\"%s\" %s", szPath, szArgs);
    } else {
        wsprintfW(szCmd, L"\"%s\"", szPath);
    }

    // 3. 写入注册表
    lRes = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey);
    if (lRes == ERROR_SUCCESS) {
        DWORD dwSize = (DWORD)((lstrlenW(szCmd) + 1) * sizeof(WCHAR));
        lRes = RegSetValueExW(hKey, szName, 0, REG_SZ, (const BYTE*)szCmd, dwSize);
        RegCloseKey(hKey);

        if (lRes == ERROR_SUCCESS) {
            MessageBoxW(hWnd, L"已成功添加到开机自启！", L"提示", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hWnd, L"写入注册表失败！", L"提示", MB_OK | MB_ICONERROR);
        }
    }
}

// 删除开机自启
void OnDelRun(HWND hWnd) {
    WCHAR szPath[MAX_PATH] = {0};
    GetWindowTextW(g_hEditPath, szPath, MAX_PATH);

    if (lstrlenW(szPath) == 0) {
        MessageBoxW(hWnd, L"请先选择需要配置的可执行文件！", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    const WCHAR* szName = GetFileNameFromPath(szPath);
    HKEY hKey;

    LONG lRes = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey);
    if (lRes == ERROR_SUCCESS) {
        lRes = RegDeleteValueW(hKey, szName);
        RegCloseKey(hKey);

        if (lRes == ERROR_SUCCESS) {
            MessageBoxW(hWnd, L"已成功从开机自启中删除！", L"提示", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hWnd, L"开机启动项中未找到该程序，无需删除！", L"提示", MB_OK | MB_ICONWARNING);
        }
    }
}

// 窗口回调
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        g_hFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        // 1. 程序路径 Label, Edit, Browse
        HWND hLabelPath = CreateWindowExW(0, L"STATIC", L"程序路径:", WS_CHILD | WS_VISIBLE,
                                        20, 22, 75, 24, hWnd, NULL, NULL, NULL);
        SendMessageW(hLabelPath, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", 
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      95, 18, 280, 28, hWnd, (HMENU)ID_EDIT_PATH, NULL, NULL);
        SendMessageW(g_hEditPath, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE,
                                          385, 17, 90, 30, hWnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);
        SendMessageW(hBtnBrowse, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 2. 启动参数 Label, Edit
        HWND hLabelArgs = CreateWindowExW(0, L"STATIC", L"启动参数:", WS_CHILD | WS_VISIBLE,
                                        20, 62, 75, 24, hWnd, NULL, NULL, NULL);
        SendMessageW(hLabelArgs, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hEditArgs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", 
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      95, 58, 380, 28, hWnd, (HMENU)ID_EDIT_ARGS, NULL, NULL);
        SendMessageW(g_hEditArgs, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // 3. 动作按钮
        HWND hBtnAdd = CreateWindowExW(0, L"BUTTON", L"添加开机自启", WS_CHILD | WS_VISIBLE,
                                       95, 105, 135, 38, hWnd, (HMENU)ID_BTN_ADD, NULL, NULL);
        SendMessageW(hBtnAdd, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hBtnDel = CreateWindowExW(0, L"BUTTON", L"删除开机自启", WS_CHILD | WS_VISIBLE,
                                       245, 105, 135, 38, hWnd, (HMENU)ID_BTN_DEL, NULL, NULL);
        SendMessageW(hBtnDel, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        return 0;
    }

    case WM_COMMAND: {
        WORD wmId = LOWORD(wParam);
        if (wmId == ID_BTN_BROWSE) OnBrowse(hWnd);
        else if (wmId == ID_BTN_ADD) OnAddRun(hWnd);
        else if (wmId == ID_BTN_DEL) OnDelRun(hWnd);
        return 0;
    }

    case WM_DESTROY:
        if (g_hFont) DeleteObject(g_hFont);
        ExitProcess(0);
        return 0;

    default:
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
}

// 无 CRT 纯原生入口
void EntryPoint(void) {
    EnableHighDPI();

    HINSTANCE hInstance = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"StartupMgrClassC";

    if (!RegisterClassExW(&wc)) ExitProcess(0);

    HWND hWnd = CreateWindowExW(0, L"StartupMgrClassC", L"开机启动项设置",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 510, 210,
                                NULL, NULL, hInstance, NULL);

    if (!hWnd) ExitProcess(0);

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ExitProcess((UINT)msg.wParam);
}

// 兜底 WinMain
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    EntryPoint();
    return 0;
}