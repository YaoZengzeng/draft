#as --32 factorial.s -o factorial.o
#ld -m elf_i386 factorial.o -o factorial
.section .data
.section .text

.globl _start
_start:
	pushl 	$4
	call 	factorial
	addl 	$4, 	%esp
	movl 	%eax, 	%ebx
	
	movl 	$1, 	%eax
	int 	$0x80

.type factorial, @function

factorial:
	pushl 	%ebp
	movl 	%esp, 	%ebp
	movl 	8(%ebp),%ebx
	cmpl 	$1, 	%ebx
	je 	recursive_end
	decl 	%ebx
	pushl 	%ebx
	call 	factorial
	addl	$4, %esp
	movl	8(%ebp),%ebx
	imull	%ebx, %eax
	jmp 	end
recursive_end:
	movl 	$1, %eax
end:
	movl 	%ebp, %esp
	popl 	%ebp
	ret

