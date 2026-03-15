// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/hashtable.h>
#include <linux/sizes.h>
#include <linux/vfio_pci_core.h>
#include <uapi/linux/egm.h>

#define NVGRACE_EGM_DEV_NAME "egm"
#define MAX_EGM_NODES 4
#define EGM_OFFSET_SHIFT 40

struct h_node {
	unsigned long mem_offset;
	struct hlist_node node;
};

static dev_t dev;
static struct class *class;

struct gpu_node {
	struct list_head list;
	struct pci_dev *pdev;
};

struct nvgrace_egm_dev {
	struct device device;
	struct cdev cdev;
	atomic_t open_count;
	DECLARE_HASHTABLE(htbl, 16);
	phys_addr_t egmphys;
	size_t egmlength;
	phys_addr_t retiredpagesphys;
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
	void *memaddr;

	if (atomic_inc_return(&egm_dev->open_count) > 1)
		return 0;

	/*
	 * nvgrace-egm module is responsible to manage the EGM memory as
	 * the host kernel has no knowledge of it. Clear the region before
	 * handing over to userspace.
	 */
	memaddr = memremap(egm_dev->egmphys, egm_dev->egmlength, MEMREMAP_WB);
	if (!memaddr) {
		atomic_dec(&egm_dev->open_count);
		return -ENOMEM;
	} else {
		size_t remaining = egm_dev->egmlength;
		u8 *chunk_addr = memaddr;
		size_t chunk_size;

		while (remaining > 0) {
			chunk_size = min(remaining, SZ_1G);
			memset(chunk_addr, 0, chunk_size);
			cond_resched();
			chunk_addr += chunk_size;
			remaining -= chunk_size;
		}
	}

	memunmap(memaddr);

	file->private_data = egm_dev;

	return 0;
}

static int nvgrace_egm_release(struct inode *inode, struct file *file)
{
	struct nvgrace_egm_dev *egm_dev =
		container_of(inode->i_cdev, struct nvgrace_egm_dev, cdev);

	if (atomic_dec_and_test(&egm_dev->open_count))
		file->private_data = NULL;

	return 0;
}

static int nvgrace_egm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct nvgrace_egm_dev *egm_dev = file->private_data;
	u64 req_len, pgoff, end;
	unsigned long start_pfn;

	pgoff = vma->vm_pgoff &
		((1U << (EGM_OFFSET_SHIFT - PAGE_SHIFT)) - 1);

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

static long nvgrace_egm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned long minsz = offsetofend(struct egm_retired_pages_list, count);
	struct egm_retired_pages_list info;
	void __user *uarg = (void __user *)arg;
	struct nvgrace_egm_dev *egm_dev = file->private_data;

	if (copy_from_user(&info, uarg, minsz))
		return -EFAULT;

	if (info.argsz < minsz || !egm_dev)
		return -EINVAL;

	switch (cmd) {
	case EGM_RETIRED_PAGES_LIST: {
		unsigned long retired_page_struct_size = sizeof(struct egm_retired_pages_info);
		struct egm_retired_pages_info tmp;
		struct h_node *cur_page;
		struct hlist_node *tmp_node;
		unsigned long bkt;
		int count = 0, index = 0;
		int ret;

		hash_for_each_safe(egm_dev->htbl, bkt, tmp_node, cur_page, node)
			count++;

		if (info.argsz < (minsz + count * retired_page_struct_size)) {
			info.argsz = minsz + count * retired_page_struct_size;
			info.count = 0;
			goto done;
		} else {
			hash_for_each_safe(egm_dev->htbl, bkt, tmp_node, cur_page, node) {
				/*
				 * This check fails if there was an ECC error
				 * after the usermode app read the count of
				 * bad pages through this ioctl.
				 */
				if (minsz + index * retired_page_struct_size >= info.argsz) {
					info.argsz = minsz + index * retired_page_struct_size;
					info.count = index;
					goto done;
				}

				tmp.offset = cur_page->mem_offset;
				tmp.size = PAGE_SIZE;

				ret = copy_to_user(uarg + minsz +
						   index * retired_page_struct_size,
						   &tmp, retired_page_struct_size);
				if (ret)
					return -EFAULT;
				index++;
			}

			info.count = index;
		}
		break;
	}
	default:
		return -EINVAL;
	}

done:
	return copy_to_user(uarg, &info, minsz) ? -EFAULT : 0;
}

static const struct file_operations file_ops = {
	.owner = THIS_MODULE,
	.open = nvgrace_egm_open,
	.release = nvgrace_egm_release,
	.mmap = nvgrace_egm_mmap,
	.unlocked_ioctl = nvgrace_egm_ioctl,
};

static int add_gpu(struct nvgrace_egm_dev *egm_dev, struct pci_dev *pdev)
{
	struct gpu_node *node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->pdev = pdev;

	list_add_tail(&node->list, &egm_dev->gpus);

	return 0;
}

static void remove_gpus(struct nvgrace_egm_dev *egm_dev)
{
	struct gpu_node *node, *tmp;

	list_for_each_entry_safe(node, tmp, &egm_dev->gpus, list) {
		list_del(&node->list);
		kfree(node);
	}
}

static void egm_chardev_release(struct device *dev)
{
	struct nvgrace_egm_dev *egm_chardev = container_of(dev, struct nvgrace_egm_dev, device);

	remove_gpus(egm_chardev);
	kfree(egm_chardev);
}

static struct nvgrace_egm_dev *setup_egm_chardev(u64 egmphys, u64 egmlength,
						  u64 egmpxm,
						  u64 retiredpagesphys)
{
	struct nvgrace_egm_dev *egm_chardev;
	int ret;

	egm_chardev = kzalloc(sizeof(*egm_chardev), GFP_KERNEL);
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
	egm_chardev->retiredpagesphys = retiredpagesphys;
	atomic_set(&egm_chardev->open_count, 0);
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

static void cleanup_retired_pages(struct nvgrace_egm_dev *egm_dev)
{
	struct h_node *cur_page;
	unsigned long bkt;
	struct hlist_node *temp_node;

	hash_for_each_safe(egm_dev->htbl, bkt, temp_node, cur_page, node) {
		hash_del(&cur_page->node);
		kvfree(cur_page);
	}
}

static int nvgrace_egm_fetch_retired_pages(struct nvgrace_egm_dev *egm_dev)
{
	u64 count;
	void *memaddr;
	int index, ret = 0;

	memaddr = memremap(egm_dev->retiredpagesphys, PAGE_SIZE, MEMREMAP_WB);
	if (!memaddr)
		return -ENOMEM;

	count = *(u64 *)memaddr;
	if (count > PAGE_SIZE / sizeof(count))
		return -EINVAL;

	for (index = 0; index < count; index++) {
		struct h_node *retired_page;

		/*
		 * Since the EGM is linearly mapped, the offset in the
		 * carveout is the same offset in the VM system memory.
		 *
		 * Calculate the offset to communicate to the usermode
		 * apps.
		 */
		retired_page = kvzalloc(sizeof(*retired_page), GFP_KERNEL);
		if (!retired_page) {
			ret = -ENOMEM;
			break;
		}

		retired_page->mem_offset = *((u64 *)memaddr + index + 1) -
					   egm_dev->egmphys;
		hash_add(egm_dev->htbl, &retired_page->node,
			 retired_page->mem_offset);
	}

	memunmap(memaddr);

	if (ret)
		cleanup_retired_pages(egm_dev);

	return ret;
}

static char *egm_devnode(const struct device *device, umode_t *mode)
{
	if (mode)
		*mode = 0600;

	return NULL;
}

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
			      u64 *pegmlength, u64 *pretiredpagesphys)
{
	int ret;

	/*
	 * The EGM memory information is present in the system ACPI tables
	 * as DSD properties nvidia,egm-base-pa and nvidia,egm-size.
	 */
	ret = device_property_read_u64(&pdev->dev, "nvidia,egm-size",
				       pegmlength);
	if (ret)
		goto error_exit;

	ret = device_property_read_u64(&pdev->dev, "nvidia,egm-base-pa",
				       pegmphys);
	if (ret)
		goto error_exit;

	/*
	 * SBIOS puts the list of retired pages on a region. The region
	 * SPA is exposed as "nvidia,egm-retired-pages-data-base".
	 */
	ret = device_property_read_u64(&pdev->dev,
				       "nvidia,egm-retired-pages-data-base",
				       pretiredpagesphys);
	if (ret)
		goto error_exit;

	/* Catch firmware bug and avoid a crash */
	if (*pretiredpagesphys == 0) {
		dev_err(&pdev->dev, "Retired pages region is not setup\n");
		ret = -EINVAL;
	}

error_exit:
	return ret;
}

static bool is_duplicate_egm_entry(u64 egmpxm)
{
	struct nvgrace_egm_dev_entry *egm_entry;

	list_for_each_entry(egm_entry, &egm_chardevs, list) {
		/*
		 * A system could have multiple GPUs associated with an
		 * EGM region and will have the same set of EGM region
		 * information. Skip the EGM region information fetch if
		 * already done through a differnt GPU on the same socket.
		 */
		if (egm_entry->egm_dev->egmpxm == egmpxm)
			return true;
	}

	return false;
}

static void nvgrace_egm_destroy_pci_devs(void)
{
	struct nvgrace_egm_dev_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &egm_chardevs, list) {
		list_del(&entry->list);
		cleanup_retired_pages(entry->egm_dev);
		del_egm_chardev(entry->egm_dev);
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
		u64 egmphys, egmlength, egmpxm, retiredpagesphys;

		if (has_egm_property(pdev, &egmpxm))
			continue;

		ret = fetch_egm_property(pdev, &egmphys, &egmlength,
					 &retiredpagesphys);
		if (ret)
			continue;

		if (is_duplicate_egm_entry(egmpxm))
			continue;

		egm_entry = kzalloc(sizeof(*egm_entry), GFP_KERNEL);
		if (!egm_entry)
			return -ENOMEM;

		egm_dev = setup_egm_chardev(egmphys, egmlength, egmpxm,
					    retiredpagesphys);
		if (!egm_dev) {
			kfree(egm_entry);
			return -EINVAL;
		}

		hash_init(egm_dev->htbl);

		ret = nvgrace_egm_fetch_retired_pages(egm_dev);
		if (ret) {
			del_egm_chardev(egm_dev);
			kfree(egm_entry);
			return ret;
		}

		egm_entry->egm_dev = egm_dev;

		ret = add_gpu(egm_entry->egm_dev, pdev);
		if (ret)
			goto free_dev;

		list_add_tail(&egm_entry->list, &egm_chardevs);
	}

	return 0;

free_dev:
	nvgrace_egm_destroy_pci_devs();
	return ret;
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
