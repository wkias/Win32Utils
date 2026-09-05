/* 定时ping网关 后台静默
zig cc -Oz -s ping_gateway.c -o ping_gateway.exe -liphlpapi -lws2_32 '-Wl,--subsystem,windows'
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

void write_log(const char *msg) {
    FILE *f = fopen("log\\ping.log", "a");
    if (f) {
        fputs(msg, f);
        fclose(f);
    }
}

int get_default_gateway(char *out_ip, size_t size) {
    ULONG bufLen = sizeof(IP_ADAPTER_INFO);
    PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO *)malloc(bufLen);

    if (GetAdaptersInfo(pAdapterInfo, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *)malloc(bufLen);
    }

    if (GetAdaptersInfo(pAdapterInfo, &bufLen) == NO_ERROR) {
        for (PIP_ADAPTER_INFO pAdapter = pAdapterInfo; pAdapter; pAdapter = pAdapter->Next) {
            if (pAdapter->GatewayList.IpAddress.String[0] != '\0' &&
                strcmp(pAdapter->GatewayList.IpAddress.String, "0.0.0.0") != 0) {
                strncpy(out_ip, pAdapter->GatewayList.IpAddress.String, size - 1);
                free(pAdapterInfo);
                return 0;
            }
        }
    }

    if (pAdapterInfo) free(pAdapterInfo);
    return -1;
}

int ping_ip(const char *ip_str) {
    HANDLE hIcmpFile = IcmpCreateFile();
    if (hIcmpFile == INVALID_HANDLE_VALUE) return -1;

    unsigned long ipaddr = inet_addr(ip_str);
    char sendData[] = "PingPayloadData";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    LPVOID replyBuffer = malloc(replySize);

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

    free(replyBuffer);
    IcmpCloseHandle(hIcmpFile);
    return rtt;
}

// 强制使用 Windows GUI 入口点
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 强制立即剥离任何潜在挂载的控制台窗口
    FreeConsole();

    char gateway_ip[64] = {0};

    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        strncpy(gateway_ip, lpCmdLine, sizeof(gateway_ip) - 1);
    } else {
        if (get_default_gateway(gateway_ip, sizeof(gateway_ip)) != 0) {
            write_log("[!] 未找到可用网关，退出运行\n");
            return 1;
        }
    }

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf), "[*] 静默 Ping 监控启动，目标网关: %s\n", gateway_ip);
    write_log(log_buf);

    while (1) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

        int rtt = ping_ip(gateway_ip);

        if (rtt >= 0) {
            snprintf(log_buf, sizeof(log_buf), "[%s] Ping %s - 成功 | 延迟: %d ms\n", time_str, gateway_ip, rtt);
        } else {
            snprintf(log_buf, sizeof(log_buf), "[%s] Ping %s - 超时或失败\n", time_str, gateway_ip);
        }

        write_log(log_buf);
        Sleep(60000);
    }

    return 0;
}