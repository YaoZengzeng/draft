#as --32 toupper.s -o toupper.o
#ld -m elf_i386 toupper.o -o toupper
#if argc is 3, input from argv2 and output to argv3,
#otherwise, read from STDIN and output to STDOUT

.section .data

.equ	SYS_CLOSE,	6
.equ 	SYS_OPEN, 	5
.equ	SYS_WRITE,	4
.equ	SYS_READ,	3
.equ	SYS_EXIT,	1

.equ	O_RDONLY,	0
.equ	O_CREAT_WRONLY_TRUNC,	03101

.equ	STDIN,	0
.equ	STDOUT,	1
.equ	STDERR,	2

.equ	LINUX_SYSCALL, 0x80

.equ	END_OF_FILE,	0

.equ	NUMBER_ARGUMENTS, 2

.section .bss
.equ BUFFER_SIZE, 500
.lcomm	BUFFER_DATA, BUFFER_SIZE

.lcomm	FD_IN, 4
.lcomm	FD_OUT, 4

.section .text

.equ	ST_ARGC, 0
.equ	ST_ARGV_0, 4
.equ	ST_ARGV_1, 8
.equ	ST_ARGV_2, 12

.globl	_start
_start:
	movl 	%esp, %ebp
	movl	ST_ARGC(%ebp), %eax
	cmpl	$3, %eax
	je	open_files
	movl	$0, FD_IN
	movl	$1, FD_OUT
	jmp	read_loop_begin	

open_files:
open_fd_in:
	movl 	$SYS_OPEN,	%eax
	movl 	ST_ARGV_1(%ebp), %ebx
	movl 	$O_RDONLY, %ecx
	movl 	$0666, %edx
	int 	$LINUX_SYSCALL

store_fd_in:
	movl 	%eax, FD_IN

open_fd_out:
	movl 	$SYS_OPEN, %eax
	movl 	ST_ARGV_2(%ebp), %ebx
	movl 	$O_CREAT_WRONLY_TRUNC, %ecx
	movl 	$0666, %edx
	int 	$LINUX_SYSCALL

store_fd_out:
	movl 	%eax, FD_OUT

read_loop_begin:
	movl 	$SYS_READ, %eax
	movl 	FD_IN,	%ebx
	movl 	$BUFFER_DATA, %ecx
	movl 	$BUFFER_SIZE, %edx
	int 	$LINUX_SYSCALL

	#check for end of file marker
	cmpl 	$END_OF_FILE, %eax
	jle	end_loop

continue_read_loop:
	pushl	$BUFFER_DATA
	pushl	%eax
	call 	convert_to_upper
	popl	%eax
	addl	$4, %esp

	#write the block out to the output file
	movl	%eax, %edx
	movl	$SYS_WRITE, %eax
	movl	FD_OUT, %ebx
	movl	$BUFFER_DATA, %ecx
	int 	$LINUX_SYSCALL

	jmp	read_loop_begin

end_loop:
	movl	$SYS_CLOSE, %eax
	movl	FD_OUT, %ebx
	int	$LINUX_SYSCALL

	movl	$SYS_CLOSE, %eax
	movl	FD_IN, %ebx
	int	$LINUX_SYSCALL

	movl	$SYS_EXIT, %eax
	movl	$0, %ebx
	int	$LINUX_SYSCALL

.equ	LOWERCASE_A, 'a'
.equ	LOWERCASE_Z, 'z'
.equ	UPPER_CONVERSION, 'A' - 'a'

.equ	ST_BUFFER_LEN, 8
.equ	ST_BUFFER, 12

convert_to_upper:
	pushl	%ebp
	movl	%esp, %ebp

	movl	ST_BUFFER(%ebp), %eax
	movl	ST_BUFFER_LEN(%ebp), %ebx
	movl	$0, %edi

	cmpl	$0, %ebx
	je	end_convert_loop

convert_loop:
	movb	(%eax, %edi, 1), %cl

	cmpb	$LOWERCASE_A, %cl
	jl	next_byte
	cmpb	$LOWERCASE_Z, %cl
	jg	next_byte
	
	addb	$UPPER_CONVERSION, %cl
	movb	%cl, (%eax, %edi, 1)

next_byte:
	incl	%edi
	cmpl	%edi, %ebx
	jne	convert_loop

end_convert_loop:
	movl	%ebp, %esp
	popl	%ebp
	ret

