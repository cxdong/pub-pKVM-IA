/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_PKVM_H
#define __PKVM_X86_PKVM_H

#include <asm/kvm_pkvm.h>

DECLARE_PER_CPU(struct pkvm_pcpu *, phys_cpu);
DECLARE_PER_CPU(struct kvm_vcpu *, host_vcpu);

int pkvm_handle_kvm_call(unsigned long func, unsigned long a0,
			 unsigned long a1, unsigned long a2,
			 unsigned long a3);

#endif /* __PKVM_X86_PKVM_H */
