// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/thunderbolt.h>

#include "../proto/native_wire.h"
#include "tbv.h"
#include "verbs_dmabuf.h"
#include "verbs_internal.h"
#include "verbs_control.h"

static u32 tbv_debugfs_wire_path_flags(const struct tbv_path *path)
{
	u32 flags = 0;

	if (path->cfg.tx_flags & RING_FLAG_FRAME)
		flags |= TBV_NATIVE_WIRE_PATH_FRAME;
	if (path->cfg.e2e)
		flags |= TBV_NATIVE_WIRE_PATH_E2E;

	return flags;
}

static int tbv_debugfs_summary_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;

	seq_puts(s, "profile: linux_perf\n");
	seq_puts(s, "native_enabled: 1\n");
	seq_puts(s, "rc_supported: 1\n");
	seq_puts(s, "uc_supported: 1\n");
	seq_printf(s, "native_control: %s\n",
		   tbv_native_control_mode_name(state));
	seq_printf(s, "native_same_peer_multicable: %s\n",
		   !state->native_control_registered ? "off" :
		   state->native_control_source_aware ||
		   state->native_control_ids ? "enabled" : "limited");
	seq_printf(s, "native_host_id: %16phN\n", state->native_host_id);
	seq_printf(s, "native_legacy_ambiguous_limited: %lld\n",
		   atomic64_read(&state->native_legacy_ambiguous_limited));
	seq_printf(s, "configured_links: %u\n", tbv_link_count(state));
	seq_printf(s, "native_service_count: %u\n",
		   state->native_dir_count);
	seq_printf(s, "services_registered: %u\n",
		   state->services_registered);
	seq_printf(s, "allocate_rings: %u\n", state->allocate_rings);
	seq_printf(s, "start_rings: %u\n", state->start_rings);
	seq_printf(s, "negotiate_native: %u\n", state->negotiate_native);
	seq_printf(s, "enable_tunnels: %u\n", state->enable_tunnels);
	seq_printf(s, "native_data: %u\n", state->native_data);
	seq_printf(s, "native_fragment_striping: %u\n",
		   state->native_fragment_striping);
	seq_printf(s, "native_write_striping: %u\n",
		   state->native_write_striping);
	tbv_dmabuf_debugfs_show(s);
	tbv_host_dma_debugfs_show(s);
	seq_printf(s, "native_write_stripe_min_bytes: %u\n",
		   state->native_write_stripe_min_bytes);
	seq_printf(s, "native_domain_mask: 0x%x\n", state->native_domain_mask);
	seq_printf(s, "register_verbs: %u\n", state->register_verbs);
	seq_printf(s, "verbs_registered: %u\n", state->verbs_registered);
	seq_printf(s, "verbs_ucontexts: %d\n",
		   atomic_read(&state->verbs_ucontexts));
	seq_printf(s, "verbs_pds: %d\n", atomic_read(&state->verbs_pds));
	seq_printf(s, "verbs_cqs: %d\n", atomic_read(&state->verbs_cqs));
	seq_printf(s, "verbs_qps: %d\n", atomic_read(&state->verbs_qps));
	seq_printf(s, "verbs_mrs: %d\n", atomic_read(&state->verbs_mrs));
	seq_printf(s, "verbs_recv_wqes: %d\n",
		   atomic_read(&state->verbs_recv_wqes));
#define TBV_COUNTER_PRINT(name) \
	seq_printf(s, #name ": %lld\n", atomic64_read(&state->name));
	TBV_STATE_COUNTERS(TBV_COUNTER_PRINT)
#undef TBV_COUNTER_PRINT
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_summary);

static int tbv_debugfs_peers_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;
	struct tbv_peer *peer;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		seq_printf(s, "peer %u backend=native rails=%u native_qp_rr_rail_id=%u domain=%d remote_host=%16phN\n",
			   peer->peer_id,
			   peer->nr_rails, peer->native_qp_rr_rail_id,
			   peer->xd && peer->xd->tb ? peer->xd->tb->index : -1,
			   peer->remote_host_known ? peer->remote_host_id :
			   (const u8[16]){ 0 });
		tbv_control_debugfs_show(s, peer);

		list_for_each_entry(rail, &peer->rails, node) {
			bool data_ready = tbv_rail_data_ready(rail);

			seq_printf(s,
				   "  rail=0x%x route=0x%llx local=%u remote=%u path=%u link_speed=%uGb/s link_width=0x%x active=%u service_ready=%u data_ready=%u native_qp_bind_count=%d state=%s negotiated=%u ready_sent=%u remote_ready=%u attempts=%u last_error=%d local_out=%d local_tx=%d local_rx=%d remote_rail=0x%x remote_out=%d remote_tx=%d remote_rx=%d gpu_stopped=%u\n",
				   rail->rail_id, rail->key.route,
				   rail->key.local_adapter,
				   rail->key.remote_adapter,
				   rail->key.path_id,
				   rail->link_speed,
				   rail->link_width,
				   rail->active,
				   data_ready,
				   data_ready,
				   atomic_read(&rail->native_qp_bind_count),
				   tbv_path_state_name(rail->path.state),
				   rail->native_negotiated,
				   rail->native_ready_sent,
				   rail->native_remote_ready,
				   rail->native_attempts +
				   rail->native_ready_attempts,
				   rail->native_last_error,
				   rail->path.local_transmit_path,
				   rail->path.local_tx_hop,
				   rail->path.local_rx_hop,
				   rail->remote_rail_id,
				   rail->remote_transmit_path,
				   rail->remote_tx_hop,
				   rail->remote_rx_hop,
				   READ_ONCE(rail->rings_stopped));
			seq_printf(s, "    descriptor_ready=%u quarantine_reason=%d\n",
				   tbv_dmabuf_rail_ready(rail),
				   READ_ONCE(rail->dmabuf_quarantine_reason));
			seq_printf(s,
				   "    data_tx_enqueued=%lld data_tx_posted=%lld data_tx_completed=%lld data_tx_credit_stalls=%lld data_tx_credit_received=%lld tx_credits=%u/%u\n",
				   atomic64_read(&rail->path.data_tx_enqueued),
				   atomic64_read(&rail->path.data_tx_posted),
				   atomic64_read(&rail->path.data_tx_completed),
				   atomic64_read(&rail->path.data_tx_credit_stalls),
				   atomic64_read(&rail->path.data_tx_credit_received),
				   rail->path.tx_remote_data_credits,
				   rail->path.tx_remote_data_credit_max);
			seq_printf(s,
				   "    control_tx_enqueued=%lld control_tx_posted=%lld control_tx_completed=%lld control_tx_queue_max_ms=%lld\n",
				   atomic64_read(&rail->path.control_tx_enqueued),
				   atomic64_read(&rail->path.control_tx_posted),
				   atomic64_read(&rail->path.control_tx_completed),
				   atomic64_read(&rail->path.control_tx_queue_max_ms));
			seq_printf(s,
				   "    path_cfg tx_flags=0x%x rx_flags=0x%x e2e=%u wire_flags=0x%x tx_ring=%u rx_ring=%u sof_mask=0x%x eof_mask=0x%x\n",
				   rail->path.cfg.tx_flags,
				   rail->path.cfg.rx_flags,
				   rail->path.cfg.e2e,
				   tbv_debugfs_wire_path_flags(&rail->path),
				   rail->path.cfg.tx_ring_size,
				   rail->path.cfg.rx_ring_size,
				   rail->path.cfg.sof_mask,
				   rail->path.cfg.eof_mask);
			seq_printf(s,
				   "    data_rx_completed=%lld data_rx_canceled=%lld data_rx_credit_sent=%lld data_rx_credit_send_error=%lld data_rx_repost_failed=%lld data_rx_lost=%lld rx_credit_pending=%u\n",
				   atomic64_read(&rail->path.data_rx_completed),
				   atomic64_read(&rail->path.data_rx_canceled),
				   atomic64_read(&rail->path.data_rx_credit_sent),
				   atomic64_read(&rail->path.data_rx_credit_send_error),
				   atomic64_read(&rail->path.data_rx_repost_failed),
				   atomic64_read(&rail->path.data_rx_lost),
				   rail->path.rx_data_credit_pending);
			/* Why a path with queued frames is not sending. */
			seq_printf(s,
				   "    tx_pump data_queued=%u control_queued=%u reserved=%u inflight=%d frames=%u free=%zu remote_credits=%u scheduling=%u raw_stream=%u raw_inflight=%u\n",
				   rail->path.tx_data_queued,
				   rail->path.tx_control_queued,
				   rail->path.tx_data_reserved,
				   atomic_read(&rail->path.tx_inflight),
				   rail->path.tx_frame_count,
				   list_count_nodes(&rail->path.tx_free),
				   rail->path.tx_remote_data_credits,
				   rail->path.tx_scheduling,
				   rail->path.tx_raw_stream_active,
				   rail->path.tx_raw_stream_inflight);
			seq_printf(s,
				   "    tx_poll enabled=%u calls=%lld completed=%lld\n",
				   rail->path.tx_poll_enabled,
				   atomic64_read(&rail->path.tx_poll_calls),
				   atomic64_read(&rail->path.tx_poll_completed));
			seq_printf(s,
				   "    rx_supp_poll enabled=%u calls=%lld completed=%lld\n",
				   rail->path.rx_supp_poll_enabled,
				   atomic64_read(&rail->path.rx_supp_poll_calls),
				   atomic64_read(&rail->path.rx_supp_poll_completed));
		}
	}
	mutex_unlock(&state->lock);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_peers);

static int tbv_debugfs_configured_links_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;

	tbv_link_debugfs_show(s, state);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_configured_links);

int tbv_debugfs_init(struct tbv_state *state)
{
	state->debugfs_dir = debugfs_create_dir(TBV_DRV_NAME, NULL);
	if (IS_ERR(state->debugfs_dir)) {
		state->debugfs_dir = NULL;
		return 0;
	}

	debugfs_create_file("summary", 0444, state->debugfs_dir, state,
			    &tbv_debugfs_summary_fops);
	debugfs_create_file("peers", 0444, state->debugfs_dir, state,
			    &tbv_debugfs_peers_fops);
	tbv_control_debugfs_init(state, state->debugfs_dir);
	debugfs_create_file("configured_links", 0444, state->debugfs_dir,
			    state, &tbv_debugfs_configured_links_fops);
	return 0;
}

void tbv_debugfs_exit(struct tbv_state *state)
{
	tbv_control_debugfs_exit();
	debugfs_remove_recursive(state->debugfs_dir);
	state->debugfs_dir = NULL;
}
