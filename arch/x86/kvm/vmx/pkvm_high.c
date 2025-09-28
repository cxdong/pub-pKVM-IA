// SPDX-License-Identifier: GPL-2.0
#include <linux/kvm_host.h>
#include <asm/kvm_pkvm.h>
#include "pkvm_constants.h"
#include "x86_ops.h"
#include "vmx.h"

static void *kvm_host_va(phys_addr_t phys)
{
	return __va(phys);
}

static void host_free_pkvm_mem_range(struct pkvm_mem_range mem_range)
{
	if (!mem_range.size)
		return;

	WARN_ON_ONCE(!PAGE_ALIGNED(mem_range.size));

	if (mem_range.size > PAGE_SIZE)
		free_pages_exact(__va(mem_range.addr), mem_range.size);
	else
		free_page((unsigned long)__va(mem_range.addr));
}

static void host_free_pkvm_memcache(struct pkvm_memcache *mc)
{
	free_pkvm_memcache(mc, host_free_pkvm_mem_range, kvm_host_va);
}

static void free_pml_buffer(struct vcpu_vmx *vmx)
{
	if (vmx->pml_pg) {
		free_page((unsigned long)vmx->pml_pg);
		vmx->pml_pg = NULL;
	}
}

static void free_ve_info(struct vcpu_vmx *vmx)
{
	if (vmx->ve_info) {
		free_page((unsigned long)vmx->ve_info);
		vmx->ve_info = NULL;
	}
}

static void pkvm_free_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	if (!loaded_vmcs->vmcs)
		return;
	free_vmcs(loaded_vmcs->vmcs);
	loaded_vmcs->vmcs = NULL;
	if (loaded_vmcs->msr_bitmap)
		free_page((unsigned long)loaded_vmcs->msr_bitmap);
	WARN_ON(loaded_vmcs->shadow_vmcs != NULL);
}

static int pkvm_alloc_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	loaded_vmcs->vmcs = alloc_vmcs(false);
	if (!loaded_vmcs->vmcs)
		return -ENOMEM;

	loaded_vmcs->shadow_vmcs = NULL;
	loaded_vmcs->cpu = -1;

	if (cpu_has_vmx_msr_bitmap()) {
		loaded_vmcs->msr_bitmap = (unsigned long *)
				__get_free_page(GFP_KERNEL_ACCOUNT);
		if (!loaded_vmcs->msr_bitmap)
			goto out_vmcs;
	}

	return 0;

out_vmcs:
	pkvm_free_loaded_vmcs(loaded_vmcs);
	return -ENOMEM;
}

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

static int pkvm_vm_init(struct kvm *kvm)
{
	void *pkvm_vm;
	int ret;

	/*
	 * Some of struct kvm elements are initialized by the vmx_vm_init()
	 * which can be leveraged by the pKVM host as this initialization is
	 * simple and no VMX hardware involved.
	 */
	ret = vmx_vm_init(kvm);
	if (ret)
		return ret;

	pkvm_vm = alloc_pages_exact(PKVM_VMX_VM_SIZE, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vm)
		return -ENOMEM;

	ret = kvm_call_pkvm(vm_init, __pa(kvm), __pa(pkvm_vm));
	if (ret < 0)
		goto free_page;

	kvm->arch.pkvm_vm_handle = ret;

	if (pkvm_is_protected_vm(kvm))
		kvm->arch.has_protected_state = true;

	return 0;

free_page:
	free_pages_exact(pkvm_vm, PKVM_VMX_VM_SIZE);
	return ret;
}

static void pkvm_vm_destroy(struct kvm *kvm)
{
	union pkvm_fn_data inout = { 0 };
	int ret;

	inout.vm_handle = kvm->arch.pkvm_vm_handle;
	ret = kvm_call_pkvm_inout(vm_destroy, &inout);
	if (ret)
		return;

	host_free_pkvm_memcache(&inout.memcache);

	vmx_vm_destroy(kvm);
}

static int pkvm_vcpu_create(struct kvm_vcpu *vcpu)
{
	void *pkvm_vcpu, *fps;
	struct vcpu_vmx *vmx;
	struct page *page;
	size_t fps_size;
	int ret;

	vmx = to_vmx(vcpu);

	INIT_LIST_HEAD(&vmx->pi_wakeup_list);

	/*
	 * If PML is turned on, failure on enabling PML just results in failure
	 * of creating the vcpu, therefore we can simplify PML logic (by
	 * avoiding dealing with cases, such as enabling PML partially on vcpus
	 * for the guest), etc.
	 */
	if (enable_pml) {
		page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!page)
			return -ENOMEM;
		vmx->pml_pg = page_to_virt(page);
	}

	ret = pkvm_alloc_loaded_vmcs(&vmx->vmcs01);
	if (ret < 0)
		goto free_pml;

	vmx->loaded_vmcs = &vmx->vmcs01;
	vmx->loaded_vmcs->cpu = -1;

	ret = -ENOMEM;

	BUILD_BUG_ON(sizeof(*vmx->ve_info) > PAGE_SIZE);
	/* ve_info must be page aligned. */
	page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!page)
		goto free_vmcs;
	vmx->ve_info = page_to_virt(page);

	pkvm_vcpu = alloc_pages_exact(PKVM_VMX_VCPU_SIZE, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vcpu)
		goto free_ve;

	fps_size = pkvm_guest_initial_fpstate_size(vcpu->kvm);
	fps = alloc_pages_exact(fps_size, GFP_KERNEL_ACCOUNT);
	if (!fps)
		goto free_vcpu;

	ret = kvm_call_pkvm(vcpu_create, vcpu->kvm->arch.pkvm_vm_handle,
			    __pa(vcpu), __pa(pkvm_vcpu), __pa(fps));
	if (ret < 0)
		goto free_fpu;

	vcpu->arch.pkvm_vcpu_handle = ret;

	return 0;

free_fpu:
	free_pages_exact(fps, fps_size);
free_vcpu:
	free_pages_exact(pkvm_vcpu, PKVM_VMX_VCPU_SIZE);
free_ve:
	free_ve_info(vmx);
free_vmcs:
	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
free_pml:
	free_pml_buffer(vmx);
	return ret;
}

static void pkvm_vcpu_free(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	union pkvm_fn_data inout = { 0 };
	int ret;

	inout.vm_handle = vcpu->kvm->arch.pkvm_vm_handle;
	inout.vcpu_handle = vcpu->arch.pkvm_vcpu_handle;

	ret = kvm_call_pkvm_inout(vcpu_free, &inout);
	if (ret) {
		kvm_err("The pkvm-hyp is failed to free pkvm_vcpu: %d", ret);
		return;
	}

	host_free_pkvm_memcache(&inout.memcache);

	if (enable_pml)
		free_pml_buffer(vmx);
	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
	free_ve_info(vmx);
}

struct kvm_x86_ops pkvm_host_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.check_processor_compatibility = pkvm_check_processor_compat,

	.enable_virtualization_cpu = pkvm_enable_virtualization_cpu,
	.disable_virtualization_cpu = pkvm_disable_virtualization_cpu,
	.emergency_disable_virtualization_cpu = pkvm_emergency_disable_virtualization_cpu,

	.vm_size = sizeof(struct kvm_vmx),
	.vm_init = pkvm_vm_init,
	.vm_destroy = pkvm_vm_destroy,

	.vcpu_create = pkvm_vcpu_create,
	.vcpu_free = pkvm_vcpu_free,
};
