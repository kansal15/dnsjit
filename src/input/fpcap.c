/*
 * Copyright (c) 2018, OARC, Inc.
 * All rights reserved.
 *
 * This file is part of dnsjit.
 *
 * dnsjit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dnsjit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dnsjit.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "input/fpcap.h"
#include "core/object/pcap.h"

#include <stdio.h>
#include <pthread.h>

#define MAX_SNAPLEN 0x40000

static core_log_t    _log      = LOG_T_INIT("input.fpcap");
static input_fpcap_t _defaults = {
    LOG_T_INIT_OBJ("input.fpcap"),
    0, 0,
    0, 0, 0,
    CORE_OBJECT_PCAP_INIT(0), 0,
    0, 10000, 100,
    0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0
};

struct _ctx {
    pthread_mutex_t m;
    pthread_cond_t  c;
    size_t          ref;
};

struct _prod_ctx {
    struct {
        uint32_t ts_sec;
        uint32_t ts_usec;
        uint32_t incl_len;
        uint32_t orig_len;
    } hdr;
    struct _ctx    ctx;
    size_t         n, m, buf_left;
    uint8_t*       bufp;
    unsigned short wait : 1;
    unsigned short conthdr : 1;
};
static struct _prod_ctx _prod_ctx_defaults = {
    { 0, 0, 0, 0 },
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 },
    0, 0, 0, 0,
    0, 0
};

static void _ref(core_object_t* obj, core_object_reference_t ref)
{
    struct _ctx* ctx = (struct _ctx*)obj->obj_refctx;

    pthread_mutex_lock(&ctx->m);
    if (ref == CORE_OBJECT_INCREF) {
        ctx->ref++;
    } else {
        ctx->ref--;
        if (!ctx->ref)
            pthread_cond_signal(&ctx->c);
    }
    pthread_mutex_unlock(&ctx->m);
}

core_log_t* input_fpcap_log()
{
    return &_log;
}

int input_fpcap_init(input_fpcap_t* self)
{
    struct _prod_ctx* ctx = malloc(sizeof(struct _prod_ctx));

    if (!self || !ctx) {
        free(ctx);
        return 1;
    }

    *self          = _defaults;
    *ctx           = _prod_ctx_defaults;
    self->prod_ctx = (void*)ctx;

    ldebug("init");

    return 0;
}

int input_fpcap_destroy(input_fpcap_t* self)
{
    if (!self) {
        return 1;
    }

    ldebug("destroy");

    if (self->file) {
        fclose(self->file);
    }
    free(self->buf);
    free(self->shared_pkts);
    free(self->prod_ctx);

    return 0;
}

static inline uint16_t _flip16(uint16_t u16)
{
    return ((u16 & 0xff) << 8) | (u16 >> 8);
}

static inline uint32_t _flip32(uint32_t u32)
{
    return ((u32 & 0xff) << 24) | ((u32 & 0xff00) << 8) | ((u32 & 0xff0000) >> 8) | (u32 >> 24);
}

int input_fpcap_open(input_fpcap_t* self, const char* file)
{
    int ret;

    if (!self || !file) {
        return 1;
    }

    if (self->file) {
        fclose(self->file);
        self->file = 0;
    }
    free(self->buf);
    self->buf = 0;
    free(self->shared_pkts);
    self->shared_pkts = 0;

    if (!(self->file = fopen(file, "rb"))) {
        return 1;
    }

    if ((ret = fread(&self->magic_number, 1, 24, self->file)) != 24) {
        fclose(self->file);
        self->file = 0;
        return 1;
    }
    switch (self->magic_number) {
    case 0x4d3cb2a1:
        self->is_nanosec = 1;
    case 0xd4c3b2a1:
        self->is_swapped    = 1;
        self->version_major = _flip16(self->version_major);
        self->version_minor = _flip16(self->version_minor);
        self->thiszone      = (int32_t)_flip32((uint32_t)self->thiszone);
        self->sigfigs       = _flip32(self->sigfigs);
        self->snaplen       = _flip32(self->snaplen);
        self->network       = _flip32(self->network);
        break;
    case 0xa1b2c3d4:
    case 0xa1b23c4d:
        break;
    default:
        fclose(self->file);
        self->file = 0;
        return 2;
    }

    if (self->snaplen > MAX_SNAPLEN) {
        lcritical("too large snaplen (%u)", self->snaplen);
        return 2;
    }

    if (self->version_major == 2 && self->version_minor == 4) {
        if (self->use_shared) {
            size_t n;

            self->buf_size = self->num_shared_pkts * 1500;
            if (!(self->buf = malloc(self->buf_size))) {
                fclose(self->file);
                self->file = 0;
                return 1;
            }

            if (!(self->shared_pkts = malloc(sizeof(core_object_pcap_t) * self->num_shared_pkts))) {
                fclose(self->file);
                self->file = 0;
                return 1;
            }

            for (n = 0; n < self->num_shared_pkts; n++) {
                self->shared_pkts[n].obj_type   = CORE_OBJECT_PCAP;
                self->shared_pkts[n].snaplen    = self->snaplen;
                self->shared_pkts[n].linktype   = self->network;
                self->shared_pkts[n].bytes      = 0;
                self->shared_pkts[n].is_swapped = self->is_swapped;
                self->shared_pkts[n].obj_ref    = _ref;

                self->shared_pkts[n].obj_refctx  = &((struct _prod_ctx*)self->prod_ctx)->ctx;
                self->shared_pkts[n].is_multiple = 1;
            }

            ((struct _prod_ctx*)self->prod_ctx)->bufp     = self->buf;
            ((struct _prod_ctx*)self->prod_ctx)->buf_left = self->buf_size;
        } else {
            if (!(self->buf = malloc(self->snaplen))) {
                fclose(self->file);
                self->file = 0;
                return 1;
            }

            self->prod_pkt.snaplen    = self->snaplen;
            self->prod_pkt.linktype   = self->network;
            self->prod_pkt.bytes      = (unsigned char*)self->buf;
            self->prod_pkt.is_swapped = self->is_swapped;
        }
        ldebug("pcap v%u.%u snaplen:%lu %s", self->version_major, self->version_minor, self->snaplen, self->is_swapped ? " swapped" : "");
        return 0;
    }

    fclose(self->file);
    self->file = 0;
    return 2;
}

static int _run(input_fpcap_t* self)
{
    struct {
        uint32_t ts_sec;
        uint32_t ts_usec;
        uint32_t incl_len;
        uint32_t orig_len;
    } hdr;
    core_object_pcap_t pkt = CORE_OBJECT_PCAP_INIT(0);

    pkt.snaplen    = self->snaplen;
    pkt.linktype   = self->network;
    pkt.bytes      = (unsigned char*)self->buf;
    pkt.is_swapped = self->is_swapped;

    while (fread(&hdr, 1, 16, self->file) == 16) {
        if (self->is_swapped) {
            hdr.ts_sec   = _flip32(hdr.ts_sec);
            hdr.ts_usec  = _flip32(hdr.ts_usec);
            hdr.incl_len = _flip32(hdr.incl_len);
            hdr.orig_len = _flip32(hdr.orig_len);
        }
        if (hdr.incl_len > self->snaplen) {
            return 2;
        }
        if (fread(self->buf, 1, hdr.incl_len, self->file) != hdr.incl_len) {
            return 3;
        }

        self->pkts++;

        pkt.ts.sec = hdr.ts_sec;
        if (self->is_nanosec) {
            pkt.ts.nsec = hdr.ts_usec;
        } else {
            pkt.ts.nsec = hdr.ts_usec * 1000;
        }
        pkt.caplen = hdr.incl_len;
        pkt.len    = hdr.orig_len;

        self->recv(self->ctx, (core_object_t*)&pkt);
    }

    return 0;
}

static int _run_shared(input_fpcap_t* self)
{
    struct {
        uint32_t ts_sec;
        uint32_t ts_usec;
        uint32_t incl_len;
        uint32_t orig_len;
    } hdr;
    struct _ctx ctx = {
        .m   = PTHREAD_MUTEX_INITIALIZER,
        .c   = PTHREAD_COND_INITIALIZER,
        .ref = 0
    };
    size_t   n, m, buf_left;
    uint8_t* bufp;

    for (n = 0; n < self->num_shared_pkts; n++) {
        self->shared_pkts[n].obj_refctx  = (void*)&ctx;
        self->shared_pkts[n].is_multiple = 1;
    }

    n        = 0;
    m        = 0;
    bufp     = self->buf;
    buf_left = self->buf_size;

    while (fread(&hdr, 1, 16, self->file) == 16) {
        if (self->is_swapped) {
            hdr.ts_sec   = _flip32(hdr.ts_sec);
            hdr.ts_usec  = _flip32(hdr.ts_usec);
            hdr.incl_len = _flip32(hdr.incl_len);
            hdr.orig_len = _flip32(hdr.orig_len);
        }
        if (hdr.incl_len > self->snaplen) {
            return 2;
        }
        if (hdr.incl_len > buf_left || n == self->num_shared_pkts) {
            if (m) {
                self->recv(self->ctx, (core_object_t*)&self->shared_pkts[n - 1]);
            }
            pthread_mutex_lock(&ctx.m);
            while (ctx.ref) {
                pthread_cond_wait(&ctx.c, &ctx.m);
            }
            pthread_mutex_unlock(&ctx.m);

            n        = 0;
            m        = 0;
            bufp     = self->buf;
            buf_left = self->buf_size;
        }
        if (fread(bufp, 1, hdr.incl_len, self->file) != hdr.incl_len) {
            return 3;
        }

        self->pkts++;

        self->shared_pkts[n].ts.sec = hdr.ts_sec;
        if (self->is_nanosec) {
            self->shared_pkts[n].ts.nsec = hdr.ts_usec;
        } else {
            self->shared_pkts[n].ts.nsec = hdr.ts_usec * 1000;
        }
        self->shared_pkts[n].caplen = hdr.incl_len;
        self->shared_pkts[n].len    = hdr.orig_len;
        self->shared_pkts[n].bytes  = bufp;

        if (!m) {
            self->shared_pkts[n].obj_prev = 0;
        } else {
            self->shared_pkts[n].obj_prev = (core_object_t*)&self->shared_pkts[n - 1];
        }
        m++;
        if (m == self->num_multiple_pkts) {
            self->recv(self->ctx, (core_object_t*)&self->shared_pkts[n]);
            m = 0;
        }

        bufp += hdr.incl_len;
        buf_left -= hdr.incl_len;
        n++;
    }

    if (m) {
        self->recv(self->ctx, (core_object_t*)&self->shared_pkts[n - 1]);
    }
    pthread_mutex_lock(&ctx.m);
    while (ctx.ref) {
        pthread_cond_wait(&ctx.c, &ctx.m);
    }
    pthread_mutex_unlock(&ctx.m);

    return 0;
}

int input_fpcap_run(input_fpcap_t* self)
{
    if (!self || !self->file || !self->recv) {
        return 1;
    }

    if (self->use_shared) {
        return _run_shared(self);
    }
    return _run(self);
}

static const core_object_t* _produce(void* ctx)
{
    input_fpcap_t* self = (input_fpcap_t*)ctx;
    struct {
        uint32_t ts_sec;
        uint32_t ts_usec;
        uint32_t incl_len;
        uint32_t orig_len;
    } hdr;

    if (!self) {
        return 0;
    }

    if (fread(&hdr, 1, 16, self->file) == 16) {
        if (self->is_swapped) {
            hdr.ts_sec   = _flip32(hdr.ts_sec);
            hdr.ts_usec  = _flip32(hdr.ts_usec);
            hdr.incl_len = _flip32(hdr.incl_len);
            hdr.orig_len = _flip32(hdr.orig_len);
        }
        if (hdr.incl_len > self->snaplen) {
            return 0;
        }
        if (fread(self->buf, 1, hdr.incl_len, self->file) != hdr.incl_len) {
            return 0;
        }

        self->pkts++;

        self->prod_pkt.ts.sec = hdr.ts_sec;
        if (self->is_nanosec) {
            self->prod_pkt.ts.nsec = hdr.ts_usec;
        } else {
            self->prod_pkt.ts.nsec = hdr.ts_usec * 1000;
        }
        self->prod_pkt.caplen = hdr.incl_len;
        self->prod_pkt.len    = hdr.orig_len;

        return (core_object_t*)&self->prod_pkt;
    }

    return 0;
}

static const core_object_t* _produce_shared(void* self_ctx)
{
    input_fpcap_t*    self = (input_fpcap_t*)self_ctx;
    struct _prod_ctx* ctx;

    if (!self) {
        return 0;
    }
    ctx = (struct _prod_ctx*)self->prod_ctx;
    if (!ctx) {
        return 0;
    }

    if (ctx->wait) {
        pthread_mutex_lock(&ctx->ctx.m);
        while (ctx->ctx.ref) {
            pthread_cond_wait(&ctx->ctx.c, &ctx->ctx.m);
        }
        pthread_mutex_unlock(&ctx->ctx.m);

        ctx->n        = 0;
        ctx->m        = 0;
        ctx->bufp     = self->buf;
        ctx->buf_left = self->buf_size;
        ctx->wait     = 0;
    }

    if (ctx->conthdr || fread(&ctx->hdr, 1, 16, self->file) == 16) {
        ctx->conthdr = 0;

        if (self->is_swapped) {
            ctx->hdr.ts_sec   = _flip32(ctx->hdr.ts_sec);
            ctx->hdr.ts_usec  = _flip32(ctx->hdr.ts_usec);
            ctx->hdr.incl_len = _flip32(ctx->hdr.incl_len);
            ctx->hdr.orig_len = _flip32(ctx->hdr.orig_len);
        }
        if (ctx->hdr.incl_len > self->snaplen) {
            return 0;
        }
        if (ctx->hdr.incl_len > ctx->buf_left || ctx->n == self->num_shared_pkts) {
            ctx->wait    = 1;
            ctx->conthdr = 1;
            return (core_object_t*)&self->shared_pkts[ctx->n - 1];
        }
        if (fread(ctx->bufp, 1, ctx->hdr.incl_len, self->file) != ctx->hdr.incl_len) {
            return 0;
        }

        self->pkts++;

        self->shared_pkts[ctx->n].ts.sec = ctx->hdr.ts_sec;
        if (self->is_nanosec) {
            self->shared_pkts[ctx->n].ts.nsec = ctx->hdr.ts_usec;
        } else {
            self->shared_pkts[ctx->n].ts.nsec = ctx->hdr.ts_usec * 1000;
        }
        self->shared_pkts[ctx->n].caplen = ctx->hdr.incl_len;
        self->shared_pkts[ctx->n].len    = ctx->hdr.orig_len;
        self->shared_pkts[ctx->n].bytes  = ctx->bufp;

        if (!ctx->m) {
            self->shared_pkts[ctx->n].obj_prev = 0;
        } else {
            self->shared_pkts[ctx->n].obj_prev = (core_object_t*)&self->shared_pkts[ctx->n - 1];
        }
        ctx->m++;
        if (ctx->m == self->num_multiple_pkts) {
            size_t n = ctx->n;

            ctx->m = 0;
            ctx->bufp += ctx->hdr.incl_len;
            ctx->buf_left -= ctx->hdr.incl_len;
            ctx->n++;
            return (core_object_t*)&self->shared_pkts[n];
        }

        ctx->bufp += ctx->hdr.incl_len;
        ctx->buf_left -= ctx->hdr.incl_len;
        ctx->n++;
    }

    if (ctx->m) {
        ctx->wait = 1;
        return (core_object_t*)&self->shared_pkts[ctx->n - 1];
    }

    pthread_mutex_lock(&ctx->ctx.m);
    while (ctx->ctx.ref) {
        pthread_cond_wait(&ctx->ctx.c, &ctx->ctx.m);
    }
    pthread_mutex_unlock(&ctx->ctx.m);

    return 0;
}

core_producer_t input_fpcap_producer(input_fpcap_t* self)
{
    if (self && self->use_shared) {
        return _produce_shared;
    }
    return _produce;
}
