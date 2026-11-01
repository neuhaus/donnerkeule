/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include "proto/verbs_control.h"

static struct tbv_dmabuf_wire message(unsigned int rails, unsigned int bytes)
{
	struct tbv_dmabuf_wire m = {0};
	unsigned int i;

	m.op = TBV_DMABUF_PREPARE;
	m.token = 0x1122334455667788ull;
	m.hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
	m.hdr.flags = TBV_NATIVE_DATA_F_RAW_STREAM;
	m.hdr.src_qp = 0x901;
	m.hdr.dest_qp = 0x902;
	m.hdr.psn = 0xabcdef;
	m.hdr.length = bytes;
	m.hdr.remote_addr = 0x123456780000ull;
	m.hdr.rkey = 17;
	m.hdr.imm_data = 0x87654321;
	m.bytes = bytes;
	m.count = rails;
	memset(m.src_host, 0x11, 16);
	memset(m.dst_host, 0x22, 16);
	memset(m.src_domain, 0x33, 16);
	for (i = 0; i < rails; i++) {
		memset(m.lanes[i].domain, 0x40 + i, 16);
		m.lanes[i].path = 8 + i;
	}
	return m;
}

int main(void)
{
	const unsigned int sizes[] = {0, 1, 4, 4095, 4096, 4097, 65536,
		2 * 1024 * 1024, 2 * 1024 * 1024 + 1, 8 * 1024 * 1024};
	unsigned char packet[TBV_DMABUF_WIRE_SIZE], bad[TBV_DMABUF_WIRE_SIZE];
	_Static_assert(sizeof(packet) + 4 <= 256, "config frame includes a four-byte CRC");
	struct tbv_dmabuf_wire m, decoded;
	unsigned int rails, i, op;

	for (rails = 1; rails <= 4; rails++) {
		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			m = message(rails, sizes[i]);
			if (sizes[i] > rails * 2 * 1024 * 1024) {
				assert(tbv_dmabuf_shape(&m) == -EMSGSIZE);
				continue;
			}
			for (op = 1; op <= 6; op++) {
				if (!tbv_dmabuf_opcode(op))
					continue;
				m.hdr.opcode = op;
				assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
				assert(tbv_dmabuf_wire_parse(packet, sizeof(packet), &decoded) == 0);
				assert(decoded.token == m.token && decoded.count == rails);
				assert(decoded.hdr.length == sizes[i] && decoded.bytes == sizes[i]);
				assert(decoded.hdr.imm_data == 0x87654321);
				assert(decoded.hdr.psn == 0xabcdef && decoded.hdr.rkey == 17);
				assert(decoded.hdr.remote_addr == 0x123456780000ull);
			}
		}
	}
	m = message(4, 8 * 1024 * 1024);
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
	/* Independent byte-level ABI expectations. */
	assert(packet[8] == 60 && packet[32] == 'T' && packet[33] == 'D');
	assert(packet[48] == 0x88 && packet[55] == 0x11);
	assert(packet[168] == 0x40 && packet[228] == 0x43 && packet[244] == 11);
	assert(tbv_dmabuf_wire_parse(packet, 251, &decoded) == -EINVAL);
	for (i = 0; i < sizeof(packet); i++) {
		if (i != 40 && i != 41 && i != 42 && i != 43 &&
		    i != 164 && i != 165 && i != 166 && i != 167 && i < 248)
			continue;
		memcpy(bad, packet, sizeof(bad));
		bad[i] = 1;
		assert(tbv_dmabuf_wire_parse(bad, sizeof(bad), &decoded) == -EINVAL);
	}
	memcpy(bad, packet, sizeof(bad));
	bad[160] = 5;
	assert(tbv_dmabuf_wire_parse(bad, sizeof(bad), &decoded) == -EINVAL);
	m.op = TBV_DMABUF_STATUS | TBV_DMABUF_REPLY;
	m.status = -EACCES;
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
	assert(tbv_dmabuf_wire_parse(packet, sizeof(packet), &decoded) == 0);
	assert(decoded.status == -EACCES && decoded.op == m.op);
	assert(!tbv_dmabuf_wire_notification(packet, sizeof(packet)));
	/* A pushed DONE must be consumed before the kernel's route/UUID-only
	 * response matcher, even if the body cannot be trusted. Ordinary
	 * replies must remain visible to their synchronous requester. */
	m.op = TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY;
	m.status = TBV_DMABUF_DONE;
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
	assert(tbv_dmabuf_wire_notification(packet, sizeof(packet)));
	assert(tbv_dmabuf_wire_parse(packet, sizeof(packet), &decoded) == 0);
	assert(tbv_dmabuf_wire_same(&m, &decoded, true));
	assert(!tbv_dmabuf_wire_notification(NULL, sizeof(packet)));
	assert(!tbv_dmabuf_wire_notification(packet, 31));
	assert(tbv_dmabuf_wire_notification(packet, 32));
	assert(tbv_dmabuf_wire_notification(packet, 251));
	for (i = 0; i < 4; i++) {
		memcpy(bad, packet, sizeof(bad));
		switch (i) {
		case 0: bad[40] = 1; break; /* Reserved body field. */
		case 1: bad[28] = TBV_DMABUF_PREPARE; break; /* Header opcode. */
		case 2: bad[38] = TBV_DMABUF_PREPARE; break; /* Body opcode. */
		case 3: bad[160] = 5; break; /* Impossible rail count. */
		}
		assert(tbv_dmabuf_wire_parse(bad, sizeof(bad), &decoded) == -EINVAL);
		assert(tbv_dmabuf_wire_notification(bad, sizeof(bad)));
	}
	memcpy(bad, packet, sizeof(bad));
	bad[12] ^= 1; /* Another protocol's traffic belongs to its own handler. */
	assert(!tbv_dmabuf_wire_notification(bad, sizeof(bad)));
	m.op = TBV_DMABUF_PREPARE | TBV_DMABUF_REPLY;
	m.status = TBV_DMABUF_READY;
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
	assert(!tbv_dmabuf_wire_notification(packet, sizeof(packet)));
	m = message(1, 1);
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
	packet[188] = 1; /* Unused lane must be zero. */
	assert(tbv_dmabuf_wire_parse(packet, sizeof(packet), &decoded) == -EINVAL);
	m = message(4, TBV_NATIVE_DATA_MAX_MSG_SIZE);
	m.bytes = 8 * 1024 * 1024;
	m.offset = 8 * 1024 * 1024;
	assert(tbv_dmabuf_shape(&m) == 0);
	m.offset++;
	assert(tbv_dmabuf_shape(&m) == -EINVAL);
	m.offset = TBV_NATIVE_DATA_MAX_MSG_SIZE;
	assert(tbv_dmabuf_shape(&m) == -EINVAL);
	m.offset = TBV_NATIVE_DATA_MAX_MSG_SIZE - 8 * 1024 * 1024;
	assert(tbv_dmabuf_shape(&m) == 0); /* Last chunk of the largest message. */
	m.hdr.length = TBV_NATIVE_DATA_MAX_MSG_SIZE + 1u;
	m.offset = 0;
	assert(tbv_dmabuf_shape(&m) == -EINVAL);
	m.offset = 0;
	m.bytes = 4095; /* Only the final chunk can have a short tail. */
	assert(tbv_dmabuf_shape(&m) == -EINVAL);
	m.bytes = 0;
	assert(tbv_dmabuf_shape(&m) == -EINVAL);
	m = message(4, 1);
	m.hdr.psn = 0x1000000;
	assert(tbv_dmabuf_shape(&m) == -EINVAL);
	m = message(4, 1);
	m.token = 0;
	assert(tbv_dmabuf_shape(&m) == -EINVAL);
	/* Credit notifications carry a nonzero sequence at byte 164; every
	 * other operation keeps it zero. All are consumed before core matching. */
	for (op = TBV_DMABUF_CREDIT; op <= TBV_DMABUF_RELEASE; op++) {
		m = message(2, 10304);
		m.hdr.opcode = TBV_NATIVE_DATA_OP_SEND_IMM;
		m.op = op | TBV_DMABUF_REPLY;
		m.status = op == TBV_DMABUF_CREDIT ? -ECANCELED : 0;
		assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == -EINVAL);
		m.credit = 0x01020304;
		assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
		assert(packet[164] == 4 && packet[167] == 1);
		assert(tbv_dmabuf_wire_notification(packet, sizeof(packet)));
		assert(tbv_dmabuf_wire_parse(packet, sizeof(packet), &decoded) == 0);
		assert(decoded.op == m.op && decoded.credit == m.credit &&
		       decoded.status == m.status);
		assert(tbv_dmabuf_wire_same(&m, &decoded, true));
		memcpy(bad, packet, sizeof(bad));
		memset(bad + 164, 0, 4);
		assert(tbv_dmabuf_wire_parse(bad, sizeof(bad), &decoded) == -EINVAL);
		assert(tbv_dmabuf_wire_notification(bad, sizeof(bad)));
	}
	m = message(2, 4096);
	m.credit = 1;
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == -EINVAL);
	/* A READY reply names the pre-armed credit its PREPARE adopted. */
	m.op = TBV_DMABUF_PREPARE | TBV_DMABUF_REPLY;
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == 252);
	assert(!tbv_dmabuf_wire_notification(packet, sizeof(packet)));
	assert(tbv_dmabuf_wire_parse(packet, sizeof(packet), &decoded) == 0);
	assert(decoded.credit == 1);
	m.op = TBV_DMABUF_STATUS | TBV_DMABUF_REPLY;
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == -EINVAL);
	m.op = 13;
	m.credit = 0;
	assert(tbv_dmabuf_wire_build(packet, sizeof(packet), &m, 1) == -EINVAL);
	{
		unsigned char frame[TBV_CTRL_FRAME_SIZE], broken[TBV_CTRL_FRAME_SIZE];
		unsigned char setup[TBV_CTRL_SETUP_SIZE];
		struct tbv_ctrl_setup hello = {0}, parsed;

		m = message(4, 8 * 1024 * 1024);
		m.op = TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY;
		m.status = TBV_DMABUF_DONE;
		assert(tbv_ctrl_frame_build(frame, sizeof(frame), &m) == 256);
		assert(tbv_ctrl_frame_parse(frame, sizeof(frame), &decoded) == 0);
		assert(decoded.op == m.op && decoded.status == TBV_DMABUF_DONE);
		assert(tbv_dmabuf_wire_same(&m, &decoded, true));
		assert(frame[0] == 'T' && frame[1] == 'B' && frame[2] == 'C');
		assert(tbv_ctrl_frame_parse(frame, 255, &decoded) == -EINVAL);
		memcpy(broken, frame, sizeof(frame));
		broken[0] ^= 1;
		assert(tbv_ctrl_frame_parse(broken, sizeof(broken), &decoded) == -EINVAL);
		for (i = 0; i < 7; i++) {
			decoded = m;
			switch (i) {
			case 0: decoded.token++; break;
			case 1: decoded.src_host[0]++; break;
			case 2: decoded.dst_host[0]++; break;
			case 3: decoded.src_domain[0]++; break;
			case 4: decoded.hdr.psn++; break;
			case 5: decoded.offset += 4096; break;
			case 6: decoded.lanes[0].path++; break;
			}
			assert(!tbv_dmabuf_wire_same(&m, &decoded, true));
		}
		hello.op = TBV_CTRL_SETUP;
		hello.path = 12;
		memcpy(hello.src_host, m.src_host, 16);
		memcpy(hello.dst_host, m.dst_host, 16);
		memcpy(hello.domain, m.src_domain, 16);
		assert(tbv_ctrl_setup_build(setup, sizeof(setup), &hello, 1) == 128);
		assert(tbv_ctrl_setup_parse(setup, sizeof(setup), &parsed) == 0);
		assert(parsed.path == 12 && !memcmp(parsed.domain, hello.domain, 16));
		assert(setup[8] == 29 && setup[32] == 'T' && setup[34] == 'S');
		setup[100] = 1;
		assert(tbv_ctrl_setup_parse(setup, sizeof(setup), &parsed) == -EINVAL);
		setup[100] = 0;
		setup[96] = 64;
		assert(tbv_ctrl_setup_parse(setup, sizeof(setup), &parsed) == -EINVAL);
		hello.op = TBV_CTRL_READY | TBV_DMABUF_REPLY;
		hello.status = -EAGAIN;
		assert(tbv_ctrl_setup_build(setup, sizeof(setup), &hello, 1) == 128);
		assert(tbv_ctrl_setup_parse(setup, sizeof(setup), &parsed) == 0);
		assert(parsed.status == -EAGAIN && parsed.op == hello.op);
		hello.path = 7;
		assert(tbv_ctrl_setup_build(setup, sizeof(setup), &hello, 1) == -EINVAL);
	}
	puts("Verbs DMA-BUF control: PASS (ABI, tails, batches, control rings, generations, malformed packets)");
	return 0;
}
