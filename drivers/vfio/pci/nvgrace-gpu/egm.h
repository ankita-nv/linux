/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#ifndef EGM_H
#define EGM_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <uapi/linux/egm.h>

struct chardev;
struct nvgrace_egm_dev;

struct nvgrace_egm_dev *egm_chardev_to_nvgrace_egm_dev(struct chardev *egm_chardev);

int nvgrace_egm_export_dmabuf(struct chardev *egm_chardev,
			       struct egm_dma_buf_export __user *uarg);
void nvgrace_egm_dma_buf_cleanup(struct chardev *egm_chardev);

#endif /* EGM_H */
