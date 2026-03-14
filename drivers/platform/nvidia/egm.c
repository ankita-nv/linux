// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/vfio_pci_core.h>

#define NVGRACE_EGM_DEV_NAME "egm"
#define MAX_EGM_NODES 4

static dev_t dev;
static struct class *class;

static char *egm_devnode(const struct device *device, umode_t *mode)
{
	if (mode)
		*mode = 0600;

	return NULL;
}

/*
 * Walk all PCI devices and, for each one that has a companion ACPI object
 * advertising EGM properties, create an auxiliary device for that EGM range.
 * Multiple GPUs on the same socket share a single EGM region (same proximity
 * domain); de-duplicate by skipping a pxm that is already registered.
 */
static int nvgrace_egm_create_pci_egm_devs(void)
{
	struct pci_dev *pdev = NULL;

	for_each_pci_dev(pdev) {
		u64 egmpxm;

		/*
		 * EGM is an optional feature controlled by SBIOS. If it is
		 * disabled, the companion ACPI object will not carry the
		 * "nvidia,egm-pxm" property and the read will fail. Treat
		 * that as a non-fatal absence and skip to the next device.
		 */
		if (device_property_read_u64(&pdev->dev, "nvidia,egm-pxm",
					     &egmpxm))
			continue;
	}

	return 0;
}

static void nvgrace_egm_destroy_pci_devs(void)
{
}

static int __init nvgrace_egm_init(void)
{
	int ret;

	/*
	 * Each EGM region on a system is represented with a unique
	 * char device with a different minor number. Allow a range
	 * of char device creation.
	 */
	ret = alloc_chrdev_region(&dev, 0, MAX_EGM_NODES,
				  NVGRACE_EGM_DEV_NAME);
	if (ret < 0)
		return ret;

	class = class_create(NVGRACE_EGM_DEV_NAME);
	if (IS_ERR(class)) {
		unregister_chrdev_region(dev, MAX_EGM_NODES);
		return PTR_ERR(class);
	}

	class->devnode = egm_devnode;

	ret = nvgrace_egm_create_pci_egm_devs();
	if (ret)
		goto cleanup_pci_devs;

	return 0;

cleanup_pci_devs:
	nvgrace_egm_destroy_pci_devs();

	return ret;
}

static void __exit nvgrace_egm_cleanup(void)
{
	nvgrace_egm_destroy_pci_devs();
	class_destroy(class);
	unregister_chrdev_region(dev, MAX_EGM_NODES);
}

module_init(nvgrace_egm_init);
module_exit(nvgrace_egm_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ankit Agrawal <ankita@nvidia.com>");
MODULE_DESCRIPTION("NVGRACE EGM - Module to support Extended GPU Memory on NVIDIA Grace Based systems");
