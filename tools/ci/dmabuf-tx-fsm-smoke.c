/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#include <assert.h>
#include <stdio.h>

#include "proto/dmabuf_tx_fsm.h"

static void test_normal_prepare_post_done(void)
{
	struct tbv_dmabuf_tx_fsm fsm;

	tbv_dmabuf_tx_fsm_init(&fsm, 3, 3, false, 0);
	assert(tbv_dmabuf_tx_fsm_initial_action(&fsm) == TBV_DMABUF_TX_ACT_BUILD);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_ARM);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_PREPARE_READY, 0, false) ==
	       TBV_DMABUF_TX_ACT_POST);
	assert(fsm.remote_armed);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_POST_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_WAIT_DMA);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_DMA_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_WAIT_DONE);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_DONE_BUSY, 0, false) ==
	       TBV_DMABUF_TX_ACT_SLEEP_DONE);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_DONE_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == 0);
}

static void test_direct_post_ok_and_err(void)
{
	struct tbv_dmabuf_tx_fsm fsm;

	tbv_dmabuf_tx_fsm_init(&fsm, 3, 3, true, 0);
	assert(tbv_dmabuf_tx_fsm_initial_action(&fsm) == TBV_DMABUF_TX_ACT_WAIT_DMA);
	assert(fsm.remote_armed);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_DMA_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_WAIT_DONE);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_DONE_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == 0);

	/* A failed direct post already sent COMMIT, so it must ABORT before FINISH. */
	tbv_dmabuf_tx_fsm_init(&fsm, 3, 3, true, -EIO);
	assert(tbv_dmabuf_tx_fsm_initial_action(&fsm) == TBV_DMABUF_TX_ACT_ABORT);
	assert(tbv_dmabuf_tx_needs_abort(&fsm));
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_ABORT_DONE, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == -EIO);
}

static void test_cancel_rules(void)
{
	struct tbv_dmabuf_tx_fsm fsm;

	/* Unposted cancel before READY: finishes without ABORT and without canceled drain flag. */
	tbv_dmabuf_tx_fsm_init(&fsm, 3, 3, false, 0);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_ARM);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_CANCELED, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(!fsm.canceled);
	assert(!tbv_dmabuf_tx_needs_abort(&fsm));
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == -ECANCELED);

	/* Canceled after posting: drains DMA, waits for DONE, finishes with -ECANCELED. */
	tbv_dmabuf_tx_fsm_init(&fsm, 3, 3, false, 0);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_ARM);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_COMMIT_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_POST);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_POST_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_WAIT_DMA);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_DMA_DRAINED, 0, false) ==
	       TBV_DMABUF_TX_ACT_WAIT_DONE);
	assert(fsm.canceled);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_DONE_OK, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == -ECANCELED);
	assert(!fsm.err);
}

static void test_rnr_and_retries(void)
{
	struct tbv_dmabuf_tx_fsm fsm;

	/* Finite rnr_retries=1: first RNR waits, second returns -EAGAIN without ABORT. */
	tbv_dmabuf_tx_fsm_init(&fsm, 2, 1, false, 0);
	tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_PREPARE_RNR, 0, false) ==
	       TBV_DMABUF_TX_ACT_WAIT_RNR);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_PREPARE_RNR, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == -EAGAIN);

	/* Infinite rnr_retries=0xff never exhausts. */
	tbv_dmabuf_tx_fsm_init(&fsm, 2, 0xff, false, 0);
	tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false);
	for (int i = 0; i < 300; i++)
		assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_PREPARE_RNR, 0, false) ==
		       TBV_DMABUF_TX_ACT_WAIT_RNR);

	/* PREPARE timeout retries up to `retries`, then ABORTs. */
	tbv_dmabuf_tx_fsm_init(&fsm, 1, 1, false, 0);
	tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_PREPARE_ERR, -ETIMEDOUT, false) ==
	       TBV_DMABUF_TX_ACT_SLEEP_ARM);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_PREPARE_ERR, -ETIMEDOUT, false) ==
	       TBV_DMABUF_TX_ACT_ABORT);
	assert(tbv_dmabuf_tx_needs_abort(&fsm));
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_ABORT_DONE, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == -ETIMEDOUT);
}

static void test_commit_err_and_zero_byte_done(void)
{
	struct tbv_dmabuf_tx_fsm fsm;

	/* COMMIT failure never armed remote: finishes directly without ABORT. */
	tbv_dmabuf_tx_fsm_init(&fsm, 1, 1, false, 0);
	tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_COMMIT_ERR, -ENOTCONN, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(!tbv_dmabuf_tx_needs_abort(&fsm));
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == -ENOTCONN);

	/* Zero-byte PREPARE returning DONE finishes immediately with 0. */
	tbv_dmabuf_tx_fsm_init(&fsm, 1, 1, false, 0);
	tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_BUILD_OK, 0, false);
	assert(tbv_dmabuf_tx_fsm_step(&fsm, TBV_DMABUF_TX_EV_PREPARE_DONE, 0, false) ==
	       TBV_DMABUF_TX_ACT_FINISH);
	assert(tbv_dmabuf_tx_fsm_result(&fsm) == 0);
}

int main(void)
{
	test_normal_prepare_post_done();
	test_direct_post_ok_and_err();
	test_cancel_rules();
	test_rnr_and_retries();
	test_commit_err_and_zero_byte_done();
	puts("dmabuf tx fsm smoke OK");
	return 0;
}
