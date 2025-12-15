/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PKVM_GUEST_H
#define _ASM_X86_PKVM_GUEST_H

#ifdef CONFIG_PKVM_GUEST
#include <linux/jump_label.h>

void __init pkvm_guest_init_coco(void);
DECLARE_STATIC_KEY_FALSE(pkvm_guest_detected);

static inline bool pkvm_is_protected_guest(void)
{
	return static_branch_likely(&pkvm_guest_detected);
}
#else
static inline bool pkvm_is_protected_guest(void) { return false; }
#endif

#endif /* _ASM_X86_PKVM_GUEST_H */
