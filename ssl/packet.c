/*
 * Copyright 2015-2016 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <assert.h>
#include "packet_locl.h"

#define DEFAULT_BUF_SIZE    256

int WPACKET_allocate_bytes(WPACKET *pkt, size_t len, unsigned char **allocbytes)
{
    /* Internal API, so should not fail */
    assert(pkt->subs != NULL && len != 0);
    if (pkt->subs == NULL || len == 0)
        return 0;

    if (pkt->maxsize - pkt->written < len)
        return 0;

    if (pkt->buf->length - pkt->written < len) {
        size_t newlen;
        size_t reflen;

        reflen = (len > pkt->buf->length) ? len : pkt->buf->length;

        if (reflen > SIZE_MAX / 2) {
            newlen = SIZE_MAX;
        } else {
            newlen = reflen * 2;
            if (newlen < DEFAULT_BUF_SIZE)
                newlen = DEFAULT_BUF_SIZE;
        }
        if (BUF_MEM_grow(pkt->buf, newlen) == 0)
            return 0;
    }
    *allocbytes = (unsigned char *)pkt->buf->data + pkt->curr;
    pkt->written += len;
    pkt->curr += len;

    return 1;
}

int WPACKET_sub_allocate_bytes__(WPACKET *pkt, size_t len,
                                 unsigned char **allocbytes, size_t lenbytes)
{
    if (!WPACKET_start_sub_packet_len__(pkt, lenbytes)
            || !WPACKET_allocate_bytes(pkt, len, allocbytes)
            || !WPACKET_close(pkt))
        return 0;

    return 1;
}

static size_t maxmaxsize(size_t lenbytes)
{
    if (lenbytes >= sizeof(size_t) || lenbytes == 0)
        return SIZE_MAX;

    return ((size_t)1 << (lenbytes * 8)) - 1 + lenbytes;
}

int WPACKET_init_len(WPACKET *pkt, BUF_MEM *buf, size_t lenbytes)
{
    unsigned char *lenchars;

    /* Internal API, so should not fail */
    assert(buf != NULL);
    if (buf == NULL)
        return 0;

    pkt->buf = buf;
    pkt->curr = 0;
    pkt->written = 0;
    pkt->maxsize = maxmaxsize(lenbytes);

    pkt->subs = OPENSSL_zalloc(sizeof(*pkt->subs));
    if (pkt->subs == NULL)
        return 0;

    if (lenbytes == 0)
        return 1;

    pkt->subs->pwritten = lenbytes;
    pkt->subs->lenbytes = lenbytes;

    if (!WPACKET_allocate_bytes(pkt, lenbytes, &lenchars)) {
        OPENSSL_free(pkt->subs);
        pkt->subs = NULL;
        return 0;
    }
    pkt->subs->packet_len = lenchars - (unsigned char *)pkt->buf->data;

    return 1;
}

int WPACKET_init(WPACKET *pkt, BUF_MEM *buf)
{
    return WPACKET_init_len(pkt, buf, 0);
}

int WPACKET_set_flags(WPACKET *pkt, unsigned int flags)
{
    /* Internal API, so should not fail */
    assert(pkt->subs != NULL);
    if (pkt->subs == NULL)
        return 0;

    pkt->subs->flags = flags;

    return 1;
}

/* Store the |value| of length |len| at location |data| */
static int put_value(unsigned char *data, size_t value, size_t len)
{
    for (data += len - 1; len > 0; len--) {
        *data = (unsigned char)(value & 0xff);
        data--;
        value >>= 8;
    }

    /* Check whether we could fit the value in the assigned number of bytes */
    if (value > 0)
        return 0;

    return 1;
}


/*
 * Internal helper function used by WPACKET_close() and WPACKET_finish() to
 * close a sub-packet and write out its length if necessary.
 */
static int wpacket_intern_close(WPACKET *pkt)
{
    WPACKET_SUB *sub = pkt->subs;
    size_t packlen = pkt->written - sub->pwritten;

    if (packlen == 0
            && (sub->flags & WPACKET_FLAGS_NON_ZERO_LENGTH) != 0)
        return 0;

    if (packlen == 0
            && sub->flags & WPACKET_FLAGS_ABANDON_ON_ZERO_LENGTH) {
        /* Deallocate any bytes allocated for the length of the WPACKET */
        if ((pkt->curr - sub->lenbytes) == sub->packet_len) {
            pkt->written -= sub->lenbytes;
            pkt->curr -= sub->lenbytes;
        }

        /* Don't write out the packet length */
        sub->packet_len = 0;
        sub->lenbytes = 0;
    }

    /* Write out the WPACKET length if needed */
    if (sub->lenbytes > 0 
                && !put_value((unsigned char *)&pkt->buf->data[sub->packet_len],
                              packlen, sub->lenbytes))
            return 0;

    pkt->subs = sub->parent;
    OPENSSL_free(sub);

    return 1;
}

int WPACKET_close(WPACKET *pkt)
{
    /*
     * Internal API, so should not fail - but we do negative testing of this
     * so no assert (otherwise the tests fail)
     */
    if (pkt->subs == NULL || pkt->subs->parent == NULL)
        return 0;

    return wpacket_intern_close(pkt);
}

int WPACKET_finish(WPACKET *pkt)
{
    int ret;

    /*
     * Internal API, so should not fail - but we do negative testing of this
     * so no assert (otherwise the tests fail)
     */
    if (pkt->subs == NULL || pkt->subs->parent != NULL)
        return 0;

    ret = wpacket_intern_close(pkt);
    if (ret) {
        OPENSSL_free(pkt->subs);
        pkt->subs = NULL;
    }

    return ret;
}

int WPACKET_start_sub_packet_len__(WPACKET *pkt, size_t lenbytes)
{
    WPACKET_SUB *sub;
    unsigned char *lenchars;

    /* Internal API, so should not fail */
    assert(pkt->subs != NULL);
    if (pkt->subs == NULL)
        return 0;

    sub = OPENSSL_zalloc(sizeof(*sub));
    if (sub == NULL)
        return 0;

    sub->parent = pkt->subs;
    pkt->subs = sub;
    sub->pwritten = pkt->written + lenbytes;
    sub->lenbytes = lenbytes;

    if (lenbytes == 0) {
        sub->packet_len = 0;
        return 1;
    }

    if (!WPACKET_allocate_bytes(pkt, lenbytes, &lenchars))
        return 0;
    /* Convert to an offset in case the underlying BUF_MEM gets realloc'd */
    sub->packet_len = lenchars - (unsigned char *)pkt->buf->data;

    return 1;
}

int WPACKET_start_sub_packet(WPACKET *pkt)
{
    return WPACKET_start_sub_packet_len__(pkt, 0);
}

int WPACKET_put_bytes__(WPACKET *pkt, unsigned int val, size_t size)
{
    unsigned char *data;

    /* Internal API, so should not fail */
    assert(size <= sizeof(unsigned int));

    if (size > sizeof(unsigned int)
            || !WPACKET_allocate_bytes(pkt, size, &data)
            || !put_value(data, val, size))
        return 0;

    return 1;
}

int WPACKET_set_max_size(WPACKET *pkt, size_t maxsize)
{
    WPACKET_SUB *sub;
    size_t lenbytes;

    /* Internal API, so should not fail */
    assert(pkt->subs != NULL);
    if (pkt->subs == NULL)
        return 0;

    /* Find the WPACKET_SUB for the top level */
    for (sub = pkt->subs; sub->parent != NULL; sub = sub->parent)
        continue;

    lenbytes = sub->lenbytes;
    if (lenbytes == 0)
        lenbytes = sizeof(pkt->maxsize);

    if (maxmaxsize(lenbytes) < maxsize || maxsize < pkt->written)
        return 0;

    pkt->maxsize = maxsize;

    return 1;
}

int WPACKET_memcpy(WPACKET *pkt, const void *src, size_t len)
{
    unsigned char *dest;

    if (len == 0)
        return 1;

    if (!WPACKET_allocate_bytes(pkt, len, &dest))
        return 0;

    memcpy(dest, src, len);

    return 1;
}

int WPACKET_sub_memcpy__(WPACKET *pkt, const void *src, size_t len,
                         size_t lenbytes)
{
    if (!WPACKET_start_sub_packet_len__(pkt, lenbytes)
            || !WPACKET_memcpy(pkt, src, len)
            || !WPACKET_close(pkt))
        return 0;

    return 1;
}

int WPACKET_get_total_written(WPACKET *pkt, size_t *written)
{
    /* Internal API, so should not fail */
    assert(written != NULL);
    if (written == NULL)
        return 0;

    *written = pkt->written;

    return 1;
}

int WPACKET_get_length(WPACKET *pkt, size_t *len)
{
    /* Internal API, so should not fail */
    assert(pkt->subs != NULL && len != NULL);
    if (pkt->subs == NULL || len == NULL)
        return 0;

    *len = pkt->written - pkt->subs->pwritten;

    return 1;
}

void WPACKET_cleanup(WPACKET *pkt)
{
    WPACKET_SUB *sub, *parent;

    for (sub = pkt->subs; sub != NULL; sub = parent) {
        parent = sub->parent;
        OPENSSL_free(sub);
    }
    pkt->subs = NULL;
}
