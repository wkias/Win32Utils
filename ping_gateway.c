#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

// 自动读取系统的默认网关 IP
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

// 执行 ICMP Echo 请求
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
        2000 // 2秒超时
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

int main(int argc, char *argv[]) {
    // 兼容 936 代码页：设置当前进程控制台输出编码为 UTF-8 (65001)
    SetConsoleOutputCP(65001);

    char gateway_ip[64] = {0};

    // 优先使用传入参数作为 IP，无参数则自动获取默认网关
    if (argc > 1) {
        strncpy(gateway_ip, argv[1], sizeof(gateway_ip) - 1);
        printf("[*] 目标 IP (指定): %s\n", gateway_ip);
    } else {
        printf("[*] 正在检测默认网关...\n");
        if (get_default_gateway(gateway_ip, sizeof(gateway_ip)) != 0) {
            fprintf(stderr, "[!] 未找到可用网关，请手动传入 IP 参数（例如: ping_gateway.exe 192.168.1.1）\n");
            return 1;
        }
        printf("[*] 自动检测到网关 IP: %s\n", gateway_ip);
    }

    printf("[*] 监控已启动，每 60 秒 Ping 一次（按 Ctrl+C 退出）\n\n");

    while (1) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

        int rtt = ping_ip(gateway_ip);

        if (rtt >= 0) {
            printf("[%s] Ping %s - 成功 | 延迟: %d ms\n", time_str, gateway_ip, rtt);
        } else {
            printf("[%s] Ping %s - 超时或失败\n", time_str, gateway_ip);
        }

        fflush(stdout);
        Sleep(60000); // 暂停 60 秒
    }

    return 0;
}