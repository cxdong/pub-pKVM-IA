/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_UNDEF_H
#define __PKVM_X86_UNDEF_H

/*
 * Special hack: pKVM runs in the highest privilege level, which is higher than
 * the linux kernel. This means that pKVM cannot use any of the linux kernel
 * symbols. To make pKVM being able to use the linux kernel headers without
 * introducing additional symbols, some kernel configuration options are
 * disabled. (This list needs to be extended when new variants are added.)
 */
#ifndef CONFIG_PKVM_X86_DEBUG
#undef CONFIG_PRINTK
#undef CONFIG_BUG
#undef CONFIG_GENERIC_BUG
#endif
#undef CONFIG_CALL_THUNKS_DEBUG
#undef CONFIG_DEBUG_PREEMPT
#undef CONFIG_PARAVIRT
#undef CONFIG_PARAVIRT_XXL
#undef CONFIG_PARAVIRT_SPINLOCKS
#undef CONFIG_TRACEPOINTS
#undef CONFIG_KVM_INTEL_TDX

#endif /* __PKVM_X86_UNDEF_H */
