// SPDX-License-Identifier: GPL-2.0
#include <asm/hypervisor.h>

#define PKVM_GUEST_SIGNATURE		"PKVMPKVMPKVM"

static u32 __init pkvm_detect(void)
{
	if (boot_cpu_has(X86_FEATURE_HYPERVISOR))
		return cpuid_base_hypervisor(PKVM_GUEST_SIGNATURE, 0);

	return 0;
}

static bool pkvm_x2apic_available(void)
{
	return boot_cpu_has(X86_FEATURE_X2APIC);
}

const __initconst struct hypervisor_x86 x86_hyper_pkvm = {
	.name                   = "PKVM",
	.detect                 = pkvm_detect,
	.type			= X86_HYPER_PKVM,
	.init.init_platform	= x86_init_noop,
	.init.x2apic_available  = pkvm_x2apic_available,
};
