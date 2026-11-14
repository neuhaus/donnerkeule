/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_COUNTERS_H
#define TBV_COUNTERS_H

/*
 * The global atomic64 counters, declared once.
 *
 * TBV_STATE_COUNTERS expands to one C(name) per counter, in the order the
 * debugfs summary prints them. It is expanded into the struct tbv_state
 * members and into that printer, so a counter in this list cannot be added
 * to one and forgotten in the other, and the printed name is always the
 * field name. native_legacy_ambiguous_limited and the per-path counters are
 * outside the list and are declared and printed by hand.
 *
 * Benchmark scripts parse the summary by name, so the order is not
 * load-bearing for them; keep it stable anyway. Renaming a counter changes
 * a userspace-visible contract.
 */
#define TBV_STATE_COUNTERS(C) \
	C(data_wr_send) \
	C(data_wr_op_send) \
	C(data_wr_op_send_imm) \
	C(data_wr_op_write) \
	C(data_wr_op_write_imm) \
	C(data_wr_op_unsupported) \
	C(data_wr_live) \
	C(data_wr_no_path) \
	C(data_wr_no_recv_credit) \
	C(data_wr_copied) \
	C(data_wr_zcopy) \
	C(data_wr_zcopy_fallback) \
	C(data_wr_zcopy_fallback_striping) \
	C(data_wr_zcopy_fallback_unsafe_sge) \
	C(data_wr_block_split) \
	C(data_wr_block_async) \
	C(data_wr_block_deferred) \
	C(data_wr_copy_error) \
	C(data_wr_path_send) \
	C(data_wr_path_send_error) \
	C(data_wr_retransmit) \
	C(data_wr_rnr_retransmit) \
	C(data_wr_retry_enqueue_error) \
	C(data_wr_retry_exhausted) \
	C(data_wr_rnr_retry_exhausted) \
	C(data_wr_timeout) \
	C(data_tx_accepted) \
	C(data_tx_posted) \
	C(data_tx_completed) \
	C(data_tx_canceled) \
	C(data_tx_errors) \
	C(data_tx_credit_stalls) \
	C(data_tx_credit_received) \
	C(data_rx_completed) \
	C(data_rx_canceled) \
	C(data_rx_credit_sent) \
	C(data_rx_credit_send_error) \
	C(data_rx_lost) \
	C(data_rx_repost_failed) \
	C(data_rx_bad_frame) \
	C(data_rx_crc_error) \
	C(data_rx_overrun) \
	C(data_rx_raw_poisoned) \
	C(data_rx_raw_poison_drop) \
	C(data_rx_bad_header) \
	C(data_rx_bad_header_parse) \
	C(data_rx_bad_header_len) \
	C(data_rx_bad_header_path_credit) \
	C(data_rx_bad_header_opcode) \
	C(data_rx_bad_header_recv_credit) \
	C(data_rx_bad_header_ack) \
	C(data_rx_bad_header_write) \
	C(data_rx_bad_header_read_req) \
	C(data_rx_bad_header_mad) \
	C(data_rx_send) \
	C(data_rx_op_send) \
	C(data_rx_op_send_imm) \
	C(data_rx_op_write) \
	C(data_rx_op_write_imm) \
	C(data_rx_ack) \
	C(data_rx_ack_matched) \
	C(data_rx_ack_match_retried) \
	C(data_rx_ack_match_max_ms) \
	C(data_rx_ack_match_current_max_ms) \
	C(data_rx_ack_match_over_10ms) \
	C(data_rx_ack_match_over_64ms) \
	C(data_rx_ack_miss) \
	C(data_rx_late_ack) \
	C(data_rx_ack_cumulative) \
	C(data_tx_ack_ok) \
	C(data_tx_ack_rnr) \
	C(data_tx_ack_error) \
	C(data_tx_ack_send_error) \
	C(data_rx_ack_rnr) \
	C(data_rx_duplicate_ack) \
	C(data_rx_ack_history_miss) \
	C(data_tx_read_ack_ok) \
	C(data_tx_read_ack_retry) \
	C(data_tx_read_ack_error) \
	C(data_rx_read_ack_ok) \
	C(data_rx_read_ack_retry) \
	C(data_rx_read_ack_error) \
	C(data_read_resp_retransmit) \
	C(data_read_resp_drop) \
	C(data_rx_read_resp_duplicate) \
	C(data_rx_read_resp_gap) \
	C(data_rx_read_resp_remote_error) \
	C(data_rx_read_resp_bad_header) \
	C(data_rx_read_resp_copy_error) \
	C(data_rx_read_resp_short) \
	C(data_rx_read_req_no_access) \
	C(data_rx_read_req_no_mr) \
	C(data_rx_read_req_mr_access) \
	C(data_rx_read_req_too_large) \
	C(data_rx_read_req_bad_iova) \
	C(data_rx_read_req_alloc_error) \
	C(data_rx_read_req_resp_busy) \
	C(data_rx_read_req_resp_error) \
	C(data_rx_no_qp) \
	C(data_rx_bad_peer) \
	C(data_rx_unconnected_qp) \
	C(data_rx_qp_error) \
	C(data_rx_no_recv) \
	C(data_rx_rnr) \
	C(data_rx_rnr_suppressed) \
	C(data_rx_copy_error) \
	C(data_rx_send_len_error) \
	C(data_rx_send_prot_error) \
	C(data_rx_send_cq_error) \
	C(data_rx_send_bad_fragment) \
	C(data_rx_send_sequence_error) \
	C(data_rx_active_timeout) \
	C(data_rx_reorder_buffered) \
	C(data_rx_reorder_delivered) \
	C(data_rx_block_write) \
	C(data_rx_reorder_dropped) \
	C(data_rx_reorder_timeout) \
	C(data_rx_reorder_window) \
	C(data_rx_pending_discarded) \
	C(data_cq_overflow)

#define TBV_COUNTER_DECL(name) atomic64_t name;

#endif /* TBV_COUNTERS_H */
