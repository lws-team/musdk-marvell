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

#include <stdlib.h>
#include <string.h>

#include "lib/mv_aes.h"
#include "lib/mv_sha1.h"
#include "lib/mv_sha2.h"
#include "lib/mv_md5.h"
#include "mv_sam.h"
#include "fileSets.h"
#include "encryptedBlock.h"

#define NUM_CONCURRENT_SESSIONS		64
#define NUM_CONCURRENT_REQUESTS		32
#define MAX_BUFFER_SIZE			2048 /* bytes */
#define MAX_CIPHER_KEY_SIZE		32 /* 256 Bits = 32 Bytes */
#define MAX_CIPHER_BLOCK_SIZE		32 /* Bytes */
#define AUTH_BLOCK_SIZE_64B		64 /* Bytes */
#define MAX_AUTH_BLOCK_SIZE		128 /* Bytes */
#define MAX_AUTH_ICV_SIZE		64 /* Bytes */

static struct sam_cio		*cio_hndl;
static struct sam_sa		*sa_hndl[NUM_CONCURRENT_SESSIONS];
static struct sam_session_params sa_params[NUM_CONCURRENT_SESSIONS];

static struct sam_buf_info	in_buf;
static u32			in_data_size;
static struct sam_buf_info	out_bufs[NUM_CONCURRENT_REQUESTS];
static int			next_request;
static u8			cipher_iv[MAX_CIPHER_BLOCK_SIZE];
static u8			auth_icv[MAX_AUTH_ICV_SIZE];
static u32			auth_icv_size;
static u8			expected_data[MAX_BUFFER_SIZE];
static u32			expected_data_size;
static bool			same_bufs;
static int			num_to_print = 1;
static int			num_requests_per_enq = 1;
static int			num_requests_before_deq = NUM_CONCURRENT_REQUESTS;

static generic_list		test_db;

static enum sam_dir direction_str_to_val(char *data)
{
	/* Direction must be valid */
	if (!data || (data[0] == '\0')) {
		printf("Direction is not defined\n");
		return SAM_DIR_LAST;
	}
	if (strcmp(data, "encryption") == 0)
		return SAM_DIR_ENCRYPT;

	if (strcmp(data, "decryption") == 0)
		return SAM_DIR_DECRYPT;

	printf("Syntax error in Direction: %s is unknown\n", data);
	return SAM_DIR_LAST;
}

static enum sam_cipher_alg cipher_algorithm_str_to_val(char *data)
{
	if (!data || (data[0] == '\0'))
		return SAM_CIPHER_NONE;

	if (strcmp(data, "DES") == 0)
		return SAM_CIPHER_DES;

	if (strcmp(data, "3DES") == 0)
		return SAM_CIPHER_3DES;

	if (strcmp(data, "AES") == 0)
		return SAM_CIPHER_AES;

	if (strcmp(data, "NULL") == 0)
		return SAM_CIPHER_NONE;

	printf("Syntax error in Algorithm: %s is unknown\n", data);
	return SAM_CIPHER_NONE;
}

static enum sam_cipher_mode cipher_mode_str_to_val(char *data)
{
	if (!data || (data[0] == '\0'))
		return SAM_CIPHER_MODE_LAST;

	if (strcmp(data, "ECB") == 0)
		return SAM_CIPHER_ECB;

	if (strcmp(data, "CBC") == 0)
		return SAM_CIPHER_CBC;

	if (strcmp(data, "CTR") == 0)
		return SAM_CIPHER_CTR;

	if (strcmp(data, "GCM") == 0)
		return SAM_CIPHER_GCM;

	if (strcmp(data, "GMAC") == 0)
		return SAM_CIPHER_GMAC;

	printf("Syntax error in Mode: %s is unknown\n", data);
	return SAM_CIPHER_MODE_LAST;
}

static enum sam_auth_alg auth_algorithm_str_to_val(char *data, int auth_key_len)
{
	if (!data || (data[0] == '\0'))
		return SAM_AUTH_NONE;

	if (auth_key_len > 0) {
		if (strcmp(data, "MD5") == 0)
			return SAM_AUTH_HMAC_MD5;

		if (strcmp(data, "SHA1") == 0)
			return SAM_AUTH_HMAC_SHA1;

		if (strcmp(data, "SHA224") == 0)
			return SAM_AUTH_HMAC_SHA2_224;

		if (strcmp(data, "SHA256") == 0)
			return SAM_AUTH_HMAC_SHA2_256;

		if (strcmp(data, "SHA384") == 0)
			return SAM_AUTH_HMAC_SHA2_384;

		if (strcmp(data, "SHA512") == 0)
			return SAM_AUTH_HMAC_SHA2_512;
	} else {
		if (strcmp(data, "MD5") == 0)
			return SAM_AUTH_HASH_MD5;

		if (strcmp(data, "SHA1") == 0)
			return SAM_AUTH_HASH_SHA1;

		if (strcmp(data, "SHA224") == 0)
			return SAM_AUTH_HASH_SHA2_224;

		if (strcmp(data, "SHA256") == 0)
			return SAM_AUTH_HASH_SHA2_256;

		if (strcmp(data, "SHA384") == 0)
			return SAM_AUTH_HASH_SHA2_384;

		if (strcmp(data, "SHA512") == 0)
			return SAM_AUTH_HASH_SHA2_512;
	}
	if (strcmp(data, "NULL") == 0)
		return SAM_AUTH_NONE;

	printf("Syntax error in Auth algorithm: %s is unknown\n", data);
	return SAM_AUTH_NONE;
}

static void hmac_create_iv(enum sam_auth_alg auth_alg, unsigned char key[], int key_len,
			   unsigned char inner[], unsigned char outer[])
{
	unsigned char   in[MAX_AUTH_BLOCK_SIZE];
	unsigned char   out[MAX_AUTH_BLOCK_SIZE];
	int             i, max_key_len;

	max_key_len = AUTH_BLOCK_SIZE_64B;
	if (auth_alg == SAM_AUTH_HMAC_SHA2_384)
		max_key_len = SHA384_BLOCK_LENGTH;
	else if (auth_alg == SAM_AUTH_HMAC_SHA2_512)
		max_key_len = SHA512_BLOCK_LENGTH;

	for (i = 0 ; i < key_len ; i++) {
		in[i] = 0x36 ^ key[i];
		out[i] = 0x5c ^ key[i];
	}
	for (i = key_len ; i < max_key_len ; i++) {
		in[i] = 0x36;
		out[i] = 0x5c;
	}

	if (auth_alg == SAM_AUTH_HMAC_MD5) {
		MV_MD5_CONTEXT ctx;

		memset(&ctx, 0, sizeof(ctx));
		mv_md5_init(&ctx);
		mv_md5_update(&ctx, in, max_key_len);
		mv_md5_digest(inner, &ctx);

		memset(&ctx, 0, sizeof(ctx));
		mv_md5_init(&ctx);
		mv_md5_update(&ctx, out, max_key_len);
		mv_md5_digest(outer, &ctx);

	} else if (auth_alg == SAM_AUTH_HMAC_SHA1) {
		MV_SHA1_CTX ctx;

		memset(&ctx, 0, sizeof(ctx));
		mv_sha1_init(&ctx);
		mv_sha1_update(&ctx, in, max_key_len);
		for (i = 0; i < MV_SHA1_DIGEST_SIZE; i++) {
			inner[i] = (unsigned char)
				((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
		}

		memset(&ctx, 0, sizeof(ctx));
		mv_sha1_init(&ctx);
		mv_sha1_update(&ctx, out, max_key_len);
		for (i = 0; i < MV_SHA1_DIGEST_SIZE; i++) {
			outer[i] = (unsigned char)
				((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
	}

	} else if (auth_alg == SAM_AUTH_HMAC_SHA2_256) {
		SHA256_CTX ctx;

		memset(&ctx, 0, sizeof(ctx));
		mv_sha256_init(&ctx);
		mv_sha256_update(&ctx, in, max_key_len);
		mv_sha256_result_copy(&ctx, inner);

		memset(&ctx, 0, sizeof(ctx));
		mv_sha256_init(&ctx);
		mv_sha256_update(&ctx, out, max_key_len);
		mv_sha256_result_copy(&ctx, outer);

	} else if (auth_alg == SAM_AUTH_HMAC_SHA2_384) {
		SHA384_CTX context;

		memset(&context, 0, sizeof(context));
		mv_sha384_init(&context);
		mv_sha384_update(&context, in, max_key_len);
		mv_sha384_result_copy(&context, inner);

		memset(&context, 0, sizeof(context));
		mv_sha384_init(&context);
		mv_sha384_update(&context, out, max_key_len);
		mv_sha384_result_copy(&context, outer);

	} else if (auth_alg == SAM_AUTH_HMAC_SHA2_512) {
		SHA512_CTX context;

		memset(&context, 0, sizeof(context));
		mv_sha512_init(&context);
		mv_sha512_update(&context, in, max_key_len);
		mv_sha512_result_copy(&context, inner);

		memset(&context, 0, sizeof(context));
		mv_sha512_init(&context);
		mv_sha512_update(&context, out, max_key_len);
		mv_sha512_result_copy(&context, outer);
	} else {
		printf("\n%s: Unexpected authentication algorithm - %d\n",
			__func__, auth_alg);
	}
}

static void dump_buf(const unsigned char *p, unsigned int len)
{
	unsigned int i = 0, j;

	while (i < len) {
		j = 0;
		printf("%p: ", (p + i));
		for (j = 0 ; j < 32 && i < len ; j++) {
			printf("%02x ", p[i]);
			i++;
		}
		printf("\n");
	}
}

static int delete_sessions(void)
{
	int i, num = 0;

	for (i = 0; i < NUM_CONCURRENT_SESSIONS; i++) {
		if (sa_hndl[i]) {
			sam_session_destroy(sa_hndl[i]);
			num++;
		}
	}
	printf("%d sessions deleted\n", num);

	return 0;
}

static int create_sessions(generic_list tests_db)
{
	EncryptedBlockPtr block;
	int i, num_tests, auth_key_len;
	u8 cipher_key[MAX_CIPHER_KEY_SIZE];
	u8 auth_inner[MAX_AUTH_ICV_SIZE];
	u8 auth_outer[MAX_AUTH_ICV_SIZE];

	num_tests = generic_list_get_size(tests_db);
	if (num_tests > NUM_CONCURRENT_SESSIONS)
		num_tests = NUM_CONCURRENT_SESSIONS;

	block = generic_list_get_first(tests_db);
	for (i = 0; i < num_tests; i++) {

		memset(auth_inner, 0, sizeof(auth_inner));
		memset(auth_outer, 0, sizeof(auth_outer));

		if (i > 0)
			block = generic_list_get_next(tests_db);

		if (!block)
			return -1;

		sa_params[i].dir = direction_str_to_val(encryptedBlockGetDirection(block));

		sa_params[i].cipher_alg = cipher_algorithm_str_to_val(encryptedBlockGetAlgorithm(block));
		if (sa_params[i].cipher_alg != SAM_CIPHER_NONE) {
			sa_params[i].cipher_key_len = encryptedBlockGetKeyLen(block);
			if (sa_params[i].cipher_key_len > sizeof(cipher_key)) {
				printf("Cipher key size is too long: %d bytes > %d bytes\n",
					sa_params[i].cipher_key_len, (int)sizeof(cipher_key));
				return -1;
			}
			encryptedBlockGetKey(block, sa_params[i].cipher_key_len, cipher_key);

			sa_params[i].cipher_mode = cipher_mode_str_to_val(encryptedBlockGetMode(block));
			sa_params[i].cipher_iv = NULL;
			sa_params[i].cipher_key = cipher_key;
			/* encryptedBlockGetIvLen(block, idx); - Check IV length with cipher algorithm */
		}

		auth_key_len = encryptedBlockGetAuthKeyLen(block);
		sa_params[i].auth_alg = auth_algorithm_str_to_val(encryptedBlockGetAuthAlgorithm(block), auth_key_len);
		if (sa_params[i].auth_alg != SAM_AUTH_NONE) {
			sa_params[i].auth_icv_len = encryptedBlockGetIcbLen(block, 0);
			sa_params[i].auth_aad_len = 0;

			if (auth_key_len > 0) {
				u8 auth_key[MAX_AUTH_BLOCK_SIZE];

				/* Calculate inner and outer blocks from authentication key */
				if (auth_key_len > MAX_AUTH_BLOCK_SIZE) {
					printf("auth_key_len %d bytes is too big. Maximum is %d bytes\n",
						auth_key_len, MAX_AUTH_BLOCK_SIZE);
					return -EINVAL;
				}
				if (encryptedBlockGetAuthKey(block, auth_key_len, auth_key) != ENCRYPTEDBLOCK_SUCCESS) {
					printf("Can't get authentication key of %d bytes\n", auth_key_len);
					return -EINVAL;
				}
				hmac_create_iv(sa_params[i].auth_alg, auth_key, auth_key_len,
						auth_inner, auth_outer);

				sa_params[i].auth_inner = auth_inner;
				sa_params[i].auth_outer = auth_outer;
			}
		}

		if (sam_session_create(cio_hndl, &sa_params[i], &sa_hndl[i])) {
			printf("%s: failed\n", __func__);
			return -1;
		}
	}
	printf("%d of %d sessions created successfully\n", i, num_tests);

	return 0;
}

static int poll_results(struct sam_cio *cio, struct sam_cio_op_result *result,
			u16 *num)
{
	int rc;
	int count = 1000;

	while (count--) {
		rc = sam_cio_deq(cio, result, num);
		if (rc != -EBUSY)
			return rc;
	}
	/* Timeout */
	pr_err("%s: Timeout\n", __func__);
	return -EINVAL;
}

static int check_results(struct sam_session_params *session_params,
			struct sam_cio_op_result *result, u16 num)
{
	int i, errors = 0;
	u8 *out_data;

	for (i = 0; i < num; i++) {
		/* cookie is pointer to output buffer */
		out_data = result->cookie;
		if (!out_data) {
			pr_err("%s: Wrong cookie value: %p\n",
				__func__, out_data);
			return -EINVAL;
		}
		if (i < num_to_print) {
			printf("\nInput buffer: %d bytes\n", in_data_size);
			dump_buf(in_buf.vaddr, in_data_size);

			printf("\nOutput buffer: %d bytes\n", expected_data_size);
			dump_buf(out_data, expected_data_size);

			printf("\nExpected buffer: %d bytes\n", expected_data_size);
			dump_buf(expected_data, expected_data_size);

			if (auth_icv_size) {
				if (session_params->dir == SAM_DIR_ENCRYPT) {
					printf("\nICV output value: %d bytes\n", auth_icv_size);
					dump_buf(&out_data[expected_data_size], auth_icv_size);

					printf("\nICV expected value: %d bytes\n", auth_icv_size);
					dump_buf(auth_icv, auth_icv_size);
				} else
					printf("\nICV verified by HW\n");
			}
			printf("\n");
		}

		if (result->status != SAM_CIO_OK) {
			errors++;
			printf("Error: result->status = %d\n", result->status);
		} else if (memcmp(out_data, expected_data, expected_data_size)) {
			/* Compare output and expected data */
			errors++;
			printf("Error: out_data != expected_data (%d bytes)\n", expected_data_size);
		} else if ((auth_icv_size) && (session_params->dir == SAM_DIR_ENCRYPT)) {
			/* compare ICV */
			if (memcmp(auth_icv, &out_data[expected_data_size], auth_icv_size)) {
				errors++;
				printf("Error: out_icv != expected_icv (%d bytes)\n", auth_icv_size);
			}
		}
	}
	return errors;
}

static void free_bufs(void)
{
	int i;

	if (in_buf.vaddr)
		free(in_buf.vaddr);

	for (i = 0; i < NUM_CONCURRENT_REQUESTS; i++) {
		if (out_bufs[i].vaddr)
			free(out_bufs[i].vaddr);
	}
}


static int allocate_bufs(int buf_size)
{
	int i;

	in_buf.vaddr = malloc(buf_size);
	if (!in_buf.vaddr) {
		pr_err();
		return -ENOMEM;
	}
	in_buf.len = buf_size;

	for (i = 0; i < NUM_CONCURRENT_REQUESTS; i++) {
		out_bufs[i].vaddr = malloc(buf_size);
		if (!out_bufs[i].vaddr) {
			pr_err();
			return -ENOMEM;
		}
		out_bufs[i].len = buf_size;
	}
	return 0;
}

static void prepare_bufs(EncryptedBlockPtr block, struct sam_session_params *session_params, int num)
{
	int i;

	if (num > NUM_CONCURRENT_REQUESTS)
		num = NUM_CONCURRENT_REQUESTS;

	if (session_params->cipher_alg != SAM_CIPHER_NONE) {
		/* plain text and cipher text must be valid */
		if (session_params->dir == SAM_DIR_ENCRYPT) {
			expected_data_size = encryptedBlockGetCipherTextLen(block, 0);
			encryptedBlockGetCipherText(block, expected_data_size, expected_data, 0);

			in_data_size = encryptedBlockGetPlainTextLen(block, 0);
			encryptedBlockGetPlainText(block, in_data_size, in_buf.vaddr, 0);

			if (same_bufs) {
				for (i = 0; i < num; i++)
					encryptedBlockGetPlainText(block, in_data_size, out_bufs[i].vaddr, 0);
			}
		} else if (session_params->dir == SAM_DIR_DECRYPT) {
			expected_data_size = encryptedBlockGetPlainTextLen(block, 0);
			encryptedBlockGetPlainText(block, expected_data_size, expected_data, 0);

			in_data_size = encryptedBlockGetCipherTextLen(block, 0);
			encryptedBlockGetCipherText(block, in_data_size, in_buf.vaddr, 0);
			if (same_bufs) {
				for (i = 0; i < num; i++)
					encryptedBlockGetCipherText(block, in_data_size, out_bufs[i].vaddr, 0);
			}
		}
	} else if (session_params->auth_alg != SAM_AUTH_NONE) {
		/* Authentication only */
		in_data_size = encryptedBlockGetPlainTextLen(block, 0);
		encryptedBlockGetPlainText(block, in_data_size, in_buf.vaddr, 0);

		/* Data must left the same */
		expected_data_size = in_data_size;
		encryptedBlockGetPlainText(block, expected_data_size, expected_data, 0);
	} else {
		/* Nothing to do */
		printf("Warning: cipher_alg and auth_alg are NONE both\n");
	}

	if (session_params->auth_icv_len > 0) {
		auth_icv_size = session_params->auth_icv_len;
		encryptedBlockGetIcb(block, session_params->auth_icv_len, auth_icv, 0);
		if (session_params->dir == SAM_DIR_DECRYPT) {
			/* copy ICV to end of input buffer */
			memcpy((in_buf.vaddr + in_data_size), auth_icv, session_params->auth_icv_len);
		}
	} else
		auth_icv_size = 0;

}

static void prepare_requests(EncryptedBlockPtr block, struct sam_session_params *session_params,
			     struct sam_sa *session_hndl, struct sam_cio_op_params *request, int num)
{
	int i, iv_len;

	for (i = 0; i < num; i++) {
		request->sa = session_hndl;
		request->num_bufs = 1;

		if (session_params->cipher_alg != SAM_CIPHER_NONE) {
			request->cipher_iv_offset = 0; /* not supported */
			request->cipher_offset = encryptedBlockGetCryptoOffset(block, 0);
			request->cipher_len = encryptedBlockGetPlainTextLen(block, 0) - request->cipher_offset;

			iv_len = encryptedBlockGetIvLen(block, 0);
			if (iv_len) {
				encryptedBlockGetIv(block, iv_len, cipher_iv, 0);
				request->cipher_iv = cipher_iv;
			}
		}
		if (session_params->auth_alg != SAM_AUTH_NONE) {
			request->auth_aad_offset = 0; /* not supported */
			request->auth_aad = NULL;
			request->auth_offset = 0;
			request->auth_len = encryptedBlockGetPlainTextLen(block, 0);
			request->auth_icv_offset = request->auth_len;
		}
		request++;
	}
};

/* There are few parameters can be configured:
 * - number of requests per enqueue/dequeue call: [1..NUM_CONCURRENT_REQUESTS] [default = 1]
 * - src/dst are different/same buffers [default: src != dst]
 */
static int run_tests(generic_list tests_db)
{
	EncryptedBlockPtr block;
	int i, num_tests, total_enqs, total_deqs, in_process, to_enq;
	u16 num, to_deq = 0;
	char *test_name;
	struct sam_cio_op_params request;
	struct sam_cio_op_result results[NUM_CONCURRENT_REQUESTS];
	int rc, count, total_passed, total_errors, errors;

	num_tests = generic_list_get_size(tests_db);

	block = generic_list_get_first(tests_db);
	num = 1;
	total_passed = total_errors = 0;
	for (i = 0; i < num_tests; i++) {

		if (i > 0)
			block = generic_list_get_next(tests_db);

		if (!block)
			return -1;

		test_name = encryptedBlockGetName(block);

		count = total_enqs = total_deqs = encryptedBlockGetTestCounter(block);

		prepare_bufs(block, &sa_params[i], count);

		memset(&request, 0, sizeof(request));
		prepare_requests(block, &sa_params[i], sa_hndl[i], &request, num_requests_per_enq);

		/* Check plain_len == cipher_len */
		errors = 0;
		in_process = 0;
		while (total_deqs) {
			to_enq = min(total_enqs, num_requests_before_deq);
			while (in_process < to_enq) {
				if (same_bufs) {
					/* Input buffers are different pre request */
					request.src = request.dst = &out_bufs[next_request];
				} else {
					/* Output buffers are different per request */
					request.src = &in_buf;
					request.dst = &out_bufs[next_request];
				}
				request.cookie = request.dst->vaddr;

				/* Increment next_request */
				next_request++;
				if (next_request == NUM_CONCURRENT_REQUESTS)
					next_request = 0;

				num = (u16)num_requests_per_enq;
				rc = sam_cio_enq(cio_hndl, &request, &num);
				if ((rc != 0) || (num != num_requests_per_enq)) {
					printf("%s: sam_cio_enq failed. num = %d, rc = %d\n",
						__func__, num, rc);
					return -1;
				}
				in_process += num;
				total_enqs -= num;
			}

			/* Get all ready results together */
			to_deq = in_process;
			rc = poll_results(cio_hndl, results, &to_deq);
			in_process -= to_deq;
			total_deqs -= to_deq;
			/* check result */
			errors += check_results(&sa_params[i], results, to_deq);
		}
		if (errors == 0)
			printf("%2d. %s: passed %d times\n", i, test_name, count);
		else
			printf("%2d. %s: failed %d of %d times\n", i, test_name, errors, count);

		total_errors += errors;
		total_passed += (count - errors);
	}
	printf("\n");
	printf("SAM tests passed:   %d\n", total_passed);
	printf("SAM tests failed:   %d\n", total_errors);
	printf("\n");

	return 0;
}

int main(int argc, char **argv)
{
	struct sam_cio_params cio_params;
	char *tf_name;

	if (argc < 2) {
		printf("\nConfiguration file needed\n");
		return -1;
	}

	tf_name = argv[1];
	printf("%s: tests file is %s\n", argv[0], tf_name);

	test_db = generic_list_create(fileSetsEncryptedBlockCopyForList,
				      fileSetsEncryptedBlockDestroyForList);
	if (test_db == NULL) {
		printf("generic_list_create failed\n");
		return -1;
	}

	if (fileSetsReadBlocksFromFile(tf_name, test_db) == FILE_OPEN_PROBLEM) {
		printf("Can't read tests from file %s\n", tf_name);
		return -1;
	}
	printf("%d tests read from file %s\n", generic_list_get_size(test_db), tf_name);

	cio_params.id = 0;
	cio_params.size = NUM_CONCURRENT_REQUESTS;
	cio_params.num_sessions = NUM_CONCURRENT_SESSIONS;
	cio_params.max_buf_size = MAX_BUFFER_SIZE;

	if (sam_cio_init(&cio_params, &cio_hndl)) {
		printf("%s: initialization failed\n", argv[0]);
		return 1;
	}
	printf("%s successfully loaded\n", argv[0]);

	/* allocate in_buf and out_bufs */
	if (allocate_bufs(cio_params.max_buf_size))
		goto exit;

	if (create_sessions(test_db))
		goto exit;

	if (run_tests(test_db))
		goto exit;

exit:
	delete_sessions();

	free_bufs();

	if (sam_cio_deinit(cio_hndl)) {
		printf("%s: un-initialization failed\n", argv[0]);
		return 1;
	}
	printf("%s successfully unloaded\n", argv[0]);

	return 0;
}