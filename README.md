### 📘 Windows C 语言系统实用工具集 — 综合帮助与技术手册

这套工具集由 5 个基于原生 Win32 API 打造的轻量级 C 语言程序组成，具备**零外部 DLL 依赖、体积小巧、编译快速**的特点，非常适合 Windows 运维、轻量级服务端开发或日常系统调优。

---

### 目录与快速参考表

| 程序名称 | 源码文件 | 运行形态 | 依赖核心 API / 模块 | 默认配置与日志路径 |
| --- | --- | --- | --- | --- |
| **嵌入式 WebSocket 聊天服务端** | `chat_server.c` | 后台 / 网络服务 | Winsock2, CryptoAPI (SHA1/Base64), Win32 Heap | 消息日志：`D:\zig\log\messages.txt` |
| **DDC/CI 显示器控亮托盘** | `DDCBrightness.c` | 系统托盘 GUI | DXVA2 (Physical Monitor), Shell Tray, DWM Dark Mode | 注册表自启：`HKCU\...\Run` |
| **网关连通性静默监控** | `ping_gateway.c` | 静默后台服务 | IP Helper API (`GetAdaptersInfo`), ICMP API | 日志文件：`log\ping.log` |
| **进程感知电源计划切换器** | `power_monitor.c` | 静默后台服务 | Toolhelp32 (Process Snapshot), `powrprof.dll` | 配置：`D:\zig\config\power_monitor.txt`<br>

<br>日志：`D:\zig\log\power_monitor.log` |
| **开机自启项可视化管理工具** | `start.c` | 桌面 GUI | Win32 Standard Controls, Comdlg32, DPI Aware | 注册表：`HKCU\...\Run` |

---

### 一、`chat_server.c` — 嵌入式 WebSocket & HTTP 实时聊天服务器

#### 1. 功能概述

`chat_server.c` 是一个单文件构建的嵌入式 Web & WebSocket 实时通讯服务器。前端响应式 HTML5 聊天页面已完整内置于 C 源码中，无需另外部署静态网页文件。

#### 2. 编译命令

* **Zig cc（推荐）**：
```bash
zig cc chat_server.c -o chat_server.exe -lws2_32 -lcrypt32 -luser32 -ladvapi32 "-Wl,--subsystem,windows" -Oz -s

```


* **TCC**：
```bash
tcc chat_server.c -o chat_server.exe -lws2_32 -lcrypt32 -luser32 -ladvapi32 -mwindows

```



#### 3. 核心设计亮点

* **零 MSVCRT 依赖**：使用 Win32 堆内存（`HeapAlloc`）及自研 C 字符串函数（`win_strlen`, `win_memcpy`, `win_strcmp`, `win_strstr` 等），大幅降低运行时体积。
* **原生 CryptoAPI 握手**：通过 Windows `CryptCreateHash` 计算 SHA-1 并用 `CryptBinaryToStringA` 进行 Base64 编码，实现标准 WebSocket `Sec-WebSocket-Accept` 握手。
* **双重临界区 (Critical Section)**：`file_lock` 保证日志追加的线程安全；`clients_lock` 保证动态客户端链表在广播消息时的线程安全。

#### 4. API 与通信规范

* **Web 主页**：`GET /` 或 `GET /index.html`（端口 `8080`）
* **历史消息**：`GET /api/messages`（返回 UTF-8 带 BOM 的日志文件）
* **WebSocket 握手**：`GET /ws`（长连接全双工推送）
* **消息格式**：客户端发送 `[昵称]\n[文本]`，服务端广播 `[时间] [IP] [昵称]: [消息]`。

---

### 二、`DDCBrightness.c` — 显示器 DDC/CI 硬件亮度调节托盘工具

#### 1. 功能概述

直接与外接显示器固件通信的 Windows 托盘调光工具，无需依赖显卡驱动软件，即可通过 DDC/CI 协议调节显示器物理背光亮度。

#### 2. 编译命令

* **TCC**：
```bash
tcc DDCBrightness.c -o DDCBrightness.exe -luser32 -lshell32 -lgdi32 -lcomctl32 -ldxva2 -ladvapi32

```



#### 3. 核心功能与亮点

* **硬件真实型号识别**：解析 DDC/CI Capabilities 响应中的 `model(...)` 标签，准确显示显示器型号（如 Dell U2720Q）。
* **Win10/11 现代暗色 Flyout 悬浮窗**：通过 `dwmapi.dll`（启用 `DWMWA_USE_IMMERSIVE_DARK_MODE`）与 `uxtheme.dll`（`DarkMode_Explorer`），融合 Segoe MDL2 矢量图标（`\uE706` 亮度 / `\uE7E8` 息屏）。
* **快捷硬件息屏**：提供专属息屏按钮，通过 VCP Code `0xD6` 发送节电指令并触发系统深度休眠。
* **开机自启**：右键托盘菜单可快速切换开机自启动状态。

---

### 三、`ping_gateway.c` — 静默后台网关连通性监控服务

#### 1. 功能概述

无窗口静默运行的网络连通性诊断工具，启动后自动感知默认网关并定期（默认 60s）发送 ICMP Echo 探测包，实时记录延迟与断网事件。

#### 2. 编译命令

* **Zig cc**：
```bash
zig cc -Oz -s ping_gateway.c -o ping_gateway.exe -liphlpapi -lws2_32 -luser32 '-Wl,--subsystem,windows'

```


* **TCC**：
```bash
tcc ping_gateway.c -o ping_gateway.exe -liphlpapi -lws2_32 -luser32 -mwindows

```



#### 3. 核心机制

* **控制台自动剥离**：源码入口使用 `WinMain` 并执行 `FreeConsole()`，无感隐形运行。
* **网关自动检索**：调用 IP Helper API (`GetAdaptersInfo`) 自动定位非 `0.0.0.0` 的有效网关。
* **指定 IP 监控**：支持命令行直接指定目标（如 `ping_gateway.exe 1.1.1.1`）。
* **日志格式**：自动追加输出至 `log\ping.log`。

---

### 四、`power_monitor.c` — 进程感知的电源计划动态切换器

#### 1. 功能概述

自动化电源管理工具。当检测到指定的重度应用（如游戏、渲染器或编译引擎）运行时，自动将系统电源计划提升为“平衡模式”**；当所有目标进程退出后，自动回退到**“节能模式”。

#### 2. 编译命令

* **TCC**：
```bash
tcc power_monitor.c -o power_monitor.exe -mwindows

```



#### 3. 配置文件与路径

* **配置文件**：`D:\zig\config\power_monitor.txt`（纯文本，每行写一个目标进程名，如 `chrome.exe` 或 `game.exe`）。
* **运行日志**：`D:\zig\log\power_monitor.log`。
* **动态 API 加载**：零静态链接依赖，使用 `Toolhelp32` 进行进程快照扫描，并通过 `LoadLibraryA("powrprof.dll")` 动态调用 `PowerSetActiveScheme`。

---

### 五、`start.c` — 开机自启动注册表项可视化管理工具

#### 1. 功能概述

开机自启项配置 GUI 工具，方便直观地管理 Windows 注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，支持自定义添加启动参数。

#### 2. 编译命令

* **TCC**：
```bash
tcc start.c -o start.exe -lkernel32 -luser32 -lgdi32 -ladvapi32 -lcomdlg32

```



#### 3. 核心功能

* **高分屏 (DPI Awareness) 适配**：优先加载 `SetProcessDpiAwarenessContext(-4)`，高分屏下界面清晰不模糊。
* **原生文件选择器**：集成 `GetOpenFileNameW`，可浏览选择 `.exe` 文件并自动生成正确的引导路径与双引号转义。
* **防重校验**：写入自启项前读取注册表校验碰撞，避免覆盖已有配置。

---

### 💡 附录：一键编译批处理脚本

你可以将以下脚本另存为 `build_all.bat`，即可使用 TCC 一键编译所有 5 个可执行文件：

```cmd
@echo off
chcp 65001 >nul
echo [1/5] 正在编译 chat_server.exe ...
tcc chat_server.c -o chat_server.exe -lws2_32 -lcrypt32 -luser32 -ladvapi32 -mwindows

echo [2/5] 正在编译 DDCBrightness.exe ...
tcc DDCBrightness.c -o DDCBrightness.exe -luser32 -lshell32 -lgdi32 -lcomctl32 -ldxva2 -ladvapi32

echo [3/5] 正在编译 ping_gateway.exe ...
tcc ping_gateway.c -o ping_gateway.exe -liphlpapi -lws2_32 -luser32 -mwindows

echo [4/5] 正在编译 power_monitor.exe ...
tcc power_monitor.c -o power_monitor.exe -mwindows

echo [5/5] 正在编译 start.exe ...
tcc start.c -o start.exe -lkernel32 -luser32 -lgdi32 -ladvapi32 -lcomdlg32

echo.
echo 所有程序编译完成！
pause

```