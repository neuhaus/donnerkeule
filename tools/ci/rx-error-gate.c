// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "proto/native_data.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef long long atomic64_t;
#define atomic64_inc(p) (++*(p))
#define atomic64_add(n,p) (*(p)+=(n))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define pr_warn_ratelimited(...) ((void)0)
#define DMA_FROM_DEVICE 0
#define TBV_DATA_FRAME_SIZE 4096u
#define TBV_DATA_CREDIT_CONTROL_RESERVE 256u
#define TBV_BACKEND_NATIVE 0
#define TBV_BACKEND_APPLE 1
#define TBV_PATH_RING_STARTED 1
#define TBV_PATH_TUNNEL_ENABLED 2
/* Public Linux7.3 ring descriptor error flag values. */
#define RING_DESC_CRC_ERROR 1u
#define RING_DESC_BUFFER_OVERRUN 4u
struct ring_frame {u32 size,flags,sof,eof;};
struct tb_ring {int unused;};
struct tbv_state {
 bool verbs_registered;
 atomic64_t data_rx_canceled,data_rx_completed,data_rx_bad_frame,data_rx_crc_error,data_rx_overrun;
 atomic64_t data_rx_raw_poisoned,data_rx_raw_poison_drop,data_rx_bad_header,data_rx_bad_header_len;
 atomic64_t data_rx_bad_header_path_credit,data_rx_repost_failed,data_rx_lost;
};
struct peer {int backend;};
struct tbv_rail {struct peer *peer;struct {u32 path_id;}key;};
struct tbv_path {
 struct tbv_rail *rail;bool rx_raw_pending,rx_raw_poisoned,rx_data_seq_valid;
 u16 rx_data_seq_next;int state;
 struct {u32 rx_ring_size;}cfg;
 atomic64_t data_rx_canceled,data_rx_completed,rx_bytes,data_rx_repost_failed,data_rx_lost;
};
struct tbv_data_frame {struct ring_frame frame;struct tbv_path *path;u8 *buf;u64 dma;};
static struct tbv_state state;
static unsigned forwarded,raw_calls,apple_calls,reposts,credits,remote_credits;
static int repost_error;
static struct tbv_state *tbv_path_state(struct tbv_path *p){(void)p;return &state;}
static void *tb_ring_dma_device(struct tb_ring *r){return r;}
static void dma_sync_single_for_cpu(void *d,u64 dma,u32 n,int dir){(void)d;(void)dma;(void)n;(void)dir;}
static void tbv_ibdev_rx_native_frame(struct tbv_state *s,struct tbv_path *p,const struct tbv_native_data_header *h,const u8 *b){(void)s;(void)p;(void)h;(void)b;forwarded++;}
static void tbv_ibdev_rx_apple_frame(struct tbv_state *s,struct tbv_path *p,const void*b,u32 n,u32 a,u32 z){(void)s;(void)p;(void)b;(void)n;(void)a;(void)z;apple_calls++;}
static void tbv_path_rx_raw_payload(struct tbv_path*p,struct tbv_state*s,const void*b,u32 n){(void)p;(void)s;(void)b;(void)n;raw_calls++;}
static void tbv_path_rx_start_raw(struct tbv_path*p,const struct tbv_native_data_header*h){(void)h;p->rx_raw_pending=true;}
static int tbv_path_post_rx_frame(struct tbv_data_frame*f){(void)f;reposts++;return repost_error;}
static void tbv_path_return_rx_data_credit(struct tbv_path*p,u32 n){(void)p;credits+=n;}
static void tbv_path_add_remote_rx_credits(struct tbv_path*p,u32 n){(void)p;remote_credits+=n;}
static void tbv_rx_bad_header_parse_note(struct tbv_state*s,struct tbv_path*p,const void*b,u32 n,int ret){(void)p;(void)b;(void)n;(void)ret;s->data_rx_bad_header++;}
static void tbv_rx_bad_header_note(struct tbv_state*s,struct tbv_path*p,atomic64_t*c,const char*why,const struct tbv_native_data_header*h,u32 n,int ret){(void)p;(void)why;(void)h;(void)n;(void)ret;s->data_rx_bad_header++;(*c)++;}
#include "helpers.c"
static struct peer peer={.backend=TBV_BACKEND_NATIVE};
static struct tbv_rail rail={.peer=&peer,.key={.path_id=257}};
static struct tbv_path path;
static struct tbv_data_frame frame;
static struct tb_ring ring;
static u8 buffer[4096];
static void reset(void){
 memset(&state,0,sizeof(state));state.verbs_registered=true;
 memset(&path,0,sizeof(path));path.rail=&rail;path.state=TBV_PATH_TUNNEL_ENABLED;path.cfg.rx_ring_size=1024;
 memset(&frame,0,sizeof(frame));frame.path=&path;frame.buf=buffer;frame.frame.size=4096;frame.frame.flags=0xa;
 forwarded=raw_calls=apple_calls=reposts=credits=remote_credits=0;repost_error=0;
}
static void packet(u16 seq){
 struct tbv_native_data_header h={.opcode=TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM,.length=4048,.flags=TBV_NATIVE_DATA_F_LAST};
 assert(tbv_native_data_build_header(buffer,sizeof(buffer),&h)>=0);
 tbv_wire_put_le16(buffer+TBV_NATIVE_DATA_PATH_SEQ_OFFSET,seq|TBV_NATIVE_DATA_PATH_SEQ_PRESENT);
}
static void deliver(void){tbv_path_rx_complete(&ring,&frame.frame,false);}
int main(void){
 reset();packet(0);deliver();assert(forwarded==1&&reposts==1&&credits==1);
 /* Real callback and real path-sequence helper must ignore corrupt headers,
  * then recover exactly the skipped data credit on the next valid sequence. */
 packet(1);frame.frame.flags=0xb;deliver();
 assert(forwarded==1&&state.data_rx_crc_error==1&&state.data_rx_overrun==0&&state.data_rx_bad_frame==1);
 assert(reposts==2&&credits==1&&!path.rx_raw_poisoned&&path.rx_data_seq_next==1);
 packet(2);frame.frame.flags=0xa;deliver();assert(forwarded==2&&credits==3&&reposts==3&&state.data_rx_lost==1);
 reset();packet(4);frame.frame.flags=0xe;deliver();assert(!forwarded&&state.data_rx_overrun==1&&reposts==1&&!credits);
 reset();packet(4);frame.frame.flags=0xf;deliver();assert(!forwarded&&state.data_rx_overrun==1&&state.data_rx_crc_error==1&&reposts==1);
 reset();packet(4);frame.frame.flags=0xf;tbv_path_rx_complete(&ring,&frame.frame,true);assert(!forwarded&&!reposts&&!state.data_rx_crc_error&&state.data_rx_canceled==1);
 /* Never shift a later headerless chunk into a damaged RAW stream. */
 reset();packet(4);path.rx_raw_pending=true;frame.frame.flags=0xb;deliver();assert(path.rx_raw_poisoned&&!raw_calls&&!forwarded&&reposts==1&&state.data_rx_raw_poisoned==1);
 frame.frame.flags=0xa;deliver();assert(!raw_calls&&!forwarded&&reposts==2&&state.data_rx_raw_poison_drop==1&&!credits);
 reset();packet(4);frame.frame.size=3840;deliver();assert(!forwarded&&state.data_rx_bad_header_len==1&&reposts==1);
 reset();packet(4);repost_error=-1;deliver();assert(forwarded==1&&state.data_rx_repost_failed==1&&!credits);
 reset();packet(4);frame.frame.flags=0xb;path.state=0;deliver();assert(!forwarded&&!reposts&&state.data_rx_crc_error==1);
 puts("Actual RX callback: CRC/overrun rejection, credit-gap recovery, RAW fail-closed, cancellation and repost checks passed");
 return 0;
}
