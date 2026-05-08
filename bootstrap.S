/* kexec at home
 * Copyright (C) 2026  provide salad
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <https://www.gnu.org/licenses/>.
 */

.macro __reset
	movb $0x06,%al
	movw $0x0CF9,%dx
	outb %al,%dx
	__hang
.endm

.macro __hang
	hlt
	jmp .
.endm
	endbr64

	leal .Lgdt64(%rip),%esi
	movl $0x00003000,%edi
	movl $.Lgdt64_len,%ecx
	movl %edi,%ebx
	cld
	rep movsq

	subq $0x0A,%rsp
	movw $.Lgdt64_size,(%rsp)
	movq %rbx,0x02(%rsp)
	lgdt (%rsp)

	movw $.Ls_data,%ax
	movw %ax,%ss
	movw %ax,%ds
	movw %ax,%es
	movq $0x00010000,%rsp /* move stack to low 640KiB */

	leal .Lcode32(%rip),%eax
	pushq $.Ls_code32
	pushq %rax
	lretq

.code32
.Lcode32:
	.macro __rell imm32,r32
		call .L0\@
	.L0\@:	popl \r32
		addl $\imm32-.L0\@,\r32
	.endm

	.macro __printl _32
		pushl %eax
		pushl %edx
		movl \_32,%eax
		movb %al,%dl
		andb $0x0F,%dl
		addb $'0,%dl
		cmpb $'9,%dl
		jna .L0\@
		addb $'A-'0-10,%dl
	.L0\@:
		decl %esp
		movb %dl,(%esp)
	.irpc i,0123456
		shrl $0x04,%eax
		movb %al,%dl
		andb $0x0F,%dl
		addb $'0,%dl
		cmpb $'9,%dl
		jna .L0\@_\i
		addb $'A-'0-10,%dl
	.L0\@_\i:
		decl %esp
		movb %dl,(%esp)
	.endr
		movw $0x03F8,%dx
		movb $'0,%al
		outb %al,%dx
		movb $'x,%al
		outb %al,%dx
	.rept 8
		movb (%esp),%al
		incl %esp
		outb %al,%dx
	.endr
		movb $'\n,%al
		outb %al,%dx
		popl %edx
		popl %eax
	.endm

	/* compatibilitymöde */
	movl %cr0,%eax
	andl $~0x80000000,%eax
	movl %eax,%cr0
	movl $0xC0000080,%ecx
	rdmsr
	andl $~0x00000100,%eax
	wrmsr

	/* protectedmöde */

	movl $0x00080000,%edi
	movl $0x00200000,%esi
	movl $(kx_bzImage-kx_blob)>>2,%ecx
	cld
	rep movsl
	pushl $1f-kx_blob+0x80000
	retl

1:
	movzbl 0x002001F1+kx_bzImage-kx_blob,%eax /* setup_sects */
	shll $0x09,%eax

	/* bootstrap code + bzImage (was) loaded at 0x200000 */
	movl $0x00100000,%edi
	movl $0x00200200+kx_bzImage-kx_blob,%esi
	movl $(kx_blob_end-kx_bzImage-0x00000200)>>0x02,%ecx
	addl %eax,%esi
	shrl $0x02,%eax
	subl %eax,%ecx
	cld
	rep movsl

	/* CRASH FIX */

	/* mask 8259 PIC */
	movb $0xFF,%al
	outb %al,$0x21
	outb %al,$0xA1

	/* mask IOAPIC entries */
	movl $0x01,0xFEC00000
	movl 0xFEC00010,%eax
	shrl $16,%eax
	movzbl %al,%ecx
1:
	leal (,%ecx,2),%edi
	addl $0x0E,%edi
	movl %edi,0xFEC00000
	movl 0xFEC00010,%eax
	movl %edi,0xFEC00000
	orl $0x00010000,%eax
	movl %eax,0xFEC00010
	decl %ecx
	jnz 1b

	/* stop LAPIC timer */
	movl 0xFEE00320,%eax
	orl $0x00010000,%eax
	movl %eax,0xFEE00320
	movl $0,0xFEE00380
	movl $0,0xFEE00390
	movl $0,0xFEE003E0

	/* no idea what this does but purgatory does it so it must be good */
	xorl %eax,%eax
	movl %eax,%cr4

	/* TODO: stop device interrupt sources */

	/* TODO: drain interrupt controller state */
/*	sti */
/*	nop */
/*	nop */
/*	cli */

	/* --- */

	/* linux boot protocol */
	movl $0x00001000,%esi
	xorl %ebx,%ebx
	xorl %ebp,%ebp
	xorl %edi,%edi

	pushl $0x00100000 /* hardcoded for now */
	retl

.p2align 3
.Lgdt64:
	.quad 0 /* null */
.Ls_code64 = . - .Lgdt64
	.quad 0x00AF9A000000FFFF
.Ls_code32 = . - .Lgdt64
	.quad 0x00CF9A000000FFFF
.Ls_data = . - .Lgdt64
	.quad 0x00CF92000000FFFF
.Lgdt64_size = . - .Lgdt64 - 1
.Lgdt64_len = (. - .Lgdt64) >> 3

/* vim: set ft=asm: */

