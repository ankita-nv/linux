// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/dma-buf-mapping.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/overflow.h>
#include <uapi/linux/egm.h>

#include "egm.h"

MODULE_IMPORT_NS("DMA_BUF");

struct nvgrace_egm_dma_buf {
	struct dma_buf *dmabuf;
	struct chardev *egm_chardev;
	struct list_head dmabufs_elm;
	size_t size;
	struct dma_buf_phys_vec *phys_vec;
	u32 nr_ranges;
	u8 revoked : 1;
};

static int nvgrace_egm_dma_buf_attach(struct dma_buf *dmabuf,
				      struct dma_buf_attachment *attachment)
{
	struct nvgrace_egm_dma_buf *priv = dmabuf->priv;

	if (priv->revoked)
		return -ENODEV;

	return 0;
}

static struct dma_buf_phys_list *
nvgrace_egm_dma_pal_map_phys(struct dma_buf_attachment *attach)
{
	struct nvgrace_egm_dma_buf *priv = attach->dmabuf->priv;
	struct dma_buf_phys_list *phys;

	phys = kvmalloc(struct_size(phys, phys, priv->nr_ranges), GFP_KERNEL);
	if (!phys)
		return ERR_PTR(-ENOMEM);

	phys->length = priv->nr_ranges;
	memcpy(phys->phys, priv->phys_vec,
	       sizeof(phys->phys[0]) * priv->nr_ranges);

	return phys;
}

static void nvgrace_egm_dma_pal_unmap_phys(struct dma_buf_attachment *attach,
					   struct dma_buf_phys_list *phys)
{
	kvfree(phys);
}

static const struct dma_buf_mapping_pal_exp_ops nvgrace_egm_dma_buf_pal_ops = {
	.map_phys = nvgrace_egm_dma_pal_map_phys,
	.unmap_phys = nvgrace_egm_dma_pal_unmap_phys,
};

static void nvgrace_egm_dma_buf_release(struct dma_buf *dmabuf)
{
	struct nvgrace_egm_dma_buf *priv = dmabuf->priv;

	/*
	 * Either this or nvgrace_egm_dma_buf_cleanup() will remove from the list.
	 * The refcount prevents both.
	 */
	if (priv->egm_chardev) {
		mutex_lock(&priv->egm_chardev->dmabuf_lock);
		list_del_init(&priv->dmabufs_elm);
		mutex_unlock(&priv->egm_chardev->dmabuf_lock);
	}
	kfree(priv->phys_vec);
	kfree(priv);
}

static int nvgrace_egm_dma_buf_match_mapping(struct dma_buf_match_args *args)
{
	struct nvgrace_egm_dma_buf *priv = args->dmabuf->priv;
	struct dma_buf_mapping_match pal_match;

	dma_resv_assert_held(priv->dmabuf->resv);

	/*
	 * Once we pass nvgrace_egm_dma_buf_cleanup() the dmabuf will never be
	 * usable again.
	 */
	if (!priv->egm_chardev)
		return -ENODEV;

	/* Only provide PAL mapping, no SGT */
	pal_match = DMA_BUF_EMAPPING_PAL(&nvgrace_egm_dma_buf_pal_ops);

	return dma_buf_match_mapping(args, &pal_match, 1);
}

static const struct dma_buf_ops nvgrace_egm_dmabuf_ops = {
	.attach = nvgrace_egm_dma_buf_attach,
	.release = nvgrace_egm_dma_buf_release,
	.match_mapping = nvgrace_egm_dma_buf_match_mapping,
};

static int validate_dmabuf_input(struct egm_dma_buf_export *dma_buf,
				  struct egm_dma_range *dma_ranges,
				  size_t *lengthp)
{
	size_t length = 0;
	u32 i;

	for (i = 0; i < dma_buf->nr_ranges; i++) {
		u64 offset = dma_ranges[i].offset;
		u64 len = dma_ranges[i].length;

		if (!len || !PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
			return -EINVAL;

		if (check_add_overflow(length, len, &length))
			return -EINVAL;
	}

	*lengthp = length;
	return 0;
}

static int nvgrace_egm_fill_phys_vec(struct dma_buf_phys_vec *phys_vec,
				     struct egm_dma_range *dma_ranges,
				     size_t nr_ranges, phys_addr_t start,
				     phys_addr_t len)
{
	phys_addr_t max_addr;
	unsigned int i;

	max_addr = start + len;
	for (i = 0; i < nr_ranges; i++) {
		phys_addr_t end;

		if (!dma_ranges[i].length)
			return -EINVAL;

		if (check_add_overflow(start, dma_ranges[i].offset,
				       &phys_vec[i].paddr) ||
		    check_add_overflow(phys_vec[i].paddr,
				       dma_ranges[i].length, &end))
			return -EOVERFLOW;
		if (end > max_addr)
			return -EINVAL;

		phys_vec[i].len = dma_ranges[i].length;
	}
	return 0;
}

int nvgrace_egm_export_dmabuf(struct chardev *egm_chardev,
			       struct egm_dma_buf_export __user *uarg)
{
	struct egm_dma_buf_export get_dma_buf = {};
	struct egm_dma_range *dma_ranges;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct nvgrace_egm_dma_buf *priv;
	struct nvgrace_egm_dev *egm_dev;
	size_t length;
	int ret;

	if (!egm_chardev)
		return -ENODEV;

	egm_dev = egm_chardev_to_nvgrace_egm_dev(egm_chardev);

	if (copy_from_user(&get_dma_buf, uarg, sizeof(get_dma_buf)))
		return -EFAULT;

	if (!get_dma_buf.nr_ranges || get_dma_buf.flags)
		return -EINVAL;

	dma_ranges = memdup_array_user(&uarg->dma_ranges, get_dma_buf.nr_ranges,
				       sizeof(*dma_ranges));
	if (IS_ERR(dma_ranges))
		return PTR_ERR(dma_ranges);

	ret = validate_dmabuf_input(&get_dma_buf, dma_ranges, &length);
	if (ret)
		goto err_free_ranges;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_free_ranges;
	}
	priv->phys_vec = kcalloc(get_dma_buf.nr_ranges, sizeof(*priv->phys_vec),
				 GFP_KERNEL);
	if (!priv->phys_vec) {
		ret = -ENOMEM;
		goto err_free_priv;
	}

	priv->egm_chardev = egm_chardev;
	priv->nr_ranges = get_dma_buf.nr_ranges;
	priv->size = length;

	ret = nvgrace_egm_fill_phys_vec(priv->phys_vec, dma_ranges,
					priv->nr_ranges, egm_dev->egmphys,
					egm_dev->egmlength);
	if (ret)
		goto err_free_phys;

	kfree(dma_ranges);
	dma_ranges = NULL;

	exp_info.ops = &nvgrace_egm_dmabuf_ops;
	exp_info.size = priv->size;
	exp_info.flags = get_dma_buf.open_flags;
	exp_info.priv = priv;

	priv->dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(priv->dmabuf)) {
		ret = PTR_ERR(priv->dmabuf);
		goto err_free_phys;
	}

	/* dma_buf_put() now frees priv */
	INIT_LIST_HEAD(&priv->dmabufs_elm);
	mutex_lock(&egm_chardev->dmabuf_lock);
	dma_resv_lock(priv->dmabuf->resv, NULL);
	priv->revoked = 0;
	list_add_tail(&priv->dmabufs_elm, &egm_chardev->dmabufs);
	dma_resv_unlock(priv->dmabuf->resv);
	mutex_unlock(&egm_chardev->dmabuf_lock);

	/*
	 * dma_buf_fd() consumes the reference, when the file closes the dmabuf
	 * will be released.
	 */
	ret = dma_buf_fd(priv->dmabuf, get_dma_buf.open_flags);
	if (ret < 0)
		goto err_dma_buf;
	return ret;

err_dma_buf:
	dma_buf_put(priv->dmabuf);
err_free_phys:
	kfree(priv->phys_vec);
err_free_priv:
	kfree(priv);
err_free_ranges:
	kfree(dma_ranges);
	return ret;
}

void nvgrace_egm_dma_buf_cleanup(struct chardev *egm_chardev)
{
	struct nvgrace_egm_dma_buf *priv;
	struct nvgrace_egm_dma_buf *tmp;

	if (!egm_chardev)
		return;

	mutex_lock(&egm_chardev->dmabuf_lock);
	list_for_each_entry_safe(priv, tmp, &egm_chardev->dmabufs, dmabufs_elm) {
		if (!get_file_active(&priv->dmabuf->file))
			continue;

		dma_resv_lock(priv->dmabuf->resv, NULL);
		list_del_init(&priv->dmabufs_elm);
		priv->egm_chardev = NULL;
		priv->revoked = true;
		dma_buf_move_notify(priv->dmabuf);
		dma_resv_unlock(priv->dmabuf->resv);
		fput(priv->dmabuf->file);
	}
	mutex_unlock(&egm_chardev->dmabuf_lock);
}
