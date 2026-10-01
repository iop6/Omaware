/* SPDX-License-Identifier: GPL-3.0-or-later
 * Disposable BIOS test guest: write an increasing counter to sector 2.
 * Every DWORD in the sector has the same counter, exposing torn captures.
 */
.code16
.global _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    sti
    cld
    movb %dl, drive
again:
    incl counter
    movl counter, %eax
    movw $0x600, %di
    movw $128, %cx
    rep stosl
    movw $0x600, %bx
    movw $0x0301, %ax
    movw $0x0002, %cx
    xorw %dx, %dx
    movb drive, %dl
    int $0x13
    movb $0x86, %ah
    xorw %cx, %cx
    movw $10000, %dx
    int $0x15
    jmp again
drive: .byte 0
counter: .long 0
.org 510
.word 0xaa55
