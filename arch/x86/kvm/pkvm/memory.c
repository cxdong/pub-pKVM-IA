// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/cache.h>
#include <asm/kvm_pkvm.h>

#ifdef CONFIG_DYNAMIC_MEMORY_LAYOUT
unsigned long page_offset_base __ro_after_init;
#endif
unsigned long phys_base;

struct memblock_region pkvm_memory[PKVM_MEMBLOCK_REGIONS];
unsigned int pkvm_memblock_nr;
