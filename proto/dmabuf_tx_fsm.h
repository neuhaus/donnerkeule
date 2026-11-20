/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_DMABUF_TX_FSM_H
#define TBV_DMABUF_TX_FSM_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

/*
 * Explicit state machine for one DMA-BUF TX batch.
 *
 * Pure transition logic lives here with no kernel locks, timers, or work
 * items so both the kernel driver and userspace unit tests execute the same
 * rules.
 */
enum tbv_dmabuf_tx_state {
	TBV_DMABUF_TX_BUILD,
	TBV_DMABUF_TX_ARM,
	TBV_DMABUF_TX_POST,
	TBV_DMABUF_TX_WAIT_DMA,
	TBV_DMABUF_TX_WAIT_DONE,
	TBV_DMABUF_TX_ABORT,
	TBV_DMABUF_TX_FINISH,
};

enum tbv_dmabuf_tx_event {
	TBV_DMABUF_TX_EV_BUILD_OK,
	TBV_DMABUF_TX_EV_BUILD_ERR,
	TBV_DMABUF_TX_EV_COMMIT_OK,
	TBV_DMABUF_TX_EV_COMMIT_ERR,
	TBV_DMABUF_TX_EV_CANCELED,
	TBV_DMABUF_TX_EV_PREPARE_READY,
	TBV_DMABUF_TX_EV_PREPARE_DONE,
	TBV_DMABUF_TX_EV_PREPARE_BUSY,
	TBV_DMABUF_TX_EV_PREPARE_RNR,
	TBV_DMABUF_TX_EV_PREPARE_ERR,
	TBV_DMABUF_TX_EV_POST_OK,
	TBV_DMABUF_TX_EV_POST_ERR,
	TBV_DMABUF_TX_EV_DMA_OK,
	TBV_DMABUF_TX_EV_DMA_DRAINED,
	TBV_DMABUF_TX_EV_DMA_ERR,
	TBV_DMABUF_TX_EV_DONE_OK,
	TBV_DMABUF_TX_EV_DONE_BUSY,
	TBV_DMABUF_TX_EV_DONE_ERR,
	TBV_DMABUF_TX_EV_ABORT_DONE,
};

enum tbv_dmabuf_tx_action {
	TBV_DMABUF_TX_ACT_BUILD,
	TBV_DMABUF_TX_ACT_ARM,
	TBV_DMABUF_TX_ACT_WAIT_RNR,
	TBV_DMABUF_TX_ACT_SLEEP_ARM,
	TBV_DMABUF_TX_ACT_POST,
	TBV_DMABUF_TX_ACT_WAIT_DMA,
	TBV_DMABUF_TX_ACT_WAIT_DONE,
	TBV_DMABUF_TX_ACT_SLEEP_DONE,
	TBV_DMABUF_TX_ACT_ABORT,
	TBV_DMABUF_TX_ACT_FINISH,
};

struct tbv_dmabuf_tx_fsm {
	enum tbv_dmabuf_tx_state state;
	u8 retries;
	u8 rnr_retries;
	u32 failures;
	u32 rnr;
	int err;
	bool remote_armed;
	bool canceled;
};

static inline void tbv_dmabuf_tx_fsm_init(struct tbv_dmabuf_tx_fsm *fsm,
					  u8 retries, u8 rnr_retries,
					  bool direct_posted, int direct_ret)
{
	fsm->retries = retries;
	fsm->rnr_retries = rnr_retries;
	fsm->failures = 0;
	fsm->rnr = 0;
	fsm->canceled = false;
	if (direct_posted) {
		fsm->remote_armed = true;
		fsm->err = direct_ret;
		fsm->state = direct_ret ? TBV_DMABUF_TX_ABORT :
					  TBV_DMABUF_TX_WAIT_DMA;
	} else {
		fsm->remote_armed = false;
		fsm->err = 0;
		fsm->state = TBV_DMABUF_TX_BUILD;
	}
}

static inline enum tbv_dmabuf_tx_action
tbv_dmabuf_tx_fsm_initial_action(const struct tbv_dmabuf_tx_fsm *fsm)
{
	switch (fsm->state) {
	case TBV_DMABUF_TX_WAIT_DMA:
		return TBV_DMABUF_TX_ACT_WAIT_DMA;
	case TBV_DMABUF_TX_ABORT:
		return TBV_DMABUF_TX_ACT_ABORT;
	default:
		return TBV_DMABUF_TX_ACT_BUILD;
	}
}

static inline bool
tbv_dmabuf_tx_needs_abort(const struct tbv_dmabuf_tx_fsm *fsm)
{
	return fsm->err && (fsm->remote_armed || fsm->err == -ETIMEDOUT ||
			    fsm->err == -EPROTO);
}

static inline enum tbv_dmabuf_tx_action
tbv_dmabuf_tx_fsm_step(struct tbv_dmabuf_tx_fsm *fsm,
		       enum tbv_dmabuf_tx_event ev, int status,
		       bool deadline_expired)
{
	switch (fsm->state) {
	case TBV_DMABUF_TX_BUILD:
		if (ev == TBV_DMABUF_TX_EV_BUILD_OK) {
			fsm->state = TBV_DMABUF_TX_ARM;
			return TBV_DMABUF_TX_ACT_ARM;
		}
		fsm->err = status;
		fsm->state = TBV_DMABUF_TX_FINISH;
		return TBV_DMABUF_TX_ACT_FINISH;

	case TBV_DMABUF_TX_ARM:
		switch (ev) {
		case TBV_DMABUF_TX_EV_COMMIT_OK:
			fsm->remote_armed = true;
			fsm->state = TBV_DMABUF_TX_POST;
			return TBV_DMABUF_TX_ACT_POST;
		case TBV_DMABUF_TX_EV_COMMIT_ERR:
			fsm->err = status;
			fsm->state = TBV_DMABUF_TX_FINISH;
			return TBV_DMABUF_TX_ACT_FINISH;
		case TBV_DMABUF_TX_EV_CANCELED:
			fsm->err = -ECANCELED;
			fsm->state = TBV_DMABUF_TX_FINISH;
			return TBV_DMABUF_TX_ACT_FINISH;
		case TBV_DMABUF_TX_EV_PREPARE_READY:
			fsm->remote_armed = true;
			fsm->state = TBV_DMABUF_TX_POST;
			return TBV_DMABUF_TX_ACT_POST;
		case TBV_DMABUF_TX_EV_PREPARE_DONE:
			fsm->err = 0;
			fsm->state = TBV_DMABUF_TX_FINISH;
			return TBV_DMABUF_TX_ACT_FINISH;
		case TBV_DMABUF_TX_EV_PREPARE_RNR:
			if (fsm->rnr_retries != 0xffu &&
			    fsm->rnr++ >= fsm->rnr_retries) {
				fsm->err = -EAGAIN;
				fsm->state = TBV_DMABUF_TX_FINISH;
				return TBV_DMABUF_TX_ACT_FINISH;
			}
			if (deadline_expired) {
				fsm->err = -ETIMEDOUT;
				fsm->state = TBV_DMABUF_TX_ABORT;
				return TBV_DMABUF_TX_ACT_ABORT;
			}
			return TBV_DMABUF_TX_ACT_WAIT_RNR;
		case TBV_DMABUF_TX_EV_PREPARE_BUSY:
			if (deadline_expired) {
				fsm->err = -ETIMEDOUT;
				fsm->state = TBV_DMABUF_TX_ABORT;
				return TBV_DMABUF_TX_ACT_ABORT;
			}
			return TBV_DMABUF_TX_ACT_SLEEP_ARM;
		case TBV_DMABUF_TX_EV_PREPARE_ERR:
			if (status == -ETIMEDOUT &&
			    fsm->failures++ < fsm->retries &&
			    !deadline_expired)
				return TBV_DMABUF_TX_ACT_SLEEP_ARM;
			fsm->err = status;
			if (tbv_dmabuf_tx_needs_abort(fsm)) {
				fsm->state = TBV_DMABUF_TX_ABORT;
				return TBV_DMABUF_TX_ACT_ABORT;
			}
			fsm->state = TBV_DMABUF_TX_FINISH;
			return TBV_DMABUF_TX_ACT_FINISH;
		default:
			fsm->err = -EINVAL;
			fsm->state = TBV_DMABUF_TX_FINISH;
			return TBV_DMABUF_TX_ACT_FINISH;
		}

	case TBV_DMABUF_TX_POST:
		if (ev == TBV_DMABUF_TX_EV_POST_OK) {
			fsm->state = TBV_DMABUF_TX_WAIT_DMA;
			return TBV_DMABUF_TX_ACT_WAIT_DMA;
		}
		fsm->err = status;
		fsm->state = TBV_DMABUF_TX_ABORT;
		return TBV_DMABUF_TX_ACT_ABORT;

	case TBV_DMABUF_TX_WAIT_DMA:
		if (ev == TBV_DMABUF_TX_EV_DMA_OK) {
			fsm->state = TBV_DMABUF_TX_WAIT_DONE;
			return TBV_DMABUF_TX_ACT_WAIT_DONE;
		}
		if (ev == TBV_DMABUF_TX_EV_DMA_DRAINED) {
			fsm->canceled = true;
			fsm->state = TBV_DMABUF_TX_WAIT_DONE;
			return TBV_DMABUF_TX_ACT_WAIT_DONE;
		}
		if (status == -ETIMEDOUT && fsm->canceled) {
			/* canceled already set by caller on drain */
		}
		fsm->err = status;
		fsm->state = TBV_DMABUF_TX_ABORT;
		return TBV_DMABUF_TX_ACT_ABORT;

	case TBV_DMABUF_TX_WAIT_DONE:
		if (ev == TBV_DMABUF_TX_EV_DONE_OK) {
			fsm->err = 0;
			fsm->state = TBV_DMABUF_TX_FINISH;
			return TBV_DMABUF_TX_ACT_FINISH;
		}
		if (ev == TBV_DMABUF_TX_EV_DONE_BUSY) {
			if (deadline_expired) {
				fsm->err = -ETIMEDOUT;
				fsm->state = TBV_DMABUF_TX_ABORT;
				return TBV_DMABUF_TX_ACT_ABORT;
			}
			return TBV_DMABUF_TX_ACT_SLEEP_DONE;
		}
		fsm->err = status;
		fsm->state = TBV_DMABUF_TX_ABORT;
		return TBV_DMABUF_TX_ACT_ABORT;

	case TBV_DMABUF_TX_ABORT:
		fsm->state = TBV_DMABUF_TX_FINISH;
		return TBV_DMABUF_TX_ACT_FINISH;

	case TBV_DMABUF_TX_FINISH:
		return TBV_DMABUF_TX_ACT_FINISH;
	}
	return TBV_DMABUF_TX_ACT_FINISH;
}

static inline int tbv_dmabuf_tx_fsm_result(const struct tbv_dmabuf_tx_fsm *fsm)
{
	if (fsm->err)
		return fsm->err;
	return fsm->canceled ? -ECANCELED : 0;
}

#endif /* TBV_DMABUF_TX_FSM_H */
