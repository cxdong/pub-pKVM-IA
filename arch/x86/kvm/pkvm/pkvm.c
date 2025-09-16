// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/kvm_host.h>
#include "init_finalize.h"
#include "pkvm.h"

/*
 * Needed by kvm_spurious_fault() which is a generic fault function for the
 * vendor operations, e.g., vmx ops or svm ops. The pkvm hypervisor doesn't
 * have the knowledge about the platform reboot or shutdown, so kvm_rebooting
 * is always false in the pkvm hypervisor.
 */
__visible bool kvm_rebooting;

struct pkvm_hyp *pkvm_hyp;
DEFINE_PER_CPU(struct pkvm_pcpu *, phys_cpu);
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);

static int pkvm_handle_kvm_call_inout(unsigned long func, unsigned long a0,
				      unsigned long a1, unsigned long a2,
				      unsigned long a3)
{
	struct kvm_vcpu *hvcpu = this_cpu_read(host_vcpu);
	union pkvm_fn_data data = { 0 };

	hvcpu->arch.regs[VCPU_REGS_RBX] = data.val1;
	hvcpu->arch.regs[VCPU_REGS_RCX] = data.val2;
	hvcpu->arch.regs[VCPU_REGS_RDX] = data.val3;
	hvcpu->arch.regs[VCPU_REGS_RSI] = data.val4;

	return -EINVAL;
}

int pkvm_handle_kvm_call(unsigned long func, unsigned long a0,
			 unsigned long a1, unsigned long a2,
			 unsigned long a3)
{
	int ret;

	if (func >= PKVM_FIRST_INOUT_PV_INTERFACE)
		return pkvm_handle_kvm_call_inout(func, a0, a1, a2, a3);

	switch (func) {
	case __pkvm__init_finalize:
		ret = pkvm_init_finalize((struct pkvm_mem_info *)a0, a1,
					 (struct pkvm_init_ops *)a2);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
