/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_H
#define TBV_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/if.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/refcount.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "counters.h"
#include "proto/config.h"

#define TBV_DRV_NAME "thunderbolt_ibverbs"
#define TBV_NATIVE_PROTOCOL_KEY "tbverbs"
#define TBV_NATIVE_MAX_LANES 4
/* Rails one QP may spread data over: those of every link to its host. */
#define TBV_NATIVE_MAX_DATA_PATHS (2 * TBV_NATIVE_MAX_LANES)
#define TBV_DATA_PDF_FRAME_START 1
#define TBV_DATA_PDF_FRAME_END 3
#define TBV_NATIVE_PRTCID 1
#define TBV_NATIVE_PRTCVERS 1
#define TBV_NATIVE_PRTCREVS 0

static inline bool tbv_dma_device_ready(const struct device *dev)
{
	if (!dev)
		return false;

	/*
	 * A Thunderbolt core reprobe can briefly expose ring DMA devices whose
	 * IOMMU group is attached before dev->iommu is populated. Calling
	 * dma_map_* in that window reaches iommu_get_dma_domain() and oopses.
	 * Treat it as probe deferral; direct-DMA systems have no iommu_group and
	 * still pass this check.
	 */
	return !dev->iommu_group || dev->iommu;
}

struct tb_property_dir;
struct seq_file;

enum tbv_path_state {
	TBV_PATH_NEW,
	TBV_PATH_RING_ALLOCATED,
	TBV_PATH_RING_STARTED,
	TBV_PATH_TUNNEL_ENABLED,
	TBV_PATH_STOPPED,
};

struct tbv_config {
	u32 lanes_min;
	u32 lanes_max;
	bool lanes_auto;
};

struct tbv_rail_key {
	u64 route;
	u32 local_adapter;
	u32 remote_adapter;
	u32 path_id;
};

struct tbv_path_config {
	u32 tx_ring_size;
	u32 rx_ring_size;
	u32 tx_flags;
	u32 rx_flags;
	int tx_hop;
	int rx_hop;
	int transmit_path;
	int receive_path;
	u16 sof_mask;
	u16 eof_mask;
	bool e2e;
};

struct tbv_path_owned_frame {
	struct list_head node;
	void *data;
	u32 len;
	u8 sof;
	u8 eof;
};

struct tbv_path {
	enum tbv_path_state state;
	struct tbv_path_config cfg;
	struct tbv_rail *rail;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	struct tbv_data_frame *tx_frames;
	struct tbv_data_frame *rx_frames;
	struct tbv_tx_packet *tx_control_packets;
	u32 tx_frame_count;
	u32 rx_frame_count;
	u32 tx_control_packet_count;
	u32 tx_control_queued;
	u32 tx_data_queued;
	u32 tx_data_reserved;
	u32 tx_data_queue_limit;
	u32 tx_remote_data_credits;
	u32 tx_remote_data_credit_max;
	u32 rx_data_credit_pending;
	/* Native per-path frame sequence (TBV_NATIVE_DATA_PATH_SEQ_*). */
	u16 tx_data_seq;
	u16 rx_data_seq_next;
	bool rx_data_seq_valid;
	spinlock_t tx_lock;
	struct list_head tx_free;
	struct list_head tx_control_free;
	struct list_head tx_control_queue;
	struct list_head tx_data_queue;
	struct list_head tx_zcopy_inflight;
	struct delayed_work tx_poll_work;
	struct delayed_work rx_supp_poll_work;
	/*
	 * Blocks of native sends whose frames this path still has to build
	 * (ibdev.c). A work item builds them in order, in parallel with the
	 * other paths, so post_send does not build large sends itself.
	 */
	spinlock_t build_lock;
	struct list_head build_jobs;
	struct delayed_work build_work;
	atomic_t tx_inflight;
	atomic64_t data_tx_enqueued;
	atomic64_t data_tx_posted;
	atomic64_t data_tx_completed;
	atomic64_t control_tx_enqueued;
	atomic64_t control_tx_posted;
	atomic64_t control_tx_completed;
	atomic64_t control_tx_queue_max_ms;
	atomic64_t data_tx_credit_stalls;
	atomic64_t data_tx_credit_received;
	atomic64_t data_rx_completed;
	atomic64_t data_rx_canceled;
	atomic64_t data_rx_credit_sent;
	atomic64_t data_rx_credit_send_error;
	atomic64_t data_rx_repost_failed;
	atomic64_t data_rx_lost;
	/* Frame bytes, headers included, of every frame this path moved. */
	atomic64_t tx_bytes;
	atomic64_t rx_bytes;
	atomic64_t tx_poll_calls;
	atomic64_t tx_poll_completed;
	atomic64_t rx_supp_poll_calls;
	atomic64_t rx_supp_poll_completed;
	unsigned long rx_supp_poll_until;
	u8 rx_raw_opcode;
	u8 rx_raw_flags;
	u32 rx_raw_dest_qp;
	u32 rx_raw_src_qp;
	u32 rx_raw_psn;
	u32 rx_raw_imm_data;
	u32 rx_raw_rkey;
	u32 rx_raw_done;
	u32 rx_raw_remaining;
	u64 rx_raw_base;
	bool rx_raw_pending;
	bool rx_raw_poisoned;
	bool tx_poll_enabled;
	bool rx_supp_poll_enabled;
	bool tx_scheduling;
	bool tx_raw_stream_active;
	bool tx_raw_stream_end_seen;
	u32 tx_raw_stream_inflight;
	void *tx_raw_stream_owner;
	int local_transmit_path;
	int local_tx_hop;
	int local_rx_hop;
	int remote_transmit_path;
};

struct tbv_rail {
	struct list_head node;
	struct tbv_peer *peer;
	struct tbv_state *native_work_state;
	struct tbv_rail_key key;
	struct tbv_path path;
	struct delayed_work native_work;
	refcount_t refcnt;
	struct completion refs_zero;
	/*
	 * Per-rail IB device. Lifecycle managed by tbv_ibdev_rail_event()
	 * (see ibdev.c) under state->rail_register_lock. NULL means this rail
	 * has not yet reached the data-ready edge (or has been torn down).
	 */
	struct tbv_ibdev *ibdev;
	atomic_t native_qp_bind_count;
	u32 rail_id;
	u32 link_speed;
	u32 link_width;
	u32 remote_rail_id;
	int remote_transmit_path;
	int remote_tx_hop;
	int remote_rx_hop;
	u32 native_attempts;
	u32 native_tunnel_attempts;
	u32 native_ready_attempts;
	u32 native_control_attempts;
	int native_last_error;
	/*
	 * Physical lane index for native rails (0..TBV_NATIVE_MAX_LANES-1).
	 * Set in tbv_peer_add_rail() from the matched service id and consumed
	 * by tbv_ibdev_rail_name_index(). Don't derive this from rail->key.path_id;
	 * the encoded path_id uses TBV_NATIVE_PRTCID as its low byte for lane 0,
	 * which collides with the (prtcid << 8) | lane scheme used for the
	 * other lanes (both lane 0 and lane 1 would round-trip to "lane 1").
	 */
	u32 native_lane;
	bool active;
	bool removing;
	/* The DMA-BUF engine stopped the data rings (data error, abort or
	 * shutdown); they stay stopped until the connection is recreated. */
	bool rings_stopped;
	/* Why the rings were stopped: 1 abort/data error, 2 interrupted batch. */
	int dmabuf_quarantine_reason;
	struct tbv_dmabuf_cache *dmabuf_cache[2];
	/*
	 * Registration unwinding marker. Set under state->rail_register_lock
	 * when ib_register_device() returns nonzero for this rail. While true
	 * tbv_ibdev_start()'s catchup loop and tbv_ibdev_rail_event() will
	 * skip the rail, breaking the spin loop the failed lane would
	 * otherwise cause.
	 */
	bool ibdev_register_failed;
	/*
	 * Retryable registration blocker. A ready rail can reach verbs
	 * registration before the configured RoCE netdev exists; keep it out of
	 * the catchup loop until a matching netdev notifier event retries it.
	 */
	bool ibdev_register_deferred;
	bool native_negotiated;
	bool native_ready_sent;
	bool native_remote_ready;
	bool native_work_stop;
};

struct tbv_peer {
	struct list_head node;
	refcount_t refcnt;
	struct tbv_state *state;
	u32 peer_id;
	struct tb_xdomain *xd;
	struct list_head rails;
	struct ida rail_ids;
	/* Serializes XDomain control and tunnel setup transactions per link. */
	struct mutex control_lock;
	/* Control storage lives until the peer is destroyed, and is stopped
	 * before any rail teardown drains verbs/data callbacks. */
	struct mutex dmabuf_control_lock;
	struct tbv_control *dmabuf_control;
	bool dmabuf_control_dead;
	bool remote_control_ring;
	bool remote_push_done;
	bool remote_dmabuf_credit;
	u32 native_qp_rr_rail_id;
	u32 nr_rails;
	/*
	 * Host identity the remote side sent in native control (protected by
	 * state->lock). Peers with the same one are links to the same host,
	 * and a QP may spread data over the rails of all of them.
	 */
	u8 remote_host_id[16];
	bool remote_host_known;
	/* Descriptor rendezvous replay watermark, protected by state->lock. */
	u64 dmabuf_rx_token;
};

static inline bool tbv_rail_data_ready(const struct tbv_rail *rail)
{
	if (!rail || rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return false;
	if (!rail->peer)
		return true;
	return rail->native_ready_sent && rail->native_remote_ready;
}

#define TBV_CONFIGURED_LINK_NAME_LEN (TBV_CFG_LINK_NAME_MAX + 1u)

struct tbv_configured_link {
	struct list_head node;
	u32 link_id;
	struct tbv_id_selection app_selection;
	char name[TBV_CONFIGURED_LINK_NAME_LEN];
};

struct tbv_state {
	struct tbv_config cfg;
	struct mutex lock;
	struct list_head peers;
	struct list_head configured_links;
	u32 next_peer_id;
	u32 configured_link_count;
	struct tb_property_dir *native_dirs[TBV_NATIVE_MAX_LANES];
	u32 native_dir_count;
	struct dentry *debugfs_dir;
	bool allocate_rings;
	bool start_rings;
	bool negotiate_native;
	bool enable_tunnels;
	bool native_data;
	bool native_fragment_striping;
	bool native_write_striping;
	u32 native_write_stripe_min_bytes;
	u32 native_domain_mask;
	bool register_verbs;
	bool services_registered;
	bool verbs_registered;
	bool native_control_registered;
	bool native_control_source_aware;
	/*
	 * Native control messages name their source link and host, so links
	 * are told apart even by a source-blind XDomain handler.
	 */
	bool native_control_ids;
	u8 native_host_id[16];
	bool native_legacy_multicable_warned;
	struct workqueue_struct *workqueue;
	struct notifier_block ibdev_netdev_nb;
	atomic_t verbs_ucontexts;
	atomic_t verbs_pds;
	atomic_t verbs_cqs;
	atomic_t verbs_qps;
	atomic_t verbs_mrs;
	atomic_t verbs_recv_wqes;
	TBV_STATE_COUNTERS(TBV_COUNTER_DECL)
	atomic64_t native_legacy_ambiguous_limited;
	struct xarray verbs_mrs_xa;
	struct xarray verbs_qps_xa;
	/*
	 * Serializes per-rail ib_device registration against teardown.
	 * tbv_ibdev_rail_event() publishes one ib_device per active rail as
	 * its data path comes up; tbv_peer_remove_rail() and module-exit
	 * tear them down. Kept separate from state->lock so the sleeping
	 * ib_(un)register_device path doesn't invert against verbs ops that
	 * take state->lock.
	 */
	struct mutex rail_register_lock;
	struct work_struct ibdev_netdev_retry_work;
	/*
	 * Up-event gate, owned by rail_register_lock. Set to true by
	 * tbv_ibdev_start() before any rising-edge events may publish; cleared
	 * by tbv_ibdev_stop() before draining so no late ready-edge event
	 * sneaks a fresh ib_device past module exit. Down events ignore this
	 * flag so existing devices can always be torn down.
	 */
	bool register_enabled;
	bool ibdev_netdev_nb_registered;
};

struct dentry;
struct tbv_service_config {
	u32 native_prtcstns;
	bool allocate_rings;
	bool start_rings;
	bool negotiate_native;
	bool enable_tunnels;
};

struct tb_property_dir;
struct tbv_data_frame;
struct tbv_native_data_header;
struct tbv_tx_packet;
struct device;
struct page;
struct tb_ring;
struct tb_xdomain;
typedef void (*tbv_path_tx_done_fn)(void *ctx, int status);
typedef int (*tbv_path_next_page_fn)(void *ctx, struct page **page,
				     u32 *page_off, u32 *length,
				     tbv_path_tx_done_fn *done,
				     void **done_ctx);
#define TBV_PATH_SEND_CONTROL	BIT(0)
#define TBV_PATH_SEND_DEFER	BIT(1)
/*
 * The caller reserved queue room with tbv_path_reserve_data() and this frame
 * uses one frame of it. Without it a frame never takes another sender's
 * reservation.
 */
#define TBV_PATH_SEND_RESERVED	BIT(2)
extern const uuid_t tbv_native_service_uuid;

int tbv_config_parse(struct tbv_config *cfg, const char *compat,
		     const char *profile, const char *tbnet, const char *lanes);

int tbv_ibdev_start(struct tbv_state *state, bool register_verbs);
void tbv_ibdev_stop(struct tbv_state *state);
const char *tbv_ibdev_roce_netdev_name(void);
/*
 * Notify the verbs layer that rail's data path has come up (joined=true) or
 * is about to be torn down (joined=false). Safe to call repeatedly; only the
 * rising/falling edge of "ibdev published" causes registration changes.
 *
 * Up events are gated on state->register_enabled (flipped off by
 * tbv_ibdev_stop()). Down events are unconditional so module-exit and rail
 * remove can always undo a published device. Returns 0 on success/no-op, or a
 * negative errno if an up event failed to publish (the rail is then
 * permanently marked failed and will be skipped on retry).
 */
int tbv_ibdev_rail_event(struct tbv_state *state, struct tbv_rail *rail,
			 bool joined);
void tbv_ibdev_rx_frame(struct tbv_state *state, struct tbv_path *rx_path,
			const void *data, u32 len);
void tbv_ibdev_rx_native_frame(struct tbv_state *state,
			       struct tbv_path *rx_path,
			       const struct tbv_native_data_header *hdr,
			       const void *payload);
int tbv_services_start(struct tbv_state *state, bool bind_services,
		       const struct tbv_service_config *service_cfg);
void tbv_services_stop(struct tbv_state *state);
int tbv_native_control_start(struct tbv_state *state);
void tbv_native_control_stop(struct tbv_state *state);
const char *tbv_native_control_mode_name(const struct tbv_state *state);
int tbv_native_control_xdomain_start(struct tbv_state *state);
void tbv_native_control_xdomain_stop(void);
int tbv_native_control_legacy_start(struct tbv_state *state);
void tbv_native_control_legacy_stop(void);
int tbv_native_control_handle_packet(struct tbv_state *state,
				     struct tb_xdomain *source_xd,
				     const void *buf, size_t size);
void tbv_native_control_init_rail(struct tbv_rail *rail,
				  struct tbv_peer *peer);
void tbv_native_control_queue_rail(struct tbv_state *state,
				   struct tbv_rail *rail);
void tbv_native_control_cancel_rail(struct tbv_rail *rail);
void tbv_rail_key_init(struct tbv_rail_key *key, u64 route,
		       u32 local_adapter, u32 remote_adapter, u32 path_id);
int tbv_rail_key_cmp(const struct tbv_rail_key *a,
		     const struct tbv_rail_key *b);
u32 tbv_rail_key_hash(const struct tbv_rail_key *key);
struct tbv_peer *tbv_peer_get_or_create(struct tbv_state *state,
					struct tb_xdomain *xd);
void tbv_peer_put(struct tbv_state *state, struct tbv_peer *peer);
struct tbv_rail *tbv_peer_add_rail(struct tbv_peer *peer,
				   const struct tbv_rail_key *key,
				   u32 native_lane);
void tbv_peer_remove_rail(struct tbv_rail *rail);
void tbv_rail_put(struct tbv_rail *rail);
void tbv_path_default_config(struct tbv_path_config *cfg);
void tbv_path_init_optional_symbols(void);
void tbv_path_exit_optional_symbols(void);
void tbv_path_init(struct tbv_path *path,
		   const struct tbv_path_config *cfg, struct tbv_rail *rail);
const char *tbv_path_state_name(enum tbv_path_state state);
int tbv_path_alloc_rings(struct tbv_path *path, struct tb_xdomain *xd,
			 int requested_transmit_path);
int tbv_path_start_rings(struct tbv_path *path);
int tbv_path_enable_tunnel(struct tbv_path *path, struct tb_xdomain *xd,
			   int remote_transmit_path);
int tbv_path_disable_tunnel(struct tbv_path *path, struct tb_xdomain *xd);
void tbv_path_set_remote_rx_capacity(struct tbv_path *path, u32 rx_ring_size);
void tbv_path_add_remote_rx_credits(struct tbv_path *path, u32 credits);
int tbv_path_reserve_data(struct tbv_path *path, u32 frames);
void tbv_path_release_data_reservation(struct tbv_path *path, u32 frames);
void tbv_ibdev_path_build_work(struct work_struct *work);
int tbv_path_send(struct tbv_path *path, const void *data, u32 len,
		  unsigned int flags,
		  tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_owned(struct tbv_path *path, void *data, u32 len,
			unsigned int flags,
			tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_marked_owned(struct tbv_path *path, void *data, u32 len,
			       u8 sof, u8 eof, unsigned int flags,
			       tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_prepare_owned_list(struct tbv_path *path,
				struct list_head *frames,
				struct list_head *packets,
				u32 *packet_count,
				unsigned int flags,
				tbv_path_tx_done_fn done,
				void *done_ctx);
int tbv_path_enqueue_prepared_reserved(struct tbv_path *path,
				       struct list_head *packets,
				       u32 packet_count,
				       unsigned int flags);
void tbv_path_release_prepared_list_silent(struct list_head *packets,
					   int status);
int tbv_path_send_page_stream(struct tbv_path *path,
			      const struct tbv_native_data_header *hdr,
			      u32 total_length, unsigned int flags,
			      tbv_path_tx_done_fn meta_done,
			      void *meta_done_ctx,
			      tbv_path_next_page_fn next, void *next_ctx);
void tbv_path_kick_tx(struct tbv_path *path);
void tbv_path_cancel_data_done_ctx(struct tbv_path *path,
				   tbv_path_tx_done_fn done, void *done_ctx);
void tbv_path_cancel_data_owner_ctx(struct tbv_path *path, void *owner_ctx);
void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd);

int tbv_native_validate_config(const struct tbv_cfg_link *link);
int tbv_link_activate_config(struct tbv_state *state, const char *name,
			     const struct tbv_cfg_link *cfg);
void tbv_link_deactivate_config(struct tbv_state *state, u32 link_id);
u32 tbv_link_count(struct tbv_state *state);
void tbv_link_debugfs_show(struct seq_file *s, struct tbv_state *state);
int tbv_debugfs_init(struct tbv_state *state);
void tbv_debugfs_exit(struct tbv_state *state);
int tbv_configfs_start(struct tbv_state *state);
void tbv_configfs_stop(struct tbv_state *state);

int tbv_core_init(struct tbv_state *state, const struct tbv_config *cfg);
void tbv_core_exit(struct tbv_state *state);

#include "verbs_dmabuf.h"

#endif
