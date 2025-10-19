// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/kvm_host.h>
#include <asm/fpu/xcr.h>
#include <asm/pkvm_spinlock.h>
#include <lapic.h>
#include "init_finalize.h"
#include "mem_protect.h"
#include "memory.h"
#include "pkvm/lapic.h"
#include "pkvm.h"
#include "x86.h"
#include "debug.h"

/*
 * Needed by kvm_spurious_fault() which is a generic fault function for the
 * vendor operations, e.g., vmx ops or svm ops. The pKVM hypervisor doesn't
 * have the knowledge about the platform reboot or shutdown, so kvm_rebooting
 * is always false in the pKVM hypervisor.
 */
__visible bool kvm_rebooting;

/*
 * Needed by code sharing with the KVM. As the pKVM hypervisor requires to have
 * a second level page table to translate GPA to HPA, set tdp_enabled as true.
 */
bool tdp_enabled = true;

struct pkvm_hyp *pkvm_hyp;
DEFINE_PER_CPU(struct pkvm_pcpu *, phys_cpu);
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);

/* The maximum number of VMs under pkvm. */
#define MAX_PKVM_VMS				64

static DECLARE_BITMAP(pkvm_vms_bitmap, MAX_PKVM_VMS);
static pkvm_spinlock_t pkvm_vms_lock = __PKVM_SPINLOCK_UNLOCKED;
static struct pkvm_vm_ref {
	/* Reference counter to indicate if pkvm_vm is inuse */
	atomic_t refcount;
	/* Point to pkvm_vm in pkvm */
	struct pkvm_vm *pkvm_vm;
} pkvm_vms_ref[MAX_PKVM_VMS];

/*
 * Represents the actual size of the kvm_vcpu instance. It is initialized as
 * the size of struct kvm_vcpu. And if the vendor code extends kvm_vcpu instance
 * via embedding struct kvm_vcpu to its specific structure, this size should also
 * be extended by the vendor code.
 */
size_t kvm_vcpu_sz = sizeof(struct kvm_vcpu);

static void *donate_host_memory(phys_addr_t phys, size_t size, bool clear)
{
	void *va;

	if (pkvm_host_donate_hyp(phys, size))
		return NULL;

	va = __pkvm_va(phys);
	if (clear)
		memset(va, 0, size);

	return va;
}

static int pkvm_enable_virtualization_cpu(void)
{
	kvm_user_return_msr_cpu_online();

	return kvm_x86_call(enable_virtualization_cpu)();
}

static void pkvm_disable_virtualization_cpu(void)
{
	kvm_x86_call(disable_virtualization_cpu)();
}

static int allocate_pkvm_vm_handle(struct pkvm_vm *pkvm_vm)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	int idx;

	/*
	 * The pkvm_vm_handle is an int so cannot exceed the INT_MAX.
	 * Meanwhile pkvm_vm_handle will also be used as owner_id in
	 * the page state machine so it also cannot exceed the max
	 * owner_id.
	 */
	BUILD_BUG_ON(MAX_PKVM_VMS >
		     min(INT_MAX, ((1 << PKVM_INVALID_PTE_OWNER_BITS) - 1)));

	pkvm_spin_lock(&pkvm_vms_lock);

	idx = find_next_zero_bit(pkvm_vms_bitmap, MAX_PKVM_VMS, 0);
	if (idx == MAX_PKVM_VMS) {
		pkvm_spin_unlock(&pkvm_vms_lock);
		return -ENOMEM;
	}
	__set_bit(idx, pkvm_vms_bitmap);

	pkvm_vm_ref = &pkvm_vms_ref[idx];
	pkvm_vm_ref->pkvm_vm = pkvm_vm;
	atomic_set(&pkvm_vm_ref->refcount, 1);

	pkvm_spin_unlock(&pkvm_vms_lock);

	return idx;
}

static struct pkvm_vm *free_pkvm_vm_handle(int handle)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	struct pkvm_vm *pkvm_vm = NULL;
	int idx = handle;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	pkvm_spin_lock(&pkvm_vms_lock);

	idx = array_index_nospec(idx, MAX_PKVM_VMS);
	pkvm_vm_ref = &pkvm_vms_ref[idx];
	if ((atomic_cmpxchg(&pkvm_vm_ref->refcount, 1, 0) != 1)) {
		pkvm_err("VM%d is busy, refcount %d\n", handle,
			 atomic_read(&pkvm_vm_ref->refcount));
		goto out;
	}

	pkvm_vm = pkvm_vm_ref->pkvm_vm;
	pkvm_vm_ref->pkvm_vm = NULL;

	__clear_bit(idx, pkvm_vms_bitmap);
out:
	pkvm_spin_unlock(&pkvm_vms_lock);
	return pkvm_vm;
}

static int pkvm_vm_init(phys_addr_t host_kvm_pa, phys_addr_t pkvm_vm_pa)
{
	struct pkvm_vm *pkvm_vm;
	struct kvm *shared_kvm;
	size_t size;
	u8 vm_type;
	int ret;

	ret = pkvm_host_share_hyp(host_kvm_pa, kvm_x86_call(vm_size));
	if (ret)
		return ret;

	shared_kvm = __pkvm_va(host_kvm_pa);
	vm_type = shared_kvm->arch.vm_type;
	if (!kvm_is_vm_type_supported(vm_type)) {
		ret = -EOPNOTSUPP;
		goto unshare;
	}

	size = PAGE_ALIGN(PKVM_VM_BASE_SIZE + kvm_x86_call(vm_size));
	pkvm_vm = donate_host_memory(pkvm_vm_pa, size, true);
	if (!pkvm_vm) {
		ret = -EINVAL;
		goto unshare;
	}

	pkvm_vm->size = size;
	pkvm_vm->shared_kvm = shared_kvm;
	pkvm_vm->lock = __PKVM_SPINLOCK_UNLOCKED;
	pkvm_vm->kvm.arch.vm_type = vm_type;

	ret = allocate_pkvm_vm_handle(pkvm_vm);
	if (ret < 0)
		goto undonate;

	pkvm_vm->kvm.arch.pkvm_vm_handle = ret;

	ret = kvm_x86_call(vm_init)(&pkvm_vm->kvm);
	if (ret)
		goto free_handle;

	return pkvm_vm->kvm.arch.pkvm_vm_handle;

free_handle:
	free_pkvm_vm_handle(pkvm_vm->kvm.arch.pkvm_vm_handle);
undonate:
	pkvm_hyp_donate_host(__pkvm_pa(pkvm_vm), size, false);
unshare:
	pkvm_host_unshare_hyp(host_kvm_pa, kvm_x86_call(vm_size));
	return ret;
}

static void push_mem_to_memcache(struct pkvm_memcache *mc, void *addr, size_t size)
{
	/*
	 * The pKVM hypervisor will push the memory range [addr, addr + size)
	 * to the memcache and eventually donate to the host. The memory range
	 * should be PAGE_SIZE aligned. If not, it must be a code bug.
	 */
	BUG_ON(!PAGE_ALIGNED(addr) || !PAGE_ALIGNED(size));

	pkvm_clear_memory(addr, size);

	push_pkvm_memcache(mc, addr, size, pkvm_virt_to_host_gpa);
}

static void donate_mem_in_memcache(struct pkvm_memcache *mc)
{
	struct pkvm_mem_range *range;
	int i;

	for_each_pkvm_mem_range(i, range, mc, pkvm_host_gpa_to_virt)
		pkvm_hyp_donate_host(pkvm_host_gpa_to_phys(range->addr),
				     range->size, false);
}

static void pkvm_vm_destroy(int vm_handle, struct pkvm_memcache *mc)
{
	struct pkvm_vm *pkvm_vm = free_pkvm_vm_handle(vm_handle);

	if (!pkvm_vm)
		return;

	kvm_x86_call(vm_destroy)(&pkvm_vm->kvm);

	pkvm_host_unshare_hyp(__pkvm_pa(pkvm_vm->shared_kvm), kvm_x86_call(vm_size));

	push_mem_to_memcache(mc, (void *)pkvm_vm, pkvm_vm->size);

	donate_mem_in_memcache(mc);
}

static int attach_pkvm_vcpu_to_vm(struct pkvm_vm *pkvm_vm, struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm *kvm = &pkvm_vm->kvm;
	int vcpu_handle;

	pkvm_spin_lock(&pkvm_vm->lock);

	if (kvm->created_vcpus == KVM_MAX_VCPUS) {
		pkvm_spin_unlock(&pkvm_vm->lock);
		return -EINVAL;
	}
	vcpu_handle = kvm->created_vcpus++;
	pkvm_vcpu->vcpu.arch.pkvm_vcpu_handle = vcpu_handle;
	pkvm_vm->vcpus[vcpu_handle] = pkvm_vcpu;

	pkvm_spin_unlock(&pkvm_vm->lock);

	atomic_set(&pkvm_vm->vcpu_refs[vcpu_handle], 1);

	return vcpu_handle;
}

static int pkvm_arch_vcpu_create(struct pkvm_vcpu *pkvm_vcpu, struct fpstate *fps)
{
	struct kvm_vcpu *vcpu = &pkvm_vcpu->vcpu;

	vcpu->kvm = &pkvm_vcpu->pkvm_vm->kvm;
	/* Set cpu to -1 to indicate it is not loaded on any CPU */
	vcpu->cpu = -1;
	vcpu->vcpu_id = pkvm_vcpu->shared_vcpu->vcpu_id;

	vcpu->arch.apic_base = pkvm_vcpu->shared_vcpu->arch.apic_base;

	vcpu->arch.last_vmentry_cpu = -1;
	vcpu->arch.regs_avail = ~0;
	vcpu->arch.regs_dirty = ~0;
	vcpu->arch.pat = MSR_IA32_CR_PAT_DEFAULT;

	vcpu->arch.mce_banks = (void *)pkvm_vcpu + PKVM_VCPU_BASE_SIZE + kvm_vcpu_sz;
	vcpu->arch.mci_ctl2_banks = (void *)vcpu->arch.mce_banks +
				    KVM_MCE_SIZE;
	vcpu->arch.mcg_cap = KVM_MAX_MCE_BANKS;

	vcpu->arch.guest_fpu.fpstate = fps;

	return kvm_x86_call(vcpu_create)(vcpu);
}

static void pkvm_unshare_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	void *apic_regs = kern_pkvm_va(pkvm_vcpu->vcpu.arch.apic->regs);

	pkvm_host_unshare_hyp(__pkvm_pa(pkvm_vcpu->shared_vcpu), kvm_vcpu_sz);
	pkvm_host_unshare_hyp(__pkvm_pa(pkvm_vcpu->vcpu.arch.apic),
			     sizeof(struct kvm_lapic));
	if (apic_regs)
		pkvm_host_unshare_hyp(__pkvm_pa(apic_regs), PAGE_SIZE);
}

static int pkvm_share_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *shared_vcpu = pkvm_vcpu->shared_vcpu;
	struct kvm_lapic *apic;
	void *apic_regs;
	int ret;

	apic = kern_pkvm_va(shared_vcpu->arch.apic);
	if (!apic)
		return -EINVAL;

	apic_regs = kern_pkvm_va(apic->regs);
	if (!apic_regs)
		return -EINVAL;

	ret = pkvm_host_share_hyp(__pkvm_pa(shared_vcpu), kvm_vcpu_sz);
	if (ret)
		return ret;

	ret = pkvm_host_share_hyp(__pkvm_pa(apic), sizeof(struct kvm_lapic));
	if (ret)
		goto unshare_vcpu;

	ret = pkvm_host_share_hyp(__pkvm_pa(apic_regs), PAGE_SIZE);
	if (ret)
		goto unshare_apic;

	pkvm_vcpu->vcpu.arch.apic = apic;

	return 0;

unshare_apic:
	pkvm_host_unshare_hyp(__pkvm_pa(apic), sizeof(struct kvm_lapic));
unshare_vcpu:
	pkvm_host_unshare_hyp(__pkvm_pa(shared_vcpu), kvm_vcpu_sz);
	return ret;
}

static int pkvm_vcpu_create(int vm_handle, phys_addr_t host_vcpu_pa,
			    phys_addr_t pkvm_vcpu_pa, phys_addr_t fpu_pa)
{
	struct pkvm_vcpu *pkvm_vcpu;
	size_t vcpu_size, fps_size;
	struct pkvm_vm *pkvm_vm;
	struct fpstate *fps;
	int ret;

	pkvm_vm = pkvm_get_vm(vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	vcpu_size = PAGE_ALIGN(PKVM_VCPU_BASE_SIZE +
			       kvm_vcpu_sz +
			       KVM_MCE_SIZE +
			       KVM_MCI_CTL2_SIZE);
	pkvm_vcpu = donate_host_memory(pkvm_vcpu_pa, vcpu_size, true);
	if (!pkvm_vcpu) {
		ret = -EINVAL;
		goto put_vm;
	}
	pkvm_vcpu->size = vcpu_size;

	fps_size = pkvm_guest_initial_fpstate_size(&pkvm_vm->kvm);
	fps = donate_host_memory(fpu_pa, fps_size, true);
	if (!fps) {
		ret = -EINVAL;
		goto undonate_vcpu;
	}
	fps->size = fps_size;

	pkvm_vcpu->shared_vcpu = __pkvm_va(host_vcpu_pa);
	ret = pkvm_share_vcpu(pkvm_vcpu);
	if (ret)
		goto undonate_fps;

	pkvm_vcpu->pkvm_vm = pkvm_vm;
	ret = pkvm_arch_vcpu_create(pkvm_vcpu, fps);
	if (ret)
		goto unshare;

	ret = attach_pkvm_vcpu_to_vm(pkvm_vm, pkvm_vcpu);
	if (ret < 0)
		goto destroy_vcpu;

	pkvm_put_vm(pkvm_vm);

	return pkvm_vcpu->vcpu.arch.pkvm_vcpu_handle;

destroy_vcpu:
	kvm_x86_call(vcpu_free)(&pkvm_vcpu->vcpu);
unshare:
	pkvm_unshare_vcpu(pkvm_vcpu);
undonate_fps:
	pkvm_hyp_donate_host(__pkvm_pa(fps), fps_size, false);
undonate_vcpu:
	pkvm_hyp_donate_host(__pkvm_pa(pkvm_vcpu), vcpu_size, false);
put_vm:
	pkvm_put_vm(pkvm_vm);
	return ret;
}

int pkvm_handle_kvm_call(unsigned long func, union pkvm_fn_data *in,
			 union pkvm_fn_data *out)
{
	int ret = 0;

	switch (func) {
	case __pkvm__init_finalize:
		ret = pkvm_init_finalize((struct pkvm_mem_info *)in->val1, in->val2,
					 (struct pkvm_init_ops *)in->val3);
		break;
	case __pkvm__check_processor_compatibility:
		ret = kvm_x86_call(check_processor_compatibility)();
		break;
	case __pkvm__enable_virtualization_cpu:
		ret = pkvm_enable_virtualization_cpu();
		break;
	case __pkvm__disable_virtualization_cpu:
		pkvm_disable_virtualization_cpu();
		break;
	case __pkvm__vm_init:
		ret = pkvm_vm_init(pkvm_host_gpa_to_phys(in->val1),
				   pkvm_host_gpa_to_phys(in->val2));
		break;
	case __pkvm__vm_destroy:
		pkvm_vm_destroy(in->vm_handle, &out->memcache);
		break;
	case __pkvm__vcpu_create:
		ret = pkvm_vcpu_create(in->val1, pkvm_host_gpa_to_phys(in->val2),
				       pkvm_host_gpa_to_phys(in->val3),
				       pkvm_host_gpa_to_phys(in->val4));
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

struct pkvm_vm *pkvm_get_vm(int vm_handle)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	int idx = vm_handle;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	idx = array_index_nospec(idx, MAX_PKVM_VMS);
	pkvm_vm_ref = &pkvm_vms_ref[idx];

	return atomic_inc_not_zero(&pkvm_vm_ref->refcount) ? pkvm_vm_ref->pkvm_vm : NULL;
}

void pkvm_put_vm(struct pkvm_vm *pkvm_vm)
{
	int idx = pkvm_vm->kvm.arch.pkvm_vm_handle;
	struct pkvm_vm_ref *pkvm_vm_ref;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return;

	pkvm_vm_ref = &pkvm_vms_ref[idx];

	WARN_ON(atomic_dec_if_positive(&pkvm_vm_ref->refcount) <= 0);
}
