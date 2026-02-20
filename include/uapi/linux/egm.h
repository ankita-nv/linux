/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#ifndef _UAPIEGM_H
#define _UAPIEGM_H

#include <linux/types.h>

#define EGM_TYPE ('E')

struct egm_retired_pages_info {
	__aligned_u64 offset;
	__aligned_u64 size;
};

struct egm_retired_pages_list {
	__u32 argsz;
	/* out */
	__u32 count;
	/* out */
	struct egm_retired_pages_info retired_pages[];
};

struct egm_dma_range {
	__aligned_u64 offset;
	__aligned_u64 length;
};

struct egm_dma_buf_export {
	__u32 argsz;
	__u32 flags;
	__u32 nr_ranges;
	__u32 open_flags;
	struct egm_dma_range dma_ranges[];
};

#define EGM_RETIRED_PAGES_LIST     _IO(EGM_TYPE, 100)
#define EGM_EXPORT_DMABUF          _IOWR(EGM_TYPE, 101, struct egm_dma_buf_export)

#endif /* _UAPIEGM_H */
