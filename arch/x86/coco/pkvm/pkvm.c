// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <asm/coco.h>
#include <asm/pkvm_guest.h>

void __init pkvm_guest_init_coco(void)
{
	cc_vendor = CC_VENDOR_PKVM;
}
