/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_VERBS_DMABUF_H
#define TBV_VERBS_DMABUF_H

#include "../proto/verbs_dmabuf.h"

struct ib_device;
struct tbv_dmabuf_mr;
struct tbv_mr;
struct seq_file;

/* The verbs layer holds QP/MR references until finish, including failures. */
struct tbv_dmabuf_buffer {
	struct tbv_dmabuf_mr *mr;
	struct tbv_mr *host_mr;
	bool uncached; /* Internal credit filler; never part of the wire ABI. */
	u64 offset;
	u64 wr_id; /* Local profiling correlation only; never added to the wire. */
	void *owner;
	void *context;
	int (*copy)(void *context, u32 offset, void *data, u32 bytes, bool to_mr);
	bool (*alive)(void *context);
	int (*finish)(void *context, int status);
};

void tbv_dmabuf_progress_arm(void);
bool tbv_dmabuf_verbs_enabled(void);
bool tbv_dmabuf_credits_enabled(void);
void tbv_dmabuf_recv_posted(void *owner);
void tbv_dmabuf_credit_received(struct tbv_peer *peer,
				const struct tbv_dmabuf_wire *m);
void tbv_dmabuf_debugfs_show(struct seq_file *s);
int tbv_dmabuf_start(struct tbv_state *state);
void tbv_dmabuf_stop(void);
void tbv_dmabuf_disconnect(struct tbv_rail *rail);
int tbv_dmabuf_quiesce_rail(struct tbv_rail *rail);
void tbv_dmabuf_cancel(void *context);
void tbv_dmabuf_cancel_recv(void *owner);
int tbv_dmabuf_handle_packet(struct tbv_state *state,
			     struct tb_xdomain *source, const void *buf, size_t size);
struct tbv_dmabuf_mr *tbv_dmabuf_import(struct ib_device *device, u64 offset,
				       u64 length, int fd, int access);
void tbv_dmabuf_release(struct tbv_dmabuf_mr *mr);
int tbv_dmabuf_submit(struct tbv_state *state, struct tbv_rail *rail,
		       const struct tbv_native_data_header *hdr,
		       const struct tbv_dmabuf_buffer *buffer, u8 retries,
		       u8 rnr_retries, unsigned long timeout);
bool tbv_dmabuf_rail_ready(struct tbv_rail *rail);

int tbv_ibdev_dmabuf_begin(struct tbv_state *state, struct tbv_rail *rail,
			    const struct tbv_native_data_header *hdr,
			    struct tbv_dmabuf_buffer *buffer);
int tbv_ibdev_dmabuf_peek(struct tbv_state *state, struct tbv_rail *rail,
			  struct tbv_native_data_header *hdr,
			  struct tbv_dmabuf_buffer *buffer);
int tbv_ibdev_dmabuf_claim(struct tbv_dmabuf_buffer *buffer,
			   const struct tbv_native_data_header *hdr);

#endif
