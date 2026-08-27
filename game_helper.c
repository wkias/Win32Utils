/* =============================================================
# 游戏辅助：监控进程切换电源 + 显示真实DX版本和帧率(ETW)
# 编译：zig cc -Oz game_helper.c -o game_helper.exe -lpowrprof -luser32 -lgdi32 -ladvapi32 -lole32 -lws2_32 "-Wl,/subsystem:windows" "-Wl,-s"
# 需要管理员权限才能捕获真实帧率，否则显示模拟帧率
# =============================================================*/

#include <windows.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <process.h>
#include <evntrace.h>
#include <evntcons.h>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

// ---------- 常量 ----------
#define CONFIG_FILE "config\\game_helper.txt"
#define LOG_FILE    "log\\game_helper.log"
#define MAX_TARGETS 128
#define MAX_NAME_LEN 256
#define CHECK_INTERVAL_MS 60000

// 电源方案 GUID
static const GUID GUID_BALANCED   = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
static const GUID GUID_POWER_SAVER = { 0xa1841308, 0x3541, 0x4fab, { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a } };

// DXGI 提供者 GUID
static const GUID DXGI_PROVIDER_GUID = { 0xCA11C036, 0x0102, 0x4A2D, { 0x9D, 0xAD, 0xFA, 0xCF, 0xE6, 0x1B, 0x6A, 0x11 } };

// ---------- 全局变量 ----------
static HWND g_hWndOSD = NULL;
static int  g_currentMode = -1;
static char g_dxVersion[16] = "DX: --";
static volatile int g_fps = 0;
static volatile int g_frameCounter = 0;
static DWORD g_targetPid = 0;
static HANDLE g_etwThreadHandle = NULL;
static volatile BOOL g_etwRunning = FALSE;
static TRACEHANDLE g_traceHandle = INVALID_PROCESSTRACE_HANDLE;

// 动态加载的 ETW 函数指针（使用标准指针类型）
typedef ULONG (WINAPI *pStartTraceW)(LPCWSTR, EVENT_TRACE_PROPERTIES*);
typedef ULONG (WINAPI *pEnableTraceEx2)(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, PENABLE_TRACE_PARAMETERS);
typedef TRACEHANDLE (WINAPI *pOpenTraceW)(EVENT_TRACE_LOGFILEW*);
typedef ULONG (WINAPI *pProcessTrace)(TRACEHANDLE*, ULONG, LPFILETIME, LPFILETIME);
typedef ULONG (WINAPI *pCloseTrace)(TRACEHANDLE);

static pStartTraceW      dyn_StartTraceW = NULL;
static pEnableTraceEx2   dyn_EnableTraceEx2 = NULL;
static pOpenTraceW       dyn_OpenTraceW = NULL;
static pProcessTrace     dyn_ProcessTrace = NULL;
static pCloseTrace       dyn_CloseTrace = NULL;

// ---------- 日志 ----------
void write_log(const char *format, ...) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file) {
        fprintf(log_file, "[%s] %s\n", time_str, buffer);
        fclose(log_file);
    }
}

void trim_newline(char *str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len-1] == '\r' || str[len-1] == '\n' || str[len-1] == ' '))
        str[--len] = '\0';
}

int load_config(char targets[MAX_TARGETS][MAX_NAME_LEN]) {
    FILE *file = fopen(CONFIG_FILE, "r");
    if (!file) return 0;
    int count = 0;
    char line[MAX_NAME_LEN];
    while (fgets(line, sizeof(line), file) && count < MAX_TARGETS) {
        trim_newline(line);
        if (strlen(line) > 0) {
            strncpy(targets[count], line, MAX_NAME_LEN-1);
            targets[count][MAX_NAME_LEN-1] = '\0';
            count++;
        }
    }
    fclose(file);
    return count;
}

DWORD find_process_pid(char targets[MAX_TARGETS][MAX_NAME_LEN], int target_count) {
    if (target_count <= 0) return 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(snapshot, &pe)) { CloseHandle(snapshot); return 0; }
    do {
        for (int i = 0; i < target_count; i++) {
            if (_stricmp(pe.szExeFile, targets[i]) == 0) {
                CloseHandle(snapshot);
                return pe.th32ProcessID;
            }
        }
    } while (Process32Next(snapshot, &pe));
    CloseHandle(snapshot);
    return 0;
}

void detect_dx_version(DWORD pid) {
    if (pid == 0) {
        strcpy(g_dxVersion, "DX: --");
        return;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        strcpy(g_dxVersion, "DX: ?");
        return;
    }
    MODULEENTRY32 me;
    me.dwSize = sizeof(MODULEENTRY32);
    int has_d3d11 = 0, has_d3d12 = 0;
    if (Module32First(snapshot, &me)) {
        do {
            if (_stricmp(me.szModule, "d3d11.dll") == 0) has_d3d11 = 1;
            if (_stricmp(me.szModule, "d3d12.dll") == 0) has_d3d12 = 1;
        } while (Module32Next(snapshot, &me));
    }
    CloseHandle(snapshot);
    if (has_d3d12) strcpy(g_dxVersion, "DX: 12");
    else if (has_d3d11) strcpy(g_dxVersion, "DX: 11");
    else strcpy(g_dxVersion, "DX: ?");
}

void set_power_mode(int is_balanced) {
    const GUID *guid = is_balanced ? &GUID_BALANCED : &GUID_POWER_SAVER;
    DWORD result = PowerSetActiveScheme(NULL, guid);
    if (result == ERROR_SUCCESS) {
        write_log("【电源】切换至 %s", is_balanced ? "平衡" : "节能");
    } else {
        write_log("【错误】切换电源失败，错误码: %lu", result);
    }
}

// ---------- OSD 窗口 ----------
LRESULT CALLBACK OSD_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            SetBkMode(hdc, TRANSPARENT);
            HFONT hFont = CreateFont(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, TEXT("Consolas"));
            SelectObject(hdc, hFont);
            SetTextColor(hdc, RGB(255,255,255));
            char text[64];
            snprintf(text, sizeof(text), "%s  FPS: %d", g_dxVersion, g_fps);
            RECT rect = {0, 0, 340, 60};
            DrawTextA(hdc, text, -1, &rect, DT_LEFT | DT_TOP | DT_SINGLELINE);
            DeleteObject(hFont);
            EndPaint(hWnd, &ps);
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

HWND CreateOSDWindow(HINSTANCE hInst) {
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = OSD_WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = TEXT("OSDWindowClass");
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        TEXT("OSDWindowClass"), NULL, WS_POPUP,
        10, 10, 360, 70,
        NULL, NULL, hInst, NULL);
    if (hWnd) {
        SetLayeredWindowAttributes(hWnd, RGB(0,0,0), 0, LWA_COLORKEY);
        ShowWindow(hWnd, SW_HIDE);
    }
    return hWnd;
}

// ---------- 动态加载 ETW ----------
int load_etw_functions(void) {
    HMODULE hLib = LoadLibraryW(L"advapi32.dll");
    if (!hLib) hLib = LoadLibraryW(L"sechost.dll");
    if (!hLib) {
        write_log("【ETW】无法加载动态库，帧率将模拟");
        return 0;
    }
    dyn_StartTraceW = (pStartTraceW)GetProcAddress(hLib, "StartTraceW");
    dyn_EnableTraceEx2 = (pEnableTraceEx2)GetProcAddress(hLib, "EnableTraceEx2");
    dyn_OpenTraceW = (pOpenTraceW)GetProcAddress(hLib, "OpenTraceW");
    dyn_ProcessTrace = (pProcessTrace)GetProcAddress(hLib, "ProcessTrace");
    dyn_CloseTrace = (pCloseTrace)GetProcAddress(hLib, "CloseTrace");
    if (!dyn_StartTraceW || !dyn_EnableTraceEx2 || !dyn_OpenTraceW ||
        !dyn_ProcessTrace || !dyn_CloseTrace) {
        write_log("【ETW】获取函数指针失败");
        FreeLibrary(hLib);
        return 0;
    }
    return 1;
}

// ---------- ETW 回调 ----------
static VOID WINAPI EventRecordCallback(PEVENT_RECORD pEvent) {
    if (pEvent->EventHeader.ProcessId == g_targetPid && g_targetPid != 0) {
        InterlockedIncrement((volatile LONG*)&g_frameCounter);
    }
}

// ETW 消费者线程
unsigned __stdcall EtwConsumerThread(void *param) {
    if (!dyn_StartTraceW || !dyn_EnableTraceEx2 || !dyn_OpenTraceW ||
        !dyn_ProcessTrace || !dyn_CloseTrace) {
        return 1;
    }

    WCHAR sessionName[] = L"PowerMonETW";
    ULONG bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(sessionName) + 256;
    EVENT_TRACE_PROPERTIES *pProp = (EVENT_TRACE_PROPERTIES *)malloc(bufferSize);
    if (!pProp) return 1;
    ZeroMemory(pProp, bufferSize);
    pProp->Wnode.BufferSize = bufferSize;
    pProp->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    pProp->Wnode.ClientContext = 1;
    pProp->Wnode.Guid = DXGI_PROVIDER_GUID;
    pProp->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    pProp->MaximumFileSize = 0;
    pProp->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    pProp->LogFileNameOffset = 0;

    ULONG status = dyn_StartTraceW(sessionName, pProp);
    if (status != ERROR_SUCCESS) {
        write_log("【ETW】StartTrace 失败 (%lu)，可能权限不足", status);
        free(pProp);
        return 1;
    }
    g_traceHandle = (TRACEHANDLE)pProp->Wnode.HistoricalContext;

    ENABLE_TRACE_PARAMETERS enableParams = {0};
    enableParams.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    status = dyn_EnableTraceEx2(g_traceHandle, &DXGI_PROVIDER_GUID,
                                EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                TRACE_LEVEL_INFORMATION,
                                0x2,
                                0, 0, &enableParams);
    if (status != ERROR_SUCCESS) {
        write_log("【ETW】EnableTraceEx2 失败 (%lu)", status);
        dyn_CloseTrace(g_traceHandle);
        free(pProp);
        return 1;
    }

    EVENT_TRACE_LOGFILEW logfile = {0};
    logfile.LoggerName = sessionName;
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = EventRecordCallback;
    logfile.Context = NULL;

    TRACEHANDLE hTrace = dyn_OpenTraceW(&logfile);
    if (hTrace == INVALID_PROCESSTRACE_HANDLE) {
        write_log("【ETW】OpenTrace 失败");
        dyn_EnableTraceEx2(g_traceHandle, &DXGI_PROVIDER_GUID, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, NULL);
        dyn_CloseTrace(g_traceHandle);
        free(pProp);
        return 1;
    }

    g_etwRunning = TRUE;
    write_log("【ETW】开始监听 DXGI Present 事件 (PID=%lu)", g_targetPid);
    status = dyn_ProcessTrace(&hTrace, 1, NULL, NULL);
    if (status != ERROR_SUCCESS) {
        write_log("【ETW】ProcessTrace 结束，状态 %lu", status);
    }

    dyn_EnableTraceEx2(g_traceHandle, &DXGI_PROVIDER_GUID, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, NULL);
    dyn_CloseTrace(g_traceHandle);
    dyn_CloseTrace(hTrace);
    free(pProp);
    g_etwRunning = FALSE;
    return 0;
}

void start_etw_for_pid(DWORD pid) {
    if (g_etwThreadHandle) {
        g_etwRunning = FALSE;
        if (g_traceHandle != INVALID_PROCESSTRACE_HANDLE) {
            dyn_CloseTrace(g_traceHandle);
        }
        WaitForSingleObject(g_etwThreadHandle, 3000);
        CloseHandle(g_etwThreadHandle);
        g_etwThreadHandle = NULL;
    }
    g_targetPid = pid;
    if (pid == 0) {
        g_fps = 0;
        g_frameCounter = 0;
        return;
    }
    g_etwThreadHandle = (HANDLE)_beginthreadex(NULL, 0, EtwConsumerThread, NULL, 0, NULL);
    if (!g_etwThreadHandle) {
        write_log("【错误】创建 ETW 线程失败");
    }
}

// ---------- 模拟帧率线程（回退） ----------
unsigned __stdcall SimulateFpsThread(void *param) {
    while (1) {
        Sleep(1000);
        if (g_targetPid != 0) {
            InterlockedIncrement((volatile LONG*)&g_frameCounter);
        }
    }
    return 0;
}

// ---------- 定时器：刷新 FPS ----------
void CALLBACK FpsTimerProc(HWND hWnd, UINT msg, UINT_PTR id, DWORD time) {
    g_fps = g_frameCounter;
    g_frameCounter = 0;
    if (g_hWndOSD && g_targetPid != 0) {
        InvalidateRect(g_hWndOSD, NULL, FALSE);
    }
}

// ---------- 定时器：检查进程 ----------
void CALLBACK CheckProcTimer(HWND hWnd, UINT msg, UINT_PTR id, DWORD time) {
    char targets[MAX_TARGETS][MAX_NAME_LEN];
    int count = load_config(targets);
    DWORD pid = find_process_pid(targets, count);
    int running = (pid != 0);

    if (running != g_currentMode) {
        g_currentMode = running;
        set_power_mode(running);
    }

    if (running && pid != g_targetPid) {
        detect_dx_version(pid);
        start_etw_for_pid(pid);
    } else if (!running && g_targetPid != 0) {
        start_etw_for_pid(0);
        strcpy(g_dxVersion, "DX: --");
    }

    if (g_hWndOSD) {
        ShowWindow(g_hWndOSD, running ? SW_SHOW : SW_HIDE);
        if (running) InvalidateRect(g_hWndOSD, NULL, FALSE);
    }
}

// ---------- 入口 ----------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    write_log("游戏辅助程序启动");

    BOOL isAdmin = FALSE;
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev;
        DWORD size;
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &size)) {
            isAdmin = elev.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    if (!isAdmin) {
        write_log("【警告】未以管理员身份运行，将使用模拟帧率");
    }

    int etw_loaded = load_etw_functions();

    g_hWndOSD = CreateOSDWindow(hInstance);
    if (!g_hWndOSD) {
        write_log("【错误】创建 OSD 窗口失败");
        return 1;
    }

    if (!isAdmin || !etw_loaded) {
        HANDLE hSim = (HANDLE)_beginthreadex(NULL, 0, SimulateFpsThread, NULL, 0, NULL);
        if (hSim) {
            write_log("【信息】已启用模拟帧率（非真实）");
            CloseHandle(hSim);
        } else {
            write_log("【错误】创建模拟线程失败");
        }
    }

    SetTimer(g_hWndOSD, 1, CHECK_INTERVAL_MS, CheckProcTimer);
    SetTimer(g_hWndOSD, 2, 1000, FpsTimerProc);

    CheckProcTimer(g_hWndOSD, 0, 0, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(g_hWndOSD, 1);
    KillTimer(g_hWndOSD, 2);
    start_etw_for_pid(0);
    DestroyWindow(g_hWndOSD);
    return 0;
}