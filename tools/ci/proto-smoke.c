// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "proto/native_data.h"

static int hello_equal(const struct tbv_native_wire_hello *a,
		       const struct tbv_native_wire_hello *b)
{
	return a->capabilities == b->capabilities &&
	       a->rail_id == b->rail_id &&
	       a->route == b->route &&
	       a->tx_hop == b->tx_hop &&
	       a->rx_hop == b->rx_hop &&
	       a->transmit_path == b->transmit_path &&
	       a->tx_ring_size == b->tx_ring_size &&
	       a->rx_ring_size == b->rx_ring_size &&
	       a->path_flags == b->path_flags &&
	       !memcmp(a->src_domain, b->src_domain, sizeof(a->src_domain)) &&
	       !memcmp(a->host_id, b->host_id, sizeof(a->host_id));
}

/* A version 1 HELLO is a version 2 one without the trailing identities. */
static int check_hello_v1(const unsigned char *v2, const struct tbv_native_wire_hello *hello)
{
	unsigned char v1[TBV_NATIVE_WIRE_HELLO_MSG_SIZE_V1];
	struct tbv_native_wire_hello parsed;
	struct tbv_native_wire_info info;
	unsigned int length_sn;

	memcpy(v1, v2, sizeof(v1));
	length_sn = (unsigned int)v1[8] | (unsigned int)v1[9] << 8 |
		    (unsigned int)v1[10] << 16 | (unsigned int)v1[11] << 24;
	length_sn = (length_sn & ~0x3fu) |
		    (TBV_NATIVE_WIRE_HELLO_MSG_SIZE_V1 - 12u) / 4u;
	tbv_wire_put_le32(v1 + 8, length_sn);
	tbv_wire_put_le16(v1 + TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE + 4,
			  TBV_NATIVE_WIRE_VERSION_V1);
	tbv_wire_put_le16(v1 + TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE + 8,
			  TBV_NATIVE_WIRE_HELLO_MSG_SIZE_V1);

	memset(&parsed, 0xff, sizeof(parsed));
	if (tbv_native_wire_parse_hello(v1, sizeof(v1), &parsed, &info))
		return -1;
	if (parsed.has_ids || parsed.rail_id != hello->rail_id ||
	    parsed.path_flags != hello->path_flags ||
	    parsed.src_domain[0] || parsed.host_id[15])
		return -1;
	return 0;
}

static int data_header_equal(const struct tbv_native_data_header *a,
			     const struct tbv_native_data_header *b)
{
	return a->opcode == b->opcode &&
	       a->flags == b->flags &&
	       a->dest_qp == b->dest_qp &&
	       a->src_qp == b->src_qp &&
	       a->psn == b->psn &&
	       a->length == b->length &&
	       a->imm_data == b->imm_data &&
	       a->remote_addr == b->remote_addr &&
	       a->rkey == b->rkey &&
	       a->frag_offset == b->frag_offset;
}

static unsigned int native_data_frags(unsigned int len)
{
	return (len + TBV_NATIVE_DATA_MAX_PAYLOAD - 1u) /
	       TBV_NATIVE_DATA_MAX_PAYLOAD;
}

static int test_raw_stream_payload_header(void)
{
	struct tbv_native_data_header stream = {
		.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE,
		.flags = TBV_NATIVE_DATA_F_LAST |
			 TBV_NATIVE_DATA_F_SOLICITED |
			 TBV_NATIVE_DATA_F_RAW_STREAM,
		.dest_qp = 0x900,
		.src_qp = 0x901,
		.psn = 42,
		.length = 8192,
		.imm_data = 8192,
		.remote_addr = 0x100000000ull,
		.rkey = 0x1234,
	};
	struct tbv_native_data_header frag;
	int ret;

	ret = tbv_native_data_raw_payload_header(&stream, 0, 8192, 4096,
						 &frag);
	if (ret)
		return 1;
	if (frag.remote_addr != stream.remote_addr || frag.frag_offset != 0 ||
	    frag.length != 4096 || frag.flags)
		return 2;

	ret = tbv_native_data_raw_payload_header(&stream, 4096, 4096, 4096,
						 &frag);
	if (ret)
		return 3;
	if (frag.remote_addr != stream.remote_addr ||
	    frag.frag_offset != 4096 ||
	    frag.length != 4096 ||
	    frag.flags != (TBV_NATIVE_DATA_F_LAST |
			   TBV_NATIVE_DATA_F_SOLICITED))
		return 4;

	if (tbv_native_data_raw_payload_header(&stream, 4096, 4096, 4097,
					       &frag) != -EINVAL)
		return 5;

	stream.flags &= ~TBV_NATIVE_DATA_F_RAW_STREAM;
	if (tbv_native_data_raw_payload_header(&stream, 0, 4096, 4096,
					       &frag) != -EINVAL)
		return 6;

	return 0;
}

static int test_fragment_shapes(void)
{
	unsigned int idx;
	unsigned int count;

	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS, 0,
					   TBV_NATIVE_DATA_MAX_PAYLOAD, false,
					   &idx, &count))
		return 1;
	if (idx != 0 || count != 3)
		return 2;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS,
					   TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_PAYLOAD, false,
					   &idx, &count))
		return 3;
	if (idx != 1 || count != 3)
		return 4;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS, 8096,
					   96, true, &idx, &count))
		return 5;
	if (idx != 2 || count != 3)
		return 6;

	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_FRAME_SIZE,
					   TBV_NATIVE_DATA_MAX_FRAGS, 0,
					   TBV_NATIVE_DATA_FRAME_SIZE, false,
					   &idx, &count))
		return 7;
	if (idx != 0 || count != 2)
		return 8;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_FRAME_SIZE,
					   TBV_NATIVE_DATA_MAX_FRAGS, 4096,
					   TBV_NATIVE_DATA_FRAME_SIZE, true,
					   &idx, &count))
		return 9;
	if (idx != 1 || count != 2)
		return 10;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS, 4096,
					   TBV_NATIVE_DATA_FRAME_SIZE, true,
					   &idx, &count) != -EINVAL)
		return 11;

	return 0;
}

static int max_message_last_fragment(unsigned int total_len,
				     unsigned int max_payload,
				     unsigned int *count)
{
	unsigned int offset = (total_len - 1u) / max_payload * max_payload;
	unsigned int idx;

	if (tbv_native_data_fragment_shape(total_len, max_payload,
					   TBV_NATIVE_DATA_MAX_FRAGS, offset,
					   total_len - offset, true, &idx,
					   count))
		return -1;
	return idx + 1u == *count ? 0 : -1;
}

static int test_max_message_size(void)
{
	/* Gufo's TP2 prefill partial: 16,512 rows of 2,560 floats + header. */
	const unsigned int gufo_prefill = 16512u * 2560u * 4u + 64u;
	struct tbv_native_data_header hdr = {
		.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM,
		.flags = TBV_NATIVE_DATA_F_LAST | TBV_NATIVE_DATA_F_RAW_STREAM,
		.dest_qp = 0x900,
		.src_qp = 0x901,
		.length = TBV_NATIVE_DATA_MAX_MSG_SIZE,
	};
	unsigned char buf[TBV_NATIVE_DATA_HDR_SIZE];
	struct tbv_native_data_header parsed;
	unsigned int count;

	if (TBV_NATIVE_DATA_MAX_MSG_SIZE != 248u * 1024u * 1024u ||
	    TBV_NATIVE_DATA_MAX_FRAGS > 0xffffu)
		return 1;
	if (tbv_native_data_build_header(buf, sizeof(buf), &hdr) !=
	    (int)TBV_NATIVE_DATA_HDR_SIZE)
		return 2;
	if (tbv_native_data_parse_header(buf, sizeof(buf), &parsed) ||
	    parsed.length != TBV_NATIVE_DATA_MAX_MSG_SIZE)
		return 3;
	hdr.length = TBV_NATIVE_DATA_MAX_MSG_SIZE + 1u;
	if (tbv_native_data_build_header(buf, sizeof(buf), &hdr) != -EMSGSIZE)
		return 4;
	/* A peer's oversized length is rejected on parse as well. */
	buf[24] = (unsigned char)hdr.length;
	buf[25] = (unsigned char)(hdr.length >> 8);
	buf[26] = (unsigned char)(hdr.length >> 16);
	buf[27] = (unsigned char)(hdr.length >> 24);
	if (tbv_native_data_parse_header(buf, sizeof(buf), &parsed) !=
	    -EMSGSIZE)
		return 5;

	/* Both WRITE payload sizes fit the receiver's u16 fragment counters. */
	if (max_message_last_fragment(TBV_NATIVE_DATA_MAX_MSG_SIZE,
				      TBV_NATIVE_DATA_MAX_PAYLOAD, &count) ||
	    count != TBV_NATIVE_DATA_MAX_FRAGS)
		return 6;
	if (max_message_last_fragment(TBV_NATIVE_DATA_MAX_MSG_SIZE,
				      TBV_NATIVE_DATA_FRAME_SIZE, &count) ||
	    count != TBV_NATIVE_DATA_MAX_MSG_SIZE / TBV_NATIVE_DATA_FRAME_SIZE)
		return 7;
	if (max_message_last_fragment(gufo_prefill,
				      TBV_NATIVE_DATA_MAX_PAYLOAD, &count) ||
	    count != (gufo_prefill + TBV_NATIVE_DATA_MAX_PAYLOAD - 1u) /
			     TBV_NATIVE_DATA_MAX_PAYLOAD)
		return 8;

	return 0;
}

int main(void)
{
	unsigned char hello_buf[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	unsigned char data_buf[TBV_NATIVE_DATA_HDR_SIZE];
	struct tbv_native_wire_hello hello = {
		.capabilities = TBV_NATIVE_WIRE_CAP_UC |
				TBV_NATIVE_WIRE_CAP_RC |
				TBV_NATIVE_WIRE_CAP_MULTI_RAIL,
		.rail_id = 7,
		.route = 0x1122334455667788ull,
		.tx_hop = 8,
		.rx_hop = 9,
		.transmit_path = 10,
		.tx_ring_size = 128,
		.rx_ring_size = 256,
		.path_flags = TBV_NATIVE_WIRE_PATH_FRAME |
			      TBV_NATIVE_WIRE_PATH_E2E,
		.src_domain = { 0x3d, 0x23, 0x38, 0x04, 0x41, 0xe4, 0x73, 0x7a,
				0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
		.host_id = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
			     16 },
		.has_ids = 1,
	};
	struct tbv_native_wire_hello parsed_hello;
	struct tbv_native_wire_info info;
	struct tbv_native_data_header hdr = {
		.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM,
		.flags = TBV_NATIVE_DATA_F_LAST | TBV_NATIVE_DATA_F_SOLICITED,
		.dest_qp = 11,
		.src_qp = 12,
		.psn = 13,
		.length = 64,
		.imm_data = 0x58494f01u,
		.remote_addr = 0x8877665544332211ull,
		.rkey = 14,
		.frag_offset = 4096,
	};
	struct tbv_native_data_header parsed_hdr;
	int ret;

	ret = tbv_native_wire_build_hello(hello_buf, sizeof(hello_buf), &hello,
					  TBV_NATIVE_WIRE_OP_HELLO, 0, 123,
					  0xaabbccddeeff0011ull, 2);
	if (ret != (int)sizeof(hello_buf))
		return 1;

	memset(&parsed_hello, 0, sizeof(parsed_hello));
	memset(&info, 0, sizeof(info));
	ret = tbv_native_wire_parse_hello(hello_buf, sizeof(hello_buf),
					  &parsed_hello, &info);
	if (ret)
		return 2;
	if (!hello_equal(&hello, &parsed_hello) || !parsed_hello.has_ids)
		return 3;
	if (info.op != TBV_NATIVE_WIRE_OP_HELLO || info.seq != 123 ||
	    info.xdomain_sequence != 2)
		return 4;
	if (check_hello_v1(hello_buf, &hello))
		return 30;

	ret = tbv_native_data_build_header(data_buf, sizeof(data_buf), &hdr);
	if (ret != (int)sizeof(data_buf))
		return 5;

	memset(&parsed_hdr, 0, sizeof(parsed_hdr));
	ret = tbv_native_data_parse_header(data_buf, sizeof(data_buf),
					   &parsed_hdr);
	if (ret)
		return 6;
	if (!data_header_equal(&hdr, &parsed_hdr))
		return 7;

	if (tbv_native_data_credit_return_threshold(768) !=
	    TBV_NATIVE_DATA_CREDIT_BATCH)
		return 8;
	if (tbv_native_data_credit_return_threshold(16) != 16)
		return 9;
	if (tbv_native_data_start_credit_required(
		    native_data_frags(65536), 768) <= 15)
		return 10;
	if (tbv_native_data_start_credit_required(
		    native_data_frags(32768), 768) <= 7)
		return 11;
	if (tbv_native_data_start_credit_required(65, 768) !=
	    TBV_NATIVE_DATA_CREDIT_BATCH)
		return 12;
	if (tbv_native_data_start_credit_required(65, 16) != 16)
		return 13;
	if (test_raw_stream_payload_header())
		return 14;
	if (test_fragment_shapes())
		return 15;
	if (test_max_message_size())
		return 19;

	puts("protocol header smoke OK");
	return 0;
}
