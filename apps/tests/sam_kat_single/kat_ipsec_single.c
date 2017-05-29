/******************************************************************************
 *	Copyright (C) 2016 Marvell International Ltd.
 *
 *  If you received this File from Marvell, you may opt to use, redistribute
 *  and/or modify this File under the following licensing terms.
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *
 *	* Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 *
 *	* Neither the name of Marvell nor the names of its contributors may be
 *	  used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include "mv_std.h"
#include "env/mv_sys_dma.h"
#include "lib/lib_misc.h"
#include "lib/net.h"
#include "utils.h"

#include "mv_sam.h"

#define SAM_DMA_MEM_SIZE	(1 * 1024 * 1204) /* 1 MBytes */

#define MAX_BUF_SIZE		2048

static struct sam_cio *cio_hndl;
static struct sam_cio *cio_hndl_1;

static struct sam_sa  *sa_hndl;
static struct sam_sa  *sa_hndl_1;

static char *test_names[] = {
	/* 0 */ "ip4_transport_aes_cbc",
	/* 1 */ "ip4_transport_aes_cbc_sha1",
	/* 2 */ "ip4_tunnel_aes_cbc",
	/* 3 */ "ip4_tunnel_aes_cbc_sha1",
};

static int test_id;
static int num_pkts = 1;
static u32 debug_flags = 0x3;

static u8 IPSEC_ESP_L2_HEADER[] = {
	0x00, 0x01, 0x01, 0x01, 0x55, 0x66,
	0x00, 0x02, 0x02, 0x02, 0x77, 0x88,
	0x08, 0x00
};

static u8 IPSEC_ESP_IP4_HEADER[] = {
	0x45, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x00, 0x00,
	0x40, 0x11, 0x00, 0x00,
	0x50, 0x00, 0x00, 0x0A,
	0x3C, 0x00, 0x00, 0x0A
};

static u8 IPSEC_ESP_UDP_HEADER[] = {
	0x00, 0x63, 0x00, 0x64,
	0x00, 0x00, 0x00, 0x00
};


static u8 ExampleAESKey[] = {
	0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50
};

static u8 ExampleHmacKey[] = {
	0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
	0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
	0x0b, 0x0b, 0x0b, 0x0b
};

static u8 IPSEC_ESP_AES128_CBC_T1_PT[] = { /* "Single block msg" */
	0x53, 0x69, 0x6E, 0x67, 0x6C, 0x65, 0x20, 0x62,
	0x6C, 0x6F, 0x63, 0x6B, 0x20, 0x6D, 0x73, 0x67
};

static u8 SrcIP4[] = {192, 168, 2, 3};
static u8 DstIP4[] = {192, 168, 2, 5};

static u8 SrcIP6[] = {
	0x20, 0x00, 0x1f, 0x3c, 0x55, 0x18, 0x00, 0x00,
	0x00, 0x00, 0x1f, 0x33, 0x44, 0x55, 0x66, 0x77
};
static u8 DstIP6[] = {
	0x20, 0x00, 0x1f, 0x3c, 0x55, 0x18, 0x00, 0x00,
	0x00, 0x00, 0x1f, 0x33, 0x44, 0x55, 0x66, 0x88
};

static struct sam_buf_info aes128_t1_buf;
static u8 expected_data[MAX_BUF_SIZE];
static u32 expected_data_size;

static struct sam_session_params aes_cbc_sa = {
	.dir = SAM_DIR_ENCRYPT,   /* operation direction: encode/decode */
	.cipher_alg = SAM_CIPHER_AES,  /* cipher algorithm */
	.cipher_mode = SAM_CIPHER_CBC, /* cipher mode */
	.cipher_key = ExampleAESKey,    /* cipher key */
	.cipher_key_len = sizeof(ExampleAESKey), /* cipher key size (in bytes) */
	.auth_alg = SAM_AUTH_NONE, /* authentication algorithm */
	.auth_key = NULL,    /* pointer to authentication key */
	.auth_key_len = 0,    /* authentication key size (in bytes) */
	.proto = SAM_PROTO_IPSEC,
	.u.ipsec.is_esp = 1,
	.u.ipsec.is_ip6 = 0,
	.u.ipsec.is_tunnel = 0,
	.u.ipsec.is_natt = 0,
	.u.ipsec.spi = 0xABCD,
	.u.ipsec.seq = 0x4,
};

static struct sam_session_params aes_cbc_sha1_sa = {
	.dir = SAM_DIR_ENCRYPT,   /* operation direction: encode/decode */
	.cipher_alg = SAM_CIPHER_AES,  /* cipher algorithm */
	.cipher_mode = SAM_CIPHER_CBC, /* cipher mode */
	.cipher_key = ExampleAESKey,    /* cipher key */
	.cipher_key_len = sizeof(ExampleAESKey), /* cipher key size (in bytes) */
	.auth_alg = SAM_AUTH_HMAC_SHA1, /* authentication algorithm */
	.auth_key = ExampleHmacKey,    /* pointer to authentication key */
	.auth_key_len = sizeof(ExampleHmacKey),    /* authentication key size (in bytes) */
	.proto = SAM_PROTO_IPSEC,
	.u.ipsec.is_esp = 1,
	.u.ipsec.is_ip6 = 0,
	.u.ipsec.is_tunnel = 0,
	.u.ipsec.is_natt = 0,
	.u.ipsec.spi = 0x1234,
	.u.ipsec.seq = 0x6,
};

static struct sam_cio_ipsec_params aes128_cbc_t1 = {
	.sa = NULL,
	.cookie = (void *)0x12345678,
	.num_bufs = 1,
	.src = &aes128_t1_buf,
	.dst = &aes128_t1_buf,
	/* all auth fields are zero */
};

static int create_session(struct sam_sa **hndl, const char *name, enum sam_dir dir)
{
	int rc;
	struct sam_session_params *sa_params;

	if (!strcmp(name, "ip4_transport_aes_cbc")) {
		sa_params = &aes_cbc_sa;
		sa_params->u.ipsec.is_tunnel = 0;
	} else if (!strcmp(name, "ip4_transport_aes_cbc_sha1")) {
		sa_params = &aes_cbc_sha1_sa;
		sa_params->u.ipsec.is_tunnel = 0;
	} else if (!strcmp(name, "ip4_tunnel_aes_cbc")) {
		sa_params = &aes_cbc_sa;
		sa_params->u.ipsec.is_tunnel = 1;
	} else if (!strcmp(name, "ip4_tunnel_aes_cbc_sha1")) {
		sa_params = &aes_cbc_sha1_sa;
		sa_params->u.ipsec.is_tunnel = 1;
	} else {
		printf("%s: unknown session name - %s\n", __func__, name);
		return -EINVAL;
	}
	sa_params->dir = dir;

	if (sa_params->u.ipsec.is_tunnel) {
		if (sa_params->u.ipsec.is_ip6) {
			sa_params->u.ipsec.tunnel.u.ipv6.sip = SrcIP6;
			sa_params->u.ipsec.tunnel.u.ipv6.dip = DstIP6;
			sa_params->u.ipsec.tunnel.u.ipv6.dscp = 0;
			sa_params->u.ipsec.tunnel.u.ipv6.hlimit = 64;
			sa_params->u.ipsec.tunnel.u.ipv6.flabel = 0;

		} else {
			sa_params->u.ipsec.tunnel.u.ipv4.sip = SrcIP4;
			sa_params->u.ipsec.tunnel.u.ipv4.dip = DstIP4;
			sa_params->u.ipsec.tunnel.u.ipv4.dscp = 0;
			sa_params->u.ipsec.tunnel.u.ipv4.ttl = 64;
			sa_params->u.ipsec.tunnel.u.ipv4.df = 0;
		}
		sa_params->u.ipsec.tunnel.copy_flabel = 0;
		sa_params->u.ipsec.tunnel.copy_dscp = 0;
		sa_params->u.ipsec.tunnel.copy_df = 0;
	}
	rc = sam_session_create(sa_params, hndl);
	if (rc) {
		printf("%s: failed - rc = %d\n", __func__, rc);
		return rc;
	}
	return 0;
}

static int poll_results(struct sam_cio *cio, struct sam_cio_op_result *result,
			u16 *num)
{
	int rc;
	int count = 1000;
	u16 to_deq = *num;

	while (count--) {
		rc = sam_cio_deq(cio, result, num);
		if (rc) {
			printf("%s: sam_cio_deq failed. num = %d, rc = %d\n",
					__func__, *num, rc);
			return rc;
		}
		if (*num)
			return 0;

		*num = to_deq;
	}
	/* Timeout */
	pr_err("%s: Timeout\n", __func__);
	return -EINVAL;
}

/* Build UDP packet and return total packet length */
static int build_udp_pkt_for_encrypt(struct sam_buf_info *buf_info)
{
	int offset, l3_offset, l4_offset;
	u8 *src_buf = buf_info->vaddr;
	struct iphdr *iph;
	struct udphdr *udph;

	offset = 0;
	memcpy(src_buf + offset, IPSEC_ESP_L2_HEADER, sizeof(IPSEC_ESP_L2_HEADER));
	offset = sizeof(IPSEC_ESP_L2_HEADER);
	l3_offset = offset;

	memcpy(src_buf + offset, IPSEC_ESP_IP4_HEADER, sizeof(IPSEC_ESP_IP4_HEADER));
	offset += sizeof(IPSEC_ESP_IP4_HEADER);
	l4_offset = offset;

	memcpy(src_buf + offset, IPSEC_ESP_UDP_HEADER, sizeof(IPSEC_ESP_UDP_HEADER));
	offset += sizeof(IPSEC_ESP_UDP_HEADER);

	memcpy(src_buf + offset, IPSEC_ESP_AES128_CBC_T1_PT, sizeof(IPSEC_ESP_AES128_CBC_T1_PT));
	offset += sizeof(IPSEC_ESP_AES128_CBC_T1_PT);

	iph = (struct iphdr *)(src_buf + l3_offset);

	/* Set IP length */
	iph->tot_len = htobe16(offset - l3_offset);

	/* Set IP checksum */
	iph->check = 0;
	iph->check = mv_ip4_csum((u16 *)iph, iph->ihl);

	udph = (struct udphdr *)(src_buf + l4_offset);

	/* Set UDP payload length */
	udph->len = htobe16(offset - l4_offset);

	/* Set UDP checksum */
	/* udph->check =  */

	/* Save buffer for compare after loopback */
	expected_data_size = offset;
	memcpy(expected_data, src_buf, offset);

	return offset;
}

static int check_udp_pkt_after_encrypt(struct sam_buf_info *buf_info, struct sam_cio_op_result *result)
{
	if (result->out_len > buf_info->len) {
		printf("%s: result->out_len  = %u > buf_info->len = %u\n",
			__func__, result->out_len, buf_info->len);
		return 1;
	}
	printf("\nAfter encryption: %d bytes\n", result->out_len);
	mv_mem_dump(buf_info->vaddr, result->out_len);

	if (result->out_len == 0)
		return 1;

	return 0;
}

static int check_udp_pkt_after_decrypt(struct sam_buf_info *buf_info, struct sam_cio_op_result *result)
{
	int err = 0;

	if (result->out_len > buf_info->len) {
		printf("%s: result->out_len  = %u > buf_info->len = %u\n",
			__func__, result->out_len, buf_info->len);
		return 1;
	}
	printf("\nAfter decryption: %d bytes\n", result->out_len);
	mv_mem_dump(buf_info->vaddr, result->out_len);

	printf("\nExpected data: %d bytes\n", expected_data_size);
	mv_mem_dump(expected_data, expected_data_size);

	/* Compare output and expected data */
	if (result->out_len != expected_data_size) {
		printf("Error: out_len = %u != expected_data_size = %u\n",
			result->out_len, expected_data_size);
		err = 1;
	} else if (memcmp(buf_info->vaddr, expected_data, result->out_len)) {
		printf("Error: out_data != expected_data\n");
		err = 1;
	}

	printf("\n");
	printf("Loopback test: %s\n", err ? "failed" : "success");
	printf("\n");

	return 0;
}

static void usage(char *progname)
{
	printf("Usage: %s [OPTIONS]\n", MVAPPS_NO_PATH(progname));
	printf("OPTIONS are optional:\n");
	printf("\t-t <number>      - Test ID (default: %d)\n", test_id);
	printf("\t-n <number>      - Number of packets (default: %d)\n", num_pkts);
	printf("\t-f <bitmask>     - Debug flags: 0x%x - SA, 0x%x - CIO. (default: 0x%x)\n",
					SAM_SA_DEBUG_FLAG, SAM_CIO_DEBUG_FLAG, debug_flags);
}

static int parse_args(int argc, char *argv[])
{
	int i = 1;

	while (i < argc) {
		if ((strcmp(argv[i], "?") == 0) ||
		    (strcmp(argv[i], "-h") == 0) ||
		    (strcmp(argv[i], "--help") == 0)) {
			usage(argv[0]);
			exit(0);
		}
		if (strcmp(argv[i], "-t") == 0) {
			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			test_id = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-n") == 0) {
			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			num_pkts = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-f") == 0) {
			int scanned;

			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			scanned = sscanf(argv[i + 1], "0x%x", &debug_flags);
			if (scanned != 1) {
				pr_err("Invalid number if scanned arguments: %d != 1\n",
					scanned);
				return -EINVAL;
			}
			i += 2;
		} else {
			pr_err("argument (%s) not supported!\n", argv[i]);
			return -EINVAL;
		}
	}
	if (test_id >= ARRAY_SIZE(test_names)) {
		pr_err("test_id (%d) is out of range [0 .. %d]\n",
			test_id, (unsigned)ARRAY_SIZE(test_names));

		return -EINVAL;
	}

	/* Print all inputs arguments */
	printf("Test ID     : %d\n", test_id);
	printf("Test name   : %s\n", test_names[test_id]);
	printf("Debug flags : 0x%x\n", debug_flags);


	return 0;
}

int main(int argc, char **argv)
{
	struct sam_init_params init_params;
	struct sam_cio_params cio_params;
	struct sam_cio_op_result result;
	struct sam_cio_stats cio_stats;
	struct sam_session_stats sa_stats;
	u16 num;
	int rc = 0;
	int input_size, i;

	rc = parse_args(argc, argv);
	if (rc)
		return rc;

	rc = mv_sys_dma_mem_init(SAM_DMA_MEM_SIZE);
	if (rc) {
		pr_err("Can't initialize %d KBytes of DMA memory area, rc = %d\n", SAM_DMA_MEM_SIZE, rc);
		return rc;
	}

	aes128_t1_buf.len = MAX_BUF_SIZE;
	aes128_t1_buf.vaddr = mv_sys_dma_mem_alloc(aes128_t1_buf.len, 16);
	aes128_t1_buf.paddr = mv_sys_dma_mem_virt2phys(aes128_t1_buf.vaddr);

	if (!aes128_t1_buf.vaddr || !aes128_t1_buf.paddr) {
		pr_err("Can't allocate DMA buffer of %d bytes\n", aes128_t1_buf.len);
		goto exit;
	}

	pr_info("DMA Buffer %d bytes allocated: vaddr = %p, paddr = %p\n",
		aes128_t1_buf.len, aes128_t1_buf.vaddr, (void *)aes128_t1_buf.paddr);

	init_params.max_num_sessions = 64;
	sam_init(&init_params);

	cio_params.match = "cio-0:0";
	cio_params.size = 32;

	if (sam_cio_init(&cio_params, &cio_hndl)) {
		printf("%s: initialization failed\n", argv[0]);
		return 1;
	}
	sam_set_debug_flags(debug_flags);

	cio_params.match = "cio-0:1";
	cio_params.size = 32;

	if (sam_cio_init(&cio_params, &cio_hndl_1)) {
		printf("%s: initialization failed\n", argv[0]);
		return 1;
	}
	printf("%s successfully loaded\n", argv[0]);
	sam_set_debug_flags(debug_flags);

	if (create_session(&sa_hndl, test_names[test_id], SAM_DIR_ENCRYPT))
		goto exit;

	pr_info("%s encrypt session successfully created on %s\n", test_names[test_id], "cio-0:0");

	if (create_session(&sa_hndl_1, test_names[test_id], SAM_DIR_DECRYPT))
		goto exit;

	pr_info("%s decrypt session successfully created on %s\n", test_names[test_id], "cio-0:1");

	memset(aes128_t1_buf.vaddr, 0, aes128_t1_buf.len);

	/* Build input packet for encryption */
	input_size = build_udp_pkt_for_encrypt(&aes128_t1_buf);

	printf("\nInput buffer    : %d bytes\n", input_size);
	mv_mem_dump(aes128_t1_buf.vaddr, input_size);

	for (i = 0; i < num_pkts; i++) {
		/* Do encrypt */
		num = 1;
		aes128_cbc_t1.sa = sa_hndl;
		aes128_cbc_t1.l3_offset = sizeof(IPSEC_ESP_L2_HEADER);
		aes128_cbc_t1.pkt_size = input_size;

		rc = sam_cio_enq_ipsec(cio_hndl, &aes128_cbc_t1, &num);
		if ((rc != 0) || (num != 1)) {
			printf("%s: sam_cio_enq failed. num = %d, rc = %d\n",
				__func__, num, rc);
			goto exit;
		}
		/* polling for result */
		rc = poll_results(cio_hndl, &result, &num);
		if ((rc != 0) || (num != 1)) {
			pr_err("No result: rc = %d, num = %d\n", rc, num);
			goto exit;
		}
		if (check_udp_pkt_after_encrypt(&aes128_t1_buf, &result))
			goto exit;

		/* Do decrypt */
		num = 1;
		aes128_cbc_t1.sa = sa_hndl_1;
		aes128_cbc_t1.pkt_size = result.out_len;

		rc = sam_cio_enq_ipsec(cio_hndl_1, &aes128_cbc_t1, &num);
		if ((rc != 0) || (num != 1)) {
			printf("%s: sam_cio_enq failed. num = %d, rc = %d\n",
				__func__, num, rc);
			goto exit;
		}
		/* polling for result */
		rc = poll_results(cio_hndl_1, &result, &num);
		if ((rc != 0) || (num != 1)) {
			pr_err("No result: rc = %d, num = %d\n", rc, num);
			goto exit;
		}
		if (check_udp_pkt_after_decrypt(&aes128_t1_buf, &result))
			goto exit;
	}
exit:
	if (sa_hndl) {
		if (sam_session_destroy(sa_hndl))
			printf("Can't destroy sa_hndl session");
	}
	if (sam_cio_flush(cio_hndl))
		printf("%s: sam_cio_flush failed for cio_hndl\n", argv[0]);

	if (sa_hndl_1) {
		if (sam_session_destroy(sa_hndl_1))
			printf("Can't destroy sa_hndl_1 session");
	}
	if (sam_cio_flush(cio_hndl_1))
		printf("%s: sam_cio_flush failed for cio_hndl_1\n", argv[0]);

	if (aes128_t1_buf.vaddr)
		mv_sys_dma_mem_free(aes128_t1_buf.vaddr);

	if (!sam_cio_get_stats(cio_hndl, &cio_stats, true)) {
		printf("------- cio_hndl Statistics -------\n");
		printf("Enqueue packets             : %lu packets\n", cio_stats.enq_pkts);
		printf("Enqueue bytes               : %lu bytes\n", cio_stats.enq_bytes);
		printf("Enqueue full                : %lu times\n", cio_stats.enq_full);
		printf("Dequeue packets             : %lu packets\n", cio_stats.deq_pkts);
		printf("Dequeue bytes               : %lu bytes\n", cio_stats.deq_bytes);
		printf("Dequeue empty               : %lu times\n", cio_stats.deq_empty);
	}

	if (!sam_cio_get_stats(cio_hndl_1, &cio_stats, true)) {
		printf("------- cio_hndl_1 Statistics -------\n");
		printf("Enqueue packets             : %lu packets\n", cio_stats.enq_pkts);
		printf("Enqueue bytes               : %lu bytes\n", cio_stats.enq_bytes);
		printf("Enqueue full                : %lu times\n", cio_stats.enq_full);
		printf("Dequeue packets             : %lu packets\n", cio_stats.deq_pkts);
		printf("Dequeue bytes               : %lu bytes\n", cio_stats.deq_bytes);
		printf("Dequeue empty               : %lu times\n", cio_stats.deq_empty);
	}

	if (!sam_session_get_stats(&sa_stats, true)) {
		printf("------- Session Statistics -------\n");
		printf("Created sessions            : %lu\n", sa_stats.sa_add);
		printf("Deleted sessions:	    : %lu\n", sa_stats.sa_del);
		printf("Invalidated sessions:	    : %lu\n", sa_stats.sa_inv);
	}
	if (cio_hndl) {
		if (sam_cio_deinit(cio_hndl)) {
			printf("%s: un-initialization failed\n", argv[0]);
			return 1;
		}
	}

	if (cio_hndl_1) {
		if (sam_cio_deinit(cio_hndl_1)) {
			printf("%s: un-initialization failed\n", argv[0]);
			return 1;
		}
	}
	sam_deinit();

	printf("%s successfully unloaded\n", argv[0]);

	return 0;
}
