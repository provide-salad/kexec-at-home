<!--

 kexec at home Copyright (C) 2026  provide salad

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 2 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 for more details.

 You should have received a copy of the GNU General Public License along
 with this program; if not, see <https://www.gnu.org/licenses/>.

-->

# kexec at home

Single-architecture DIY kexec.

## IMPORTANT DISCLAIMER

**I do not know what I'm doing, and neither does ChatGPT.  I discourage
you from using this on your real machine, lest you accidentally clobber
your BIOS or brick your machine.  That said, if you accidentally clobber
your BIOS or brick your machine while using this program, it's your
fault and not mine because I warned you.**

**Since making this is my only experience with kernel code, and that
I hacked this together as fast as possible with AI, I don't know how
many invariants I have unknowingly broken in this process. If you find
an invariant that I have unknowingly broken, please create an issue.**

**The kexec will occur at the moment the module is loaded, without
syncing. This means that any storage devices that are currently having
data written to them may have the writes interrupted, resulting in data
corruption. I recommend only running this in an environment where the
root is a tmpfs or similar, such as initrd, and no other block devices
are open.**

---

This project is a goal of making a kernel module that can use minimal
kexec functionality on kernels that do not have kexec enabled.  For fun.

Since I value my time, this only works on x86\_64 (I'm not learning
another assembly variant. Never again), and the target kernel is baked
into the module and not available to change without changing the whole
module, and the initrd/initramfs needs to be embedded in the kernel image.
If this bothers you, you can use `kexec at home` to boot a kernel that
has a real kexec made by people who actually know what they're doing.
Additionally, if you want to configure this to your needs you will have
to change the source code.  Don't worry, there's not much source code
and it shouldn't be too bad.

There are the following files:

|Source file    |meaning                                            |
|---------------|---------------------------------------------------|
|main.c         |module entry point                                 |
|trampoline.S   |boot-unloader entry point after disabling Linux    |
|bootstrap.S    |boot-unloader and bootloader                       |
|bzImage        |target kernel to boot                              |
|kernel         |symlink this to kernel build directory             |

---

## Requirement

The source kernel needs to be compiled such that the `set_memory_x`
function is exported, and `/sys/kernel/boot_params/data` is enabled.
Other than that it should work with basically most kernel, as long as
the kernel does not have strict kernel module signatures enabled, or
kernel module loading disabled.

## FAQ: "It's not work."

That's great! If you know it's not working, and you know why, please
open a issue on the git hub. If you know it's not working, you know why,
and you know how to fix it, please open a pull request to help make it
work on as many individuals' machines as possible so that everyone says
"it works on my machine" and nobody complains and everyone is happy.



