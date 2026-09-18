// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>

bool __read_mostly enable_pkvm;

static int __init early_pkvm_parse_cmdline(char *buf)
{
	return kstrtobool(buf, &enable_pkvm);
}
early_param("kvm-x86.pkvm", early_pkvm_parse_cmdline);
