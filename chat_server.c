/*
zig cc chat_server.c -o chat_server.exe -lws2_32 -lcrypt32 -luser32 -ladvapi32 "-Wl,--subsystem,windows" -Oz -s
tcc chat_server.c -o chat_server.exe -lws2_32 -lcrypt32 -luser32 -ladvapi32 -mwindows
*/

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windows.h>

// -------------------------------------------------------------------
// 补充 TCC 缺失的 wincrypt.h 声明 (支持 SHA1、Base64 与随机数生成)
// -------------------------------------------------------------------
typedef ULONG_PTR HCRYPTPROV;
typedef ULONG_PTR HCRYPTHASH;
typedef ULONG_PTR HCRYPTKEY;
typedef unsigned int ALG_ID;

#define PROV_RSA_FULL           1
#define CRYPT_VERIFYCONTEXT     0xF0000000

#define ALG_CLASS_HASH          (4 << 13)
#define ALG_TYPE_ANY            (0)
#define ALG_SID_SHA1            4
#define CALG_SHA1               (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA1)

#define HP_HASHVAL              0x0002
#define HP_HASHSIZE             0x0004
#define CRYPT_STRING_BASE64     0x00000001

BOOL WINAPI CryptAcquireContextA(HCRYPTPROV *phProv, LPCSTR pszContainer, LPCSTR pszProvider, DWORD dwProvType, DWORD dwFlags);
BOOL WINAPI CryptReleaseContext(HCRYPTPROV hProv, DWORD dwFlags);
BOOL WINAPI CryptCreateHash(HCRYPTPROV hProv, ALG_ID Algid, HCRYPTKEY hKey, DWORD dwFlags, HCRYPTHASH *phHash);
BOOL WINAPI CryptHashData(HCRYPTHASH hHash, const BYTE *pbData, DWORD dwDataLen, DWORD dwFlags);
BOOL WINAPI CryptGetHashParam(HCRYPTHASH hHash, DWORD dwParam, BYTE *pbData, DWORD *pdwDataLen, DWORD dwFlags);
BOOL WINAPI CryptDestroyHash(HCRYPTHASH hHash);
BOOL WINAPI CryptBinaryToStringA(const BYTE *pbBinary, DWORD cbBinary, DWORD dwFlags, LPSTR pszString, DWORD *pcchString);
// -------------------------------------------------------------------

#ifndef CRYPT_STRING_NOCRLF
#define CRYPT_STRING_NOCRLF 0x40000000
#endif

#ifndef htonll
#define htonll(x) (((unsigned long long)htonl((unsigned int)(x))) << 32 | htonl((unsigned int)((x) >> 32)))
#endif

#define PORT 8080
#define MSG_PATH "D:\\zig\\log\\messages.txt"
#define BUFFER_SIZE 8192

// ---------- 纯 Win32 替代工具函数 ----------
#define my_alloc(sz) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (sz))
#define my_free(ptr) do { if (ptr) HeapFree(GetProcessHeap(), 0, (ptr)); } while(0)

static int win_strlen(const char *s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

static void win_memcpy(void *dest, const void *src, size_t n) {
    char *d = (char*)dest;
    const char *s = (const char*)src;
    while (n--) *d++ = *s++;
}

static int win_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static char* win_strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static char* win_strchr(const char *s, char c) {
    while (*s) {
        if (*s == c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : NULL;
}

static char* win_strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && (*h == *n)) {
                h++;
                n++;
            }
            if (!*n) return (char*)haystack;
        }
    }
    return NULL;
}

// 替代 sscanf(buf, "%s %s %s", method, path, version)
static void parse_request_line(const char *buf, char *method, int m_max, char *path, int p_max, char *ver, int v_max) {
    int i = 0, j = 0;
    while (buf[i] && buf[i] != ' ' && j < m_max - 1) method[j++] = buf[i++];
    method[j] = '\0';
    while (buf[i] == ' ') i++;
    j = 0;
    while (buf[i] && buf[i] != ' ' && j < p_max - 1) path[j++] = buf[i++];
    path[j] = '\0';
    while (buf[i] == ' ') i++;
    j = 0;
    while (buf[i] && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n' && j < v_max - 1) ver[j++] = buf[i++];
    ver[j] = '\0';
}

typedef struct {
    SOCKET socket;
    char ip[64];
} ClientInfo;

CRITICAL_SECTION file_lock;
CRITICAL_SECTION clients_lock;

typedef struct ClientNode {
    SOCKET socket;
    char ip[64];
    struct ClientNode *next;
} ClientNode;

ClientNode *g_clients = NULL;

// ---------- HTML 页面（内嵌 WebSocket 客户端） ----------
const char *HTML_PAGE =
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html; charset=utf-8\r\n\r\n"
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"  <meta charset=\"utf-8\">\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"  <title>💬 实时聊天室</title>\n"
"  <style>\n"
"    * { margin: 0; padding: 0; box-sizing: border-box; }\n"
"    body {\n"
"      font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, Helvetica, Arial, sans-serif;\n"
"      background: linear-gradient(145deg, #f0f2f5 0%, #e6e9ef 100%);\n"
"      min-height: 100vh;\n"
"      display: flex;\n"
"      justify-content: center;\n"
"      align-items: center;\n"
"      padding: 20px;\n"
"    }\n"
"    .container {\n"
"      max-width: 780px;\n"
"      width: 100%;\n"
"      background: #ffffff;\n"
"      border-radius: 28px;\n"
"      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.12), 0 8px 20px rgba(0, 0, 0, 0.05);\n"
"      padding: 28px 30px 32px;\n"
"    }\n"
"    h2 {\n"
"      font-size: 26px;\n"
"      font-weight: 600;\n"
"      color: #1e293b;\n"
"      display: flex;\n"
"      align-items: center;\n"
"      gap: 10px;\n"
"      margin-bottom: 18px;\n"
"    }\n"
"    h2 small {\n"
"      font-size: 14px;\n"
"      font-weight: 400;\n"
"      color: #94a3b8;\n"
"      margin-left: auto;\n"
"      background: #f1f5f9;\n"
"      padding: 2px 12px;\n"
"      border-radius: 30px;\n"
"    }\n"
"    #chat {\n"
"      border: 1px solid #e9edf4;\n"
"      height: 400px;\n"
"      overflow-y: auto;\n"
"      padding: 16px 18px;\n"
"      margin-bottom: 20px;\n"
"      background: #fafcff;\n"
"      border-radius: 18px;\n"
"      font-size: 15px;\n"
"      line-height: 1.7;\n"
"      scroll-behavior: smooth;\n"
"      box-shadow: inset 0 2px 4px rgba(0,0,0,0.02);\n"
"    }\n"
"    #chat::-webkit-scrollbar { width: 6px; }\n"
"    #chat::-webkit-scrollbar-track { background: #eef2f6; border-radius: 8px; }\n"
"    #chat::-webkit-scrollbar-thumb { background: #cbd5e1; border-radius: 8px; }\n"
"    #chat::-webkit-scrollbar-thumb:hover { background: #94a3b8; }\n"
"    .msg-item {\n"
"      padding: 4px 0;\n"
"      display: flex;\n"
"      align-items: flex-start;\n"
"      gap: 8px;\n"
"      border-bottom: 1px solid #f1f5f9;\n"
"    }\n"
"    .msg-item:last-child { border-bottom: none; }\n"
"    .msg-time {\n"
"      color: #94a3b8;\n"
"      font-size: 13px;\n"
"      font-weight: 400;\n"
"      min-width: 80px;\n"
"      flex-shrink: 0;\n"
"      text-align: right;\n"
"    }\n"
"    .msg-nick {\n"
"      font-weight: 600;\n"
"      margin-right: 4px;\n"
"      flex-shrink: 0;\n"
"      max-width: 150px;\n"
"      white-space: normal;\n"
"      word-break: break-word;\n"
"    }\n"
"    .msg-text {\n"
"      word-break: break-word;\n"
"      flex: 1;\n"
"      min-width: 0;\n"
"    }\n"
"    .input-group {\n"
"      display: flex;\n"
"      gap: 12px;\n"
"      align-items: center;\n"
"      flex-wrap: wrap;\n"
"    }\n"
"    .input-group input, .input-group button {\n"
"      font-family: inherit;\n"
"      font-size: 15px;\n"
"      border: 1px solid #dce1ea;\n"
"      border-radius: 60px;\n"
"      padding: 12px 18px;\n"
"      outline: none;\n"
"      transition: 0.2s;\n"
"      background: #fff;\n"
"    }\n"
"    #sender { width: 130px; flex: 0 0 auto; background: #f8fafc; }\n"
"    #sender:focus, #msg:focus {\n"
"      border-color: #6366f1;\n"
"      box-shadow: 0 0 0 3px rgba(99,102,241,0.15);\n"
"    }\n"
"    #msg { flex: 1; min-width: 180px; background: #f8fafc; }\n"
"    button {\n"
"      padding: 12px 28px;\n"
"      background: #6366f1;\n"
"      color: #fff;\n"
"      border: none;\n"
"      border-radius: 60px;\n"
"      cursor: pointer;\n"
"      font-weight: 600;\n"
"      font-size: 15px;\n"
"      box-shadow: 0 4px 12px rgba(99,102,241,0.30);\n"
"      transition: 0.2s;\n"
"      white-space: nowrap;\n"
"    }\n"
"    button:hover {\n"
"      background: #4f46e5;\n"
"      transform: translateY(-1px);\n"
"      box-shadow: 0 6px 18px rgba(99,102,241,0.35);\n"
"    }\n"
"    button:active { transform: scale(0.97); }\n"
"    .status {\n"
"      font-size: 13px;\n"
"      color: #94a3b8;\n"
"      text-align: center;\n"
"      margin-top: 14px;\n"
"      border-top: 1px solid #edf2f7;\n"
"      padding-top: 14px;\n"
"    }\n"
"    .status span {\n"
"      display: inline-block;\n"
"      width: 8px;\n"
"      height: 8px;\n"
"      background: #22c55e;\n"
"      border-radius: 50%;\n"
"      margin-right: 6px;\n"
"      animation: pulse 1.8s infinite;\n"
"    }\n"
"    @keyframes pulse {\n"
"      0% { opacity: 1; transform: scale(1); }\n"
"      50% { opacity: 0.5; transform: scale(1.2); }\n"
"      100% { opacity: 1; transform: scale(1); }\n"
"    }\n"
"    @media (max-width: 600px) {\n"
"      .container { padding: 18px; }\n"
"      .input-group { flex-direction: column; align-items: stretch; }\n"
"      #sender { width: 100%; }\n"
"      button { width: 100%; }\n"
"    }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div class=\"container\">\n"
"    <h2>\n"
"      💬 实时聊天室\n"
"      <small id=\"statusBadge\">● 在线</small>\n"
"    </h2>\n"
"    <div id=\"chat\">加载历史中...</div>\n"
"    <div class=\"input-group\">\n"
"      <input type=\"text\" id=\"sender\" placeholder=\"昵称\" value=\"访客\">\n"
"      <input type=\"text\" id=\"msg\" placeholder=\"输入消息按回车发送…\" onkeydown=\"if(event.key==='Enter') sendMsg()\">\n"
"      <button onclick=\"sendMsg()\">发送 ➤</button>\n"
"    </div>\n"
"    <div class=\"status\">\n"
"      <span></span> WebSocket 已连接 · 消息实时推送\n"
"    </div>\n"
"  </div>\n"
"  <script>\n"
"    const chatDiv = document.getElementById('chat');\n"
"    const statusBadge = document.getElementById('statusBadge');\n"
"    let ws = null;\n"
"\n"
"    function nickColor(name) {\n"
"      let hash = 0;\n"
"      for (let i = 0; i < name.length; i++) {\n"
"        hash = name.charCodeAt(i) + ((hash << 5) - hash);\n"
"      }\n"
"      const hue = Math.abs(hash % 360);\n"
"      return `hsl(${hue}, 70%, 40%)`;\n"
"    }\n"
"\n"
"    function formatMessage(line) {\n"
"      const match = line.match(/^\\[([^\\]]+)\\]\\s*(.*?):\\s*(.*)$/);\n"
"      if (match) {\n"
"        const time = match[1];\n"
"        const nick = match[2].trim();\n"
"        const text = match[3];\n"
"        const color = nickColor(nick);\n"
"        return `<span class=\"msg-time\">${time}</span>` +\n"
"               `<span class=\"msg-nick\" style=\"color:${color}\">${escapeHTML(nick)}</span>` +\n"
"               `<span class=\"msg-text\">${escapeHTML(text)}</span>`;\n"
"      } else {\n"
"        return `<span class=\"msg-text\">${escapeHTML(line)}</span>`;\n"
"      }\n"
"    }\n"
"\n"
"    function escapeHTML(str) {\n"
"      const div = document.createElement('div');\n"
"      div.textContent = str;\n"
"      return div.innerHTML;\n"
"    }\n"
"\n"
"    function appendMessage(line) {\n"
"      const wrapper = document.createElement('div');\n"
"      wrapper.className = 'msg-item';\n"
"      wrapper.innerHTML = formatMessage(line);\n"
"      chatDiv.appendChild(wrapper);\n"
"      chatDiv.scrollTop = chatDiv.scrollHeight;\n"
"    }\n"
"\n"
"    async function loadHistory() {\n"
"      try {\n"
"        const res = await fetch('/api/messages');\n"
"        const text = await res.text();\n"
"        chatDiv.innerHTML = '';\n"
"        if (!text.trim()) {\n"
"          chatDiv.textContent = '✨ 暂无消息，快来发言吧！';\n"
"          return;\n"
"        }\n"
"        const lines = text.split('\\n').filter(l => l.trim());\n"
"        for (const line of lines) {\n"
"          appendMessage(line);\n"
"        }\n"
"        chatDiv.scrollTop = chatDiv.scrollHeight;\n"
"      } catch (e) {\n"
"        chatDiv.textContent = '⚠️ 加载历史失败';\n"
"      }\n"
"    }\n"
"\n"
"    function loadUsername() {\n"
"      const senderInput = document.getElementById('sender');\n"
"      const saved = localStorage.getItem('chat_username');\n"
"      if (saved) senderInput.value = saved;\n"
"      else { senderInput.value = '访客'; localStorage.setItem('chat_username', '访客'); }\n"
"    }\n"
"    function saveUsername(name) {\n"
"      const trimmed = name.trim();\n"
"      if (trimmed) localStorage.setItem('chat_username', trimmed);\n"
"      else { localStorage.setItem('chat_username', '访客'); document.getElementById('sender').value = '访客'; }\n"
"    }\n"
"\n"
"    function connectWS() {\n"
"      const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';\n"
"      ws = new WebSocket(proto + '//' + location.host + '/ws');\n"
"      ws.onmessage = function(event) {\n"
"        appendMessage(event.data);\n"
"      };\n"
"      ws.onopen = function() {\n"
"        statusBadge.innerHTML = '● 在线';\n"
"        statusBadge.style.color = '#22c55e';\n"
"      };\n"
"      ws.onclose = function() {\n"
"        statusBadge.innerHTML = '○ 离线';\n"
"        statusBadge.style.color = '#ef4444';\n"
"        setTimeout(connectWS, 3000);\n"
"      };\n"
"      ws.onerror = function() { ws.close(); };\n"
"    }\n"
"\n"
"    function sendMsg() {\n"
"      const senderInput = document.getElementById('sender');\n"
"      const msgInput = document.getElementById('msg');\n"
"      const sender = senderInput.value.trim() || '匿名';\n"
"      const text = msgInput.value.trim();\n"
"      if (!text || !ws || ws.readyState !== WebSocket.OPEN) return;\n"
"      msgInput.value = '';\n"
"      ws.send(sender + '\\n' + text);\n"
"    }\n"
"\n"
"    loadUsername();\n"
"    const senderInput = document.getElementById('sender');\n"
"    senderInput.addEventListener('change', function() { saveUsername(this.value); });\n"
"    senderInput.addEventListener('blur', function() { saveUsername(this.value); });\n"
"\n"
"    loadHistory();\n"
"    connectWS();\n"
"  </script>\n"
"</body>\n"
"</html>\n";

// ---------- 纯 Win32 目录与文件处理 ----------
void ensure_dir_and_file() {
    CreateDirectoryA("D:\\zig", NULL);
    CreateDirectoryA("D:\\zig\\log", NULL);

    HANDLE hFile = CreateFileA(MSG_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        hFile = CreateFileA(MSG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            unsigned char bom[] = {0xEF, 0xBB, 0xBF};
            DWORD written = 0;
            WriteFile(hFile, bom, 3, &written, NULL);
            CloseHandle(hFile);
        }
    } else {
        CloseHandle(hFile);
    }
}

// 计算 WebSocket Accept 值
void compute_ws_accept(const char *key, char *accept) {
    char combined[256];
    wsprintfA(combined, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);

    HCRYPTPROV hProv;
    HCRYPTHASH hHash;
    BYTE hash[20];
    DWORD hashLen = 20;
    if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
            CryptHashData(hHash, (BYTE*)combined, (DWORD)win_strlen(combined), 0);
            CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }

    DWORD len = 64;
    CryptBinaryToStringA(hash, 20, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, accept, &len);
}

// ---------- WebSocket 帧处理 ----------
int ws_recv_frame(SOCKET s, char **out_data, int *out_len) {
    unsigned char header[2];
    int recv_bytes = recv(s, (char*)header, 2, 0);
    if (recv_bytes != 2) return -1;

    int opcode = header[0] & 0x0F;
    int masked = (header[1] & 0x80) ? 1 : 0;
    int payload_len = header[1] & 0x7F;

    // 扩展长度
    if (payload_len == 126) {
        unsigned short ext_len;
        recv_bytes = recv(s, (char*)&ext_len, 2, 0);
        if (recv_bytes != 2) return -1;
        payload_len = ntohs(ext_len);
    } else if (payload_len == 127) {
        unsigned long long ext_len;
        recv_bytes = recv(s, (char*)&ext_len, 8, 0);
        if (recv_bytes != 8) return -1;
        payload_len = (int)ext_len;
        if (payload_len > 1024*1024) return -1;
    }

    // 掩码键
    unsigned char mask[4] = {0};
    if (masked) {
        recv_bytes = recv(s, (char*)mask, 4, 0);
        if (recv_bytes != 4) return -1;
    }

    // 读取数据
    char *data = (char*)my_alloc(payload_len + 1);
    if (!data) return -1;
    int total = 0;
    while (total < payload_len) {
        int r = recv(s, data + total, payload_len - total, 0);
        if (r <= 0) {
            my_free(data);
            return -1;
        }
        total += r;
    }
    data[payload_len] = '\0';

    // 解除掩码
    if (masked) {
        for (int i = 0; i < payload_len; i++) {
            data[i] ^= mask[i % 4];
        }
    }

    *out_data = data;
    *out_len = payload_len;
    return opcode;
}

void ws_send_frame(SOCKET s, const char *data, int len) {
    unsigned char header[10];
    int header_len = 0;
    header[0] = 0x81;  // FIN=1, opcode=0x1 (文本)
    if (len <= 125) {
        header[1] = (unsigned char)len;
        header_len = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        unsigned short net_len = htons((unsigned short)len);
        win_memcpy(header + 2, &net_len, 2);
        header_len = 4;
    } else {
        header[1] = 127;
        unsigned long long net_len = htonll((unsigned long long)len);
        win_memcpy(header + 2, &net_len, 8);
        header_len = 10;
    }
    send(s, (char*)header, header_len, 0);
    send(s, data, len, 0);
}

// ---------- 客户端管理 ----------
void add_client(SOCKET s, const char *ip) {
    ClientNode *node = (ClientNode*)my_alloc(sizeof(ClientNode));
    node->socket = s;
    win_strcpy(node->ip, ip);
    node->next = NULL;
    EnterCriticalSection(&clients_lock);
    node->next = g_clients;
    g_clients = node;
    LeaveCriticalSection(&clients_lock);
}

void remove_client(SOCKET s) {
    EnterCriticalSection(&clients_lock);
    ClientNode *cur = g_clients, *prev = NULL;
    while (cur) {
        if (cur->socket == s) {
            if (prev) prev->next = cur->next;
            else g_clients = cur->next;
            my_free(cur);
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    LeaveCriticalSection(&clients_lock);
}

void broadcast_message(const char *msg) {
    EnterCriticalSection(&clients_lock);
    ClientNode *cur = g_clients;
    while (cur) {
        ws_send_frame(cur->socket, msg, win_strlen(msg));
        cur = cur->next;
    }
    LeaveCriticalSection(&clients_lock);
}

// ---------- 消息存储 + 广播 ----------
void append_message(const char *ip, const char *sender, const char *msg) {
    if (win_strlen(msg) == 0) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char formatted[1024];
    wsprintfA(formatted,
             "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s]: %s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             ip, sender, msg);

    // 写入文件（带锁，纯 Win32 API）
    EnterCriticalSection(&file_lock);
    HANDLE hFile = CreateFileA(MSG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, formatted, win_strlen(formatted), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
        CloseHandle(hFile);
    }
    LeaveCriticalSection(&file_lock);

    // 广播给所有 WebSocket 客户端
    broadcast_message(formatted);
}

// ---------- 处理客户端连接 ----------
void handle_client(ClientInfo *client) {
    char buffer[BUFFER_SIZE] = {0};
    int bytes_received = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        closesocket(client->socket);
        my_free(client);
        return;
    }

    // 解析请求行
    char method[16], path[256], version[16];
    parse_request_line(buffer, method, sizeof(method), path, sizeof(path), version, sizeof(version));

    // ---------- HTTP 请求 ----------
    if (win_strcmp(method, "GET") == 0) {
        // 主页
        if (win_strcmp(path, "/") == 0 || win_strcmp(path, "/index.html") == 0) {
            send(client->socket, HTML_PAGE, win_strlen(HTML_PAGE), 0);
            closesocket(client->socket);
            my_free(client);
            return;
        }
        // 获取历史消息
        else if (win_strcmp(path, "/api/messages") == 0) {
            EnterCriticalSection(&file_lock);
            HANDLE hFile = CreateFileA(MSG_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            char *file_data = NULL;
            DWORD file_size = 0;
            if (hFile != INVALID_HANDLE_VALUE) {
                file_size = GetFileSize(hFile, NULL);
                if (file_size != INVALID_FILE_SIZE && file_size > 0) {
                    file_data = (char*)my_alloc(file_size + 1);
                    if (file_data) {
                        DWORD bytes_read = 0;
                        ReadFile(hFile, file_data, file_size, &bytes_read, NULL);
                        file_data[file_size] = '\0';
                    }
                }
                CloseHandle(hFile);
            }
            LeaveCriticalSection(&file_lock);

            char header[256];
            int header_len = wsprintfA(header,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: %lu\r\n"
                "Cache-Control: no-cache\r\n\r\n",
                (file_data && file_size > 0) ? file_size : 0);

            send(client->socket, header, header_len, 0);
            if (file_data && file_size > 0) {
                send(client->socket, file_data, file_size, 0);
                my_free(file_data);
            }
            closesocket(client->socket);
            my_free(client);
            return;
        }
        // ---------- WebSocket 握手 ----------
        else if (win_strcmp(path, "/ws") == 0) {
            char *key_start = win_strstr(buffer, "Sec-WebSocket-Key:");
            if (!key_start) {
                closesocket(client->socket);
                my_free(client);
                return;
            }
            key_start += 19;
            while (*key_start == ' ') key_start++;
            char *key_end = win_strstr(key_start, "\r\n");
            if (!key_end) {
                closesocket(client->socket);
                my_free(client);
                return;
            }
            char key[256];
            int key_len = key_end - key_start;
            if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
            win_memcpy(key, key_start, key_len);
            key[key_len] = '\0';

            char accept[64];
            compute_ws_accept(key, accept);

            char response[512];
            wsprintfA(response,
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n\r\n",
                accept);
            send(client->socket, response, win_strlen(response), 0);

            add_client(client->socket, client->ip);

            while (1) {
                char *frame_data = NULL;
                int frame_len = 0;
                int opcode = ws_recv_frame(client->socket, &frame_data, &frame_len);
                if (opcode < 0) break;
                if (opcode == 0x8) {
                    my_free(frame_data);
                    break;
                } else if (opcode == 0x1) {
                    char *newline = win_strchr(frame_data, '\n');
                    if (newline) {
                        *newline = '\0';
                        char *sender = frame_data;
                        char *msg = newline + 1;
                        append_message(client->ip, sender, msg);
                    } else {
                        append_message(client->ip, "访客", frame_data);
                    }
                    my_free(frame_data);
                } else {
                    // 必须释放 Ping/Pong 等未处理帧的内存
                    my_free(frame_data);
                }
            }

            remove_client(client->socket);
            closesocket(client->socket);
            my_free(client);
            return;
        }
    }
    else if (win_strcmp(method, "POST") == 0 && win_strcmp(path, "/api/send") == 0) {
        char *body = win_strstr(buffer, "\r\n\r\n");
        if (body) {
            body += 4;
            char *newline = win_strchr(body, '\n');
            if (newline) {
                *newline = '\0';
                char *sender = body;
                char *msg = newline + 1;
                char *r = win_strchr(sender, '\r');
                if (r) *r = '\0';
                append_message(client->ip, sender, msg);
            } else {
                append_message(client->ip, "访客", body);
            }
        }
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
        send(client->socket, resp, win_strlen(resp), 0);
        closesocket(client->socket);
        my_free(client);
        return;
    }

    const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send(client->socket, not_found, win_strlen(not_found), 0);
    closesocket(client->socket);
    my_free(client);
}

DWORD WINAPI client_thread(LPVOID arg) {
    ClientInfo *client = (ClientInfo*)arg;
    handle_client(client);
    return 0;
}

// ---------- 主函数 ----------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        MessageBoxA(NULL, "WSAStartup 失败", "错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    InitializeCriticalSection(&file_lock);
    InitializeCriticalSection(&clients_lock);
    ensure_dir_and_file();

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        MessageBoxA(NULL, "创建 Socket 失败", "错误", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        char buf[128];
        wsprintfA(buf, "绑定端口 %d 失败", PORT);
        MessageBoxA(NULL, buf, "错误", MB_OK | MB_ICONERROR);
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        MessageBoxA(NULL, "监听失败", "错误", MB_OK | MB_ICONERROR);
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    char msg[256];
    wsprintfA(msg, "聊天室服务器已启动，端口 %d\n", PORT);
    OutputDebugStringA(msg);

    while (1) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);

        if (client_socket != INVALID_SOCKET) {
            ClientInfo *client = (ClientInfo*)my_alloc(sizeof(ClientInfo));
            client->socket = client_socket;
            win_strcpy(client->ip, inet_ntoa(client_addr.sin_addr));

            HANDLE thread = CreateThread(NULL, 0, client_thread, (LPVOID)client, 0, NULL);
            if (thread) {
                CloseHandle(thread);
            } else {
                closesocket(client_socket);
                my_free(client);
            }
        }
    }

    DeleteCriticalSection(&file_lock);
    DeleteCriticalSection(&clients_lock);
    closesocket(server_socket);
    WSACleanup();
    return 0;
}