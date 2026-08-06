# =============================================================
# 监控进程修改电源计划
# 编译方式: zig cc -O2 -nostdlib power_monitor.s -o power_monitor.exe -lkernel32 -lpowrprof "-Wl,/subsystem:windows" "-Wl,-e,main" "-Wl,-s"
# =============================================================

.intel_syntax noprefix
.global main

.section .rdata
    szConfig:       .asciz "config\\power_monitor.txt"
    szLog:          .asciz "log\\power_monitor.log"
    szMsgBalanced:  .asciz " [State Change] Power mode set to: Balanced\r\n"
    szMsgSaver:     .asciz " [State Change] Power mode set to: Power Saver\r\n"

    # GUID_BALANCED: 381b4222-f694-41f0-9685-ff5bb260df2e
    guid_balanced:
        .long 0x381b4222
        .short 0xf694, 0x41f0
        .byte 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e

    # GUID_POWER_SAVER: a18441cd-379a-4669-9925-633d70466925
    guid_saver:
        .long 0xa1841308
        .short 0x3541, 0x4fab
        .byte 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a

.section .data
    last_state:     .long -1            # -1: 初始未知, 1: 平衡, 0: 节能

.section .bss
    .align 16
    config_buf:     .space 4096         # config.txt 缓冲区
    pe32:           .space 320          # PROCESSENTRY32A 结构体 (304 字节)
    systime:        .space 16           # SYSTEMTIME 结构体
    log_line:       .space 256          # 日志单行缓冲区

.section .text

# -------------------------------------------------------------
# 程序入口点 (main)
# -------------------------------------------------------------
main:
    sub     rsp, 40                     # 预留 Shadow Space

main_loop:
    call    read_config_file            # 读取配置文件到 config_buf
    call    check_processes             # 检查目标进程: RAX=1 表示存在，RAX=0 表示不存在
    
    # 判断状态是否变更
    mov     ecx, dword ptr [rip + last_state]
    cmp     eax, ecx
    je      sleep_and_loop

    # 状态改变，更新状态标示
    mov     dword ptr [rip + last_state], eax
    
    # 设置电源模式: PowerSetActiveScheme(NULL, &GUID)
    xor     rcx, rcx
    cmp     eax, 1
    je      set_balanced
    lea     rdx, [rip + guid_saver]
    jmp     do_set_power
set_balanced:
    lea     rdx, [rip + guid_balanced]
do_set_power:
    call    PowerSetActiveScheme

    # 写入日志
    call    write_status_log

sleep_and_loop:
    mov     ecx, 60000                   # 3000 ms
    call    Sleep
    jmp     main_loop

# -------------------------------------------------------------
# 读取 config.txt
# -------------------------------------------------------------
read_config_file:
    sub     rsp, 56
    # 清空 config_buf
    lea     rdi, [rip + config_buf]
    xor     eax, eax
    mov     ecx, 1024
    rep stosd

    # CreateFileA("config.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, ...)
    lea     rcx, [rip + szConfig]
    mov     edx, 0x80000000             # GENERIC_READ
    mov     r8d, 1                      # FILE_SHARE_READ
    xor     r9, r9
    mov     qword ptr [rsp + 32], 3     # OPEN_EXISTING
    mov     qword ptr [rsp + 40], 0x80  # FILE_ATTRIBUTE_NORMAL
    mov     qword ptr [rsp + 48], 0
    call    CreateFileA

    cmp     rax, -1                     # INVALID_HANDLE_VALUE
    je      read_config_end
    mov     rbx, rax

    # ReadFile(handle, config_buf, 4095, &bytesRead, NULL)
    mov     rcx, rbx
    lea     rdx, [rip + config_buf]
    mov     r8d, 4095
    lea     r9, [rsp + 32]
    mov     qword ptr [rsp + 32], 0
    call    ReadFile

    # CloseHandle(handle)
    mov     rcx, rbx
    call    CloseHandle

read_config_end:
    add     rsp, 56
    ret

# -------------------------------------------------------------
# 遍历进程列表
# -------------------------------------------------------------
check_processes:
    sub     rsp, 56

    # CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS = 2, 0)
    mov     ecx, 2
    xor     edx, edx
    call    CreateToolhelp32Snapshot
    
    cmp     rax, -1
    je      proc_not_found
    mov     rsi, rax

    # pe32.dwSize = 304
    lea     rdi, [rip + pe32]
    mov     dword ptr [rdi], 304

    # Process32First(snapshot, &pe32)
    mov     rcx, rsi
    mov     rdx, rdi
    call    Process32First
    test    eax, eax
    jz      proc_close_snap

proc_loop:
    # pe32.szExeFile 偏移地址为 +44
    lea     rcx, [rip + pe32 + 44]
    call    match_config_lines
    test    eax, eax
    jnz     proc_found                  # 匹配成功

    # Process32Next(snapshot, &pe32)
    mov     rcx, rsi
    lea     rdx, [rip + pe32]
    call    Process32Next
    test    eax, eax
    jnz     proc_loop

proc_close_snap:
    mov     rcx, rsi
    call    CloseHandle

proc_not_found:
    xor     eax, eax
    add     rsp, 56
    ret

proc_found:
    mov     rcx, rsi
    call    CloseHandle
    mov     eax, 1
    add     rsp, 56
    ret

# -------------------------------------------------------------
# 字符串比较子程序（匹配配置文件的每一行）
# -------------------------------------------------------------
match_config_lines:
    push    rbx
    push    rsi
    push    r12
    push    r13
    sub     rsp, 40

    mov     r12, rcx                    # r12 = 进程名指针
    lea     r13, [rip + config_buf]     # r13 = 配置缓冲区指针

line_start:
    mov     al, byte ptr [r13]
    test    al, al
    jz      match_fail
    cmp     al, 13                      # \r
    je      skip_char
    cmp     al, 10                      # \n
    je      skip_char
    cmp     al, 32                      # 空格
    je      skip_char
    jmp     compare_line

skip_char:
    inc     r13
    jmp     line_start

compare_line:
    mov     rsi, r12
    mov     rbx, r13

char_loop:
    mov     al, byte ptr [rsi]
    mov     dl, byte ptr [rbx]

    # 不区分大小写转小写
    cmp     al, 'A'
    jl      1f
    cmp     al, 'Z'
    jg      1f
    add     al, 32
1:
    cmp     dl, 'A'
    jl      2f
    cmp     dl, 'Z'
    jg      2f
    add     dl, 32
2:
    # 行尾判断
    cmp     dl, 13
    je      line_end
    cmp     dl, 10
    je      line_end
    cmp     dl, 0
    je      line_end
    cmp     dl, 32
    je      line_end

    cmp     al, dl
    jne     next_line

    inc     rsi
    inc     rbx
    jmp     char_loop

line_end:
    cmp     byte ptr [rsi], 0
    je      match_success

next_line:
    mov     al, byte ptr [r13]
    test    al, al
    jz      match_fail
    inc     r13
    cmp     al, 10
    jne     next_line
    jmp     line_start

match_success:
    mov     eax, 1
    jmp     match_ret

match_fail:
    xor     eax, eax

match_ret:
    add     rsp, 40
    pop     r13
    pop     r12
    pop     rsi
    pop     rbx
    ret

# -------------------------------------------------------------
# 写入带时间戳的日志文件
# -------------------------------------------------------------
write_status_log:
    push    rbx
    sub     rsp, 56

    # GetLocalTime(&systime)
    lea     rcx, [rip + systime]
    call    GetLocalTime

    # CreateFileA("power_monitor.log", FILE_APPEND_DATA, FILE_SHARE_READ, ...)
    lea     rcx, [rip + szLog]
    mov     edx, 4                      # FILE_APPEND_DATA (0x4)
    mov     r8d, 1                      # FILE_SHARE_READ
    xor     r9, r9
    mov     qword ptr [rsp + 32], 4     # OPEN_ALWAYS
    mov     qword ptr [rsp + 40], 0x80
    mov     qword ptr [rsp + 48], 0
    call    CreateFileA

    cmp     rax, -1
    je      log_end
    mov     rbx, rax

    # 格式化时间戳 [YYYY-MM-DD HH:MM:SS]
    lea     rdi, [rip + log_line]
    mov     byte ptr [rdi], '['
    inc     rdi

    # 年
    movzx   eax, word ptr [rip + systime]
    call    fmt_uint4
    mov     byte ptr [rdi], '-'
    inc     rdi

    # 月
    movzx   eax, word ptr [rip + systime + 2]
    call    fmt_uint2
    mov     byte ptr [rdi], '-'
    inc     rdi

    # 日
    movzx   eax, word ptr [rip + systime + 6]
    call    fmt_uint2
    mov     byte ptr [rdi], ' '
    inc     rdi

    # 时
    movzx   eax, word ptr [rip + systime + 8]
    call    fmt_uint2
    mov     byte ptr [rdi], ':'
    inc     rdi

    # 分
    movzx   eax, word ptr [rip + systime + 10]
    call    fmt_uint2
    mov     byte ptr [rdi], ':'
    inc     rdi

    # 秒
    movzx   eax, word ptr [rip + systime + 12]
    call    fmt_uint2
    mov     byte ptr [rdi], ']'
    inc     rdi

    # 追加文字消息
    mov     eax, dword ptr [rip + last_state]
    cmp     eax, 1
    je      msg_bal
    lea     rsi, [rip + szMsgSaver]
    jmp     copy_msg
msg_bal:
    lea     rsi, [rip + szMsgBalanced]

copy_msg:
    mov     al, byte ptr [rsi]
    mov     byte ptr [rdi], al
    inc     rsi
    inc     rdi
    test    al, al
    jnz     copy_msg
    dec     rdi

    # 计算长度并写入文件
    lea     rax, [rip + log_line]
    sub     rdi, rax

    mov     rcx, rbx
    lea     rdx, [rip + log_line]
    mov     r8, rdi
    lea     r9, [rsp + 32]
    mov     qword ptr [rsp + 32], 0
    call    WriteFile

    mov     rcx, rbx
    call    CloseHandle

log_end:
    add     rsp, 56
    pop     rbx
    ret

# 格式化两位数字辅助函数
fmt_uint2:
    mov     ecx, 10
    xor     edx, edx
    div     ecx
    add     al, '0'
    add     dl, '0'
    mov     byte ptr [rdi], al
    mov     byte ptr [rdi + 1], dl
    add     rdi, 2
    ret

# 格式化四位数字辅助函数
fmt_uint4:
    push    rbx
    mov     ecx, 100
    xor     edx, edx
    div     ecx
    mov     ebx, edx
    call    fmt_uint2
    mov     eax, ebx
    call    fmt_uint2
    pop     rbx
    ret