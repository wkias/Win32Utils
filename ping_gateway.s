/* 
zig cc -Oz -s ping_gateway.s -o ping_gateway.exe -target x86_64-windows-gnu -nostdlib '-Wl,--entry,WinMain' '-Wl,--subsystem,windows' '-Wl,--gc-sections' -lkernel32 -luser32 -liphlpapi -lws2_32 
*/

.intel_syntax noprefix
.global WinMain

.extern FreeConsole
.extern GetCommandLineA
.extern CreateDirectoryA
.extern CreateFileA
.extern WriteFile
.extern CloseHandle
.extern GetProcessHeap
.extern HeapAlloc
.extern HeapFree
.extern GetLocalTime
.extern Sleep
.extern ExitProcess
.extern wsprintfA
.extern GetAdaptersInfo
.extern IcmpCreateFile
.extern IcmpSendEcho
.extern IcmpCloseHandle
.extern inet_addr

.section .data
dir_log:        .asciz "log"
file_log:       .asciz "log\\ping.log"
ping_data:      .asciz "PingPayloadData"

msg_no_gw:      .ascii "[!] 未找到可用网关，退出运行\r\n\0"
fmt_start:      .ascii "[*] 静默 Ping 监控启动，目标网关: %s\r\n\0"
fmt_time:       .asciz "%04d-%02d-%02d %02d:%02d:%02d"
fmt_succ:       .ascii "[%s] Ping %s - 成功 | 延迟: %d ms\r\n\0"
fmt_fail:       .ascii "[%s] Ping %s - 超时或失败\r\n\0"

.section .bss
gateway_ip:     .space 64
log_buf:        .space 256
time_str:       .space 64
sys_time:       .space 16
p_adapter_buf:  .space 8
buf_len:        .space 4

.section .text

write_log:
    push rbx
    push rsi
    sub rsp, 56

    mov rsi, rcx

    lea rcx, [rip + dir_log]
    xor rdx, rdx
    call CreateDirectoryA

    lea rcx, [rip + file_log]
    mov rdx, 4
    mov r8, 1
    xor r9, r9
    mov qword ptr [rsp + 32], 4
    mov qword ptr [rsp + 40], 0x80
    mov qword ptr [rsp + 48], 0
    call CreateFileA

    cmp rax, -1
    je .write_exit
    mov rbx, rax

    mov r8d, 0
.len_loop:
    cmp byte ptr [rsi + r8], 0
    je .len_done
    inc r8d
    jmp .len_loop
.len_done:

    mov rcx, rbx
    mov rdx, rsi
    lea r9, [rsp + 32]
    mov qword ptr [rsp + 32], 0
    call WriteFile

    mov rcx, rbx
    call CloseHandle

.write_exit:
    add rsp, 56
    pop rsi
    pop rbx
    ret

get_default_gateway:
    push rbx
    push rsi
    push rdi
    push r12
    sub rsp, 56

    mov rdi, rcx
    mov r12d, edx

    call GetProcessHeap
    mov rbx, rax

    mov dword ptr [rip + buf_len], 640

    mov rcx, rbx
    xor rdx, rdx
    mov r8d, dword ptr [rip + buf_len]
    call HeapAlloc
    test rax, rax
    jz .gw_fail
    mov qword ptr [rip + p_adapter_buf], rax

    mov rcx, qword ptr [rip + p_adapter_buf]
    lea rdx, [rip + buf_len]
    call GetAdaptersInfo

    cmp eax, 111
    jne .gw_check

    mov rcx, rbx
    xor rdx, rdx
    mov r8, qword ptr [rip + p_adapter_buf]
    call HeapFree

    mov rcx, rbx
    xor rdx, rdx
    mov r8d, dword ptr [rip + buf_len]
    call HeapAlloc
    test rax, rax
    jz .gw_fail
    mov qword ptr [rip + p_adapter_buf], rax

    mov rcx, qword ptr [rip + p_adapter_buf]
    lea rdx, [rip + buf_len]
    call GetAdaptersInfo

.gw_check:
    cmp eax, 0
    jne .gw_cleanup_fail

    mov rsi, qword ptr [rip + p_adapter_buf]

.gw_loop:
    test rsi, rsi
    jz .gw_cleanup_fail

    lea rax, [rsi + 496]
    cmp byte ptr [rax], 0
    je .next_adapter

    mov rcx, qword ptr [rax]
    mov rdx, 0x00302e302e302e30
    cmp rcx, rdx
    je .next_adapter

    lea rsi, [rsi + 496]
.copy_gw:
    mov al, byte ptr [rsi]
    mov byte ptr [rdi], al
    inc rsi
    inc rdi
    dec r12d
    jz .copy_end
    test al, al
    jnz .copy_gw
.copy_end:
    mov byte ptr [rdi - 1], 0

    mov rcx, rbx
    xor rdx, rdx
    mov r8, qword ptr [rip + p_adapter_buf]
    call HeapFree
    xor eax, eax
    jmp .gw_exit

.next_adapter:
    mov rsi, qword ptr [rsi]
    jmp .gw_loop

.gw_cleanup_fail:
    mov rcx, rbx
    xor rdx, rdx
    mov r8, qword ptr [rip + p_adapter_buf]
    call HeapFree

.gw_fail:
    mov eax, -1

.gw_exit:
    add rsp, 56
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

ping_ip:
    push rbx
    push rdi
    push rsi
    sub rsp, 320

    mov rsi, rcx

    call IcmpCreateFile
    cmp rax, -1
    je .ping_err
    mov rbx, rax

    mov rcx, rsi
    call inet_addr
    mov edi, eax

    mov rcx, rbx
    mov edx, edi
    lea r8, [rip + ping_data]
    mov r9d, 16

    mov qword ptr [rsp + 32], 0
    lea rax, [rsp + 64]
    mov qword ptr [rsp + 40], rax
    mov qword ptr [rsp + 48], 256
    mov qword ptr [rsp + 56], 2000

    call IcmpSendEcho
    test rax, rax
    jz .ping_fail

    cmp dword ptr [rsp + 64 + 4], 0
    jne .ping_fail

    mov esi, dword ptr [rsp + 64 + 8]
    jmp .ping_cleanup

.ping_fail:
    mov esi, -1

.ping_cleanup:
    mov rcx, rbx
    call IcmpCloseHandle
    mov eax, esi
    jmp .ping_exit

.ping_err:
    mov eax, -1

.ping_exit:
    add rsp, 320
    pop rsi
    pop rdi
    pop rbx
    ret

WinMain:
    push rbp
    mov rbp, rsp
    sub rsp, 80

    call FreeConsole

    call GetCommandLineA
    mov rsi, rax

    cmp byte ptr [rsi], '"'
    je .in_quotes
.skip_arg0:
    mov al, byte ptr [rsi]
    test al, al
    jz .no_cmd_arg
    cmp al, ' '
    je .found_space
    inc rsi
    jmp .skip_arg0
.in_quotes:
    inc rsi
.skip_quotes:
    mov al, byte ptr [rsi]
    test al, al
    jz .no_cmd_arg
    cmp al, '"'
    je .end_quotes
    inc rsi
    jmp .skip_quotes
.end_quotes:
    inc rsi
.found_space:
.skip_spaces:
    mov al, byte ptr [rsi]
    cmp al, ' '
    jne .check_param
    inc rsi
    jmp .skip_spaces
.check_param:
    cmp byte ptr [rsi], 0
    je .no_cmd_arg

    lea rdi, [rip + gateway_ip]
    mov ecx, 63
.copy_cmd_ip:
    mov al, byte ptr [rsi]
    test al, al
    jz .cmd_done
    cmp al, ' '
    je .cmd_done
    mov byte ptr [rdi], al
    inc rsi
    inc rdi
    dec ecx
    jnz .copy_cmd_ip
.cmd_done:
    mov byte ptr [rdi], 0
    jmp .has_ip

.no_cmd_arg:
    lea rcx, [rip + gateway_ip]
    mov edx, 64
    call get_default_gateway
    test eax, eax
    jz .has_ip

    lea rcx, [rip + msg_no_gw]
    call write_log
    mov ecx, 1
    call ExitProcess

.has_ip:
    lea rcx, [rip + log_buf]
    lea rdx, [rip + fmt_start]
    lea r8, [rip + gateway_ip]
    call wsprintfA

    lea rcx, [rip + log_buf]
    call write_log

.main_loop:
    lea rcx, [rip + sys_time]
    call GetLocalTime

    lea rcx, [rip + time_str]
    lea rdx, [rip + fmt_time]
    movzx r8, word ptr [rip + sys_time + 0]
    movzx r9, word ptr [rip + sys_time + 2]

    movzx rax, word ptr [rip + sys_time + 6]
    mov qword ptr [rsp + 32], rax
    movzx rax, word ptr [rip + sys_time + 8]
    mov qword ptr [rsp + 40], rax
    movzx rax, word ptr [rip + sys_time + 10]
    mov qword ptr [rsp + 48], rax
    movzx rax, word ptr [rip + sys_time + 12]
    mov qword ptr [rsp + 56], rax

    call wsprintfA

    lea rcx, [rip + gateway_ip]
    call ping_ip

    cmp eax, 0
    jl .ping_failed

    mov rbx, rax
    lea rcx, [rip + log_buf]
    lea rdx, [rip + fmt_succ]
    lea r8, [rip + time_str]
    lea r9, [rip + gateway_ip]
    mov qword ptr [rsp + 32], rbx
    call wsprintfA
    jmp .do_log

.ping_failed:
    lea rcx, [rip + log_buf]
    lea rdx, [rip + fmt_fail]
    lea r8, [rip + time_str]
    lea r9, [rip + gateway_ip]
    call wsprintfA

.do_log:
    lea rcx, [rip + log_buf]
    call write_log

    mov ecx, 60000
    call Sleep

    jmp .main_loop

    mov rsp, rbp
    pop rbp
    ret