// tcc start.c -o start.exe -lkernel32 -luser32 -lgdi32 -ladvapi32 -lcomdlg32

#define UNICODE
#define _UNICODE
#include <windows.h>

#ifndef OFN_HIDEREADONLY
#define OFN_HIDEREADONLY     0x00000004
#define OFN_PATHMUSTEXIST    0x00000800
#define OFN_FILEMUSTEXIST    0x00001000
#define OFN_EXPLORER         0x00080000
#endif

typedef UINT_PTR (WINAPI *LPOFNHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct tagOFNW {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCWSTR lpstrFilter;
    LPWSTR lpstrCustomFilter; DWORD nMaxCustFilter, nFilterIndex; LPWSTR lpstrFile;
    DWORD nMaxFile;
    LPWSTR lpstrFileTitle; // 【修正点】由 DWORD 改为 LPWSTR
    DWORD nMaxFileTitle; LPCWSTR lpstrInitialDir, lpstrTitle;
    DWORD Flags; WORD nFileOffset, nFileExtension; LPCWSTR lpstrDefExt; LPARAM lCustData;
    LPOFNHOOKPROC lpfnHook; LPCWSTR lpTemplateName; void *pvReserved; DWORD dwReserved, FlagsEx;
} OPENFILENAMEW;

BOOL WINAPI GetOpenFileNameW(OPENFILENAMEW*);
BOOL WINAPI SetProcessDPIAware(VOID);

// -------------------------------------------------------------------
#define ID_EDIT_PATH  101
#define ID_BTN_BROWSE 102
#define ID_BTN_ADD    103
#define ID_BTN_DEL    104
#define ID_EDIT_ARGS  105
#define REG_RUN_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static HWND g_hEditPath, g_hEditArgs;
static HFONT g_hFont;

// 静态 UI 控件表定义 (用于批量创建)
typedef struct { LPCWSTR cls; LPCWSTR txt; DWORD exStyle; DWORD style; int x, y, w, h; HMENU id; } CTRL_DEF;
static const CTRL_DEF g_Ctrls[] = {
    { L"STATIC", L"程序路径:", 0, 0, 20, 22, 75, 24, NULL },
    { L"EDIT",   L"", WS_EX_CLIENTEDGE, ES_AUTOHSCROLL, 95, 18, 280, 28, (HMENU)ID_EDIT_PATH },
    { L"BUTTON", L"浏览...", 0, 0, 385, 17, 90, 30, (HMENU)ID_BTN_BROWSE },
    { L"STATIC", L"启动参数:", 0, 0, 20, 62, 75, 24, NULL },
    { L"EDIT",   L"", WS_EX_CLIENTEDGE, ES_AUTOHSCROLL, 95, 58, 380, 28, (HMENU)ID_EDIT_ARGS },
    { L"BUTTON", L"添加开机自启", 0, 0, 95, 105, 135, 38, (HMENU)ID_BTN_ADD },
    { L"BUTTON", L"删除开机自启", 0, 0, 245, 105, 135, 38, (HMENU)ID_BTN_DEL }
};

// 提取路径中的文件名
const WCHAR* GetFileNameFromPath(const WCHAR* path) {
    const WCHAR* last = path;
    for (; *path; path++) if (*path == L'\\' || *path == L'/') last = path + 1;
    return last;
}

// 通用校验与获取输入
static BOOL GetInputs(HWND hWnd, WCHAR *path, WCHAR *args, const WCHAR **name) {
    GetWindowTextW(g_hEditPath, path, MAX_PATH);
    GetWindowTextW(g_hEditArgs, args, MAX_PATH);
    if (!*path) {
        MessageBoxW(hWnd, L"请先选择需要配置的可执行文件！", L"提示", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    *name = GetFileNameFromPath(path);
    return TRUE;
}

void OnBrowse(HWND hWnd) {
    WCHAR szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn = { sizeof(ofn), hWnd, 0, L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0",
                          NULL, 0, 0, szFile, MAX_PATH, NULL, 0, NULL, NULL,
                          OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY };
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_hEditPath, szFile);
}

void OnAddRun(HWND hWnd) {
    WCHAR szPath[MAX_PATH], szArgs[MAX_PATH], szCmd[MAX_PATH * 2 + 10];
    const WCHAR *szName;
    if (!GetInputs(hWnd, szPath, szArgs, &szName)) return;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        LONG exists = RegQueryValueExW(hKey, szName, NULL, NULL, NULL, NULL);
        RegCloseKey(hKey);
        if (exists == ERROR_SUCCESS) {
            MessageBoxW(hWnd, L"该程序已存在于开机启动项中，无需重复添加！", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    if (*szArgs) wsprintfW(szCmd, L"\"%s\" %s", szPath, szArgs);
    else wsprintfW(szCmd, L"\"%s\"", szPath);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD size = (lstrlenW(szCmd) + 1) * sizeof(WCHAR);
        LONG res = RegSetValueExW(hKey, szName, 0, REG_SZ, (const BYTE*)szCmd, size);
        RegCloseKey(hKey);
        MessageBoxW(hWnd, res == ERROR_SUCCESS ? L"已成功添加到开机自启！" : L"写入注册表失败！", L"提示",
                    res == ERROR_SUCCESS ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
    }
}

void OnDelRun(HWND hWnd) {
    WCHAR szPath[MAX_PATH], szArgs[MAX_PATH];
    const WCHAR *szName;
    if (!GetInputs(hWnd, szPath, szArgs, &szName)) return;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        LONG res = RegDeleteValueW(hKey, szName);
        RegCloseKey(hKey);
        MessageBoxW(hWnd, res == ERROR_SUCCESS ? L"已成功从开机自启中删除！" : L"启动项中未找到该程序！", L"提示",
                    res == ERROR_SUCCESS ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CREATE) {
        g_hFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        for (int i = 0; i < sizeof(g_Ctrls)/sizeof(g_Ctrls[0]); i++) {
            const CTRL_DEF *c = &g_Ctrls[i];
            HWND h = CreateWindowExW(c->exStyle, c->cls, c->txt, WS_CHILD | WS_VISIBLE | c->style,
                                     c->x, c->y, c->w, c->h, hWnd, c->id, NULL, NULL);
            SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            if (c->id == (HMENU)ID_EDIT_PATH) g_hEditPath = h;
            if (c->id == (HMENU)ID_EDIT_ARGS) g_hEditArgs = h;
        }
        return 0;
    }
    if (uMsg == WM_COMMAND) {
        WORD id = LOWORD(wParam);
        if (id == ID_BTN_BROWSE) OnBrowse(hWnd);
        else if (id == ID_BTN_ADD) OnAddRun(hWnd);
        else if (id == ID_BTN_DEL) OnDelRun(hWnd);
        return 0;
    }
    if (uMsg == WM_DESTROY) {
        if (g_hFont) DeleteObject(g_hFont);
        ExitProcess(0);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void EntryPoint(void) {
    // 动态优先，静态兜底的高分屏感知
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *pfnDPI)(HANDLE);
    pfnDPI fnDPI = hUser32 ? (pfnDPI)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext") : NULL;
    if (fnDPI) fnDPI((HANDLE)-4); else SetProcessDPIAware();

    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, hInst,
                       NULL, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), NULL, L"StartupMgrC", NULL };
    if (!RegisterClassExW(&wc)) ExitProcess(0);

    HWND hWnd = CreateWindowExW(0, L"StartupMgrC", L"开机启动项设置",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 510, 210, NULL, NULL, hInst, NULL);
    if (!hWnd) ExitProcess(0);

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess((UINT)msg.wParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    EntryPoint();
    return 0;
}