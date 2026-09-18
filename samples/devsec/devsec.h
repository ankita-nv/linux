/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 - 2026 Intel Corporation */

#ifndef __DEVSEC_H__
#define __DEVSEC_H__

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)
#include <linux/pci-ecam.h>
#endif

#define NR_DEVSEC_HOST_BRIDGES 2

struct devsec_sysdata {
#ifdef CONFIG_X86
	/*
	 * Must be first member to that x86::pci_domain_nr() can type
	 * pun devsec_sysdata and pci_sysdata.
	 */
	struct pci_sysdata sd;
#else
#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)
	/*
	 * Must be first member so that the ACPI implementation of
	 * pcibios_root_bridge_prepare() can type pun devsec_sysdata and
	 * pci_config_window. Like pci-hyperv.c, ->parent is left NULL
	 * since an emulated host bridge has no ACPI companion device.
	 */
	struct pci_config_window cfg;
#endif
	int domain_nr;
#endif
};

#ifdef CONFIG_X86
static inline void devsec_set_domain_nr(struct devsec_sysdata *sd,
					int domain_nr)
{
	sd->sd.domain = domain_nr;
}
static inline int devsec_get_domain_nr(struct devsec_sysdata *sd)
{
	return sd->sd.domain;
}
#else
static inline void devsec_set_domain_nr(struct devsec_sysdata *sd,
					int domain_nr)
{
	sd->domain_nr = domain_nr;
}
static inline int devsec_get_domain_nr(struct devsec_sysdata *sd)
{
	return sd->domain_nr;
}
#endif
extern struct devsec_sysdata *devsec_sysdata[NR_DEVSEC_HOST_BRIDGES];
#endif /* __DEVSEC_H__ */
