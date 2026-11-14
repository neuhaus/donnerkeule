/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* feed() mirrors tbv_rx_handle_rdma_read_resp and must be kept in step with it. */
#include <assert.h>
#include <stdio.h>

#include "proto/read_resp.h"

/* Mirrors what tbv_rx_handle_rdma_read_resp keeps per outstanding READ. */
struct requester {
	tbv_read_u32 received;
	bool retry_sent;
	unsigned int retries_sent;
};

static enum tbv_read_resp_action feed(struct requester *r,
				      tbv_read_u32 frag_offset,
				      tbv_read_u32 length)
{
	enum tbv_read_resp_action action =
		tbv_read_resp_classify(r->received, r->retry_sent, frag_offset);
	tbv_read_u32 taken;

	if (action == TBV_READ_RESP_DUPLICATE)
		return action;
	if (action != TBV_READ_RESP_IN_ORDER) {
		if (action == TBV_READ_RESP_GAP_RETRY) {
			r->retry_sent = true;
			r->retries_sent++;
		}
		return action;
	}

	taken = r->received;
	r->received = frag_offset + length;
	if (tbv_read_resp_advanced(taken, r->received))
		r->retry_sent = false;
	return action;
}

static void test_classify_cases(void)
{
	assert(tbv_read_resp_classify(0, false, 0) == TBV_READ_RESP_IN_ORDER);
	assert(tbv_read_resp_classify(4096, false, 4096) == TBV_READ_RESP_IN_ORDER);

	assert(tbv_read_resp_classify(8192, false, 0) == TBV_READ_RESP_DUPLICATE);
	assert(tbv_read_resp_classify(8192, true, 4096) == TBV_READ_RESP_DUPLICATE);

	assert(tbv_read_resp_classify(0, false, 4096) == TBV_READ_RESP_GAP_RETRY);
	assert(tbv_read_resp_classify(0, true, 4096) == TBV_READ_RESP_GAP_QUIET);

	/* A duplicate outranks the retry flag: it is already taken. */
	assert(tbv_read_resp_classify(4096, true, 0) == TBV_READ_RESP_DUPLICATE);
}

static void test_advanced(void)
{
	assert(tbv_read_resp_advanced(0, 4096));
	assert(!tbv_read_resp_advanced(4096, 4096));
	assert(!tbv_read_resp_advanced(8192, 4096));
	assert(tbv_read_resp_advanced(0xfffff000u, 0xffffffffu));
}

/*
 * The bug this replaces: 127 out-of-order fragments after one loss asked the
 * responder to retry 127 times. One gap must cost one RETRY.
 */
static void test_one_retry_per_gap(void)
{
	struct requester r = {0};
	tbv_read_u32 frag;

	assert(feed(&r, 0, 4096) == TBV_READ_RESP_IN_ORDER);
	assert(r.received == 4096);

	/* 4096..8191 is lost; everything after it arrives. */
	for (frag = 8192; frag < 8192 + 127 * 4096; frag += 4096) {
		enum tbv_read_resp_action action = feed(&r, frag, 4096);

		if (frag == 8192)
			assert(action == TBV_READ_RESP_GAP_RETRY);
		else
			assert(action == TBV_READ_RESP_GAP_QUIET);
	}
	assert(r.retries_sent == 1);
	assert(r.received == 4096);

	/* The retransmission closes the gap and re-arms the next RETRY. */
	assert(feed(&r, 4096, 4096) == TBV_READ_RESP_IN_ORDER);
	assert(!r.retry_sent);
	assert(r.retries_sent == 1);

	/* A second, later gap is allowed its own single RETRY. */
	r.received = 8192;
	assert(feed(&r, 16384, 4096) == TBV_READ_RESP_GAP_RETRY);
	assert(feed(&r, 20480, 4096) == TBV_READ_RESP_GAP_QUIET);
	assert(r.retries_sent == 2);
}

/*
 * A READ draining in order keeps restarting its timeout; a READ stuck behind
 * a gap never does, so it can still expire.
 */
static void test_progress_gates_the_timeout(void)
{
	struct requester r = {0};
	unsigned int restarts = 0;
	tbv_read_u32 offset;

	for (offset = 0; offset < 64 * 4096; offset += 4096) {
		tbv_read_u32 taken = r.received;

		assert(feed(&r, offset, 4096) == TBV_READ_RESP_IN_ORDER);
		if (tbv_read_resp_advanced(taken, r.received))
			restarts++;
	}
	assert(restarts == 64);
	assert(r.received == 64 * 4096);

	/* Stuck: fragments keep arriving but nothing is taken. */
	restarts = 0;
	for (offset = 64 * 4096 + 4096; offset < 64 * 4096 + 10 * 4096;
	     offset += 4096) {
		tbv_read_u32 taken = r.received;

		feed(&r, offset, 4096);
		if (tbv_read_resp_advanced(taken, r.received))
			restarts++;
	}
	assert(restarts == 0);

	/* A duplicate is not progress either. */
	assert(feed(&r, 0, 4096) == TBV_READ_RESP_DUPLICATE);
	assert(r.received == 64 * 4096);
}

int main(void)
{
	test_classify_cases();
	test_advanced();
	test_one_retry_per_gap();
	test_progress_gates_the_timeout();
	puts("read response smoke OK");
	return 0;
}
