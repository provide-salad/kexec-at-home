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

#include <linux/sched.h>

// <linux/sched.h> must appear before <asm/uaccess.h> to work on my machine
// Don't ask me why, I have no idea.

#include <asm/bootparam.h>
#include <asm/e820/api.h>
#include <asm/e820/types.h>
#include <asm/io.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/set_memory.h>
#include <asm/setup.h>
#include <asm/uaccess.h>

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#define MSR_IA32_PERF_GLOBAL_CTRL 0x38F
#define MSR_IA32_PERF_GLOBAL_STATUS 0x38E
#define MSR_IA32_PERF_GLOBAL_OVF_CTRL 0x390
#define MSR_IA32_FIXED_CTR_CTRL 0x38D

#define MSR_IA32_FIXED_CTR0 0x309
#define MSR_IA32_FIXED_CTR1 0x30A
#define MSR_IA32_FIXED_CTR2 0x30B

#define MSR_IA32_PERFEVTSEL0 0x186

#ifdef __STDC_VERSION__
#define BOOL_TYPE _Bool
#else
#define BOOL_TYPE int
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
#define DIR_CONTINUE 0
#define DIR_BREAK 1
#else
#define DIR_CONTINUE 1
#define DIR_BREAK 0
#endif

#define PAGETAB_IDX(p, i) (((p) >> (i)) & 0x1FF)
#define PML4_IDX(p) PAGETAB_IDX(p, 39)
#define PDPT_IDX(p) PAGETAB_IDX(p, 30)
#define PD_IDX(p) PAGETAB_IDX(p, 21)
#define PT_IDX(p) PAGETAB_IDX(p, 12)

#define BLOB_SIZE(b) ((size_t)((uintptr_t)&b##_end - (uintptr_t)&b))
#define BLOB_PAGES(b) (BLOB_SIZE(b) >> 12)
#define PAGE_PRESENT 0x1
#define PAGE_RW 0x2
#define PAGE_PS 0x80

#define MAP_PAGES 64

#define WRITE_MSR(name, value)                                                 \
	wrmsr_safe((name), (u64)(value) & 0xFFFFFFFFULL, (u64)(value) >> 32)

static cpumask_t kx_cpus;

struct kx_data {
	void (*trampoline)(phys_addr_t, size_t);
	phys_addr_t pml4;
};

struct kx_data kx_data;
static int kx_jump(void *);
static void kx_kill_cpu(void *);
static void kx_fix_pmu_state(void);

extern const char kx_blob[];
extern const char kx_blob_end[];
extern const char kx_bzImage[];
extern const char kx_trampoline[];
extern const char kx_trampoline_end[];

static const char cmdline[PAGE_SIZE] = "";
static struct boot_params *g_bp;
static int bp_err;
static int bp_idx;

MODULE_AUTHOR("?");
MODULE_DESCRIPTION("?");
MODULE_LICENSE("GPL");

static const char hex[16] = "0123456789ABCDEF";

static unsigned long kx_read_cr4(void) {
	unsigned long val;
	asm volatile("mov %%cr4,%0" : "=r"(val));
	return val;
}

static void kx_write_cr4(unsigned long val) {
	asm volatile("mov %0,%%cr4" ::"r"(val) : "memory");
}

// TODO: find a better way ?
static void *kx_safe_addr(void) {
	void *ptr;
	do {
		struct page *const page = alloc_page(GFP_KERNEL);
		ptr = __va(page_to_phys(page));
	} while ((uintptr_t)ptr < (MAP_PAGES << 21) ||
			 virt_to_phys(ptr) < (MAP_PAGES << 21));
	return ptr;
}

static int f_read_u64(const char *const name, u64 *const out) {
	struct file *f;
	char s[32];
	loff_t i;
	ssize_t r;

	i = 0;
	f = filp_open(name, O_RDONLY, 0);
	if (IS_ERR(f)) {
		printk("(ERR) Failed to open file %s", name);
		return PTR_ERR(f);
	}

	r = kernel_read(f, s, sizeof(s) - 1, &i);
	filp_close(f, NULL);

	if (r < 0) {
		printk("(ERR) Failed to read file %s", name);
		return r;
	}

	s[r] = 0;
	return kstrtoull(s, 0, out);
}

static BOOL_TYPE yoink_e820_cb(struct dir_context *ctx, const char *name,
							   int name_size, loff_t offset, u64 ino,
							   unsigned type) {
	struct file *f;
	loff_t i;
	ssize_t r;
	enum e820_type t;
	u64 start, end;
	static char path[4096];

	printk("memmap file: %s", name);

	if (name[0] == '.') {
		return DIR_CONTINUE;
	}

	printk("did we get here?");

	snprintf(path, sizeof(path), "/sys/firmware/memmap/%s/start", name);
	bp_err = f_read_u64(path, &start);
	if (bp_err < 0) {
		return DIR_BREAK;
	}
	snprintf(path, sizeof(path), "/sys/firmware/memmap/%s/end", name);
	bp_err = f_read_u64(path, &end);
	if (bp_err < 0) {
		return DIR_BREAK;
	}
	snprintf(path, sizeof(path), "/sys/firmware/memmap/%s/type", name);
	f = filp_open(path, O_RDONLY, 0);
	i = 0;
	if (IS_ERR(f)) {
		printk("(ERR) Failed to open file %s", path);
		bp_err = PTR_ERR(f);
		return DIR_BREAK;
	}
	r = kernel_read(f, path, sizeof(path) - 1, &i);
	filp_close(f, NULL);
	if (r < 0) {
		printk("(ERR) Failed to read file %s", path);
		bp_err = r;
		return DIR_BREAK;
	}
	path[r - 1] = 0;
	if (!strcmp("System RAM", path)) {
		t = E820_TYPE_RAM;
	} else if (!strcmp("ACPI Tables", path)) {
		t = E820_TYPE_ACPI;
	} else if (!strcmp("Unusable memory", path)) {
		t = E820_TYPE_RESERVED;
	} else if (!strcmp("reserved", path)) {
		t = E820_TYPE_RESERVED;
	} else if (!strcmp("Reserved", path)) {
		t = E820_TYPE_RESERVED;
	} else if (!strcmp("Unknown E820 type", path)) {
		t = E820_TYPE_RESERVED;
	} else if (!strcmp("ACPI Non-volatile Storage", path)) {
		t = E820_TYPE_NVS;
	} else if (!strcmp("Uncached RAM", path)) {
		t = E820_TYPE_RAM;
	} else if (!strcmp("Persistent memory (legacy)", path)) {
		t = E820_TYPE_PRAM;
	} else if (!strcmp("Persistent memory", path)) {
		t = E820_TYPE_PMEM;
	} else {
		t = E820_TYPE_RESERVED;
	}

	printk("e820: [ %16llx - %-16llx ] %s", start, end, path);

	// TODO: Sort these?
	g_bp->e820_table[bp_idx].addr = start;
	g_bp->e820_table[bp_idx].size = end - start + 1;
	g_bp->e820_table[bp_idx++].type = t;
	return DIR_CONTINUE;
}

static int yoink_e820(struct boot_params *const bp) {
	struct file *f;
	loff_t loff;
	struct dir_context ctx = {
		&yoink_e820_cb,
	};

	printk("Grabbing e820");

	loff = 0;
	g_bp = bp;
	bp_idx = 0;
	bp_err = 0;
	f = filp_open("/sys/firmware/memmap", O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(f)) {
		printk("(ERR) Failed to open file /sys/firmware/memmap");
		return PTR_ERR(f);
	}
	f->f_op->iterate_shared(f, &ctx);
	filp_close(f, NULL);
	if (bp_err < 0) {
		printk("(ERR) Failed to read file /sys/firmware/memmap");
		return bp_err;
	}
	bp->e820_entries = bp_idx;
	return 0;
}

__attribute__((unused, deprecated)) static int
get_boot_params_unsafe(struct boot_params *const bp) {
	struct file *f;
	loff_t loff;
	ssize_t rbytes;

	loff = 0;
	f = filp_open("/sys/kernel/boot_params/data", O_RDONLY, 0);
	rbytes = kernel_read(f, (void *)bp, sizeof(struct boot_params), &loff);
	filp_close(f, NULL);
	if (rbytes != sizeof(struct boot_params)) {
		return 1;
	}
	return 0;
}

__attribute__((unused)) static void kx_debug_examine(const void *const addr) {
	int i, j, byte;
	const char *bytes;
	char line[] = "00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00";

	bytes = (const char *)addr;
	for (i = 0; i < 32; ++i) {
		for (j = 0; j < 8; ++j) {
			byte = bytes[(i << 4) | j];
			line[j * 3] = hex[byte >> 4];
			line[j * 3 + 1] = hex[byte & 0xF];
		}
		for (j = 8; j < 16; ++j) {
			byte = bytes[(i << 4) | j];
			line[j * 3 + 1] = hex[byte >> 4];
			line[j * 3 + 2] = hex[byte & 0xF];
		}
		printk(line);
	}
}

__attribute__((unused)) static void
print_zeropage(struct boot_params *const bp) {
#define LOG_BP(x) printk("boot_params: " #x " = 0x%08x", bp->x)
#define LOG_BP64(x) printk("boot_params: " #x " = 0x%016llx", bp->x)
	int i;

	LOG_BP64(tboot_addr);
	LOG_BP(ext_ramdisk_image);
	LOG_BP(ext_ramdisk_size);
	LOG_BP(ext_cmd_line_ptr);
	LOG_BP(alt_mem_k);
	LOG_BP(scratch);
	LOG_BP(e820_entries);
	LOG_BP(eddbuf_entries);
	LOG_BP(edd_mbr_sig_buf_entries);
	LOG_BP(kbd_status);
	LOG_BP(secure_boot);
	LOG_BP(sentinel);

	// screen_info
	LOG_BP(screen_info.orig_x);
	LOG_BP(screen_info.orig_y);
	LOG_BP(screen_info.ext_mem_k);
	LOG_BP(screen_info.orig_video_page);
	LOG_BP(screen_info.orig_video_mode);
	LOG_BP(screen_info.orig_video_cols);
	LOG_BP(screen_info.flags);
	LOG_BP(screen_info.unused2);
	LOG_BP(screen_info.orig_video_ega_bx);
	LOG_BP(screen_info.unused3);
	LOG_BP(screen_info.orig_video_lines);
	LOG_BP(screen_info.orig_video_isVGA);
	LOG_BP(screen_info.orig_video_points);
	LOG_BP(screen_info.lfb_width);
	LOG_BP(screen_info.lfb_height);
	LOG_BP(screen_info.lfb_depth);
	LOG_BP(screen_info.lfb_base);
	LOG_BP(screen_info.lfb_size);
	LOG_BP(screen_info.cl_magic);
	LOG_BP(screen_info.cl_offset);
	LOG_BP(screen_info.lfb_linelength);
	LOG_BP(screen_info.red_size);
	LOG_BP(screen_info.red_pos);
	LOG_BP(screen_info.green_size);
	LOG_BP(screen_info.green_pos);
	LOG_BP(screen_info.blue_size);
	LOG_BP(screen_info.blue_pos);
	LOG_BP(screen_info.rsvd_size);
	LOG_BP(screen_info.rsvd_pos);
	LOG_BP(screen_info.vesapm_seg);
	LOG_BP(screen_info.vesapm_off);
	LOG_BP(screen_info.pages);
	LOG_BP(screen_info.vesa_attributes);
	LOG_BP(screen_info.capabilities);
	LOG_BP(screen_info.ext_lfb_base);

	// apm_bios_info
	LOG_BP(apm_bios_info.version);
	LOG_BP(apm_bios_info.cseg);
	LOG_BP(apm_bios_info.offset);
	LOG_BP(apm_bios_info.cseg_16);
	LOG_BP(apm_bios_info.dseg);
	LOG_BP(apm_bios_info.flags);
	LOG_BP(apm_bios_info.cseg_len);
	LOG_BP(apm_bios_info.cseg_16_len);
	LOG_BP(apm_bios_info.dseg_len);

	// ist_info
	LOG_BP(ist_info.signature);
	LOG_BP(ist_info.command);
	LOG_BP(ist_info.event);
	LOG_BP(ist_info.perf_level);

	// olpc_ofw_header
	LOG_BP(olpc_ofw_header.ofw_magic);
	LOG_BP(olpc_ofw_header.ofw_version);
	LOG_BP(olpc_ofw_header.cif_handler);
	LOG_BP(olpc_ofw_header.irq_desc_table);

	// efi_info
	LOG_BP(efi_info.efi_loader_signature);
	LOG_BP(efi_info.efi_systab);
	LOG_BP(efi_info.efi_memdesc_size);
	LOG_BP(efi_info.efi_memdesc_version);
	LOG_BP(efi_info.efi_memmap);
	LOG_BP(efi_info.efi_memmap_size);
	LOG_BP(efi_info.efi_systab_hi);
	LOG_BP(efi_info.efi_memmap_hi);

	// e820_table
	for (i = 0; i < E820_MAX_ENTRIES_ZEROPAGE; ++i) {
		printk("boot_params: e820_table[%d] = { 0x%016llx, 0x%016llx, "
			   "0x%08x }",
			   i, bp->e820_table[i].addr, bp->e820_table[i].size,
			   bp->e820_table[i].type);
	}

	// eddbuf
	for (i = 0; i < EDDMAXNR; ++i) {
		printk("boot_params: eddbuf[%d] = TODO", i);
	}
}

// blindly copied from ChatGPT, no idea what this does
__attribute__((unused)) static int kx_dev_poweroff(struct device *const dev,
												   void *data) {
	pm_runtime_get_sync(dev);

	if (dev->driver && dev->driver->shutdown) {
		dev->driver->shutdown(dev);
	}

	pm_runtime_put_sync(dev);
	return 0;
}

static void kx_setup(void) {
	phys_addr_t trampoline_phys, pml4_phys, pdpt_phys, pd_phys;
	void (*trampoline)(phys_addr_t, size_t);
	int perhaps_reboot_cpu;
	uint64_t *pml4, *pdpt, *pdpt2, *pd, *pd2, *pt, *pt2, *pt3;
	struct boot_params *bp;
	void *page;
	char *cs;
	int i, j;

	int trampoline_pml4, trampoline_pdpt, trampoline_pd, trampoline_pt;
	uintptr_t trampoline_virt;

	// allocate memory
	do {
		trampoline = (void (*)(phys_addr_t, size_t))kx_safe_addr();
	} while (
		(uintptr_t)trampoline <
		(((MAP_PAGES + 1) << 21) | ((BLOB_PAGES(kx_blob) + 0x1FF) >> 9 << 21)));

	pml4 = (uint64_t *)kx_safe_addr();
	pdpt = (uint64_t *)kx_safe_addr();
	pd = (uint64_t *)kx_safe_addr();
	pt = (uint64_t *)kx_safe_addr();
	pt3 = (uint64_t *)kx_safe_addr();
	bp = (struct boot_params *)kx_safe_addr();
	cs = (char *)kx_safe_addr();

	// populate boot params
	memset(bp, 0, sizeof(boot_params));
	memcpy(cs, cmdline, PAGE_SIZE);
	if (yoink_e820(bp)) {
		printk("!! WARNING !! AN ERROR HAS BEEN DETECTED");
		return;
	}
	memcpy(&bp->hdr, kx_bzImage + 0x000001F1, sizeof(struct setup_header));
	bp->hdr.vid_mode = 0xFFFE; // VGA
	bp->hdr.type_of_loader = 0xFF;
	bp->hdr.loadflags &= bp->hdr.loadflags & ~0xA0;
	bp->hdr.setup_move_size = 0x0200;
	bp->hdr.ramdisk_image = 0x00000000;
	bp->hdr.ramdisk_size = 0x00000000;
	bp->hdr.heap_end_ptr = 0x0000;
	bp->hdr.cmd_line_ptr = 0x2000;

	bp->ext_ramdisk_image = 0x00000000;
	bp->ext_ramdisk_size = 0x00000000;
	bp->ext_cmd_line_ptr = 0x00000000;

	// physical address
	trampoline_phys = virt_to_phys(trampoline);
	pml4_phys = virt_to_phys(pml4);
	pdpt_phys = virt_to_phys(pdpt);
	pd_phys = virt_to_phys(pd);

	// setup page table
	memset(pml4, 0, PAGE_SIZE);
	memset(pdpt, 0, PAGE_SIZE);
	memset(pd, 0, PAGE_SIZE);
	memset(pt, 0, PAGE_SIZE);
	memset(pt3, 0, PAGE_SIZE);
	pml4[0] = pdpt_phys | PAGE_PRESENT | PAGE_RW;
	pdpt[0] = pd_phys | PAGE_PRESENT | PAGE_RW;
	for (i = 0; i < MAP_PAGES; ++i) {
		pd[i] = ((uintptr_t)i << 21) | PAGE_PRESENT | PAGE_RW | PAGE_PS;
	}
	pd[MAP_PAGES] = virt_to_phys(pt3) | PAGE_PRESENT | PAGE_RW;
	pt3[0] = virt_to_phys(bp) | PAGE_PRESENT | PAGE_RW;
	pt3[1] = virt_to_phys(cs) | PAGE_PRESENT | PAGE_RW;
	for (i = 0; i < (BLOB_PAGES(kx_blob) >> 9); ++i) {
		pt2 = (uint64_t *)kx_safe_addr();
		pd[MAP_PAGES + 1 + i] = virt_to_phys(pt2) | PAGE_PRESENT | PAGE_RW;
		for (j = 0; j < 0x200; ++j) {
			page = kx_safe_addr();
			memcpy(page, &kx_blob[((size_t)i << 21) | ((size_t)j << 12)],
				   PAGE_SIZE);
			pt2[j] = virt_to_phys(page) | PAGE_PRESENT | PAGE_RW;
		}
	}
	if (BLOB_PAGES(kx_blob) & 0x1FF) {
		pt2 = (uint64_t *)kx_safe_addr();
		memset(pt2, 0, PAGE_SIZE);
		pd[MAP_PAGES + 1 + (BLOB_PAGES(kx_blob) >> 9)] =
			virt_to_phys(pt2) | PAGE_PRESENT | PAGE_RW;
		for (i = 0; i < (BLOB_PAGES(kx_blob) & 0x1FF); ++i) {
			page = kx_safe_addr();
			memcpy(
				page,
				&kx_blob[(BLOB_PAGES(kx_blob) >> 9 << 21) | ((size_t)i << 12)],
				PAGE_SIZE);
			//			kx_debug_examine(page);
			pt2[i] = virt_to_phys(page) | PAGE_PRESENT | PAGE_RW;
		}
	}

	// identically map trampoline page
	trampoline_virt = (uintptr_t)trampoline;
	trampoline_pml4 = PML4_IDX(trampoline_virt);
	trampoline_pdpt = PDPT_IDX(trampoline_virt);
	trampoline_pd = PD_IDX(trampoline_virt);
	trampoline_pt = PT_IDX(trampoline_virt);
	if (trampoline_pml4) {
		pdpt2 = (uint64_t *)kx_safe_addr();
		pd2 = (uint64_t *)kx_safe_addr();
		memset(pdpt2, 0, PAGE_SIZE);
		memset(pd2, 0, PAGE_SIZE);
		pml4[trampoline_pml4] = virt_to_phys(pdpt2) | PAGE_PRESENT | PAGE_RW;
		pdpt2[trampoline_pdpt] = virt_to_phys(pd2) | PAGE_PRESENT | PAGE_RW;
		pd2[trampoline_pd] = virt_to_phys(pt) | PAGE_PRESENT | PAGE_RW;
	} else if (trampoline_pdpt) {
		pd2 = (uint64_t *)kx_safe_addr();
		memset(pd2, 0, PAGE_SIZE);
		pdpt[trampoline_pdpt] = virt_to_phys(pd2) | PAGE_PRESENT | PAGE_RW;
		pd2[trampoline_pd] = virt_to_phys(pt) | PAGE_PRESENT | PAGE_RW;
	} else {
		pd[trampoline_pd] = virt_to_phys(pt) | PAGE_PRESENT | PAGE_RW;
	}
	pt[trampoline_pt] = trampoline_phys | PAGE_PRESENT | PAGE_RW;

	// copy trampoline
	memcpy(trampoline, &kx_trampoline, BLOB_SIZE(kx_trampoline));
	set_memory_x((uintptr_t)trampoline, 1);
	// TODO: flush_tlb_kernel_range ?

	printk("----------------");
	// Enable this if you are suspicious of your boot_params
	//	print_zeropage(bp);
	//	return;

	//	for_each_possible_cpu(cpu) {
	//		per_cpu(kx_cpu_is_complete, cpu) = 1;
	//	}
	cpumask_copy(&kx_cpus, cpu_online_mask);
	kx_data.trampoline = trampoline;
	kx_data.pml4 = pml4_phys;
	smp_mb(); // make sure all CPUs in `cpus` get the correct data

	perhaps_reboot_cpu = cpumask_first(cpu_online_mask);
	printk("CPU: %d\n", smp_processor_id());
	//	set_cpus_allowed_ptr(current, cpumask_of(perhaps_reboot_cpu));
	printk("CPU: %d\n", smp_processor_id());
	stop_machine(kx_jump, NULL, cpumask_of(perhaps_reboot_cpu));
	//	stop_machine(kx_jump, NULL, NULL);
	return;
}

__attribute__((unused)) static void kx_kill_cpu(void *const _) {
	(void)_;

	preempt_disable();
	local_irq_disable();
	kx_fix_pmu_state();

	while (1) {
		cpu_relax();
	}
}

__attribute__((unused)) static void kx_fix_pmu_state(void) {
	int i;

	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL) {

		WRITE_MSR(MSR_IA32_FIXED_CTR_CTRL, 0x0);
		WRITE_MSR(MSR_IA32_DEBUGCTLMSR, 0x0);
		WRITE_MSR(MSR_IA32_PERF_GLOBAL_OVF_CTRL, 0xC00000070000000FULL);
		WRITE_MSR(MSR_IA32_PERF_GLOBAL_CTRL, 0x0);
		//	WRITE_MSR(MSR_IA32_PERF_GLOBAL_STATUS,0x0); // this one is read-only
		//so don't do that !
		WRITE_MSR(MSR_IA32_FIXED_CTR0, 0x0);
		WRITE_MSR(MSR_IA32_FIXED_CTR1, 0x0);
		WRITE_MSR(MSR_IA32_FIXED_CTR2, 0x0);

		for (i = 0; i < 4; ++i) {
			WRITE_MSR(MSR_IA32_PERFEVTSEL0 + i, 0);
			WRITE_MSR(MSR_IA32_PMC0 + i, 0);
		}
	}
}

// "i am become kexec, destroyer of kernels." --provide salad
static int kx_jump(void *_) {
	struct pci_dev *dev = NULL;
	u16 _16;
	cpumask_t cpus;
	int i;
	(void)_;

	system_state = SYSTEM_RESTART;
	local_irq_disable();
	preempt_disable();

	// What real kexec does:
	// kernel_restart_prepare(NULL);
	//  -> blocking_notifier_call_chain(&reboot_notifier_list, SYS_RESTART,
	//  NULL);
	//	(reboot_notifier_list is not exported)
	//  -> system_state = SYSTEM_RESTART;
	//  -> usermodehelper_disable();
	//	-> down_write(&umhelper_sem);
	//	    (umhelper_sem is not exported)
	//	-> usermodehelper_disabled = UMH_DISABLE;
	//	    (usermodehelper_disabled is not exported)
	//	-> up_write(&umhelper_sem);
	//	    (umhelper_sem is not exported)
	//	->
	//wait_event_timeout(running_helpers_waitq,atomic_read(&running_helpers),RUNNING_HELPERS_TIMEOUT);
	//	    (running_helpers_waitq is not exported)
	//	    (running_helpers is not exported)
	//  -> device_shutdown();
	//	(not exported)
	// migrate_to_reboot_cpu();
	//  -> set_cpus_allowed_ptr(current, cpumask_of(reboot_cpu));
	//	(reboot_cpu is not exported)
	// cpu_hotplug_enable();
	// machine_shutdown();
	//  -> clear_IO_APIC();
	//	-> clear_IO_APIC_pin(...);
	//	    -> native_io_apic_read(...);
	//		-> io_apic_base();
	//		    -> __fix_to_virt(...);
	//		    -> mpc_ioapic_addr(...);
	//			-> ioapics[ioapic_idx].mp_config.apicaddr;
	//			    -> (ioapics is not exported)
	//		-> writel(...);
	//		-> readl(...);
	//	    -> native_io_apic_write(...);
	//		-> io_apic_base();
	//		-> writel(...);
	//	    -> __eoi_ioapic_pin(...);
	//		-> mpc_ioapic_ver(...);
	//		    -> ioapics[...].mp_config.apicver;
	//			-> (ioapics is not exported)
	//		-> io_apic_eoi(...);
	//		    -> io_apic_base(...);
	//		    -> writel(...);
	//		-> native_io_apic_read(...);
	//		-> native_io_apic_read(...);
	//	    -> ioapic_mask_entry(...);
	//		-> native_io_apic_write(...);
	//  -> local_irq_disable();
	//  -> stop_other_cpus();
	//  -> lapic_shutdown();
	//  -> restore_irq_boot_mode();
	//  -> hpet_disable();
	// machine_kexec();

	cpumask_copy(&cpus, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), &cpus);
	on_each_cpu_mask(&cpus, kx_kill_cpu, NULL, 0);

	kx_fix_pmu_state();

	for (i = 0; i < 0x1000000; ++i) {
		cpu_relax();
	}

	//	outb(0xFF,0x21);
	//	outb(0xFF,0xA1);

	//	hw_breakpoint_disable(); // not exported :c
	//	cet_disable(); // not exported :c
	// Don't worry, a second kexec will clean up the mess I made, hopefully,
	// perhaps.
	smp_send_stop();

	//	bus_for_each_dev(&pci_bus_type, NULL, NULL, kx_dev_poweroff);
	wmb();
	kx_write_cr4(kx_read_cr4() & ~(X86_CR4_SMEP | X86_CR4_SMAP));
	// TODO: pci_stop_and_remove_bus_device ?
	// TODO: pci_set_power_state ?

	//	apic->send_IPI_mask(cpumask, APIC_DM_INIT);

	(void)dev;
	(void)_16;
	for_each_pci_dev(dev) {
		if (!pci_is_enabled(dev)) {
			continue;
		}

		disable_irq(dev->irq);
		(void)pci_read_config_word(dev, PCI_COMMAND, &_16);
		pci_clear_master(dev);
		(void)pci_read_config_word(dev, PCI_COMMAND, &_16);
	}

	smp_mb();

	stop_other_cpus(); // If the other CPU are still alive, kill them now.

	// 💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀💀
	kx_data.trampoline(kx_data.pml4, BLOB_SIZE(kx_blob) >> 3);

	// should not every reach here, but if it does...
	while (1) {
		halt();
	}
	__builtin_unreachable();
}

static int kx_init(void) {
	kx_setup();
	return 0;
}

static void kx_exit(void) {}

module_init(kx_init);
module_exit(kx_exit);
