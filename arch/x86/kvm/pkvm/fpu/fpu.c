// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <asm/fpu/types.h>
#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>
#include <asm/current.h>

#include "internal.h"
#include "fpu.h"

static DEFINE_PER_CPU(struct fpstate, percpu_fpstate);

void pkvm_init_percpu_fpu(void)
{
	struct fpu *fpu = &current->thread.fpu;

	/*
	 * Set the current fpstate pointer to the percpu_fpstate, which is used
	 * to restore the FPU to the initial state before switching from a pVM
	 * to the host.
	 */
	fpu->fpstate = this_cpu_ptr(&percpu_fpstate);
	fpstate_init_user(fpu->fpstate);

	/* The perm is initialized with the maximum features */
	fpu->perm.__state_perm		= fpu_kernel_cfg.max_features;
	fpu->perm.__state_size		= fpu_kernel_cfg.max_size;
}
