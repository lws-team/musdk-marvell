/**
 * @file pp2_bm.h
 *
 * Packet Processor Buffer Manager
 *
 * Presentation API for PPDK hardware managed memory resources
 */

#ifndef _PP2_BM_H_
#define _PP2_BM_H_

#define BM_TYPE_SHORT_BUF_POOL (0x00)
#define BM_TYPE_LONG_BUF_POOL  (0x01)

/* Number of buffer references per BPPE unit. Client should
 * request a number of buffers divisible to this this value
 */
#define PP2_BPPE_UNIT_SIZE           (8)

/* Forward declarations for structures required by BM internals */
struct pp2;
struct pp2_bm_pool;
struct pp2_port;

/**
 * pp2_bm_if_param
 *
 * Buffer Manager interface object parameters
 *
 * @field	id    BM-IF ID. This ID should be assigned uniquely per
 *                BM software requestor.
 *
 *                Valid range: [0 - PP2_TOTAL_NUM_REGSPACES]
 *                See <pp2_plat.h>.
 */
struct pp2_bm_if_param {
	uint32_t id;
};

/**
 * bm_pool_param
 *
 * Buffer Manager pool parameters
 *
 * @field	id        BM Pool unique logical ID
 *
 *                    Valid range: [0 - PP2_TOTAL_NUM_BMPOOLS]
 *                    See <pp2_plat.h>
 *
 * @field	buf_num   Number of buffers that shall be managed
 *                    by BM for this buffer pool. This value should be
 *                    a multiple of PP2_BPPE_UNIT_SIZE
 *
 * @field   buf_size  Size of each buffer that resides
 *                    in this BM Pool
 */
struct bm_pool_param {
	uint32_t id;
	uint32_t pp2_id;
	uint32_t buf_num;
	uint32_t buf_size;
};

/**
 * pp2_bm_if_init
 *
 * Initialize (create) and prepare an exclusive (lockless)
 * BM-IF object for accessing BM resources
 *
 * @param ppdk       The PPDK handle. Based on the param input
 *                   the corresponding packet processor shall
 *                   be used
 * @param param      Parameters for this BM-IF object
 *
 * @retval           BM-IF object handle on success, NULL otherwise
 */
struct pp2_bm_if *pp2_bm_if_init(struct pp2 *ppdk, struct pp2_bm_if_param *param);

/**
 * pp2_bm_pool_create
 *
 * Create and associate a BM pool to the BM object provided as
 * input. Multiple BM objects can share the same BM pool.
 * The BM pool is not usable after this routines returns
 *
 * @param bm_if      BM-IF object handle
 * @param param      BM Pool parameters
 *
 * @retval           BM Pool handle on success, NULL otherwise
 */
int pp2_bm_pool_create(struct pp2* pp2, struct bm_pool_param *param);

/**
 * pp2_bm_pool_alloc_memory
 *
 * Allocate a software chunk of memory and associate it to the
 * input BM pool handle.
 *
 * This should be spitted into buffers by caller and these buffers
 * have to be released to pool by calling pp2_bm_buf_put() on each buffer
 *
 * @param bm_pool    BM Pool handle
 * @param size       Size of the memory that shall be managed
 *
 * @retval           Valid handle to allocate memory, 0 otherwise
 */
uintptr_t pp2_bm_pool_alloc_memory(struct pp2_bm_pool *bm_pool,
				                  size_t size);

/**
 * pp2_bm_pool_destroy
 *
 * Destroy this BM pool from the input BM-If object. After
 * this routine returns successfully, the BM pool is free
 * and can be re-initialized and setup against another
 * BM-IF object at a later time.
 *
 * @param bm_if      Original BM-IF object pool owner
 * @param bm_pool    BM Pool handle
 *
 * @retval           0 on success, error otherwise
 */
int pp2_bm_pool_destroy(struct pp2_bm_if *bm_if,
			    struct pp2_bm_pool *bm_pool);

/**
 * pp2_bm_if_deinit
 *
 * Deinitialize (destroy) a BM-IF object. After this
 * routine returns successfully, all BM pools associated
 * with it shall also be destroyed
 *
 * @param bm_if      BM-IF object handle
 *
 * @retval           0 on success, error otherwise
 */
void pp2_bm_if_deinit(struct pp2_bm_if *bm_if);

/**
 * pp2_bm_pool_get_id
 *
 * BM Pool get physical ID
 *
 * @param bm_pool    BM Pool handle
 *
 * @retval physical ID associated with this BM pool handle
 */
uint32_t pp2_bm_pool_get_id(struct pp2_bm_pool *bm_pool);

/**
 * pp2_bm_buf_get
 *
 * Get (allocate from BM) a free buffer which resides
 * in the specified BM Pool associated with the
 * specified BM-IF object
 *
 * @param bm_if      BM-IF handle
 * @param bm_pool    BM Pool handle
 *
 * @retval           Virtual buffer address
 */
uintptr_t pp2_bm_buf_get(struct pp2_bm_if *bm_if,
			  struct pp2_bm_pool *bm_pool);

/**
 * pp2_bm_buf_put
 *
 * Put (release to BM) a used buffer which resides
 * in the specified BM Pool associated with the
 * specified BM-IF object
 *
 * @param bm_if      BM-IF handle
 * @param bm_pool    BM Pool handle
 * @param bm_buf     BM Pool virtual buffer address
 *
 * @retval           0 on success, error otherwise
 */
int pp2_bm_buf_put(struct pp2_bm_if *bm_if,
		           struct pp2_bm_pool *bm_pool,
				   uintptr_t bm_buf);

/**
 * pp2_bm_pool_assign
 *
 * Assign BM Pool to Port RXQ(s) for SWF
 * Associate port RXQ with this pool.
 * RxDMA shall construct RXDs using this BM pool
 *
 * TODO: Why this function stays here and it is not a part of RX queue code?
 *
 * @param port       pointer to PP port
 * @param pool_id    pool id
 * @param rxq_id     ingress logical queue id
 * @param type       pool type: long(jumbo)/short
 *
 */
void pp2_bm_pool_assign(struct pp2_port *port, uint32_t pool_id,
			    uint32_t rxq_id, uint32_t type);

/**
 * pp2_bm_pa2va
 *
 * Translate physical address of buffer to virtual address
 *
 * @param bm_pool    BM Pool handle
 * @param pa         buffer physical address
 *
 * @retval           buffer virtual address
 */
uintptr_t pp2_bm_pa2va(struct pp2_bm_pool *bm_pool, uintptr_t pa);

/**
 * pp2_bm_va2pa
 *
 * Translate virtual address of buffer to physical address
 *
 * @param bm_pool    BM Pool handle
 * @param va         buffer virtual address
 *
 * @retval           buffer physical address
 */
uintptr_t pp2_bm_va2pa(struct pp2_bm_pool *bm_pool, uintptr_t va);

#endif /* _PP2_BM_H_ */