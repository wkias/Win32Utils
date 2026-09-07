/* =============================================================
# 监控进程修改电源计划 (TCC 完全零依赖适配版)
# 编译方式: tcc power_monitor.c -o power_monitor.exe
# =============================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <windows.h>

#define CONFIG_FILE "D:\\zig\\config\\power_monitor.txt"
#define LOG_FILE    "D:\\zig\\log\\power_monitor.log"
#define MAX_TARGETS 128
#define MAX_NAME_LEN 256
#define CHECK_INTERVAL_MS 60000

// --- 手动补全 tlhelp32.h 定义 ---
#define TH32CS_SNAPPROCESS 0x00000002

typedef struct tagPROCESSENTRY32 {
    DWORD     dwSize;
    DWORD     cntUsage;
    DWORD     th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD     th32ModuleID;
    DWORD     cntThreads;
    DWORD     th32ParentProcessID;
    LONG      pcPriClassBase;
    DWORD     dwFlags;
    CHAR      szExeFile[MAX_PATH];
} PROCESSENTRY32;

typedef HANDLE (WINAPI *PFN_CreateToolhelp32Snapshot)(DWORD dwFlags, DWORD th32ProcessID);
typedef BOOL   (WINAPI *PFN_Process32First)(HANDLE hSnapshot, PROCESSENTRY32 *lppe);
typedef BOOL   (WINAPI *PFN_Process32Next)(HANDLE hSnapshot, PROCESSENTRY32 *lppe);

// --- powrprof 函数动态调用定义 ---
typedef DWORD  (WINAPI *PFN_PowerSetActiveScheme)(HKEY UserRootPowerKey, const GUID *SchemeGuid);

// Windows 内置电源方案 GUID
static const GUID GUID_BALANCED = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
static const GUID GUID_POWER_SAVER = { 0xa1841308, 0x3541, 0x4fab, { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xF2, 0x0b, 0x4a } };

// 带时间戳的日志输出到文件
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
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n' || str[len - 1] == ' ')) {
        str[--len] = '\0';
    }
}

int load_config(char targets[MAX_TARGETS][MAX_NAME_LEN]) {
    FILE *file = fopen(CONFIG_FILE, "r");
    if (!file) return 0;

    int count = 0;
    char line[MAX_NAME_LEN];
    while (fgets(line, sizeof(line), file) && count < MAX_TARGETS) {
        trim_newline(line);
        if (strlen(line) > 0) {
            strncpy(targets[count], line, MAX_NAME_LEN - 1);
            targets[count][MAX_NAME_LEN - 1] = '\0';
            count++;
        }
    }
    fclose(file);
    return count;
}

int is_any_target_running(char targets[MAX_TARGETS][MAX_NAME_LEN], int target_count) {
    if (target_count <= 0) return 0;

    // 动态获取 kernel32.dll 中的进程快照 API
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) return 0;

    PFN_CreateToolhelp32Snapshot pfnCreateSnapshot = (PFN_CreateToolhelp32Snapshot)GetProcAddress(hKernel32, "CreateToolhelp32Snapshot");
    PFN_Process32First pfnProcess32First = (PFN_Process32First)GetProcAddress(hKernel32, "Process32First");
    PFN_Process32Next pfnProcess32Next = (PFN_Process32Next)GetProcAddress(hKernel32, "Process32Next");

    if (!pfnCreateSnapshot || !pfnProcess32First || !pfnProcess32Next) {
        return 0;
    }

    HANDLE snapshot = pfnCreateSnapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 process_entry;
    process_entry.dwSize = sizeof(PROCESSENTRY32);

    if (!pfnProcess32First(snapshot, &process_entry)) {
        CloseHandle(snapshot);
        return 0;
    }

    int found = 0;
    do {
        for (int i = 0; i < target_count; i++) {
            if (_stricmp(process_entry.szExeFile, targets[i]) == 0) {
                found = 1;
                break;
            }
        }
    } while (!found && pfnProcess32Next(snapshot, &process_entry));

    CloseHandle(snapshot);
    return found;
}

void set_power_mode(int is_balanced) {
    const GUID *target_guid = is_balanced ? &GUID_BALANCED : &GUID_POWER_SAVER;
    
    HMODULE hPowrProf = LoadLibraryA("powrprof.dll");
    if (!hPowrProf) {
        write_log("【错误】无法加载 powrprof.dll");
        return;
    }

    PFN_PowerSetActiveScheme pfnPowerSetActiveScheme = 
        (PFN_PowerSetActiveScheme)GetProcAddress(hPowrProf, "PowerSetActiveScheme");

    if (pfnPowerSetActiveScheme) {
        DWORD result = pfnPowerSetActiveScheme(NULL, target_guid);
        if (result == ERROR_SUCCESS) {
            write_log("【状态切换】系统电源模式已设置为: %s", 
                      is_balanced ? "平衡模式 (Balanced)" : "节能模式 (Power Saver)");
        } else {
            write_log("【错误】电源模式切换失败，错误码: %lu", result);
        }
    } else {
        write_log("【错误】无法在 powrprof.dll 中找到 PowerSetActiveScheme 函数");
    }

    FreeLibrary(hPowrProf);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    char targets[MAX_TARGETS][MAX_NAME_LEN];
    int current_mode = -1; // -1: 初始未知, 1: 平衡, 0: 节能

    write_log("电源监控后台程序已启动，且已隐藏窗口。");

    while (1) {
        int target_count = load_config(targets);
        int running = is_any_target_running(targets, target_count);

        if (running != current_mode) {
            current_mode = running;
            set_power_mode(current_mode);
        }

        Sleep(CHECK_INTERVAL_MS);
    }

    return 0;
}