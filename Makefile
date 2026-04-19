# kexec at home 
# Copyright (C) 2026  provide salad 
#  
# This program is free software; you can redistribute it and/or 
# modify it under the terms of the GNU General Public License 
# as published by the Free Software Foundation; either version 2 
# of the License, or (at your option) any later version. 
#  
# This program is distributed in the hope that it will be useful, 
# but WITHOUT ANY WARRANTY; without even the implied warranty of 
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
# GNU General Public License for more details. 
#  
# You should have received a copy of the GNU General Public License 
# along with this program; if not, see 
# <https://www.gnu.org/licenses/>.

obj-m += kexec-at-home.o
kexec-at-home-objs := main.o asm.o


all: asm.o
	$(MAKE) -Ckernel M=$(PWD) modules
clean:
	$(MAKE) -Ckernel M=$(PWD) clean
	$(RM) kern.o trampoline.o

asm.o: kern.o trampoline.o
	$(LD) $(LDFLAGS) -r -o $@ $^

%.o: %.S
	$(AS) $(AFLAGS) -o $@ $<

kern.o: kern.S bootstrap.S bzImage
	$(AS) $(AFLAGS) -o $@ $<

