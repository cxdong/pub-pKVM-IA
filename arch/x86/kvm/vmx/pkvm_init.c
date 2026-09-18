// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/kvm_pkvm.h>
#include "vmx.h"

static u64 __init vmx_pkvm_total_reserve_pages(void)
{
	return pkvm_data_pages();
}

int __init vmx_pkvm_init(void)
{
	return 0;
}

int __init pkvm_vmx_init_reserve_ops(struct pkvm_reserve_ops *ops)
{
	if (!cpu_feature_enabled(X86_FEATURE_VMX))
		return -EOPNOTSUPP;

	ops->total_pages = vmx_pkvm_total_reserve_pages;
	return 0;
}

MODULE_LICENSE("GPL");
