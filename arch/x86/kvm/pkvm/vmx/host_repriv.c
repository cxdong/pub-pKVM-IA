// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Google
 */

#include <vmx/vmx.h>
#include "host_vmx.h"

struct host_cpu_state {
	unsigned long cr0, cr3, cr4;
	unsigned long rip, rsp;
	unsigned long rflags;
	unsigned long fsbase, gsbase;
	unsigned long long debugctl, perf_global_ctrl;
	unsigned long long sysenter_cs, sysenter_esp, sysenter_eip;
	unsigned long long efer, cr_pat;
	unsigned short cs, ds, es, fs, gs, ss;

	struct desc_ptr gdt, idt;
};

static inline void read_host_cpu_state(struct host_cpu_state *hcs)
{
	hcs->rsp = vmcs_readl(GUEST_RSP);
	hcs->rip = vmcs_readl(GUEST_RIP) + vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	hcs->rflags = vmcs_readl(GUEST_RFLAGS);

	hcs->ds = vmcs_read16(GUEST_DS_SELECTOR);
	hcs->es = vmcs_read16(GUEST_ES_SELECTOR);
	hcs->fs = vmcs_read16(GUEST_FS_SELECTOR);
	hcs->gs = vmcs_read16(GUEST_GS_SELECTOR);
	hcs->ss = vmcs_read16(GUEST_SS_SELECTOR);
	hcs->cs = vmcs_read16(GUEST_CS_SELECTOR);

	hcs->fsbase = vmcs_readl(GUEST_FS_BASE);
	hcs->gsbase = vmcs_readl(GUEST_GS_BASE);

	hcs->gdt.address = vmcs_readl(GUEST_GDTR_BASE);
	hcs->gdt.size = vmcs_read32(GUEST_GDTR_LIMIT);
	hcs->idt.address = vmcs_readl(GUEST_IDTR_BASE);
	hcs->idt.size = vmcs_read32(GUEST_IDTR_LIMIT);

	hcs->debugctl = vmcs_read64(GUEST_IA32_DEBUGCTL);
	hcs->perf_global_ctrl = vmcs_read64(GUEST_IA32_PERF_GLOBAL_CTRL);
	hcs->sysenter_cs = vmcs_read32(GUEST_SYSENTER_CS);
	hcs->sysenter_esp = vmcs_readl(GUEST_SYSENTER_ESP);
	hcs->sysenter_eip = vmcs_readl(GUEST_SYSENTER_EIP);
	hcs->efer = vmcs_read64(GUEST_IA32_EFER);
	hcs->cr_pat = vmcs_read64(GUEST_IA32_PAT);

	hcs->cr0 = vmcs_readl(GUEST_CR0);
	hcs->cr3 = vmcs_readl(GUEST_CR3);
	hcs->cr4 = vmcs_readl(GUEST_CR4);
}

#define PKVM_WRITE_CR(crnum, val) \
static inline void __pkvm_write_cr##crnum(unsigned long val) \
{							\
	asm volatile("mov %0,%%cr" #crnum : "+r" (val) : : "memory"); \
}

PKVM_WRITE_CR(0, val)
PKVM_WRITE_CR(3, val)
PKVM_WRITE_CR(4, val)

static inline void restore_host_special_regs(struct host_cpu_state *hcs)
{
	struct desc_struct *gdt_desc;
	tss_desc *tss;

	/* Reset the busy bit to reload TR */
	gdt_desc = (struct desc_struct *)(hcs->gdt.address);
	tss = (tss_desc *)&gdt_desc[GDT_ENTRY_TSS];
	tss->type = DESC_TSS;

	__pkvm_write_cr4(hcs->cr4);
	__pkvm_write_cr0(hcs->cr0);
	__pkvm_write_cr3(hcs->cr3);

	wrmsrq_safe(MSR_CORE_PERF_GLOBAL_CTRL, hcs->perf_global_ctrl);
	wrmsrq(MSR_IA32_DEBUGCTLMSR, hcs->debugctl);
	wrmsrq(MSR_IA32_SYSENTER_CS, hcs->sysenter_cs);
	wrmsrq(MSR_IA32_SYSENTER_ESP, hcs->sysenter_esp);
	wrmsrq(MSR_IA32_SYSENTER_EIP, hcs->sysenter_eip);
	wrmsrq(MSR_IA32_CR_PAT, hcs->cr_pat);
	wrmsrq(MSR_EFER, hcs->efer);

	asm volatile (
		"lgdt %0\n"
		"lidt %1\n"
		"ltr %w2\n"
		"mov %3, %%ds\n"
		"mov %4, %%es\n"
		"mov %5, %%fs\n"
		"mov %6, %%gs\n"

		:
		: "m"(hcs->gdt), "m"(hcs->idt), "q"(GDT_ENTRY_TSS*8),
		  "m"(hcs->ds), "m"(hcs->es), "m"(hcs->fs), "m"(hcs->gs)
		: "memory"
	);

	wrmsrq(MSR_FS_BASE, hcs->fsbase);
	wrmsrq(MSR_GS_BASE, hcs->gsbase);
}

/* Restores host cpu state and returns to host in VMX root mode. */
void pkvm_vmx_reprivilege_cpu(unsigned long *vcpu_regs)
{
	static struct host_cpu_state hcs;

	read_host_cpu_state(&hcs);
	restore_host_special_regs(&hcs);

	pkvm_vmx_reprivilege_return(vcpu_regs, hcs.ss, hcs.rsp, hcs.rflags,
				    hcs.cs, hcs.rip);
}
