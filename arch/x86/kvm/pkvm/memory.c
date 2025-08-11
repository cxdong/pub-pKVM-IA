// SPDX-License-Identifier: GPL-2.0
#include <linux/cache.h>
#include <linux/types.h>

#ifdef CONFIG_DYNAMIC_MEMORY_LAYOUT
unsigned long page_offset_base __ro_after_init;
#endif
unsigned long phys_base;
