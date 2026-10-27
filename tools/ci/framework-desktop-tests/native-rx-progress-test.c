// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
typedef uint32_t u32;typedef uint64_t u64;
#define TBV_RX_PROGRESS_BUDGET 256
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define msecs_to_jiffies(x) (x)
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!pthread_mutex_lock(p));}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(!pthread_mutex_unlock(p));}while(0)
struct work_struct {int unused;};struct delayed_work {struct work_struct work;bool pending;unsigned long delay;};
struct ring_frame;struct tb_ring {unsigned count,next;unsigned irq;struct ring_frame *frames;};
struct ring_frame {void (*callback)(struct tb_ring*,struct ring_frame*,bool);unsigned seq;};
struct rail {u32 rail_id;};
struct tbv_path {pthread_mutex_t rx_progress_lock;struct delayed_work rx_progress_work;bool rx_progress_enabled;struct tb_ring *rx_ring;struct rail *rail;};
struct tbv_perf_id {u32 rail;u64 token;};
static _Atomic long native_rx_progress_irqs,native_rx_progress_runs,native_rx_progress_frames,native_rx_progress_empty,native_rx_progress_budget;
static void atomic64_inc(_Atomic long *v){atomic_fetch_add(v,1);}static void atomic64_add(unsigned n,_Atomic long *v){atomic_fetch_add(v,n);}
static unsigned native_rx_recheck_ms=1;static int system_unbound_wq;
static int poll_calls,rearms,callbacks,next_expected;static bool inject_irq_at_complete;static struct tbv_path *current;
static bool mod_delayed_work(int q,struct delayed_work *w,unsigned long delay){(void)q;w->pending=true;w->delay=delay;return true;}
static bool queue_delayed_work(int q,struct delayed_work *w,unsigned long delay){(void)q;if(w->pending)return false;w->pending=true;w->delay=delay;return true;}
static void cancel_delayed_work_sync(struct delayed_work *w){w->pending=false;}
static struct ring_frame *tb_ring_poll(struct tb_ring *r){poll_calls++;return r->next<r->count?&r->frames[r->next++]:NULL;}
static void tbv_path_rx_start_poll(void *data);
static void tb_ring_poll_complete(struct tb_ring *r){(void)r;rearms++;if(inject_irq_at_complete){inject_irq_at_complete=false;tbv_path_rx_start_poll(current);}}
#include "helpers.c"
static void cb(struct tb_ring *r,struct ring_frame *f,bool canceled){(void)r;assert(!canceled);assert(f->seq==(unsigned)next_expected++);callbacks++;}
static void run(struct tbv_path *p){p->rx_progress_work.pending=false;tbv_path_rx_progress_work(&p->rx_progress_work.work);}
static void *irq_loop(void *arg){for(int i=0;i<100000;i++){tbv_path_rx_start_poll(arg);if(!(i%100))sched_yield();}return NULL;}
int main(void){
 struct ring_frame frames[1025];for(unsigned i=0;i<1025;i++)frames[i]=(struct ring_frame){.callback=cb,.seq=i};
 struct tb_ring ring={.count=1025,.frames=frames};struct tbv_path path={.rx_ring=&ring};current=&path;assert(!pthread_mutex_init(&path.rx_progress_lock,NULL));
 /* Disabled IRQ and worker cannot enqueue, drain or rearm. */
 tbv_path_rx_start_poll(&path);run(&path);assert(!path.rx_progress_work.pending && !poll_calls && !rearms);
 path.rx_progress_enabled=true;tbv_path_rx_start_poll(&path);assert(path.rx_progress_work.pending && path.rx_progress_work.delay==0);
 for(int i=0;i<4;i++){run(&path);assert(callbacks==256*(i+1));assert(path.rx_progress_work.pending && path.rx_progress_work.delay==0);assert(!rearms);}
 run(&path);assert(callbacks==1025 && rearms==1 && path.rx_progress_work.delay==1);
 /* IRQ at rearm must retain immediate work; idle recheck must not delay it. */
 inject_irq_at_complete=true;run(&path);assert(path.rx_progress_work.pending && path.rx_progress_work.delay==0);
 native_rx_recheck_ms=0;run(&path);assert(!path.rx_progress_work.pending);
 /* Disable/enqueue race: after disable returns, even ongoing IRQ producers
  * cannot requeue. This executes the real lock-and-enqueue helper. */
 pthread_t irq;assert(!pthread_create(&irq,NULL,irq_loop,&path));sched_yield();tbv_path_rx_progress_disable(&path);assert(!pthread_join(irq,NULL));assert(!path.rx_progress_work.pending && !path.rx_progress_enabled);
 int oldpoll=poll_calls,oldrearm=rearms;run(&path);assert(poll_calls==oldpoll && rearms==oldrearm);
 assert(callbacks==1025 && next_expected==1025 && native_rx_progress_budget==4);
 puts("PASS: ordered1025-frame drain, bounded batches, IRQ/rearm priority, IRQ-only, disabled state,100000 concurrent IRQ/disable attempts");
}
