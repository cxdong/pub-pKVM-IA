// SPDX-License-Identifier: GPL-2.0
#include <linux/kbuild.h>
#include <vmx/vmx.h>
#include "memory.h"
#include "pkvm.h"
#include "x86.h"

int main(void)
{
	DEFINE(PKVM_VMEMMAP_ENTRY_SIZE, sizeof(struct pkvm_page));
	DEFINE(PKVM_VMX_VM_SIZE, PKVM_VM_BASE_SIZE + sizeof(struct kvm_vmx));
	DEFINE(PKVM_VMX_VCPU_SIZE, PKVM_VCPU_BASE_SIZE + sizeof(struct vcpu_vmx) +
				   KVM_MCE_SIZE + KVM_MCI_CTL2_SIZE);
	return 0;
}
