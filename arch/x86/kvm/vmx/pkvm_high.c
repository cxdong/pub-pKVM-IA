// SPDX-License-Identifier: GPL-2.0
#include <linux/kvm_host.h>
#include <asm/kvm_pkvm.h>

static int pkvm_check_processor_compat(void)
{
	return kvm_call_pkvm(check_processor_compatibility);
}

static int pkvm_enable_virtualization_cpu(void)
{
	return kvm_call_pkvm(enable_virtualization_cpu);
}

static void pkvm_disable_virtualization_cpu(void)
{
	/*
	 * The pKVM hypervisor doesn't support disabling VMX via this PV
	 * interface. As a result, the CPU will remain in VMX non-root mode
	 * during shutting down or rebooting if there is no hardware level
	 * reset. In this case, if INIT-SIPI are send, such events will cause
	 * vmexits to the pKVM hypervisor rather than the operations associated
	 * with these events. Handling such vmexits is not supported by the pKVM
	 * hypervisor.
	 *
	 * TODO: Support handling INIT-SIPI vmexits for CPUs which remain in VMX
	 * non-root mode and need INIT-SIPI for shutting down or rebooting.
	 */
	kvm_call_pkvm(disable_virtualization_cpu);
}

static void pkvm_emergency_disable_virtualization_cpu(void)
{
	/*
	 * Just leverage disable_virtualization_cpu PV interface for emergency.
	 * Once the pKVM hypervisor supports emulating INIT-SIPI, this can also
	 * be leveraged.
	 *
	 * Note: an alternative is to make the pKVM hypervisor to support the
	 * host disabling VMX together with wiping all the pVMs' CPU and memory
	 * state via a dedicated PV interface for emergency.
	 */
	kvm_call_pkvm(disable_virtualization_cpu);
}

struct kvm_x86_ops pkvm_host_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.check_processor_compatibility = pkvm_check_processor_compat,

	.enable_virtualization_cpu = pkvm_enable_virtualization_cpu,
	.disable_virtualization_cpu = pkvm_disable_virtualization_cpu,
	.emergency_disable_virtualization_cpu = pkvm_emergency_disable_virtualization_cpu,
};
