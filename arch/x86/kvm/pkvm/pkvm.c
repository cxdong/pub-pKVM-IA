// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/kvm_host.h>
#include <asm/fpu/xcr.h>
#include "init_finalize.h"
#include "pkvm/lapic.h"
#include "pkvm.h"
#include "x86.h"

/*
 * Needed by kvm_spurious_fault() which is a generic fault function for the
 * vendor operations, e.g., vmx ops or svm ops. The pKVM hypervisor doesn't
 * have the knowledge about the platform reboot or shutdown, so kvm_rebooting
 * is always false in the pKVM hypervisor.
 */
__visible bool kvm_rebooting;

struct pkvm_hyp *pkvm_hyp;
DEFINE_PER_CPU(struct pkvm_pcpu *, phys_cpu);
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);

int pkvm_handle_kvm_call(unsigned long func, unsigned long a0,
			 unsigned long a1, unsigned long a2,
			 unsigned long a3)
{
	int ret;

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

void pkvm_kick_vcpu(struct kvm_vcpu *vcpu)
{
	/* No need to kick if a vcpu is already out of guest mode */
	if (kvm_vcpu_exiting_guest_mode(vcpu) != IN_GUEST_MODE)
		return;

	pkvm_lapic_send_init(READ_ONCE(vcpu->cpu));
}

int pkvm_x86_vendor_init(struct kvm_x86_init_ops *ops)
{
	int r;

	memset(&kvm_caps, 0, sizeof(kvm_caps));

	kvm_caps.supported_vm_types = BIT(KVM_X86_DEFAULT_VM) |
				      BIT(KVM_X86_PKVM_PROTECTED_VM);
	kvm_caps.supported_mce_cap = MCG_CTL_P | MCG_SER_P;

	if (boot_cpu_has(X86_FEATURE_XSAVE)) {
		kvm_host.xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
		kvm_caps.supported_xcr0 = kvm_host.xcr0 & KVM_SUPPORTED_XCR0;
	}

	rdmsrl_safe(MSR_EFER, &kvm_host.efer);

	if (boot_cpu_has(X86_FEATURE_XSAVES))
		rdmsrl(MSR_IA32_XSS, kvm_host.xss);

	if (boot_cpu_has(X86_FEATURE_ARCH_CAPABILITIES))
		rdmsrl(MSR_IA32_ARCH_CAPABILITIES, kvm_host.arch_capabilities);

	r = ops->hardware_setup();
	if (r)
		return r;

	memcpy(&kvm_x86_ops, ops->runtime_ops, sizeof(kvm_x86_ops));

	if (!kvm_cpu_cap_has(X86_FEATURE_XSAVES))
		kvm_caps.supported_xss = 0;

	if (kvm_caps.has_tsc_control) {
		/*
		 * Make sure the user can only configure tsc_khz values that
		 * fit into a signed integer.
		 * A min value is not calculated because it will always
		 * be 1 on all machines.
		 */
		u64 max = min(0x7fffffffULL,
			      mul_u64_u64_shr(tsc_khz, kvm_caps.max_tsc_scaling_ratio,
					      kvm_caps.tsc_scaling_ratio_frac_bits));

		kvm_caps.max_guest_tsc_khz = max;
	}
	kvm_caps.default_tsc_scaling_ratio = 1ULL << kvm_caps.tsc_scaling_ratio_frac_bits;

	return 0;
}
