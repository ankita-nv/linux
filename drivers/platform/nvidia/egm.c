// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/vfio_pci_core.h>

#define NVGRACE_EGM_DEV_NAME "egm"
#define MAX_EGM_NODES 4

static dev_t dev;
static struct class *class;

struct gpu_node {
	struct list_head list;
	struct pci_dev *pdev;
};

struct nvgrace_egm_dev {
	struct device device;
	struct cdev cdev;
	struct mutex open_lock; /* serialises the first-open scrub */
	unsigned int open_count; /* protected by open_lock */
	phys_addr_t egmphys;
	size_t egmlength;
	u64 egmpxm;
	struct list_head gpus;
};

struct nvgrace_egm_dev_entry {
	struct list_head list;
	struct nvgrace_egm_dev *egm_dev;
};

/*
 * Track egm device lists. Note that there is one device per socket.
 * All the GPUs belonging to the same sockets are associated with
 * the EGM device for that socket.
 */
static LIST_HEAD(egm_chardevs);

static int nvgrace_egm_open(struct inode *inode, struct file *file)
{
	struct nvgrace_egm_dev *egm_dev =
		container_of(inode->i_cdev, struct nvgrace_egm_dev, cdev);
	phys_addr_t phys;
	size_t remaining, chunk_size;
	void *chunk_addr;

	file->private_data = egm_dev;

	/*
	 * Serialise openers so that the first-open scrub completes before
	 * any opener returns. A counter alone would let a second opener
	 * mmap the region (and hand it to a VM) while the first opener is
	 * still zeroing it.
	 */
	guard(mutex)(&egm_dev->open_lock);

	if (egm_dev->open_count) {
		egm_dev->open_count++;
		return 0;
	}

	/*
	 * nvgrace-egm module is responsible to manage the EGM memory as
	 * the host kernel has no knowledge of it. Clear the region before
	 * handing over to userspace.
	 *
	 * Map and zero one chunk at a time rather than mapping the whole
	 * region at once: the EGM region can be very large (hundreds of GiB
	 * on multi-socket systems) and is not in the kernel linear map, so a
	 * single memremap() would create one huge ioremap-style mapping.
	 */
	phys = egm_dev->egmphys;
	remaining = egm_dev->egmlength;

	while (remaining > 0) {
		chunk_size = min(remaining, SZ_1G);

		chunk_addr = memremap(phys, chunk_size, MEMREMAP_WB);
		if (!chunk_addr)
			return -ENOMEM;

		memset(chunk_addr, 0, chunk_size);
		memunmap(chunk_addr);
		cond_resched();

		phys += chunk_size;
		remaining -= chunk_size;
	}

	/*
	 * Mark the device open only after the scrub completes, so a
	 * concurrent opener cannot observe a non-zero count and proceed
	 * before the region has been cleared.
	 */
	egm_dev->open_count = 1;

	return 0;
}

static int nvgrace_egm_release(struct inode *inode, struct file *file)
{
	struct nvgrace_egm_dev *egm_dev =
		container_of(inode->i_cdev, struct nvgrace_egm_dev, cdev);

	guard(mutex)(&egm_dev->open_lock);

	if (!--egm_dev->open_count)
		file->private_data = NULL;

	return 0;
}

static int nvgrace_egm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct nvgrace_egm_dev *egm_dev = file->private_data;
	u64 req_len, pgoff, end;
	unsigned long start_pfn, num_pages;

	pgoff = vma->vm_pgoff;
	num_pages = egm_dev->egmlength >> PAGE_SHIFT;

	/*
	 * Reject a page offset that already lies outside the region before it
	 * is shifted into a byte count. PFN_PHYS(pgoff) would otherwise
	 * overflow phys_addr_t for a large vm_pgoff (reachable via mmap()'s
	 * 64-bit offset) and the wrapped value would slip past the
	 * "end > egmlength" check below.
	 */
	if (pgoff >= num_pages)
		return -EINVAL;

	if (check_sub_overflow(vma->vm_end, vma->vm_start, &req_len) ||
	    check_add_overflow(PHYS_PFN(egm_dev->egmphys), pgoff, &start_pfn) ||
	    check_add_overflow(PFN_PHYS(pgoff), req_len, &end))
		return -EOVERFLOW;

	if (end > egm_dev->egmlength)
		return -EINVAL;

	/*
	 * EGM memory is invisible to the host kernel and is not managed
	 * by it. Map the usermode VMA to the EGM region.
	 */
	return remap_pfn_range(vma, vma->vm_start,
			       start_pfn, req_len,
			       vma->vm_page_prot);
}

static const struct file_operations file_ops = {
	.owner = THIS_MODULE,
	.open = nvgrace_egm_open,
	.release = nvgrace_egm_release,
	.mmap = nvgrace_egm_mmap,
};

static int nvgrace_egm_create_gpu_links(struct nvgrace_egm_dev *egm_dev,
					struct pci_dev *pdev)
{
	int ret;

	ret = sysfs_create_link(&egm_dev->device.kobj,
				&pdev->dev.kobj,
				dev_name(&pdev->dev));

	if (ret && ret != -EEXIST)
		return ret;

	return 0;
}

static void remove_egm_symlinks(struct nvgrace_egm_dev *egm_dev,
				struct pci_dev *pdev)
{
	sysfs_remove_link(&egm_dev->device.kobj, dev_name(&pdev->dev));
}

static int add_gpu(struct nvgrace_egm_dev *egm_dev, struct pci_dev *pdev)
{
	struct gpu_node *node;

	node = kzalloc_obj(*node, GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	/*
	 * The pdev is stored for the lifetime of the EGM device, but it is
	 * obtained from a for_each_pci_dev() walk that drops its reference as
	 * it advances. Take a reference here so the stored pointer stays valid
	 * even if the GPU is later unbound; remove_gpus() drops it.
	 */
	node->pdev = pci_dev_get(pdev);

	list_add_tail(&node->list, &egm_dev->gpus);

	return nvgrace_egm_create_gpu_links(egm_dev, pdev);
}

static void remove_gpus(struct nvgrace_egm_dev *egm_dev)
{
	struct gpu_node *node, *tmp;

	list_for_each_entry_safe(node, tmp, &egm_dev->gpus, list) {
		remove_egm_symlinks(egm_dev, node->pdev);
		list_del(&node->list);
		pci_dev_put(node->pdev);
		kfree(node);
	}
}

static void egm_chardev_release(struct device *dev)
{
	struct nvgrace_egm_dev *egm_chardev = container_of(dev, struct nvgrace_egm_dev, device);

	remove_gpus(egm_chardev);
	mutex_destroy(&egm_chardev->open_lock);
	kfree(egm_chardev);
}

static struct nvgrace_egm_dev *setup_egm_chardev(u64 egmphys, u64 egmlength,
						 u64 egmpxm)
{
	struct nvgrace_egm_dev *egm_chardev;
	int ret;

	egm_chardev = kzalloc_obj(*egm_chardev, GFP_KERNEL);
	if (!egm_chardev)
		goto create_err;

	device_initialize(&egm_chardev->device);

	/*
	 * Use the proximity domain number as the device minor
	 * number. So the EGM corresponding to node X would be
	 * /dev/egmX.
	 */
	egm_chardev->egmphys = egmphys;
	egm_chardev->egmlength = egmlength;
	egm_chardev->egmpxm = egmpxm;
	mutex_init(&egm_chardev->open_lock);
	INIT_LIST_HEAD(&egm_chardev->gpus);

	egm_chardev->device.devt = MKDEV(MAJOR(dev), egm_chardev->egmpxm);
	egm_chardev->device.class = class;
	egm_chardev->device.release = egm_chardev_release;
	cdev_init(&egm_chardev->cdev, &file_ops);
	egm_chardev->cdev.owner = THIS_MODULE;

	ret = dev_set_name(&egm_chardev->device, "egm%llu", egm_chardev->egmpxm);
	if (ret)
		goto error_exit;

	ret = cdev_device_add(&egm_chardev->cdev, &egm_chardev->device);
	if (ret)
		goto error_exit;

	return egm_chardev;

error_exit:
	put_device(&egm_chardev->device);
create_err:
	return NULL;
}

static void del_egm_chardev(struct nvgrace_egm_dev *egm_chardev)
{
	cdev_device_del(&egm_chardev->cdev, &egm_chardev->device);
	put_device(&egm_chardev->device);
}

static char *egm_devnode(const struct device *device, umode_t *mode)
{
	if (mode)
		*mode = 0600;

	return NULL;
}

static ssize_t egm_size_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct nvgrace_egm_dev *egm_dev =
		container_of(dev, struct nvgrace_egm_dev, device);

	return sysfs_emit(buf, "0x%zx\n", egm_dev->egmlength);
}

static DEVICE_ATTR_RO(egm_size);

static struct attribute *attrs[] = {
	&dev_attr_egm_size.attr,
	NULL,
};

static const struct attribute_group attr_group = {
	.attrs = attrs,
};

static const struct attribute_group *attr_groups[] = {
	&attr_group,
	NULL
};

/*
 * Determine if the EGM feature is enabled. If disabled, there
 * will be no EGM properties populated in the ACPI tables and this
 * fetch would fail.
 */
static int has_egm_property(struct pci_dev *pdev, u64 *pegmpxm)
{
	return device_property_read_u64(&pdev->dev, "nvidia,egm-pxm",
					pegmpxm);
}

static int fetch_egm_property(struct pci_dev *pdev, u64 *pegmphys,
			      u64 *pegmlength)
{
	int ret;

	/*
	 * The memory information is present in the system ACPI tables as DSD
	 * properties nvidia,egm-base-pa and nvidia,egm-size.
	 */
	ret = device_property_read_u64(&pdev->dev, "nvidia,egm-size",
				       pegmlength);
	if (ret)
		return ret;

	ret = device_property_read_u64(&pdev->dev, "nvidia,egm-base-pa",
				       pegmphys);

	return ret;
}

static struct nvgrace_egm_dev *get_egm_dev(u64 egmpxm)
{
	struct nvgrace_egm_dev_entry *egm_entry;

	list_for_each_entry(egm_entry, &egm_chardevs, list) {
		/*
		 * A system could have multiple GPUs associated with an
		 * EGM region and will have the same set of EGM region
		 * information. Return the existing EGM device if already
		 * registered through a different GPU on the same socket.
		 */
		if (egm_entry->egm_dev->egmpxm == egmpxm)
			return egm_entry->egm_dev;
	}

	return NULL;
}

static void nvgrace_egm_destroy_pci_devs(void)
{
	struct nvgrace_egm_dev_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &egm_chardevs, list) {
		list_del(&entry->list);
		del_egm_chardev(entry->egm_dev);
		kfree(entry);
	}
}

/*
 * Walk all PCI devices and, for each one that has a companion ACPI object
 * advertising EGM properties, create an auxiliary device for that EGM range.
 * Multiple GPUs on the same socket share a single EGM region (same proximity
 * domain); de-duplicate by skipping a pxm that is already registered.
 */
static int nvgrace_egm_create_pci_egm_devs(void)
{
	struct nvgrace_egm_dev_entry *egm_entry;
	struct nvgrace_egm_dev *egm_dev;
	struct pci_dev *pdev = NULL;
	int ret;

	for_each_pci_dev(pdev) {
		u64 egmphys, egmlength, egmpxm;

		if (has_egm_property(pdev, &egmpxm))
			continue;

		ret = fetch_egm_property(pdev, &egmphys, &egmlength);
		if (ret)
			continue;

		egm_dev = get_egm_dev(egmpxm);
		if (egm_dev) {
			ret = add_gpu(egm_dev, pdev);
			if (ret)
				goto free_dev;
			continue;
		}

		egm_entry = kzalloc_obj(*egm_entry, GFP_KERNEL);
		if (!egm_entry) {
			pci_dev_put(pdev);
			return -ENOMEM;
		}

		egm_dev = setup_egm_chardev(egmphys, egmlength, egmpxm);
		if (!egm_dev) {
			kfree(egm_entry);
			pci_dev_put(pdev);
			return -EINVAL;
		}

		egm_entry->egm_dev = egm_dev;

		ret = add_gpu(egm_entry->egm_dev, pdev);
		if (ret) {
			del_egm_chardev(egm_dev);
			kfree(egm_entry);
			goto free_dev;
		}

		list_add_tail(&egm_entry->list, &egm_chardevs);
	}

	return 0;

free_dev:
	/*
	 * Reached only via goto from inside the loop, where pdev still holds
	 * the reference from the last pci_get_device(); drop it here.
	 */
	pci_dev_put(pdev);
	nvgrace_egm_destroy_pci_devs();
	return ret;
}

/*
 * The char device minor is the EGM proximity-domain number, which is sparse
 * and need not start at 0. Find the lowest proximity domain among the EGM
 * devices so the minor range can be reserved starting there. Returns -ENODEV
 * if the system has no EGM devices.
 */
static int nvgrace_egm_lowest_pxm(u64 *pmin_pxm)
{
	struct pci_dev *pdev = NULL;
	bool found = false;

	for_each_pci_dev(pdev) {
		u64 egmphys, egmlength, egmpxm;

		if (has_egm_property(pdev, &egmpxm))
			continue;

		if (fetch_egm_property(pdev, &egmphys, &egmlength))
			continue;

		if (!found || egmpxm < *pmin_pxm) {
			*pmin_pxm = egmpxm;
			found = true;
		}
	}

	return found ? 0 : -ENODEV;
}

static int __init nvgrace_egm_init(void)
{
	u64 min_pxm;
	int ret;

	/*
	 * Each EGM region is exposed as /dev/egm<pxm> with the proximity
	 * domain as its minor number. Reserve MAX_EGM_NODES minors starting at
	 * the lowest proximity domain so that every egm<pxm> device falls
	 * within the reserved range. With no EGM devices present, fall back to
	 * base 0 and load inertly.
	 */
	if (nvgrace_egm_lowest_pxm(&min_pxm))
		min_pxm = 0;

	ret = alloc_chrdev_region(&dev, min_pxm, MAX_EGM_NODES,
				  NVGRACE_EGM_DEV_NAME);
	if (ret < 0)
		return ret;

	class = class_create(NVGRACE_EGM_DEV_NAME);
	if (IS_ERR(class)) {
		unregister_chrdev_region(dev, MAX_EGM_NODES);
		return PTR_ERR(class);
	}

	class->devnode = egm_devnode;
	class->dev_groups = attr_groups;

	ret = nvgrace_egm_create_pci_egm_devs();
	if (ret)
		goto cleanup_pci_devs;

	return 0;

cleanup_pci_devs:
	nvgrace_egm_destroy_pci_devs();
	class_destroy(class);
	unregister_chrdev_region(dev, MAX_EGM_NODES);

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
