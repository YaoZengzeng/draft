.section .data

.equ	SYS_CLOSE, 	6
.equ	SYS_OPEN,	5
.equ	SYS_WRITE,	4
.equ	SYS_EXIT,	1

.equ	O_CREAT_WRONLY_TRUNC,	03101

.equ	LINUX_SYSCALL,	0x80

FILE_NAME:
	.ascii	"heynow.txt\0"

CONTENT:
	.ascii	"Hey diddle diddle!"

.equ	CONTENT_SIZE, 18

.section .bss
.lcomm	FD, 4

.section .text

.globl _start
_start:
	movl	$SYS_OPEN,	%eax
	movl	$FILE_NAME,	%ebx
	movl	$O_CREAT_WRONLY_TRUNC,	%ecx
	movl	$0666,	%edx
	int	$LINUX_SYSCALL
	movl	%eax, FD

	movl	$CONTENT_SIZE, %edx
	movl	$SYS_WRITE, %eax
	movl	FD, %ebx
	movl	$CONTENT, %ecx
	int	$LINUX_SYSCALL

	movl	$SYS_CLOSE, %eax
	movl	FD, %ebx
	int	$LINUX_SYSCALL

	movl	$SYS_EXIT, %eax
	movl	$0, %ebx
	int	$LINUX_SYSCALL

