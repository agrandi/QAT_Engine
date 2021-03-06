/* ====================================================================
 *
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2016 Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * ====================================================================
 */

/*
 * This file contains modified code from OpenSSL/BoringSSL used
 * in order to run certain operations in constant time.
 * It is subject to the following license:
 */

/*
 * Copyright 2002-2016 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*****************************************************************************
 * @file qat_ciphers.c
 *
 * This file contains the engine implementations for cipher operations
 *
 *****************************************************************************/

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <pthread.h>
#ifdef USE_QAT_CONTIG_MEM
# include "qae_mem_utils.h"
#endif
#ifdef USE_QAE_MEM
# include "cmn_mem_drv_inf.h"
#endif

#include "qat_utils.h"
#include "e_qat.h"
#include "e_qat_err.h"

#include "cpa.h"
#include "cpa_types.h"
#include "cpa_cy_sym.h"
#include "qat_ciphers.h"
#include "qat_constant_time.h"

#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/tls1.h>
#include <openssl/async.h>
#include <openssl/lhash.h>
#include <openssl/ssl.h>
#include <string.h>

#ifdef OPENSSL_ENABLE_QAT_CIPHERS
# ifdef OPENSSL_DISABLE_QAT_CIPHERS
#  undef OPENSSL_DISABLE_QAT_CIPHERS
# endif
#endif

#define GET_TLS_HDR(qctx, i)     ((qctx)->aad[(i)])
#define GET_TLS_VERSION(hdr)     (((hdr)[9]) << QAT_BYTE_SHIFT | (hdr)[10])
#define GET_TLS_PAYLOAD_LEN(hdr) (((((hdr)[11]) << QAT_BYTE_SHIFT) & 0xff00) | \
                                  ((hdr)[12] & 0x00ff))
#define SET_TLS_PAYLOAD_LEN(hdr, len)   \
                do { \
                    hdr[11] = (len & 0xff00) >> QAT_BYTE_SHIFT; \
                    hdr[12] = len & 0xff; \
                } while(0)

#define FLATBUFF_ALLOC_AND_CHAIN(b1, b2, len) \
                do { \
                    (b1).pData = qaeCryptoMemAlloc(len, __FILE__, __LINE__); \
                    (b2).pData = (b1).pData; \
                    (b1).dataLenInBytes = len; \
                    (b2).dataLenInBytes = len; \
                } while(0)

#ifndef OPENSSL_ENABLE_QAT_SMALL_PACKET_CIPHER_OFFLOADS
# define GET_SW_CIPHER(ctx) \
            qat_chained_cipher_sw_impl(EVP_CIPHER_CTX_nid((ctx)))
#endif

#define DEBUG_PPL DEBUG
static int qat_chained_ciphers_init(EVP_CIPHER_CTX *ctx,
                                    const unsigned char *inkey,
                                    const unsigned char *iv, int enc);
static int qat_chained_ciphers_cleanup(EVP_CIPHER_CTX *ctx);
static int qat_chained_ciphers_do_cipher(EVP_CIPHER_CTX *ctx,
                                         unsigned char *out,
                                         const unsigned char *in, size_t len);
static int qat_chained_ciphers_ctrl(EVP_CIPHER_CTX *ctx, int type, int arg,
                                    void *ptr);

typedef struct _chained_info {
    const int nid;
    EVP_CIPHER *cipher;
    const int keylen;
} chained_info;

static chained_info info[] = {
    {NID_aes_128_cbc_hmac_sha1, NULL, AES_KEY_SIZE_128},
    {NID_aes_128_cbc_hmac_sha256, NULL, AES_KEY_SIZE_128},
    {NID_aes_256_cbc_hmac_sha1, NULL, AES_KEY_SIZE_256},
    {NID_aes_256_cbc_hmac_sha256, NULL, AES_KEY_SIZE_256},
};

static const unsigned int num_cc = sizeof(info) / sizeof(chained_info);

/* Qat Symmetric cipher function register */
int qat_cipher_nids[] = {
    NID_aes_128_cbc_hmac_sha1,
    NID_aes_128_cbc_hmac_sha256,
    NID_aes_256_cbc_hmac_sha1,
    NID_aes_256_cbc_hmac_sha256,
};

/* Setup template for Session Setup Data as most of the fields
 * are constant. The constant values of some of the fields are
 * chosen for Encryption operation.
 */
static const CpaCySymSessionSetupData template_ssd = {
    .sessionPriority = CPA_CY_PRIORITY_HIGH,
    .symOperation = CPA_CY_SYM_OP_ALGORITHM_CHAINING,
    .cipherSetupData = {
                        .cipherAlgorithm = CPA_CY_SYM_CIPHER_AES_CBC,
                        .cipherKeyLenInBytes = 0,
                        .pCipherKey = NULL,
                        .cipherDirection = CPA_CY_SYM_CIPHER_DIRECTION_ENCRYPT,
                        },
    .hashSetupData = {
                      .hashAlgorithm = CPA_CY_SYM_HASH_SHA1,
                      .hashMode = CPA_CY_SYM_HASH_MODE_AUTH,
                      .digestResultLenInBytes = 0,
                      .authModeSetupData = {
                                            .authKey = NULL,
                                            .authKeyLenInBytes = HMAC_KEY_SIZE,
                                            .aadLenInBytes = 0,
                                            },
                      .nestedModeSetupData = {0},
                      },
    .algChainOrder = CPA_CY_SYM_ALG_CHAIN_ORDER_HASH_THEN_CIPHER,
    .digestIsAppended = CPA_TRUE,
    .verifyDigest = CPA_FALSE,
    .partialsNotRequired = CPA_TRUE,
};

static const CpaCySymOpData template_opData = {
    .sessionCtx = NULL,
    .packetType = CPA_CY_SYM_PACKET_TYPE_FULL,
    .pIv = NULL,
    .ivLenInBytes = 0,
    .cryptoStartSrcOffsetInBytes = QAT_BYTE_ALIGNMENT,
    .messageLenToCipherInBytes = 0,
    .hashStartSrcOffsetInBytes = QAT_BYTE_ALIGNMENT - TLS_VIRT_HDR_SIZE,
    .messageLenToHashInBytes = 0,
    .pDigestResult = NULL,
    .pAdditionalAuthData = NULL
};

static inline int get_digest_len(int nid)
{
    return (((nid) == NID_aes_128_cbc_hmac_sha1 ||
             (nid) == NID_aes_256_cbc_hmac_sha1) ?
            SHA_DIGEST_LENGTH : SHA256_DIGEST_LENGTH);
}

static inline const EVP_CIPHER *qat_chained_cipher_sw_impl(int nid)
{
    switch (nid) {
    case NID_aes_128_cbc_hmac_sha1:
        return EVP_aes_128_cbc_hmac_sha1();
    case NID_aes_256_cbc_hmac_sha1:
        return EVP_aes_256_cbc_hmac_sha1();
    case NID_aes_128_cbc_hmac_sha256:
        return EVP_aes_128_cbc_hmac_sha256();
    case NID_aes_256_cbc_hmac_sha256:
        return EVP_aes_256_cbc_hmac_sha256();
    default:
        return NULL;
    }
}

static inline void qat_chained_ciphers_free_qop(qat_op_params **pqop,
                                               unsigned int *num_elem)
{
    unsigned int i = 0;
    qat_op_params *qop = NULL;
    if (pqop != NULL && ((qop = *pqop) != NULL)) {
        for (i = 0; i < *num_elem; i++) {
            QAT_CHK_QMFREE_FLATBUFF(qop[i].src_fbuf[0]);
            QAT_CHK_QMFREE_FLATBUFF(qop[i].src_fbuf[1]);
            QAT_QMEMFREE_BUFF(qop[i].src_sgl.pPrivateMetaData);
            QAT_QMEMFREE_BUFF(qop[i].dst_sgl.pPrivateMetaData);
            QAT_QMEMFREE_BUFF(qop[i].op_data.pIv);
        }
        OPENSSL_free(qop);
        *pqop = NULL;
        *num_elem = 0;
    }
}

static const EVP_CIPHER *qat_create_cipher_meth(int nid, int keylen)
{
    EVP_CIPHER *c = NULL;

#ifdef OPENSSL_DISABLE_QAT_CIPHERS
    return qat_chained_cipher_sw_impl(nid);
#endif
        if (((c = EVP_CIPHER_meth_new(nid, AES_BLOCK_SIZE, keylen)) == NULL)
            || !EVP_CIPHER_meth_set_iv_length(c, AES_IV_LEN)
            || !EVP_CIPHER_meth_set_flags(c, QAT_CHAINED_FLAG)
            || !EVP_CIPHER_meth_set_init(c, qat_chained_ciphers_init)
            || !EVP_CIPHER_meth_set_do_cipher(c, qat_chained_ciphers_do_cipher)
            || !EVP_CIPHER_meth_set_cleanup(c, qat_chained_ciphers_cleanup)
            || !EVP_CIPHER_meth_set_impl_ctx_size(c, sizeof(qat_chained_ctx))
            || !EVP_CIPHER_meth_set_set_asn1_params(c,
                                                    EVP_CIPH_FLAG_DEFAULT_ASN1 ?
                                                    NULL :
                                                    EVP_CIPHER_set_asn1_iv)
            || !EVP_CIPHER_meth_set_get_asn1_params(c,
                                                    EVP_CIPH_FLAG_DEFAULT_ASN1 ?
                                                    NULL :
                                                    EVP_CIPHER_get_asn1_iv)
            || !EVP_CIPHER_meth_set_ctrl(c, qat_chained_ciphers_ctrl)) {
        WARN("[%s]: Failed to create cipher methods for nid %d\n",
             __func__, nid);
        EVP_CIPHER_meth_free(c);
        c = NULL;
    }

    return c;
}

void qat_create_ciphers(void)
{
    int i;

    for (i = 0; i < num_cc; i++) {
        if (info[i].cipher == NULL) {
            info[i].cipher = (EVP_CIPHER *)
                qat_create_cipher_meth(info[i].nid, info[i].keylen);
        }
    }
}

void qat_free_ciphers(void)
{
    int i;

    for (i = 0; i < num_cc; i++) {
        if (info[i].cipher != NULL) {
#ifndef OPENSSL_DISABLE_QAT_CIPHERS
            EVP_CIPHER_meth_free(info[i].cipher);
#endif
            info[i].cipher = NULL;
        }
    }
}

#ifndef OPENSSL_ENABLE_QAT_SMALL_PACKET_CIPHER_OFFLOADS
# define CRYPTO_SMALL_PACKET_OFFLOAD_THRESHOLD_DEFAULT 2048

CRYPTO_ONCE qat_pkt_threshold_table_once = CRYPTO_ONCE_STATIC_INIT;
CRYPTO_THREAD_LOCAL qat_pkt_threshold_table_key;

void qat_pkt_threshold_table_make_key(void)
{
    CRYPTO_THREAD_init_local(&qat_pkt_threshold_table_key,
                             qat_free_pkt_threshold_table);
}

typedef struct cipher_threshold_table_s {
    int nid;
    int threshold;
} PKT_THRESHOLD;

DEFINE_LHASH_OF(PKT_THRESHOLD);

PKT_THRESHOLD qat_pkt_threshold_table[] = {
    {NID_aes_128_cbc_hmac_sha1, CRYPTO_SMALL_PACKET_OFFLOAD_THRESHOLD_DEFAULT},
    {NID_aes_256_cbc_hmac_sha1, CRYPTO_SMALL_PACKET_OFFLOAD_THRESHOLD_DEFAULT},
    {NID_aes_128_cbc_hmac_sha256,
     CRYPTO_SMALL_PACKET_OFFLOAD_THRESHOLD_DEFAULT},
    {NID_aes_256_cbc_hmac_sha256, CRYPTO_SMALL_PACKET_OFFLOAD_THRESHOLD_DEFAULT}
};

static int pkt_threshold_table_cmp(const PKT_THRESHOLD *a,
                                   const PKT_THRESHOLD *b)
{
    return (a->nid == b->nid) ? 0 : 1;
}

static unsigned long pkt_threshold_table_hash(const PKT_THRESHOLD * a)
{
    return (unsigned long)(a->nid);
}

LHASH_OF(PKT_THRESHOLD) *qat_create_pkt_threshold_table(void)
{
    int i;
    int tbl_size =
        (sizeof(qat_pkt_threshold_table) / sizeof(qat_pkt_threshold_table[0]));
    LHASH_OF(PKT_THRESHOLD) *ret = NULL;
    ret =
        lh_PKT_THRESHOLD_new(pkt_threshold_table_hash, pkt_threshold_table_cmp);
    if (ret == NULL) {
        return ret;
    }
    for (i = 0; i < tbl_size; i++) {
        lh_PKT_THRESHOLD_insert(ret, &qat_pkt_threshold_table[i]);
    }
    return ret;
}

int qat_pkt_threshold_table_set_threshold(int nid, int threshold)
{
    PKT_THRESHOLD entry, *ret;
    LHASH_OF(PKT_THRESHOLD) *tbl = NULL;
    if (NID_undef == nid) {
        WARN("Unsupported NID\n");
        return 0;
    }
    if ((tbl = CRYPTO_THREAD_get_local(&qat_pkt_threshold_table_key)) == NULL) {
        tbl = qat_create_pkt_threshold_table();
        if (tbl != NULL) {
            CRYPTO_THREAD_set_local(&qat_pkt_threshold_table_key, tbl);
        } else {
            WARN("Create packet threshold table fail.\n");
            return 0;
        }
    }
    entry.nid = nid;
    ret = lh_PKT_THRESHOLD_retrieve(tbl, &entry);
    if (ret == NULL) {
        WARN("Threshold entry retrieve failed for the NID : %d\n", entry.nid);
        return 0;
    }
    ret->threshold = threshold;
    lh_PKT_THRESHOLD_insert(tbl, ret);
    return 1;
}

int qat_pkt_threshold_table_get_threshold(int nid)
{
    PKT_THRESHOLD entry, *ret;
    LHASH_OF(PKT_THRESHOLD) *tbl = NULL;
    if ((tbl = CRYPTO_THREAD_get_local(&qat_pkt_threshold_table_key)) == NULL) {
        tbl = qat_create_pkt_threshold_table();
        if (tbl != NULL) {
            CRYPTO_THREAD_set_local(&qat_pkt_threshold_table_key, tbl);
        } else {
            WARN("Create packet threshold table fail.\n");
            return 0;
        }
    }
    entry.nid = nid;
    ret = lh_PKT_THRESHOLD_retrieve(tbl, &entry);
    if (ret == NULL) {
        WARN("Threshold entry retrieve failed for the NID : %d\n", entry.nid);
        return 0;
    }
    return ret->threshold;
}

void qat_free_pkt_threshold_table(void *thread_key)
{
    LHASH_OF(PKT_THRESHOLD) *tbl = (LHASH_OF(PKT_THRESHOLD) *) thread_key;
    if ((tbl = CRYPTO_THREAD_get_local(&qat_pkt_threshold_table_key))) {
        lh_PKT_THRESHOLD_free(tbl);
    }
}

#endif
/******************************************************************************
* function:
*         qat_chained_callbackFn(void *callbackTag, CpaStatus status,
*                        const CpaCySymOp operationType, void *pOpData,
*                        CpaBufferList * pDstBuffer, CpaBoolean verifyResult)
*

* @param pCallbackTag  [IN] -  Opaque value provided by user while making
*                              individual function call. Cast to op_done_pipe.
* @param status        [IN] -  Status of the operation.
* @param operationType [IN] -  Identifies the operation type requested.
* @param pOpData       [IN] -  Pointer to structure with input parameters.
* @param pDstBuffer    [IN] -  Destination buffer to hold the data output.
* @param verifyResult  [IN] -  Used to verify digest result.
*
* description:
*   Callback function used by chained ciphers with pipeline support. This
*   function is called when operation is completed for each pipeline. However
*   the paused job is woken up when all the pipelines have been proccessed.
*
******************************************************************************/
static void qat_chained_callbackFn(void *callbackTag, CpaStatus status,
                                   const CpaCySymOp operationType,
                                   void *pOpData, CpaBufferList *pDstBuffer,
                                   CpaBoolean verifyResult)
{
    struct op_done_pipe *opdone = (struct op_done_pipe *)callbackTag;
    CpaBoolean res = CPA_FALSE;

    if (opdone == NULL) {
        WARN("[%s] Callback Tag NULL!\n", __func__);
        return;
    }

    opdone->num_processed++;
    res = (status == CPA_STATUS_SUCCESS) && verifyResult ? CPA_TRUE : CPA_FALSE;

    /* If any single pipe processing failed, the entire operation
     * is treated as failure. The default value of opDone.verifyResult
     * is TRUE. Change it to false on Failure.
     */
    if (res == CPA_FALSE) {
        DEBUG("[%s] Pipe %d failed( status %d, verifyResult %d)!\n",
              __func__, opdone->num_processed, status, verifyResult);
        opdone->opDone.verifyResult = CPA_FALSE;
    }

    /* The QAT API guarantees submission order for request
     * i.e. first in first out. If not all requests have been
     * submitted or processed, wait for more callbacks.
     */
    if ((opdone->num_submitted != opdone->num_pipes) ||
        (opdone->num_submitted != opdone->num_processed))
        return;

    /* Mark job as done when all the requests have been submitted and
     * subsequently processed.
     */
    opdone->opDone.flag = 1;
    if (opdone->opDone.job) {
        qat_wake_job(opdone->opDone.job, 0);
    }
}

/******************************************************************************
* function:
*         qat_ciphers(ENGINE *e,
*                     const EVP_CIPHER **cipher,
*                     const int **nids,
*                     int nid)
*
* @param e      [IN] - OpenSSL engine pointer
* @param cipher [IN] - cipher structure pointer
* @param nids   [IN] - cipher function nids
* @param nid    [IN] - cipher operation id
*
* description:
*   Qat engine cipher operations registrar
******************************************************************************/
int qat_ciphers(ENGINE *e, const EVP_CIPHER **cipher, const int **nids, int nid)
{
    int i;

    /* No specific cipher => return a list of supported nids ... */
    if (cipher == NULL) {
        *nids = qat_cipher_nids;
        /* num ciphers supported (size of array/size of 1 element) */
        return (sizeof(qat_cipher_nids) / sizeof(qat_cipher_nids[0]));
    }

    for (i = 0; i < num_cc; i++) {
        if (nid == info[i].nid) {
            if (info[i].cipher == NULL)
                qat_create_ciphers();
            *cipher = info[i].cipher;
            return 1;
        }
    }

    *cipher = NULL;
    return 0;
}

/******************************************************************************
* function:
*         qat_setup_op_params(EVP_CIPHER_CTX *ctx)
*
* @param qctx    [IN]  - pointer to existing qat_chained_ctx
*
* @retval 1      function succeeded
* @retval 0      function failed
*
* description:
*    This function initialises the flatbuffer and flat buffer list for use.
*
******************************************************************************/
static int qat_setup_op_params(EVP_CIPHER_CTX *ctx)
{
    CpaCySymOpData *opd = NULL;
    Cpa32U msize = 0;
    qat_chained_ctx *qctx = qat_chained_data(ctx);
    int i = 0;
    unsigned int start;

    /* When no pipelines are used, numpipes = 1. The actual number of pipes are
     * not known until the start of do_cipher.
     */
    if (PIPELINE_USED(qctx)) {
        /* When Pipes have been previously used, the memory has been allocated
         * for max supported pipes although initialised only for numpipes.
         */
        start = qctx->npipes_last_used;
    } else {
        start = 1;
        /* When the context switches from using no pipes to using pipes,
         * free the previous allocated memory.
         */
        if (qctx->qop != NULL && qctx->qop_len < qctx->numpipes) {
            qat_chained_ciphers_free_qop(&qctx->qop, &qctx->qop_len);
            DEBUG_PPL("[%s:%p] qop memory freed\n", __func__, ctx);
        }
    }

    /* Allocate memory for qop depending on whether pipes are used or not.
     * In case of pipes, allocate for the maximum supported pipes.
     */
    if (qctx->qop == NULL) {
        if (PIPELINE_USED(qctx)) {
            WARN("[%s] Pipeline used but no data allocated. \
                  Possible memory leak", __func__);
        }

        qctx->qop_len = qctx->numpipes > 1 ? QAT_MAX_PIPELINES : 1;
        qctx->qop = (qat_op_params *) OPENSSL_zalloc(sizeof(qat_op_params)
                                                     * qctx->qop_len);
        if (qctx->qop == NULL) {
            WARN("[%s] Unable to allocate memory[%d bytes] for qat op params\n",
                 __func__, sizeof(qat_op_params) * qctx->qop_len);
            return 0;
        }
        /* start from 0 as New array of qat_op_params */
        start = 0;
    }

    for (i = start; i < qctx->numpipes; i++) {
        /* This is a whole block the size of the memory alignment. If the
         * alignment was to become smaller than the header size
         * (TLS_VIRT_HEADER_SIZE) which is unlikely then we would need to add
         * some more logic here to work out how many blocks of size
         * QAT_BYTE_ALIGNMENT we need to allocate to fit the header in.
         */
        FLATBUFF_ALLOC_AND_CHAIN(qctx->qop[i].src_fbuf[0],
                                 qctx->qop[i].dst_fbuf[0], QAT_BYTE_ALIGNMENT);
        if (qctx->qop[i].src_fbuf[0].pData == NULL) {
            WARN("[%s] Unable to allocate memory for TLS header\n", __func__);
            goto err;
        }
        memset(qctx->qop[i].src_fbuf[0].pData, 0, QAT_BYTE_ALIGNMENT);

        qctx->qop[i].src_fbuf[1].pData = NULL;
        qctx->qop[i].dst_fbuf[1].pData = NULL;

        qctx->qop[i].src_sgl.numBuffers = 2;
        qctx->qop[i].src_sgl.pBuffers = qctx->qop[i].src_fbuf;
        qctx->qop[i].src_sgl.pUserData = NULL;
        qctx->qop[i].src_sgl.pPrivateMetaData = NULL;

        qctx->qop[i].dst_sgl.numBuffers = 2;
        qctx->qop[i].dst_sgl.pBuffers = qctx->qop[i].dst_fbuf;
        qctx->qop[i].dst_sgl.pUserData = NULL;
        qctx->qop[i].dst_sgl.pPrivateMetaData = NULL;

        /* setup meta data for buffer lists */
        if (msize == 0 &&
            cpaCyBufferListGetMetaSize(qctx->instanceHandle,
                                       qctx->qop[i].src_sgl.numBuffers,
                                       &msize) != CPA_STATUS_SUCCESS) {
            WARN("[%s] --- cpaCyBufferListGetBufferSize failed.\n", __func__);
            goto err;
        }

        if (msize) {
            qctx->qop[i].src_sgl.pPrivateMetaData =
                qaeCryptoMemAlloc(msize, __FILE__, __LINE__);
            qctx->qop[i].dst_sgl.pPrivateMetaData =
                qaeCryptoMemAlloc(msize, __FILE__, __LINE__);
            if (qctx->qop[i].src_sgl.pPrivateMetaData == NULL ||
                qctx->qop[i].dst_sgl.pPrivateMetaData == NULL) {
                WARN("[%s] QMEM alloc failed for PrivateData\n", __func__);
                goto err;
            }
        }

        opd = &qctx->qop[i].op_data;

        /* Copy the opData template */
        memcpy(opd, &template_opData, sizeof(template_opData));

        /* Update Opdata */
        opd->sessionCtx = qctx->session_ctx;
        opd->pIv = qaeCryptoMemAlloc(EVP_CIPHER_CTX_iv_length(ctx),
                                     __FILE__, __LINE__);
        if (opd->pIv == NULL) {
            WARN("[%s] --- QMEM Mem Alloc failed for pIv for pipe %d.\n",
                 __func__, i);
            goto err;
        }

        opd->ivLenInBytes = (Cpa32U) EVP_CIPHER_CTX_iv_length(ctx);
    }

    DEBUG_PPL("[%s:%p] qop setup for %d elements\n",
              __func__, ctx, qctx->qop_len);
    return 1;

 err:
    qat_chained_ciphers_free_qop(&qctx->qop, &qctx->qop_len);
    return 0;
}

/******************************************************************************
* function:
*         qat_chained_ciphers_init(EVP_CIPHER_CTX *ctx,
*                                    const unsigned char *inkey,
*                                    const unsigned char *iv,
*                                    int enc)
*
* @param ctx    [IN]  - pointer to existing ctx
* @param inKey  [IN]  - input cipher key
* @param iv     [IN]  - initialisation vector
* @param enc    [IN]  - 1 encrypt 0 decrypt
*
* @retval 1      function succeeded
* @retval 0      function failed
*
* description:
*    This function initialises the cipher and hash algorithm parameters for this
*  EVP context.
*
******************************************************************************/
int qat_chained_ciphers_init(EVP_CIPHER_CTX *ctx,
                             const unsigned char *inkey,
                             const unsigned char *iv, int enc)
{
    CpaCySymSessionSetupData *ssd = NULL;
    Cpa32U sctx_size = 0;
    CpaCySymSessionCtx sctx = NULL;
    CpaStatus sts = 0;
    qat_chained_ctx *qctx = NULL;
    unsigned char *ckey = NULL;
    int ckeylen;
    int dlen;

    if (ctx == NULL || inkey == NULL) {
        WARN("[%s] ctx or inkey is NULL.\n", __func__);
        return 0;
    }

    qctx = qat_chained_data(ctx);
    if (qctx == NULL) {
        WARN("[%s] --- qctx is NULL.\n", __func__);
        return 0;
    }

    INIT_SEQ_CLEAR_ALL_FLAGS(qctx);

    if (iv != NULL)
        memcpy(EVP_CIPHER_CTX_iv_noconst(ctx), iv,
               EVP_CIPHER_CTX_iv_length(ctx));
    else
        memset(EVP_CIPHER_CTX_iv_noconst(ctx), 0,
               EVP_CIPHER_CTX_iv_length(ctx));

    ckeylen = EVP_CIPHER_CTX_key_length(ctx);
    ckey = OPENSSL_malloc(ckeylen);
    if (ckey == NULL) {
        WARN("[%s] --- unable to allocate memory for Cipher key.\n", __func__);
        return 0;
    }
    memcpy(ckey, inkey, ckeylen);

    memset(qctx, 0, sizeof(*qctx));

    qctx->numpipes = 1;
    qctx->total_op = 0;
    qctx->npipes_last_used = 1;

    qctx->hmac_key = OPENSSL_zalloc(HMAC_KEY_SIZE);
    if (qctx->hmac_key == NULL) {
        WARN("[%s] Unable to allocate memory for HMAC Key\n", __func__);
        goto end;
    }
#ifndef OPENSSL_ENABLE_QAT_SMALL_PACKET_CIPHER_OFFLOADS
    const EVP_CIPHER *sw_cipher = GET_SW_CIPHER(ctx);
    unsigned int sw_size = EVP_CIPHER_impl_ctx_size(sw_cipher);
    if (sw_size != 0) {
        qctx->sw_ctx_data = OPENSSL_zalloc(sw_size);
        if (qctx->sw_ctx_data == NULL) {
            WARN("[%s] Unable to allocate memory[ %d bytes] for sw_ctx_data\n",
                 __func__, sw_size);
            goto end;
        }
    }

    EVP_CIPHER_CTX_set_cipher_data(ctx, qctx->sw_ctx_data);
    EVP_CIPHER_meth_get_init(sw_cipher) (ctx, inkey, iv, enc);
    EVP_CIPHER_CTX_set_cipher_data(ctx, qctx);
#endif

    ssd = OPENSSL_malloc(sizeof(CpaCySymSessionSetupData));
    if (ssd == NULL) {
        WARN("OPENSSL_malloc() failed for session setup data allocation.\n");
        goto end;
    }

    qctx->session_data = ssd;

    /* Copy over the template for most of the values */
    memcpy(ssd, &template_ssd, sizeof(template_ssd));

    /* Change constant values for decryption */
    if (!enc) {
        ssd->cipherSetupData.cipherDirection =
            CPA_CY_SYM_CIPHER_DIRECTION_DECRYPT;
        ssd->algChainOrder = CPA_CY_SYM_ALG_CHAIN_ORDER_CIPHER_THEN_HASH;
        ssd->verifyDigest = CPA_TRUE;
    }

    ssd->cipherSetupData.cipherKeyLenInBytes = ckeylen;
    ssd->cipherSetupData.pCipherKey = ckey;

    dlen = get_digest_len(EVP_CIPHER_CTX_nid(ctx));

    ssd->hashSetupData.digestResultLenInBytes = dlen;

    if (dlen != SHA_DIGEST_LENGTH)
        ssd->hashSetupData.hashAlgorithm = CPA_CY_SYM_HASH_SHA256;

    ssd->hashSetupData.authModeSetupData.authKey = qctx->hmac_key;

    qctx->instanceHandle = get_next_inst();
    if (qctx->instanceHandle == NULL) {
        WARN("[%s] Failed to get QAT Instance Handle!.\n", __func__);
        goto end;
    }

    sts = cpaCySymSessionCtxGetSize(qctx->instanceHandle, ssd, &sctx_size);
    if (sts != CPA_STATUS_SUCCESS) {
        WARN("[%s] Failed to get SessionCtx size.\n", __func__);
        goto end;
    }

    sctx = (CpaCySymSessionCtx) qaeCryptoMemAlloc(sctx_size, __FILE__,
                                                  __LINE__);
    if (sctx == NULL) {
        WARN("[%s] QMEM alloc failed for session ctx!\n", __func__);
        goto end;
    }

    qctx->session_ctx = sctx;

    qctx->qop = NULL;
    qctx->qop_len = 0;

    INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_QAT_CTX_INIT);

    DEBUG_PPL("[%s:%p] qat chained cipher ctx %p initialised\n",
              __func__, ctx, qctx);
    return 1;

 end:
    if (ssd != NULL)
        QAT_CLEANSE_FREE_BUFF(ssd->cipherSetupData.pCipherKey, ckeylen);
    QAT_CLEANSE_FREE_BUFF(qctx->hmac_key, HMAC_KEY_SIZE);
    OPENSSL_free(qctx->session_data);
    QAT_QMEMFREE_BUFF(qctx->session_ctx);
#ifndef OPENSSL_ENABLE_QAT_SMALL_PACKET_CIPHER_OFFLOADS
    OPENSSL_free(qctx->sw_ctx_data);
#endif

    return 0;
}

/******************************************************************************
* function:
*    qat_chained_ciphers_ctrl(EVP_CIPHER_CTX *ctx,
*                             int type, int arg, void *ptr)
*
* @param ctx    [IN]  - pointer to existing ctx
* @param type   [IN]  - type of request either
*                       EVP_CTRL_AEAD_SET_MAC_KEY or EVP_CTRL_AEAD_TLS1_AAD
* @param arg    [IN]  - size of the pointed to by ptr
* @param ptr    [IN]  - input buffer contain the necessary parameters
*
* @retval x      The return value is dependent on the type of request being made
*       EVP_CTRL_AEAD_SET_MAC_KEY return of 1 is success
*       EVP_CTRL_AEAD_TLS1_AAD return value indicates the amount fo padding to
*               be applied to the SSL/TLS record
* @retval -1     function failed
*
* description:
*    This function is a generic control interface provided by the EVP API. For
*  chained requests this interface is used fro setting the hmac key value for
*  authentication of the SSL/TLS record. The second type is used to specify the
*  TLS virtual header which is used in the authentication calculationa nd to
*  identify record payload size.
*
******************************************************************************/
int qat_chained_ciphers_ctrl(EVP_CIPHER_CTX *ctx, int type, int arg, void *ptr)
{
    qat_chained_ctx *qctx = NULL;
    unsigned char *hmac_key = NULL;
    CpaCySymSessionSetupData *ssd = NULL;
    SHA_CTX hkey1;
    SHA256_CTX hkey256;
    CpaStatus sts;
    char *hdr = NULL;
    unsigned int len = 0;
    int retVal = 0;
    int dlen = 0;

    if (ctx == NULL) {
        WARN("[%s] --- ctx parameter is NULL.\n", __func__);
        return -1;
    }

    qctx = qat_chained_data(ctx);

    if (qctx == NULL) {
        WARN("[%s] --- qctx is NULL.\n", __func__);
        return -1;
    }

    dlen = get_digest_len(EVP_CIPHER_CTX_nid(ctx));

    switch (type) {
    case EVP_CTRL_AEAD_SET_MAC_KEY:
        hmac_key = qctx->hmac_key;
        ssd = qctx->session_data;

        memset(hmac_key, 0, HMAC_KEY_SIZE);

        if (arg > HMAC_KEY_SIZE) {
            if (dlen == SHA_DIGEST_LENGTH) {
                SHA1_Init(&hkey1);
                SHA1_Update(&hkey1, ptr, arg);
                SHA1_Final(hmac_key, &hkey1);
            } else {
                SHA256_Init(&hkey256);
                SHA256_Update(&hkey256, ptr, arg);
                SHA256_Final(hmac_key, &hkey256);
            }
        } else {
            memcpy(hmac_key, ptr, arg);
            ssd->hashSetupData.authModeSetupData.authKeyLenInBytes = arg;
        }

        INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_HMAC_KEY_SET);

        sts = cpaCySymInitSession(qctx->instanceHandle,
                                  qat_chained_callbackFn,
                                  ssd, qctx->session_ctx);
        if (sts != CPA_STATUS_SUCCESS) {
            WARN("[%s] --- cpaCySymInitSession failed.\n", __func__);
            retVal = 0;
        } else {
            INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_QAT_SESSION_INIT);
            retVal = 1;
        }
        break;

    case EVP_CTRL_AEAD_TLS1_AAD:
        /* This returns the amount of padding required for
           the send/encrypt direction.
         */
        if (arg != TLS_VIRT_HDR_SIZE || qctx->aad_ctr >= QAT_MAX_PIPELINES) {
            WARN("[%s] Invalid argument for AEAD_TLS1_AAD.\n", __func__);
            retVal = -1;
            break;
        }
        hdr = GET_TLS_HDR(qctx, qctx->aad_ctr);
        memcpy(hdr, ptr, TLS_VIRT_HDR_SIZE);
        qctx->aad_ctr++;
        if (qctx->aad_ctr > 1)
            INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_PPL_AADCTR_SET);

        len = GET_TLS_PAYLOAD_LEN(((char *)ptr));
        if (GET_TLS_VERSION(((char *)ptr)) >= TLS1_1_VERSION)
            len -= EVP_CIPHER_CTX_iv_length(ctx);
        else if (qctx->aad_ctr > 1) {
            /* pipelines are not supported for
             * TLS version < TLS1.1
             */
            WARN("[%s] AAD already set for TLS1.0\n", __func__);
            retVal = -1;
            break;
        }

        if (EVP_CIPHER_CTX_encrypting(ctx))
            retVal = (int)(((len + dlen + AES_BLOCK_SIZE)
                            & -AES_BLOCK_SIZE) - len);
        else
            retVal = dlen;

        INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_TLS_HDR_SET);
        break;

        /* All remaining cases are exclusive to pipelines and are not
         * used with small packet offload feature.
         */
    case EVP_CTRL_SET_PIPELINE_OUTPUT_BUFS:
        if (arg > QAT_MAX_PIPELINES) {
            WARN("[%s] PIPELINE_OUTPUT_BUFS npipes(%d) > Max(%d).\n",
                 __func__, arg, QAT_MAX_PIPELINES);
            return -1;
        }
        qctx->p_out = (unsigned char **)ptr;
        qctx->numpipes = arg;
        INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_PPL_OBUF_SET);
        return 1;

    case EVP_CTRL_SET_PIPELINE_INPUT_BUFS:
        if (arg > QAT_MAX_PIPELINES) {
            WARN("[%s] PIPELINE_OUTPUT_BUFS npipes(%d) > Max(%d).\n",
                 __func__, arg, QAT_MAX_PIPELINES);
            return -1;
        }
        qctx->p_in = (unsigned char **)ptr;
        qctx->numpipes = arg;
        INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_PPL_IBUF_SET);
        return 1;

    case EVP_CTRL_SET_PIPELINE_INPUT_LENS:
        if (arg > QAT_MAX_PIPELINES) {
            WARN("[%s] PIPELINE_INPUT_LENS npipes(%d) > Max(%d).\n",
                 __func__, arg, QAT_MAX_PIPELINES);
            return -1;
        }
        qctx->p_inlen = (size_t *)ptr;
        qctx->numpipes = arg;
        INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_PPL_BUF_LEN_SET);
        return 1;

    default:
        WARN("[%s] --- unknown type parameter.\n", __func__);
        return -1;
    }

#ifndef OPENSSL_ENABLE_QAT_SMALL_PACKET_CIPHER_OFFLOADS
    /* Openssl EVP implementation changes the size of payload encoded in TLS
     * header pointed by ptr for EVP_CTRL_AEAD_TLS1_AAD, hence call is made
     * here after ptr has been processed by engine implementation.
     */
    EVP_CIPHER_CTX_set_cipher_data(ctx, qctx->sw_ctx_data);
    EVP_CIPHER_meth_get_ctrl(GET_SW_CIPHER(ctx)) (ctx, type, arg, ptr);
    EVP_CIPHER_CTX_set_cipher_data(ctx, qctx);
#endif
    return retVal;
}

/******************************************************************************
* function:
*    qat_chained_ciphers_cleanup(EVP_CIPHER_CTX *ctx)
*
* @param ctx    [IN]  - pointer to existing ctx
*
* @retval 1      function succeeded
* @retval 0      function failed
*
* description:
*    This function will cleanup all allocated resources required to perfrom the
*  cryptographic transform.
*
******************************************************************************/
int qat_chained_ciphers_cleanup(EVP_CIPHER_CTX *ctx)
{
    qat_chained_ctx *qctx = NULL;
    CpaStatus sts = 0;
    CpaCySymSessionSetupData *ssd = NULL;
    int retVal = 1;

    if (ctx == NULL) {
        WARN("[%s] ctx parameter is NULL.\n", __func__);
        return 0;
    }

    qctx = qat_chained_data(ctx);
    if (qctx == NULL) {
        WARN("[%s] qctx parameter is NULL.\n", __func__);
        return 0;
    }
#ifndef OPENSSL_ENABLE_QAT_SMALL_PACKET_CIPHER_OFFLOADS
    OPENSSL_free(qctx->sw_ctx_data);
#endif

    /* ctx may be cleaned before it gets a chance to allocate qop */
    qat_chained_ciphers_free_qop(&qctx->qop, &qctx->qop_len);

    ssd = qctx->session_data;
    if (ssd) {
        if (INIT_SEQ_IS_FLAG_SET(qctx, INIT_SEQ_QAT_SESSION_INIT)) {
            sts = cpaCySymRemoveSession(qctx->instanceHandle,
                                        qctx->session_ctx);
            if (sts != CPA_STATUS_SUCCESS) {
                WARN("[%s] cpaCySymRemoveSession FAILED, sts = %d.!\n",
                     __func__, sts);
                retVal = 0;
            }
        }
        QAT_QMEMFREE_BUFF(qctx->session_ctx);
        QAT_CLEANSE_FREE_BUFF(ssd->hashSetupData.authModeSetupData.authKey,
                              ssd->hashSetupData.authModeSetupData.
                              authKeyLenInBytes);
        QAT_CLEANSE_FREE_BUFF(ssd->cipherSetupData.pCipherKey,
                              ssd->cipherSetupData.cipherKeyLenInBytes);
        OPENSSL_free(ssd);
    }

    INIT_SEQ_CLEAR_ALL_FLAGS(qctx);
    DEBUG_PPL("[%s:%p] EVP CTX cleaned up\n", __func__, ctx);
    return retVal;
}

/******************************************************************************
* function:
*    qat_chained_ciphers_do_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
*                                  const unsigned char *in, size_t len)
*
* @param ctx    [IN]  - pointer to existing ctx
* @param out   [OUT]  - output buffer for transform result
* @param in     [IN]  - input buffer
* @param len    [IN]  - length of input buffer
*
* @retval 0      function failed
* @retval 1      function succeeded
*
* description:
*    This function performs the cryptographic transform according to the
*  parameters setup during initialisation.
*
******************************************************************************/
int qat_chained_ciphers_do_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
                                  const unsigned char *in, size_t len)
{
    CpaStatus sts = 0;
    CpaCySymOpData *opd = NULL;
    CpaBufferList *s_sgl = NULL;
    CpaBufferList *d_sgl = NULL;
    CpaFlatBuffer *s_fbuf = NULL;
    CpaFlatBuffer *d_fbuf = NULL;
    int retVal = 0;
    unsigned int pad_check = 1;
    int pad_len = 0;
    int plen = 0;
    int plen_adj = 0;
    struct op_done_pipe done;
    qat_chained_ctx *qctx = NULL;
    AES_KEY aes_key;
    unsigned char *inb, *outb;
    unsigned char ivec[AES_BLOCK_SIZE];
    unsigned char out_blk[TLS_MAX_PADDING_LENGTH + 1] = { 0x0 };
    const unsigned char *in_blk = NULL;
    unsigned int ivlen = 0;
    int dlen, vtls, enc, i, buflen;
    int discardlen = 0;
    char *tls_hdr = NULL;
    int pipe = 0;
    int error = 0;

    if (ctx == NULL) {
        WARN("[%s] CTX parameter is NULL.\n", __func__);
        return 0;
    }

    qctx = qat_chained_data(ctx);
    if (qctx == NULL || !INIT_SEQ_IS_FLAG_SET(qctx, INIT_SEQ_QAT_CTX_INIT)) {
        WARN("[%s] %s\n", __func__, qctx == NULL ? "QAT CTX NULL"
             : "QAT Context not initialised");
        return 0;
    }

    /* Pipeline initialisation requires multiple EVP_CIPHER_CTX_ctrl
     * calls to set all required parameters. Check if all have been
     * provided. For Pipeline, in and out buffers can be NULL as these
     * are supplied through ctrl messages.
     */
    if (PIPELINE_INCOMPLETE_INIT(qctx) ||
        (!PIPELINE_SET(qctx) && (in == NULL || out == NULL
                                 || (len % AES_BLOCK_SIZE)))) {
        WARN("[%s] %s \n", __func__,
             PIPELINE_INCOMPLETE_INIT(qctx) ?
             "Pipeline not initialised completely" : len % AES_BLOCK_SIZE
             ? "Buffer Length not multiple of AES block size"
             : "in/out buffer null");
        return 0;
    }

    if (!INIT_SEQ_IS_FLAG_SET(qctx, INIT_SEQ_QAT_SESSION_INIT)) {
        /* The qat session is initialized when HMAC key is set. In case
         * HMAC key is not explicitly set, use default HMAC key of all zeros
         * and initialise a qat session.
         */
        sts = cpaCySymInitSession(qctx->instanceHandle, qat_chained_callbackFn,
                                  qctx->session_data, qctx->session_ctx);
        if (sts != CPA_STATUS_SUCCESS) {
            WARN("[%s] cpaCySymInitSession failed! Status = %d\n",
                 __func__, sts);
            return 0;
        }
        INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_QAT_SESSION_INIT);
    }

    enc = EVP_CIPHER_CTX_encrypting(ctx);
    ivlen = EVP_CIPHER_CTX_iv_length(ctx);
    dlen = get_digest_len(EVP_CIPHER_CTX_nid(ctx));

    /* Check and setup data structures for pipeline */
    if (PIPELINE_SET(qctx)) {
        /* All the aad data (tls header) should be present */
        if (qctx->aad_ctr != qctx->numpipes) {
            WARN("[%s] AAD data missing supplied %d of %d\n",
                 __func__, qctx->aad_ctr, qctx->numpipes);
            return 0;
        }
    } else {
#ifndef OPENSSL_ENABLE_QAT_SMALL_PACKET_CIPHER_OFFLOADS
        if (len <=
            qat_pkt_threshold_table_get_threshold(EVP_CIPHER_CTX_nid(ctx))) {
            EVP_CIPHER_CTX_set_cipher_data(ctx, qctx->sw_ctx_data);
            retVal = EVP_CIPHER_meth_get_do_cipher(GET_SW_CIPHER(ctx))
                     (ctx, out, in, len);
            EVP_CIPHER_CTX_set_cipher_data(ctx, qctx);
            goto cleanup;
        }
#endif
        /* When no TLS AAD information is supplied, for example: speed,
         * the payload length for encrypt/decrypt is equal to buffer len
         * and the HMAC is to be discarded. Set the fake AAD hdr to avoid
         * decision points in code for this special case handling.
         */
        if (!TLS_HDR_SET(qctx)) {
            tls_hdr = GET_TLS_HDR(qctx, 0);
            /* Mark an invalid tls version */
            tls_hdr[9] = tls_hdr[10] = 0;
            /* Set the payload length equal to entire length
             * of buffer i.e. there is no space for HMAC in
             * buffer.
             */
            SET_TLS_PAYLOAD_LEN(tls_hdr, 0);
            plen = len;
            if (!enc) {
                /* When decrypting donot verify computed digest
                 * to stored digest as there is none in this case.
                 */
                qctx->session_data->verifyDigest = CPA_FALSE;
            }
            /* Find the extra length for qat buffers to store the HMAC and
             * padding which is later discarded when the result is copied out.
             */
            discardlen = ((len + dlen + AES_BLOCK_SIZE) & -AES_BLOCK_SIZE)
                - len;
            /* Pump-up the len by this amount */
            len += discardlen;
        }
        /* If the same ctx is being re-used for multiple invocation
         * of this function without setting EVP_CTRL for number of pipes,
         * the PIPELINE_SET is true from previous invocation. Clear Pipeline
         * when add_ctr is 1. This means user wants to switch from pipeline mode
         * to non-pipeline mode for the same ctx.
         */
        CLEAR_PIPELINE(qctx);

        /* setting these helps avoid decision branches when
         * pipelines are not used.
         */
        qctx->p_in = (unsigned char **)&in;
        qctx->p_out = &out;
        qctx->p_inlen = &len;
    }

    DEBUG_PPL("[%s:%p] Start Cipher operation with num pipes %d\n",
              __func__, ctx, qctx->numpipes);

    if ((qat_setup_op_params(ctx) != 1) ||
        (initOpDonePipe(&done, qctx->numpipes) != 1))
        return 0;

    do {
        opd = &qctx->qop[pipe].op_data;
        tls_hdr = GET_TLS_HDR(qctx, pipe);
        vtls = GET_TLS_VERSION(tls_hdr);
        s_fbuf = qctx->qop[pipe].src_fbuf;
        d_fbuf = qctx->qop[pipe].dst_fbuf;
        s_sgl = &qctx->qop[pipe].src_sgl;
        d_sgl = &qctx->qop[pipe].src_sgl;
        inb = &qctx->p_in[pipe][0];
        outb = &qctx->p_out[pipe][0];
        buflen = qctx->p_inlen[pipe];

        if (vtls >= TLS1_1_VERSION) {
            /*
             * Note: The OpenSSL framework assumes that the IV field will be part
             * of the output data. In order to chain HASH and CIPHER we need to
             * present contiguous SGL to QAT, copy IV to output buffer now and
             * skip it for chained operation.
             */
            if (inb != outb)
                memcpy(outb, inb, ivlen);
            memcpy(opd->pIv, inb, ivlen);
            inb += ivlen;
            buflen -= ivlen;
            plen_adj = ivlen;
        } else {
            if (qctx->numpipes > 1) {
                WARN("[%s] Pipe %d tls hdr version < tls1.1\n", __func__, pipe);
                error = 1;
                break;
            }
            memcpy(opd->pIv, EVP_CIPHER_CTX_iv(ctx), ivlen);
        }

        /* Calculate payload and padding len */
        if (enc) {
            /* For encryption, payload length is in the header.
             * For non-TLS use case, plen has already been set above.
             * For TLS Version > 1.1 the payload length also contains IV len.
             */
            if (vtls >= TLS1_VERSION)
                plen = GET_TLS_PAYLOAD_LEN(tls_hdr) - plen_adj;

            /* Compute the padding length using total buffer length, payload
             * length, digest length and a byte to encode padding len.
             */
            pad_len = buflen - (plen + dlen) - 1;

            /* If padlen is negative, then size of supplied output buffer
             * is smaller than required.
             */
            if ((buflen % AES_BLOCK_SIZE) != 0 || pad_len < 0 ||
                pad_len > TLS_MAX_PADDING_LENGTH) {
                WARN("[%s] buffer len[%d] or pad_len[%d] incorrect\n", __func__,
                     buflen, pad_len);
                error = 1;
                break;
            }
        } else if (vtls >= TLS1_VERSION) {
            /* Decrypt the last block of the buffer to get the pad_len.
             * Calculate payload len using total length and padlen.
             * NOTE: plen so calculated does not account for ivlen
             *       if iv is appened for TLS Version >= 1.1
             */
            unsigned int tmp_padlen = TLS_MAX_PADDING_LENGTH + 1;
            unsigned int maxpad, res = 0xff;
            size_t j;
            uint8_t cmask, b;

            if ((buflen - dlen) <= TLS_MAX_PADDING_LENGTH)
                tmp_padlen = (((buflen - dlen) + (AES_BLOCK_SIZE - 1))
                              / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;
            in_blk = inb + (buflen - tmp_padlen);
            memcpy(ivec, in_blk - AES_BLOCK_SIZE, AES_BLOCK_SIZE);

            AES_set_decrypt_key(qctx->session_data->cipherSetupData.pCipherKey,
                                EVP_CIPHER_CTX_key_length(ctx) * 8, &aes_key);
            AES_cbc_encrypt(in_blk, out_blk, tmp_padlen, &aes_key, ivec, 0);

            pad_len = out_blk[tmp_padlen - 1];
            /* Determine the maximum amount of padding that could be present */
            maxpad = buflen - (dlen + 1);
            maxpad |=
                (TLS_MAX_PADDING_LENGTH - maxpad) >> (sizeof(maxpad) * 8 - 8);
            maxpad &= TLS_MAX_PADDING_LENGTH;

            /* Check the padding in constant time */
            for (j = 0; j <= maxpad; j++) {
                cmask = qat_constant_time_ge_8(pad_len, j);
                b = out_blk[tmp_padlen - 1 - j];
                res &= ~(cmask & (pad_len ^ b));
            }
            res = qat_constant_time_eq(0xff, res & 0xff);
            pad_check &= (int)res;

            /* Adjust the amount of data to digest to be the maximum by setting
             * pad_len = 0 if the padding check failed or if the padding length
             * is greater than the maximum padding allowed. This adjustment
             * is done in constant time.
             */
            pad_check &= qat_constant_time_ge(maxpad, pad_len);
            pad_len *= pad_check;
            plen = buflen - (pad_len + 1 + dlen);
        }

        opd->messageLenToCipherInBytes = buflen;
        opd->messageLenToHashInBytes = TLS_VIRT_HDR_SIZE + plen;

        /* copy tls hdr in flatbuffer's last 13 bytes */
        memcpy(d_fbuf[0].pData + (d_fbuf[0].dataLenInBytes - TLS_VIRT_HDR_SIZE),
               tls_hdr, TLS_VIRT_HDR_SIZE);
        /* Update the value of payload before HMAC calculation */
        SET_TLS_PAYLOAD_LEN((d_fbuf[0].pData +
                             (d_fbuf[0].dataLenInBytes - TLS_VIRT_HDR_SIZE)),
                            plen);

        FLATBUFF_ALLOC_AND_CHAIN(s_fbuf[1], d_fbuf[1], buflen);
        if ((s_fbuf[1].pData) == NULL) {
            WARN("[%s] --- src/dst buffer allocation.\n", __func__);
            error = 1;
            break;
        }

        memcpy(d_fbuf[1].pData, inb, buflen - discardlen);

        if (enc) {
            /* Add padding to input buffer at end of digest */
            for (i = plen + dlen; i < buflen; i++)
                d_fbuf[1].pData[i] = pad_len;
        } else {
            /* store IV for next cbc operation */
            if (vtls < TLS1_1_VERSION)
                memcpy(EVP_CIPHER_CTX_iv_noconst(ctx),
                       inb + (buflen - discardlen) - ivlen, ivlen);
        }

        sts = myPerformOp(qctx->instanceHandle, &done, opd, s_sgl, d_sgl,
                          &(qctx->session_data->verifyDigest));
        if (sts != CPA_STATUS_SUCCESS) {
            WARN("[%s] CpaCySymPerformOp failed sts=%d.\n", __func__, sts);
            error = 1;
            break;
        }
        /* Increment after successful submission */
        done.num_submitted++;
    } while (++pipe < qctx->numpipes);

    /* If there has been an error during submission of the pipes
     * indicate to the callback function not to wait for the entire
     * pipeline.
     */
    if (error == 1)
        done.num_pipes = pipe;

    /* If there is nothing to wait for, do not pause or yield */
    if (done.num_submitted == 0 || (done.num_submitted == done.num_processed))
        goto end;

    do {
        if (done.opDone.job) {
            /* If we get a failure on qat_pause_job then we will
               not flag an error here and quit because we have
               an asynchronous request in flight.
               We don't want to start cleaning up data
               structures that are still being used. If
               qat_pause_job fails we will just yield and
               loop around and try again until the request
               completes and we can continue. */
            if (qat_pause_job(done.opDone.job, 0) == 0)
                pthread_yield();
        } else {
            pthread_yield();
        }
    } while (!done.opDone.flag);

 end:
    qctx->total_op += done.num_processed;
    cleanupOpDonePipe(&done);

    if (error == 0 && (done.opDone.verifyResult == CPA_TRUE))
        retVal = 1;

    pipe = 0;
    do {
        if (retVal == 1) {
            memcpy(qctx->p_out[pipe] + plen_adj,
                   qctx->qop[pipe].dst_fbuf[1].pData,
                   qctx->p_inlen[pipe] - discardlen - plen_adj);
        }
        qaeCryptoMemFree(qctx->qop[pipe].src_fbuf[1].pData);
        qctx->qop[pipe].src_fbuf[1].pData = NULL;
        qctx->qop[pipe].dst_fbuf[1].pData = NULL;
    } while (++pipe < qctx->numpipes);

    if (enc && vtls < TLS1_1_VERSION)
        memcpy(EVP_CIPHER_CTX_iv_noconst(ctx),
               outb + buflen - discardlen - ivlen, ivlen);

 cleanup:
    /*Reset the AAD counter forcing that new AAD information is provided
     * before each repeat invocation of this function.
     */
    qctx->aad_ctr = 0;

    /* This function can be called again with the same evp_cipher_ctx. */
    if (PIPELINE_SET(qctx)) {
        /* Number of pipes can grow between multiple invocation of this call.
         * Record the maximum number of pipes used so that data structures can
         * be allocated accordingly.
         */
        INIT_SEQ_CLEAR_FLAG(qctx, INIT_SEQ_PPL_AADCTR_SET);
        INIT_SEQ_SET_FLAG(qctx, INIT_SEQ_PPL_USED);
        qctx->npipes_last_used = qctx->numpipes > qctx->npipes_last_used
            ? qctx->numpipes : qctx->npipes_last_used;
    }
    return retVal & pad_check;
}
