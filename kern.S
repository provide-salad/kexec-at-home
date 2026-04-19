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

.global kx_blob
.global kx_blob_end
.global kx_bzImage

.section .rodata
.p2align 12
kx_blob:
.include "bootstrap.S"
.p2align 12
kx_bzImage:
.incbin "bzImage"
.p2align 12
kx_blob_end:

/* vim: set ft=asm: */

