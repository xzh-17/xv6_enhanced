#pragma once

// System call numbers
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_sleep  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
#define SYS_waitpid 22
#define SYS_sigreturn 23
#define SYS_signal 24
#define SYS_fgproc 25
#define SYS_alarm 26

#define SYS_ioctl    27
#define SYS_socket   28
#define SYS_connect  29
#define SYS_bind     30
#define SYS_listen   31
#define SYS_accept   32
#define SYS_recv     33
#define SYS_send     34
#define SYS_recvfrom 35
#define SYS_sendto   36
#define SYS_error    37
#define SYS_getsockopt 38
#define SYS_gethostbyname 39
#define SYS_gethostbyaddr 40