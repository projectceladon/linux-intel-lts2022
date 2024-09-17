/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/reboot.h>
#include <asm/page.h>
#include <asm/setup.h>
#include <asm/e820/types.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/crash_core.h>
#include <linux/firmware/ivi/ivi_shm_utils.h>

#include "ivi_shm.h"

#define TEST_START 0x1000000000UL /* 64G */
#define IVI_DEVICE_NAME "ivi_shm"

extern const char linux_banner[];

struct ivi_shm_section_data {
	struct ivi_shm_section section;
	struct notifier_block panic_notifier;
	struct notifier_block reboot_notifier;
};

static bool is_sos = true;
static uint64_t shm_start = 0;
static uint64_t shm_size = IVI_SHM_SECTION_SIZE;
static struct ivi_shm_section_data ivi_shm_section_data = {};

void __meminit ivi_shm_detect(uint64_t start, uint64_t size, int type)
{
	struct ivi_shm_entry *entry;

	if ((start == TEST_START) && (size == IVI_SHM_SECTION_SIZE) &&
		(type == E820_TYPE_RESERVED)) {
		/*
		 * For now firmware (OVMF) will filter out unknown E820
		 * types so we use hardcoded 64GB address as workaround.
		 */
		pr_info("%s: Found IVI_SHM_SECTION_TYPE\n", __func__);
		entry = &ivi_shm_section_data.section.entry;
		shm_start = start;
		is_sos = false;
	} else if (type == IVI_SHM_SECTION_TYPE) {
		pr_info("%s: Found IVI_SHM_SECTION_TYPE\n", __func__);
		entry = &ivi_shm_section_data.section.entry;
		shm_start = start;
	} else {
		pr_info("%s: type 0x%x is not ivi shm type\n", __func__, type);
		return;
	}

	if (entry->valid) {
		pr_err("%s multiple entry(start %llx size 0x%llx type %d) detected\n",
			__func__, start, size, type);
		return;
	}

	entry->addr = start;
	entry->size = size;
	if (size > IVI_SHM_SECTION_SIZE) {
		/* totally 4MB SHM, first 1MB is for VMs to share data (each 16KB),
		   next 3MB is for sbl_cd */
		shm_size = IVI_SHM_SIZE;
		entry->size = IVI_SHM_SECTION_SIZE;
	}
	pr_info("%s %s detect ivi_shm@0x%llx size 0x%llx IVI_SHM_SECTION_SIZE 0x%x\n",
		__func__, is_sos ? "SOS":"GUEST", start, size, IVI_SHM_SECTION_SIZE);
	entry->valid = 1;
}

#ifdef CONFIG_CRASH_CORE
static void section_vmcoreinfo_setup(struct ivi_shm_section_data *section_data)
{
	struct ivi_shm_section_vm *section_vm = &(section_data->section.vm);

	memcpy(section_vm->shm_vm->vmcoreinfo_data, vmcoreinfo_data, vmcoreinfo_size);
	section_vm->vmcoreinfo_size = vmcoreinfo_size;
}
#endif

static int ivi_shm_section_panic_notifier(struct notifier_block *nb,
					  unsigned long action, void *data)
{
	struct ivi_shm_section_data *section_data;
	struct ivi_shm_section_vm *section_vm;

	section_data =
		container_of(nb, struct ivi_shm_section_data, panic_notifier);

	section_vm = &(section_data->section.vm);
	section_vm->shm_vm->boot_reason = BOOT_REASON_VM_PANIC;

	return NOTIFY_DONE;
}

static int ivi_shm_section_reboot_notifier(struct notifier_block *nb,
					   unsigned long code, void *data)
{
	struct ivi_shm_section_data *section_data;
	struct ivi_shm_section_vm *section_vm;

	section_data =
		container_of(nb, struct ivi_shm_section_data, reboot_notifier);

	section_vm = &(section_data->section.vm);
	section_vm->shm_vm->boot_reason = BOOT_REASON_NORMAL_BOOT;

	return NOTIFY_DONE;
}

static int
register_notifer_boot_reason(struct ivi_shm_section_data *section_data)
{
	int ret;

	section_data->panic_notifier.notifier_call =
		ivi_shm_section_panic_notifier;
	section_data->panic_notifier.priority = INT_MAX; /* Do it early */

	ret = atomic_notifier_chain_register(&panic_notifier_list,
					     &section_data->panic_notifier);
	if (ret) {
		pr_err("ivi_shm_section: Failed to register panic notifier, %d\n",
		       ret);
		return ret;
	}

	section_data->reboot_notifier.notifier_call =
		ivi_shm_section_reboot_notifier;
	ret = register_reboot_notifier(&section_data->reboot_notifier);
	if (ret) {
		pr_err("ivi_shm_section: Failed to register reboot notifier, %d\n",
		       ret);
		return ret;
	}

	return 0;
}

static int __init ivi_shm_section_init(void)
{
	if (ivi_shm_section_data.section.entry.valid == 0) {
		pr_info("ivi_shm_section: Not Found\n");
	} else {
		void *vaddr;
		struct ivi_shm_vm *shm_vm;
		uint64_t start, size, mask;
		struct ivi_shm_section *section = &ivi_shm_section_data.section;
		int ret;

		start = section->entry.addr;
		size = section->entry.size;
		mask = IVI_SHM_SECTION_SIZE - 1;
		pr_info("%s: start = 0x%llx, size = 0x%llx\n", __func__, start,
			size);

#define SECTION_INVALID_VALUE (uint64_t)(-1)

		if (size != IVI_SHM_SECTION_SIZE) {
			size = SECTION_INVALID_VALUE;
		}
		if (start & mask) {
			pr_info("%s: setting start to invalid\n", __func__);
			start = SECTION_INVALID_VALUE;
		}

		pr_info("ivi_shm_section: found, start (0x%llx) %s, size(0x%llx) %s\n",
			start,
			(start == SECTION_INVALID_VALUE) ? "is wrong" : "",
			size,
			(size == SECTION_INVALID_VALUE) ? "is wrong" : "");

		if (start == SECTION_INVALID_VALUE ||
		    size == SECTION_INVALID_VALUE) {
			return 0;
		}

		vaddr = ioremap(start, size);
		if (!vaddr) {
			section->entry.valid = 0;
			pr_info("ivi_shm_section: failed to map section, set to invalid\n");
			return -ENOMEM;
		}
		if (is_sos)
			memset(vaddr, 0, size);
		shm_vm = (struct ivi_shm_vm *)vaddr;
		/* Boot is successful here so set boot_reason to normal. */
		shm_vm->boot_reason = BOOT_REASON_NORMAL_BOOT;
		if (is_sos)
			strcpy(shm_vm->guest_name, "SOS");
		strncpy(shm_vm->guest_version, linux_banner, OS_VERSION_SIZE);
		shm_vm->guest_name[GUEST_NAME_SIZE - 1] = '\0';
		pr_info("ivi_shm_section: guest_name %s\n", shm_vm->guest_name);
		pr_info("ivi_shm_section: guest_version %s\n", shm_vm->guest_version);
		section->vm.shm_vm = shm_vm;
		section->vm.vmcoreinfo_size = 0;

#ifdef CONFIG_CRASH_CORE
		section_vmcoreinfo_setup(&ivi_shm_section_data);
#endif
		ret = register_notifer_boot_reason(&ivi_shm_section_data);
		if (ret) {
			pr_info("ivi_shm_section: failed to register boot reason\n");
			return ret;
		}
		section->entry.addr = start;
		section->entry.size = size;
	}

	return 0;
}

static int ivi_open(struct inode *inode, struct file *file)
{
	printk(KERN_INFO "ivi_open\n");
	return 0;
}

static int ivi_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long page;
	unsigned long start = (unsigned long)vma->vm_start;
	unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);
	unsigned long offset = (unsigned long)vma->vm_pgoff << PAGE_SHIFT;

	page = shm_start + offset;

	if (page == 0) {
		pr_info("invalid shared memory address\n");
		return -1;
	}

	if ((offset + size) > (shm_start + shm_size)) {
		pr_info("invalid shared memory offset\n");
		return -1;
	}

	if (remap_pfn_range(vma, start, page >> PAGE_SHIFT, size, PAGE_SHARED)) {
		pr_info("ivi_mmap remap_pfn_range failed\n");
		return -1;
	}
	pr_info("%s start 0x%lx size 0x%lx offset 0x%lx from shm_start 0x%llx shm_size 0x%llx\n",
		__func__, start, size, offset, shm_start, shm_size);

	return 0;
}

static struct file_operations dev_fops = {
	.owner = THIS_MODULE,
	.open = ivi_open,
	.mmap = ivi_mmap,
};

static struct miscdevice misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = IVI_DEVICE_NAME,
	.fops = &dev_fops,
};

static int __init ivi_dev_init(void)
{
	int ret;

	pr_info("Entering ivi_dev_init\n");
	ret = misc_register(&misc);
	if (ret < 0) {
		pr_info("ivi_dev_init failed\n");
	}

	return ret;
}

static void __exit ivi_dev_exit(void)
{
	misc_deregister(&misc);
}

module_init(ivi_dev_init);
module_exit(ivi_dev_exit);
fs_initcall(ivi_shm_section_init);
