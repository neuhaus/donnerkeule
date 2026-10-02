// SPDX-License-Identifier: MIT
/* Live kernel checks: untrusted shared indices and a failed CQ destruction. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <infiniband/driver.h>
#include "../../proto/tbv_cq_shm.h"

struct create_response {
	struct ib_uverbs_create_cq_resp core;
	struct tbv_uresp_create_cq driver;
};

#define CHECK(test) do { if (!(test)) { \
	fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #test, errno); \
	return 1; } } while (0)

static int raw_cq_check(struct ibv_context *ctx, unsigned capacity)
{
	struct ibv_cq cq = {};
	struct ibv_create_cq cmd = {};
	struct create_response response = {};
	struct ibv_wc wc;
	struct tbv_cq_shm *header;

	CHECK(!ibv_cmd_create_cq(ctx, capacity, NULL, 0, &cq, &cmd,
				sizeof(cmd), &response.core, sizeof(response)));
	pthread_mutex_init(&cq.mutex, NULL);
	pthread_cond_init(&cq.cond, NULL);
	CHECK(cq.cqe == (int)capacity);
	CHECK(response.driver.shm_abi == TBV_CQ_SHM_ABI);
	header = mmap(NULL, response.driver.map_len, PROT_READ | PROT_WRITE,
		      MAP_SHARED, ctx->cmd_fd, response.driver.cq_mmap_offset);
	CHECK(header != MAP_FAILED);
	CHECK(!ibv_cmd_poll_cq(&cq, 1, &wc));
	/* The old implementation fabricates an ib_wc with a NULL qp here. */
	header->produced = 1;
	header->cqe = 0;
	header->ovf_seq = 99;
	CHECK(!ibv_cmd_poll_cq(&cq, 1, &wc));
	/* A consumer cannot acknowledge a completion the kernel never queued. */
	__atomic_store_n(&header->consumed, 1, __ATOMIC_RELEASE);
	CHECK(ibv_cmd_poll_cq(&cq, 1, &wc) < 0);
	__atomic_store_n(&header->consumed, 0, __ATOMIC_RELEASE);
	CHECK(!ibv_cmd_poll_cq(&cq, 1, &wc));
	CHECK(!ibv_cmd_destroy_cq(&cq));
	/* An existing mapping retains its backing pages after destroy. */
	CHECK(header->magic == TBV_CQ_SHM_MAGIC);
	CHECK(!munmap(header, response.driver.map_len));
	pthread_cond_destroy(&cq.cond);
	pthread_mutex_destroy(&cq.mutex);
	printf("raw_cq capacity=%u forged_producer=rejected invalid_consumer=rejected\n",
	       capacity);
	return 0;
}

static int busy_cq_check(struct ibv_context *ctx)
{
	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	struct ibv_cq *cq = ibv_create_cq(ctx, 3, NULL, NULL, 0);
	struct ibv_qp_init_attr attr = {};
	struct ibv_wc wc;
	struct ibv_qp *qp;

	CHECK(pd && cq);
	attr.send_cq = attr.recv_cq = cq;
	attr.qp_type = IBV_QPT_RC;
	attr.cap.max_send_wr = attr.cap.max_recv_wr = 1;
	attr.cap.max_send_sge = attr.cap.max_recv_sge = 1;
	qp = ibv_create_qp(pd, &attr);
	CHECK(qp);
	CHECK(ibv_destroy_cq(cq) == EBUSY);
	CHECK(!ibv_poll_cq(cq, 1, &wc));
	CHECK(!ibv_destroy_qp(qp));
	CHECK(!ibv_destroy_cq(cq));
	CHECK(!ibv_dealloc_pd(pd));
	puts("busy_cq EBUSY preserves mapping and polling");
	return 0;
}

#define FLUSH_COUNT 511
struct poll_test {
	struct ibv_cq *cq;
	atomic_uint count, errors, seen[FLUSH_COUNT];
	struct timespec deadline;
};

static void *poll_thread(void *arg)
{
	struct poll_test *test = arg;
	struct ibv_wc wc[7];
	struct timespec now;

	while (atomic_load(&test->count) < FLUSH_COUNT &&
	       !atomic_load(&test->errors)) {
		int n = ibv_poll_cq(test->cq, 7, wc);

		if (n < 0) {
			atomic_fetch_add(&test->errors, 1);
			break;
		}
		for (int i = 0; i < n; i++) {
			if (wc[i].status != IBV_WC_WR_FLUSH_ERR ||
			    wc[i].opcode != IBV_WC_RECV || wc[i].wr_id >= FLUSH_COUNT ||
			    atomic_fetch_add(&test->seen[wc[i].wr_id], 1))
				atomic_fetch_add(&test->errors, 1);
			atomic_fetch_add(&test->count, 1);
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec >= test->deadline.tv_sec) {
			atomic_fetch_add(&test->errors, 1);
			break;
		}
	}
	return NULL;
}

/* Two pollers drain concurrently while QP error publishes flush completions.
 * A non-power-of-two CQ also exercises private head/tail wraparound.
 */
static int concurrent_cq_check(struct ibv_context *ctx)
{
	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	struct ibv_cq *cq = ibv_create_cq(ctx, FLUSH_COUNT + 2, NULL, NULL, 0);
	struct ibv_qp_init_attr init = {};
	struct ibv_qp_attr attr = {};
	struct ibv_recv_wr wr[FLUSH_COUNT] = {}, *bad;
	struct ibv_qp *qp;

	CHECK(pd && cq);
	init.send_cq = init.recv_cq = cq;
	init.qp_type = IBV_QPT_RC;
	init.cap.max_send_wr = 1;
	init.cap.max_recv_wr = FLUSH_COUNT;
	init.cap.max_send_sge = init.cap.max_recv_sge = 1;
	qp = ibv_create_qp(pd, &init);
	CHECK(qp);
	for (unsigned round = 0; round < 8; round++) {
		struct poll_test test = {.cq = cq};
		pthread_t threads[2];

		attr.qp_state = IBV_QPS_INIT;
		attr.port_num = 1;
		CHECK(!ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
				   IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
		for (unsigned i = 0; i < FLUSH_COUNT; i++) {
			wr[i].wr_id = i;
			wr[i].next = i + 1 < FLUSH_COUNT ? &wr[i + 1] : NULL;
		}
		CHECK(!ibv_post_recv(qp, wr, &bad));
		clock_gettime(CLOCK_MONOTONIC, &test.deadline);
		test.deadline.tv_sec += 5;
		CHECK(!pthread_create(&threads[0], NULL, poll_thread, &test));
		CHECK(!pthread_create(&threads[1], NULL, poll_thread, &test));
		attr.qp_state = IBV_QPS_ERR;
		CHECK(!ibv_modify_qp(qp, &attr, IBV_QP_STATE));
		CHECK(!pthread_join(threads[0], NULL));
		CHECK(!pthread_join(threads[1], NULL));
		CHECK(atomic_load(&test.count) == FLUSH_COUNT);
		CHECK(!atomic_load(&test.errors));
		for (unsigned i = 0; i < FLUSH_COUNT; i++)
			CHECK(atomic_load(&test.seen[i]) == 1);
		attr.qp_state = IBV_QPS_RESET;
		CHECK(!ibv_modify_qp(qp, &attr, IBV_QP_STATE));
	}
	CHECK(!ibv_destroy_qp(qp));
	CHECK(!ibv_destroy_cq(cq));
	CHECK(!ibv_dealloc_pd(pd));
	puts("concurrent_cq two pollers: 4088 flush completions exactly once");
	return 0;
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "usb4_rdma0";
	struct ibv_device **devices = ibv_get_device_list(NULL);
	struct ibv_context *ctx = NULL;
	unsigned capacities[] = {1, 3, 32, 4096};
	int ret = 0;

	CHECK(devices);
	for (int i = 0; devices[i]; i++)
		if (!strcmp(ibv_get_device_name(devices[i]), name))
			ctx = ibv_open_device(devices[i]);
	ibv_free_device_list(devices);
	CHECK(ctx);
	for (unsigned i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++)
		if ((ret = raw_cq_check(ctx, capacities[i])))
			break;
	if (!ret)
		ret = busy_cq_check(ctx);
	if (!ret)
		ret = concurrent_cq_check(ctx);
	ibv_close_device(ctx);
	return ret;
}
