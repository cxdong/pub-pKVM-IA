/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KVM_PKVM_H
#define _ASM_X86_KVM_PKVM_H

#ifdef CONFIG_PKVM_X86

struct pkvm_reserve_ops {
	u64 (*total_pages)(void);
};

static inline int __init pkvm_vmx_init_reserve_ops(struct pkvm_reserve_ops *ops)
{
	return -EOPNOTSUPP;
}

#define PKVM_MEMBLOCK_REGIONS		128

void *pkvm_early_alloc_contig(unsigned int nr_pages);
void pkvm_early_alloc_init(void *virt, unsigned long size);

static inline unsigned long pkvm_data_pages(void)
{
	return 0;
}

#endif /* CONFIG_PKVM_X86 */

#endif /* _ASM_X86_KVM_PKVM_H */
