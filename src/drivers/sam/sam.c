/**
 * @file sam.c
 *
 * Security accelerator module
 */

#include <mv_std.h>
#include <drivers/mv_sam.h>

#include "sam.h"

static struct sam_cio	*sam_ring;
static struct sam_sa	*sam_sessions;


static void sam_dmabuf_free(struct sam_dmabuf *dmabuf)
{
	if (dmabuf->size) {
		DMABuf_Release(dmabuf->hndl);
		dmabuf->size = 0;
	}
}

static int sam_dmabuf_alloc(u32 buf_size, struct sam_dmabuf *dmabuf)
{
	DMABuf_Status_t dma_status;
	DMABuf_Properties_t dma_properties = {0, 0, 0, 0};

	dma_properties.fCached   = true;
	dma_properties.Alignment = SAM_DMABUF_ALIGN;
	dma_properties.Size      = buf_size;
	if (buf_size == 256)
		dma_properties.Bank      = SAM_DMA_BANK_SA;
	else
		dma_properties.Bank      = SAM_DMA_BANK_PKT;

	dma_status = DMABuf_Alloc(dma_properties,
				&dmabuf->host_addr,
				&dmabuf->hndl);
	if (dma_status != DMABUF_STATUS_OK) {
		pr_err("Failed to allocate DMA buffer of %d bytes. error = %d\n",
			buf_size, dma_status);
		return -ENOMEM;
	}
	dmabuf->size = buf_size;

#ifdef SAM_DMA_DEBUG
	pr_dbg("allocate DMA buffer: vaddr = %p, handle = %p",
		dmabuf->host_addr.p, dmabuf->hndl.p);
#endif
	return 0;
}

static int sam_hw_engine_load(void)
{
	pr_info("Load SAM HW engine\n");

	if (Driver197_Init()) {
		pr_err("Can't init eip197 driver\n");
		return -ENODEV;
	}
	return 0;
}

static int sam_hw_engine_unload(void)
{
	pr_info("Unload SAM HW engine\n");
	Driver197_Exit();

	return 0;
}

static int sam_hw_ring_init(u32 ring)
{
	PEC_Status_t status;
	PEC_InitBlock_t init_block = {0, 0};
	u32 count = SAM_HW_RING_RETRY_COUNT;

	while (count > 0) {
		status = PEC_Init(ring, &init_block);
		if (status == PEC_STATUS_OK) {
			pr_info("EIP197 ring #%d loaded\n", ring);
			return 0;
		}

		if (status == PEC_STATUS_BUSY) {
			udelay(SAM_HW_RING_RETRY_US);
			count--;
			continue;
		}
		pr_err("Can't initialize ring #%d. error = %d\n", ring, status);
		return -ENODEV;
	} /* while */

	/* Timeout */
	pr_err("Can't initialize HW ring #%d. %d msec timeout expired\n",
		ring, SAM_HW_RING_RETRY_US * SAM_HW_RING_RETRY_COUNT / 1000);

	return -ENODEV;
}

static int sam_hw_ring_deinit(int ring)
{
	PEC_Status_t status;
	u32 count = SAM_HW_RING_RETRY_COUNT;

	while (count > 0) {
		status = PEC_UnInit(ring);
		if (status == PEC_STATUS_OK) {
			pr_info("EIP197 ring #%d unloaded\n", ring);
			return 0;
		}

		if (status == PEC_STATUS_BUSY) {
			udelay(SAM_HW_RING_RETRY_US);
			count--;
			continue;
		}
		pr_err("Can't un-initialize ring #%d. error = %d\n", ring, status);
		return -ENODEV;
	} /* while */

	/* Timeout */
	pr_err("Can't un-initialize ring #%d. %d msec timeout expired\n",
		ring, SAM_HW_RING_RETRY_US * SAM_HW_RING_RETRY_COUNT / 1000);

	return -ENODEV;
}

static struct sam_sa *sam_session_alloc(struct sam_cio *cio)
{
	int i;

	for (i = 0; i < cio->params.num_sessions; i++) {
		if (!sam_sessions[i].is_valid) {
			sam_sessions[i].is_valid = true;
			return &sam_sessions[i];
		}
	}
	pr_err("All sessions are busy\n");
	return NULL;
}

static void sam_session_free(struct sam_sa *sa)
{
	sa->is_valid = false;
}

static int sam_session_crypto_init(struct sam_session_params *params,
				   SABuilder_Params_Basic_t *basic_params,
				   SABuilder_Params_t *sa_params)
{
	if (params->cipher_alg == SAM_CIPHER_NONE)
		return 0;

	/* Check validity of crypto parameters */
	if (sam_max_check((int)params->cipher_alg, SAM_CIPHER_ALG_LAST, "cipher_alg"))
		return -EINVAL;

	if (sam_max_check((int)params->cipher_mode, SAM_CIPHER_MODE_LAST, "cipher_mode"))
		return -EINVAL;

	/* GCM and GMAC cipher modes are supported only for AES */
	if ((params->cipher_mode == SAM_CIPHER_GCM) || (params->cipher_mode == SAM_CIPHER_GMAC)) {
		if (params->cipher_alg != SAM_CIPHER_AES) {
			pr_err("GCM and GMAC cipher modes are supported only for AES, mode = %d, alg = %d\n",
				params->cipher_mode, params->cipher_alg);
			return -EINVAL;
		}
	}
	sa_params->CryptoAlgo = (SABuilder_Crypto_t)params->cipher_alg;
	sa_params->CryptoMode = (SABuilder_Crypto_Mode_t)params->cipher_mode;
	sa_params->KeyByteCount = params->cipher_key_len;
	sa_params->Key_p        = params->cipher_key;

	sa_params->IVSrc  = SAB_IV_SRC_TOKEN;

	/* Special processing for GCM/GMAC modes */

	return 0;
}

static int sam_session_auth_init(struct sam_session_params *params,
				 SABuilder_Params_Basic_t *basic_params,
				 SABuilder_Params_t *sa_params)
{
	if (params->auth_alg == SAM_AUTH_NONE)
		return 0;

	/* Check validity of crypto parameters */
	if (sam_max_check((int)params->auth_alg, SAM_AUTH_ALG_LAST, "auth_alg"))
		return -EINVAL;

	sa_params->AuthAlgo = (SABuilder_Auth_t)params->auth_alg;
	sa_params->AuthKey1_p   = params->auth_inner;
	sa_params->AuthKey2_p   = params->auth_outer;

	basic_params->ICVByteCount = params->auth_icv_len;
	if (params->dir == SAM_DIR_DECRYPT)
		basic_params->BasicFlags |= SAB_BASIC_FLAG_EXTRACT_ICV;

	return 0;
}

int sam_cio_init(struct sam_cio_params *params, struct sam_cio **cio)
{
	int i;

	/* Load SAM HW engine */
	sam_hw_engine_load();

	/* Only one instance of CIO is supported */
	if (sam_ring != NULL) {
		pr_err("sam_cio_init is already called\n");
		return -EEXIST;
	}
	/* Check valid range for CIO ring */
	if (params->id >= SAM_HW_RING_NUM) {
		pr_err("sam_cio ring #%d is out range: 0 ... %d\n",
			params->id, SAM_HW_RING_NUM);
		return -EINVAL;
	}
	/* Check ring size and number of sessions with HW max values */
	if (params->size > SAM_HW_RING_SIZE) {
		/* SW value can't exceed HW restriction */
		pr_warning("Warning! Ring size %d is too large. Set to maximum = %d\n",
			params->size, SAM_HW_RING_SIZE);
		params->size = SAM_HW_RING_SIZE;
	}

	if (params->num_sessions > SAM_HW_SA_NUM) {
		/* SW value can't exceed HW restriction */
		pr_warning("Warning! Number of sessions %d is too large. Set to maximum = %d\n",
			params->num_sessions, SAM_HW_SA_NUM);
		params->num_sessions = SAM_HW_SA_NUM;
	}

	/* Initialize HW ring */
	if (sam_hw_ring_init(params->id))
		goto err;

	/* Allocate single sam_cio structure */
	sam_ring = kcalloc(1, sizeof(struct sam_cio), GFP_KERNEL);
	if (!sam_ring) {
		pr_err("Can't allocate %lu bytes for sam_cio structure\n",
			 sizeof(struct sam_cio));
		goto err;
	}
	/* Save configured CIO params */
	sam_ring->params = *params;

	/* Allocate configured number of sam_sa structures */
	sam_sessions = kcalloc(params->num_sessions, sizeof(struct sam_cio), GFP_KERNEL);
	if (!sam_sessions) {
		pr_err("Can't allocate %u * %lu bytes for sam_sa structures\n",
			params->num_sessions, sizeof(struct sam_sa));
		goto err;
	}

	/* Allocate DMA buffer for each session */
	for (i = 0; i < params->num_sessions; i++) {
		if (sam_dmabuf_alloc(SAM_SA_DMABUF_SIZE, &sam_sessions[i].sa_dmabuf)) {
			pr_err("Can't allocate DMA buffer (%d bytes) for Session #%d\n",
				SAM_SA_DMABUF_SIZE, i);
			goto err;
		}
	}
	pr_info("DMA buffers allocated for %d sessions (%d bytes)\n",
		params->num_sessions, SAM_SA_DMABUF_SIZE);

	/* Allocate array of sam_cio_op structures in size of CIO ring */
	sam_ring->operations = kcalloc(params->size, sizeof(struct sam_cio_op), GFP_KERNEL);
	if (!sam_ring->operations) {
		pr_err("Can't allocate %u * %lu bytes for sam_cio_op structures\n",
			params->size, sizeof(struct sam_cio_op));
		goto err;
	}

	/* Allocate DMA buffers for Token and Data for each operation */
	for (i = 0; i < params->size; i++) {
		if (sam_dmabuf_alloc(SAM_TOKEN_DMABUF_SIZE, &sam_ring->operations[i].token_dmabuf)) {
			pr_err("Can't allocate DMA buffer (%d bytes) for Token #%d\n",
				SAM_TOKEN_DMABUF_SIZE, i);
			goto err;
		}

		if (sam_dmabuf_alloc(params->max_buf_size, &sam_ring->operations[i].data_dmabuf)) {
			pr_err("Can't allocate DMA buffer (%d bytes) for Buffer #%d\n",
				params->max_buf_size, i);
			goto err;
		}
	}
	pr_info("DMA buffers allocated for %d operations. Tokens - %d bytes, Buffers - %d bytes\n",
		i, SAM_TOKEN_DMABUF_SIZE, params->max_buf_size);

	*cio = sam_ring;

	return 0;

err:
	/* Release all allocated resources */
	sam_cio_deinit(sam_ring);

	return -ENOMEM;
}

int sam_cio_deinit(struct sam_cio *cio)
{
	int i;

	if (!cio)
		return 0;

	sam_hw_ring_deinit(cio->params.id);

	if (sam_sessions) {
		for (i = 0; i < cio->params.num_sessions; i++)
			sam_dmabuf_free(&sam_sessions[i].sa_dmabuf);

		kfree(sam_sessions);
		sam_sessions = NULL;
	}

	if (cio->operations) {
		for (i = 0; i < cio->params.size; i++) {
			sam_dmabuf_free(&cio->operations[i].token_dmabuf);
			sam_dmabuf_free(&cio->operations[i].data_dmabuf);
		}
		kfree(sam_ring->operations);
	}
	kfree(cio);
	sam_ring = NULL;

	sam_hw_engine_unload();

	return 0;
}

int sam_session_create(struct sam_cio *cio, struct sam_session_params *params, struct sam_sa **sa)
{
	SABuilder_Direction_t direction = (SABuilder_Direction_t)params->dir;
	struct sam_sa *session;
	int rc;

	/* Find free session structure */
	session = sam_session_alloc(cio);
	if (!session) {
		pr_err("%s: Can't get free session\n", __func__);
		return -EBUSY;
	}
	/* Clear session structure */
	memset(&session->sa_params, 0, sizeof(session->sa_params));
	memset(&session->basic_params, 0, sizeof(session->basic_params));

	/* Save session params */
	session->params = *params;

	/* Initialize sa_params and basic_params */
	SABuilder_Init_Basic(&session->sa_params, &session->basic_params, direction);

	/* Update sa_params and basic_params with session information */
	if (sam_session_crypto_init(params, &session->basic_params, &session->sa_params))
		goto error_session;

	if (sam_session_auth_init(params, &session->basic_params, &session->sa_params))
		goto error_session;

	/* Sanity check for SA and TCR size */
	rc = SABuilder_GetSizes(&session->sa_params, &session->sa_words, NULL, NULL);
	if (rc != 0) {
		pr_err("%s: SA not created because of error, rc = %d\n", __func__, rc);
		goto error_session;
	}
	if (session->sa_words > SAM_SA_DMABUF_SIZE / 4) {
		pr_err("%s: SA size %d words is too big. Maximum = %d words\n",
			__func__, session->sa_words, SAM_SA_DMABUF_SIZE / 4);
		goto error_session;
	}
	/* Clear DMA buffer allocated for SA */
	memset(session->sa_dmabuf.host_addr.p, 0, 4 * session->sa_words);

	rc = TokenBuilder_GetContextSize(&session->sa_params, &session->tcr_words);
	if (rc != 0) {
		pr_err("%s: TokenBuilder_GetContextSize return error, rc = %d\n", __func__, rc);
		goto error_session;
	}
	if (session->tcr_words > SAM_TCR_DATA_SIZE / 4) {
		pr_err("%s: TCR size %d words is too big. Maximum = %d words\n",
			__func__, session->tcr_words, SAM_TCR_DATA_SIZE / 4);
		goto error_session;
	}
	/* Clear TCR data buffer used allocated for SA */
	memset(session->tcr_data, 0, 4 * session->tcr_words);

	rc = TokenBuilder_BuildContext(&session->sa_params, session->tcr_data);
	if (rc != 0) {
		pr_err("%s: TokenBuilder_BuildContext failed, rc = %d\n", __func__, rc);
		goto error_session;
	}
	rc = TokenBuilder_GetSize(session->tcr_data, &session->token_words);
	if (rc != 0) {
		pr_err("%s:: TokenBuilder_GetSize failed.\n", __func__);
		goto error_session;
	}
	if (session->token_words > SAM_TOKEN_DMABUF_SIZE / 4) {
		pr_err("%s: Token size %d words is too big. Maximum = %d words\n",
			__func__, session->token_words, SAM_TOKEN_DMABUF_SIZE / 4);
		goto error_session;
	}

	/* build the SA data and init according the parameters */
	rc = SABuilder_BuildSA(&session->sa_params, (u32 *)session->sa_dmabuf.host_addr.p, NULL, NULL);
	if (rc != 0) {
		pr_err("%s: SABuilder_BuildSA failed, rx = %d\n", __func__, rc);
		goto error_session;
	}

	/* Register the SA. */
	rc = PEC_SA_Register(cio->params.id, session->sa_dmabuf.hndl, DMABuf_NULLHandle, DMABuf_NULLHandle);
	if (rc != PEC_STATUS_OK) {
		pr_err("%s: PEC_SA_Register failed.  rc = %d\n", __func__, rc);
		goto error_session;
	}

	*sa = session;

	return 0;

error_session:
	sam_session_free(session);
	return -EINVAL;
}

int sam_session_destroy(struct sam_sa *sa)
{
	return 0;
}

static int sam_cio_check_op_params(struct sam_cio_op_params *request)
{
	if (request->num_bufs != 1) {
		/* Multiple buffers not supported */
		return -ENOTSUP;
	}

	if (request->src[0] == NULL) {
		/* One source buffer is mandatory */
		return -ENOTSUP;
	}

	if (request->dst[0] == NULL)
		request->dst[0] = request->src[0];

	return 0;
}

int sam_cio_enq(struct sam_cio *cio, struct sam_cio_op_params *request)
{
	struct sam_sa *session = request->sa;

	/* Check request validity */
	if (sam_cio_check_op_params(request))
		return -EINVAL;

	/* Check maximum number of pending requests */
	if (sam_cio_is_full(cio)) {
		pr_warning("SAM cio %d is full\n", cio->params.id);
		return -EBUSY;
	}

	/* Prepare request for submit */

	/* submit request */

	return 0;
}

int sam_cio_deq(struct sam_cio *cio, struct sam_cio_op_result *result)
{
	return 0;
}

