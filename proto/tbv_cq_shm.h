/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_CQ_SHM_H
#define TBV_CQ_SHM_H
#include <linux/types.h>

#define TBV_CQ_SHM_MAGIC 0x54425643u /* "TBVC" */
#define TBV_CQ_SHM_ABI 2u

/*
 * Page zero is this header; the remaining pages hold ib_uverbs_wc entries.
 * The kernel keeps an independent ib_wc ring, containing kernel-only pointers.
 * Kernel publication uses release stores; polling uses acquire loads. Pollers
 * serialize on a provider lock and publish consumed only after reading entries.
 * The kernel validates consumed against its PRIVATE produced/consumed counters.
 * It never trusts shared metadata, produced, ovf_seq or ring contents.
 * Private head/tail cursors avoid modulo errors when the totals wrap at 2^32.
 * ovf_seq is a permanent error latch: overrun makes a CQ unusable (CQ_ERR).
 * A CQ has one consumer path: mapped provider polling or generic ioctl polling.
 */
struct tbv_cq_shm {
	__u32 magic;
	__u32 abi;
	__u32 cqe;
	__u32 entry_size;
	__u32 ring_offset;
	__u32 produced;
	__u32 consumed;
	__u32 ovf_seq;
	__u32 map_len;
	__u32 reserved[39];
};

/* Vendor tail after ib_uverbs_create_cq_resp. Zero shm_abi means fallback.
 * The first valid mmap offset may be zero. map_len permits one full mapping.
 */
struct tbv_uresp_create_cq {
	__aligned_u64 cq_mmap_offset;
	__u32 cqe;
	__u32 shm_abi;
	__u32 reserved;
	__u32 pad;
	__aligned_u64 map_len;
};
#endif
