/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __IVI_SHM_PRIVATE_H
#define __IVI_SHM_PRIVATE_H

#define IVI_SHM_SECTION_TYPE	0x6

#define IVI_SHM_SIZE 		0x100000
#define IVI_SHM_SECTION_SIZE 	0x4000
#define IVI_SHM_SECTION_COUNT 	(IVI_SHM_SIZE / IVI_SHM_SECTION_SIZE)

#define GUEST_NAME_SIZE 64
#define OS_VERSION_SIZE 1024
#define VMCORE_SIZE	4096

enum section_boot_reason {
	BOOT_REASON_NORMAL_BOOT = 0x0,
	BOOT_REASON_DEFAULT_SET = 0xff,
	BOOT_REASON_VM_PANIC = 0xfe,
};

struct ivi_shm_hdr {
	uint16_t	shm_hdr_version;
	uint8_t         dump_ctl;
	uint8_t         type;
} __attribute__((packed));

struct ivi_shm_vm {
	struct	ivi_shm_hdr shm_header;
	char    guest_name[GUEST_NAME_SIZE];
	char    guest_version[OS_VERSION_SIZE];
	uint8_t boot_reason;
	char    vmcoreinfo_data[VMCORE_SIZE];
} __attribute__((packed));

struct ivi_shm_section_vm {
	struct ivi_shm_vm *shm_vm;

	/* private data*/
	int vmcoreinfo_size;
};

struct ivi_shm_entry {
	uint64_t addr;
	uint64_t size;
	int valid;
};

struct ivi_shm {
	struct ivi_shm_entry entry;
	void *data;
};

struct ivi_shm_section {
	struct ivi_shm_entry entry;
	struct ivi_shm_section_vm vm;
};

#endif
