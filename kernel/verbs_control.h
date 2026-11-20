/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_VERBS_CONTROL_H
#define TBV_VERBS_CONTROL_H

#include "../proto/verbs_control.h"

struct tbv_control;
struct tbv_ctrl_pending;
struct tbv_peer;
struct tbv_state;
struct tb_xdomain;
struct seq_file;
struct dentry;

bool tbv_control_ring_requested(void);
bool tbv_control_enabled(void);
bool tbv_control_push_enabled(void);
bool tbv_control_xdomain_required(const struct tbv_peer *peer);
bool tbv_control_required(const struct tbv_peer *peer);
bool tbv_control_ready(const struct tbv_peer *peer);
int tbv_control_setup(struct tbv_peer *peer);
void tbv_control_stop(struct tbv_peer *peer);
void tbv_control_destroy(struct tbv_peer *peer);
void tbv_control_get(struct tbv_control *ctrl);
void tbv_control_put(struct tbv_control *ctrl);
void tbv_control_handle_done(struct tbv_state *state, struct tb_xdomain *source,
			     const struct tbv_dmabuf_wire *m);
int tbv_control_xdomain_exchange(struct tbv_peer *peer,
				 const struct tbv_dmabuf_wire *m, u32 timeout_ms);
struct tbv_ctrl_pending *tbv_control_open(struct tbv_peer *peer,
					 const struct tbv_dmabuf_wire *m);
void tbv_control_close(struct tbv_ctrl_pending *p);
int tbv_control_exchange(struct tbv_ctrl_pending *p,
			 const struct tbv_dmabuf_wire *m, u32 timeout_ms);
int tbv_control_wait_done(struct tbv_ctrl_pending *p, u32 timeout_ms);
u32 tbv_control_reply_credit(const struct tbv_ctrl_pending *p);
struct tbv_control *tbv_control_of(const struct tbv_ctrl_pending *p);
bool tbv_control_credits(const struct tbv_control *ctrl);
int tbv_control_notify(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *m);
void tbv_control_done(struct tbv_control *ctrl,
		      const struct tbv_dmabuf_wire *m, int status);
void tbv_control_debugfs_init(struct tbv_state *state, struct dentry *dir);
void tbv_control_debugfs_exit(void);
void tbv_control_debugfs_show(struct seq_file *s, struct tbv_peer *peer);
int tbv_dmabuf_control_request(struct tbv_peer *peer, struct tbv_control *ctrl,
			       struct tbv_dmabuf_wire *m);

#endif
