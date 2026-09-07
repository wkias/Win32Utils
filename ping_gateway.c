/* 定时ping网关 后台静默
zig cc -Oz -s ping_gateway.c -o ping_gateway.exe -liphlpapi -lws2_32 -luser32 '-Wl,--subsystem,windows'
tcc ping_gateway.c -o ping_gateway.exe -liphlpapi -lws2_32 -luser32 -mwindows
*/

#include <winsock2.h>
#include <windows.h>
// -------------------------------------------------------------------
// 补充 TCC 缺失的 IP Helper & ICMP 结构体与函数声明
// -------------------------------------------------------------------
#ifndef IP_SUCCESS
#define IP_SUCCESS 0
#endif

typedef struct {
    char String[16];
} IP_ADDRESS_STRING, *PIP_ADDRESS_STRING, IP_MASK_STRING, *PIP_MASK_STRING;

typedef struct _IP_ADDR_STRING {
    struct _IP_ADDR_STRING* Next;
    IP_ADDRESS_STRING IpAddress;
    IP_MASK_STRING IpMask;
    DWORD Context;
} IP_ADDR_STRING, *PIP_ADDR_STRING;

typedef struct _IP_ADAPTER_INFO {
    struct _IP_ADAPTER_INFO* Next;
    DWORD ComboIndex;
    char AdapterName[260];
    char Description[132];
    UINT AddressLength;
    BYTE Address[8];
    DWORD Index;
    UINT Type;
    UINT DhcpEnabled;
    PIP_ADDR_STRING CurrentIpAddress;
    IP_ADDR_STRING IpAddressList;
    IP_ADDR_STRING GatewayList;
    IP_ADDR_STRING DhcpServer;
    BOOL HaveWins;
    IP_ADDR_STRING PrimaryWinsServer;
    IP_ADDR_STRING SecondaryWinsServer;
    ULONG LeaseObtained;
    ULONG LeaseExpires;
} IP_ADAPTER_INFO, *PIP_ADAPTER_INFO;

typedef struct {
    UCHAR Status;
    UCHAR Tos;
    UCHAR Flags;
    UCHAR OptionsSize;
    PUCHAR OptionsData;
} IP_OPTION_INFORMATION, *PIP_OPTION_INFORMATION;

typedef struct {
    ULONG Address;
    ULONG Status;
    ULONG RoundTripTime;
    USHORT DataSize;
    USHORT Reserved;
    PVOID Data;
    IP_OPTION_INFORMATION Options;
} ICMP_ECHO_REPLY, *PICMP_ECHO_REPLY;

// 显式声明来自 iphlpapi.dll 的导出 API
DWORD WINAPI GetAdaptersInfo(PIP_ADAPTER_INFO pAdapterInfo, PULONG pOutBufLen);
HANDLE WINAPI IcmpCreateFile(VOID);
BOOL WINAPI IcmpCloseHandle(HANDLE IcmpHandle);
DWORD WINAPI IcmpSendEcho(
    HANDLE IcmpHandle,
    ULONG DestinationAddress,
    LPVOID RequestData,
    WORD RequestSize,
    PIP_OPTION_INFORMATION RequestOptions,
    LPVOID ReplyBuffer,
    DWORD ReplySize,
    DWORD Timeout
);
// -------------------------------------------------------------------

void write_log(const char *msg) {
    CreateDirectoryA("log", NULL);
    HANDLE hFile = CreateFileA("log\\ping.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, msg, lstrlenA(msg), &written, NULL);
        CloseHandle(hFile);
    }
}

int get_default_gateway(char *out_ip, size_t size) {
    ULONG bufLen = sizeof(IP_ADAPTER_INFO);
    PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bufLen);

    if (pAdapterInfo && GetAdaptersInfo(pAdapterInfo, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bufLen);
    }

    if (pAdapterInfo && GetAdaptersInfo(pAdapterInfo, &bufLen) == NO_ERROR) {
        for (PIP_ADAPTER_INFO pAdapter = pAdapterInfo; pAdapter; pAdapter = pAdapter->Next) {
            if (pAdapter->GatewayList.IpAddress.String[0] != '\0' &&
                lstrcmpA(pAdapter->GatewayList.IpAddress.String, "0.0.0.0") != 0) {
                lstrcpynA(out_ip, pAdapter->GatewayList.IpAddress.String, (int)size);
                HeapFree(GetProcessHeap(), 0, pAdapterInfo);
                return 0;
            }
        }
    }

    if (pAdapterInfo) HeapFree(GetProcessHeap(), 0, pAdapterInfo);
    return -1;
}

int ping_ip(const char *ip_str) {
    HANDLE hIcmpFile = IcmpCreateFile();
    if (hIcmpFile == INVALID_HANDLE_VALUE) return -1;

    unsigned long ipaddr = inet_addr(ip_str);
    char sendData[] = "PingPayloadData";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    LPVOID replyBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, replySize);
    if (!replyBuffer) {
        IcmpCloseHandle(hIcmpFile);
        return -1;
    }

    int rtt = -1;
    DWORD dwRetVal = IcmpSendEcho(
        hIcmpFile,
        ipaddr,
        sendData,
        sizeof(sendData),
        NULL,
        replyBuffer,
        replySize,
        2000
    );

    if (dwRetVal != 0) {
        PICMP_ECHO_REPLY pEchoReply = (PICMP_ECHO_REPLY)replyBuffer;
        if (pEchoReply->Status == IP_SUCCESS) {
            rtt = (int)pEchoReply->RoundTripTime;
        }
    }

    HeapFree(GetProcessHeap(), 0, replyBuffer);
    IcmpCloseHandle(hIcmpFile);
    return rtt;
}

// 强制使用 Windows GUI 入口点
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 强制立即剥离任何潜在挂载的控制台窗口
    FreeConsole();

    char gateway_ip[64] = {0};

    if (lpCmdLine && lstrlenA(lpCmdLine) > 0) {
        lstrcpynA(gateway_ip, lpCmdLine, sizeof(gateway_ip));
    } else {
        if (get_default_gateway(gateway_ip, sizeof(gateway_ip)) != 0) {
            write_log("[!] 未找到可用网关，退出运行\n");
            return 1;
        }
    }

    char log_buf[256];
    wsprintfA(log_buf, "[*] 静默 Ping 监控启动，目标网关: %s\n", gateway_ip);
    write_log(log_buf);

    while (1) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        int rtt = ping_ip(gateway_ip);

        if (rtt >= 0) {
            wsprintfA(log_buf, "[%04d-%02d-%02d %02d:%02d:%02d] Ping %s - 成功 | 延迟: %d ms\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     gateway_ip, rtt);
        } else {
            wsprintfA(log_buf, "[%04d-%02d-%02d %02d:%02d:%02d] Ping %s - 超时或失败\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     gateway_ip);
        }

        write_log(log_buf);
        Sleep(60000);
    }

    return 0;
}