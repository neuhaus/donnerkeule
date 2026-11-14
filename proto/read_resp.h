/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_READ_RESP_H
#define TBV_READ_RESP_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 tbv_read_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t tbv_read_u32;
#endif

/*
 * Requester-side ordering rules for RDMA READ response fragments.
 *
 * The responder sends one READ's responses in order on a single rail, so a
 * fragment that does not start at the byte the requester has already taken
 * means a gap. The gap repeats for every later fragment until the missing one
 * arrives, and asking the responder to retry once per out-of-order fragment
 * turns one loss into a retransmit storm, so a gap gets one RETRY.
 *
 * If the gap fragment is lost again in the retransmission, no second RETRY is
 * sent: retry_sent is cleared only when the requester takes new bytes. Recovery
 * then falls to the responder's own resend timer or the requester timeout.
 *
 * These are the two decisions, kept free of kernel types so the module and the
 * unit test share them.
 */

enum tbv_read_resp_action {
	/* Already taken: the responder repeated itself, acknowledge it. */
	TBV_READ_RESP_DUPLICATE = 0,
	/* First fragment past a gap: buffer it and ask for one retry. */
	TBV_READ_RESP_GAP_RETRY = 1,
	/* A later fragment of a gap already asked for: buffer it silently. */
	TBV_READ_RESP_GAP_QUIET = 2,
	/* The fragment the requester was waiting for. */
	TBV_READ_RESP_IN_ORDER = 3,
};

static inline enum tbv_read_resp_action
tbv_read_resp_classify(tbv_read_u32 received, bool retry_sent,
		       tbv_read_u32 frag_offset)
{
	if (frag_offset < received)
		return TBV_READ_RESP_DUPLICATE;
	if (frag_offset != received)
		return retry_sent ? TBV_READ_RESP_GAP_QUIET :
				    TBV_READ_RESP_GAP_RETRY;
	return TBV_READ_RESP_IN_ORDER;
}

/*
 * True when the requester took new bytes. Only then may its timeout be
 * restarted: a READ whose responses keep arriving without closing the gap is
 * not making progress and must still be allowed to expire.
 */
static inline bool tbv_read_resp_advanced(tbv_read_u32 received,
					  tbv_read_u32 next_received)
{
	return next_received > received;
}

#endif /* TBV_READ_RESP_H */
