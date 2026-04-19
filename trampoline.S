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

.global kx_trampoline
.global kx_trampoline_end

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

.text
.p2align 12
kx_trampoline:
	endbr64
	cli

	movl %esi,%edx
	leaq kx_trampoline+0x1000(%rip),%rsp
	movq %rdi,%cr3

	movq $0x001F0000,%rsp
	movq %rsp,%rbx
	movq %rsp,%rdi
	xorl %eax,%eax
	movl $0x00002000,%ecx
	rep stosq

	xorl %ecx,%ecx
	movw %cs,%cx
	andw $-8,%cx
	movabsq $0x00AF9A000000FFFF,%rax
	movq %rax,0x0008(%rbx)
	movq %rax,(%rbx,%rcx)
	movabsq $0x00CF92000000FFFF,%rax
	movq %rax,0x0008(%rbx,%rcx)
	addl $0x0F,%ecx

	xorl %eax,%eax
	movw %ax,%fs
	movw %ax,%gs

	subq $0x0A,%rsp
	movw %cx,(%rsp)
	movq %rbx,2(%rsp)
	subl $0x07,%ecx
	lgdt (%rsp)

	movw %cx,%ss
	movw %cx,%ds
	movw %cx,%es
	addq $0x0A,%rsp

	/* copy blob */
	movl $0x00200000,%edi
	movl $0x08200000,%esi
	movl %edi,%ebx
	movl %edx,%ecx
	rep movsq

	/* copy boot_params and cmdline */
	movl $0x00001000,%edi
	movl $0x08000000,%esi
	movl $0x00000400,%ecx
	rep movsq

	/* LAUNCH THE BOOT-UNLOADER! */
	pushq $0x00000008
	pushq %rbx
	lretq
kx_trampoline_end:

/* vim: set ft=asm: */

