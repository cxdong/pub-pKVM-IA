// SPDX-License-Identifier: GPL-2.0
#include <asm/trapnr.h>
#include "debug.h"
#include "idt.h"

static void default_exception_handler(struct pt_regs *regs,
				      int vector, bool has_error_code)
{
	if (has_error_code)
		pkvm_err("Exception %d @ip %pS (0x%px), err code 0x%lx\n",
			 vector, (void *)regs->ip, (void *)regs->ip, regs->orig_ax);
	else
		pkvm_err("Exception %d @ip %pS (0x%px), no err code\n",
			 vector, (void *)regs->ip, (void *)regs->ip);

	asm volatile("hlt" : : : "memory");
}

static exception_handler_t exception_handlers[X86_TRAP_IRET] = {
#define GEN(x, ...)	\
		[x] = default_exception_handler,
#include <asm/GEN-for-each-exc.h>
#undef GEN
};

void handle_exception(struct pt_regs *regs, int vector, bool has_error_code)
{
	exception_handler_t handler;

	if (vector >= X86_TRAP_IRET)
		return;

	handler = exception_handlers[vector];
	handler(regs, vector, has_error_code);
}

void pkvm_register_excp_handler(int vector, exception_handler_t handler)
{
	if (vector >= X86_TRAP_IRET)
		return;

	exception_handlers[vector] = handler;
}
