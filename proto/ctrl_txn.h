/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_CTRL_TXN_H
#define TBV_CTRL_TXN_H

#include "verbs_dmabuf.h"

/*
 * Decision logic of one descriptor-verbs control transaction, kept free of
 * lists, locks and completions so the module and the unit test share it.
 */
struct tbv_ctrl_txn {
	tbv_wire_u16 expected;
	int status, done_status;
	bool replied, completed;
};

enum tbv_ctrl_apply {
	TBV_CTRL_APPLY_NONE,
	TBV_CTRL_APPLY_DONE,
	TBV_CTRL_APPLY_REPLY,
};

static inline bool tbv_ctrl_need_xdomain(bool ring, bool remote_ring,
					 bool push, bool remote_push)
{
	return push && remote_push && !(ring && remote_ring);
}

static inline bool tbv_ctrl_need(bool ring, bool remote_ring,
				 bool push, bool remote_push)
{
	return (ring && remote_ring) ||
		tbv_ctrl_need_xdomain(ring, remote_ring, push, remote_push);
}

/* A DONE carrying a positive status other than DONE establishes nothing. */
static inline bool tbv_ctrl_done_malformed(const struct tbv_dmabuf_wire *m)
{
	return m->op == (TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY) &&
		m->status != TBV_DMABUF_DONE && m->status >= 0;
}

/* The first DONE and the first expected reply win; later ones change nothing. */
static inline enum tbv_ctrl_apply
tbv_ctrl_txn_apply(struct tbv_ctrl_txn *t, const struct tbv_dmabuf_wire *m)
{
	if (m->op == (TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY)) {
		if (t->completed)
			return TBV_CTRL_APPLY_NONE;
		t->done_status = m->status;
		t->completed = true;
		return TBV_CTRL_APPLY_DONE;
	}
	if (m->op != (t->expected | TBV_DMABUF_REPLY) || t->replied)
		return TBV_CTRL_APPLY_NONE;
	t->status = m->status;
	t->replied = true;
	return TBV_CTRL_APPLY_REPLY;
}

static inline int tbv_ctrl_txn_result(const struct tbv_ctrl_txn *t, int failure)
{
	if (failure)
		return failure;
	return t->completed ? t->done_status : TBV_DMABUF_BUSY;
}

#endif /* TBV_CTRL_TXN_H */
