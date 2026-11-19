// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "proto/ctrl_txn.h"

static struct tbv_dmabuf_wire key(void)
{
	struct tbv_dmabuf_wire m;

	memset(&m, 0, sizeof(m));
	m.token = 100;
	memset(m.src_host, 0x11, 16);
	memset(m.dst_host, 0x22, 16);
	memset(m.src_domain, 0x33, 16);
	m.count = 1;
	m.hdr.src_qp = 1;
	m.hdr.dest_qp = 2;
	m.hdr.psn = 3;
	m.hdr.length = 8192;
	m.bytes = 4096;
	m.lanes[0].path = 8;
	return m;
}

static struct tbv_dmabuf_wire msg(tbv_wire_u16 op, int status)
{
	struct tbv_dmabuf_wire m = key();

	m.op = op | TBV_DMABUF_REPLY;
	m.status = status;
	return m;
}

static void test_capabilities(void)
{
	unsigned int flags;

	for (flags = 0; flags < 16; flags++) {
		bool ring = flags & 1, remote_ring = flags & 2;
		bool push = flags & 4, remote_push = flags & 8;
		bool both_ring = ring && remote_ring;
		bool xdomain = push && remote_push && !both_ring;

		assert(tbv_ctrl_need_xdomain(ring, remote_ring, push,
					     remote_push) == xdomain);
		assert(tbv_ctrl_need(ring, remote_ring, push, remote_push) ==
		       (both_ring || xdomain));
	}
	assert(!tbv_ctrl_need(false, false, false, false));
}

static void test_done_before_ready_and_duplicate(void)
{
	struct tbv_ctrl_txn t = { .expected = TBV_DMABUF_PREPARE };
	struct tbv_dmabuf_wire m;

	assert(tbv_ctrl_txn_result(&t, 0) == TBV_DMABUF_BUSY);
	m = msg(TBV_DMABUF_COMPLETE, TBV_DMABUF_DONE);
	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_DONE);
	m = msg(TBV_DMABUF_PREPARE, TBV_DMABUF_READY);
	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_REPLY);
	assert(t.replied && t.status == TBV_DMABUF_READY);
	assert(tbv_ctrl_txn_result(&t, 0) == TBV_DMABUF_DONE);

	m = msg(TBV_DMABUF_COMPLETE, -EIO);
	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_NONE);
	assert(t.done_status == TBV_DMABUF_DONE);
	m = msg(TBV_DMABUF_PREPARE, -EIO);
	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_NONE);
	assert(t.status == TBV_DMABUF_READY);
}

static void test_unexpected_reply_ignored(void)
{
	struct tbv_ctrl_txn t = { .expected = TBV_DMABUF_PREPARE };
	struct tbv_dmabuf_wire m = msg(TBV_DMABUF_STATUS, TBV_DMABUF_DONE);

	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_NONE);
	assert(!t.replied);
}

static void test_malformed_done(void)
{
	struct tbv_dmabuf_wire m = msg(TBV_DMABUF_COMPLETE, TBV_DMABUF_BUSY);

	assert(tbv_ctrl_done_malformed(&m));
	m.status = TBV_DMABUF_DONE;
	assert(!tbv_ctrl_done_malformed(&m));
	m.status = -EACCES;
	assert(!tbv_ctrl_done_malformed(&m));
	m = msg(TBV_DMABUF_STATUS, TBV_DMABUF_BUSY);
	assert(!tbv_ctrl_done_malformed(&m));
}

static void test_status_does_not_complete(void)
{
	struct tbv_ctrl_txn t = { .expected = TBV_DMABUF_STATUS };
	struct tbv_dmabuf_wire m = msg(TBV_DMABUF_STATUS, TBV_DMABUF_DONE);

	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_REPLY);
	assert(t.replied && t.status == TBV_DMABUF_DONE);
	assert(tbv_ctrl_txn_result(&t, 0) == TBV_DMABUF_BUSY);
}

static void test_error_done_wins_and_failure_overrides(void)
{
	struct tbv_ctrl_txn t = { .expected = TBV_DMABUF_PREPARE };
	struct tbv_dmabuf_wire m = msg(TBV_DMABUF_COMPLETE, -EACCES);

	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_DONE);
	m.status = TBV_DMABUF_DONE;
	assert(tbv_ctrl_txn_apply(&t, &m) == TBV_CTRL_APPLY_NONE);
	assert(tbv_ctrl_txn_result(&t, 0) == -EACCES);
	assert(tbv_ctrl_txn_result(&t, -ENOTCONN) == -ENOTCONN);
}

static void test_matching_fields(void)
{
	struct tbv_dmabuf_wire base = key(), m;
	unsigned int field;

	m = base;
	assert(tbv_dmabuf_wire_same(&base, &m, true));
	for (field = 0; field < 8; field++) {
		m = base;
		switch (field) {
		case 0:
			m.token++;
			break;
		case 1:
			m.src_host[0]++;
			break;
		case 2:
			m.dst_host[0]++;
			break;
		case 3:
			m.src_domain[0]++;
			break;
		case 4:
			m.hdr.psn++;
			break;
		case 5:
			m.offset += 4096;
			break;
		case 6:
			m.lanes[0].path++;
			break;
		case 7:
			m.hdr.rkey++;
			break;
		}
		assert(!tbv_dmabuf_wire_same(&base, &m, true));
	}
}

int main(void)
{
	test_capabilities();
	test_done_before_ready_and_duplicate();
	test_unexpected_reply_ignored();
	test_malformed_done();
	test_status_does_not_complete();
	test_error_done_wins_and_failure_overrides();
	test_matching_fields();
	puts("ctrl txn smoke OK");
	return 0;
}
