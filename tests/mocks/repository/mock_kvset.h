/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#ifndef HSE_KVS_CN_TEST_MOCK_KVSET_H
#define HSE_KVS_CN_TEST_MOCK_KVSET_H

#include <arpa/inet.h> /* ntohl, htonl */

#include <cn/kv_iterator.h>

/**
 * struct mock_kvset - test harness
 * @tripwire:   deliberately inaccessible pages, must be first field
 * @len:        size of mock_kvset
 * @mk_entry:   mock_kvset list linkage
 * @iter_data: kvdata from mock_make_kvi, passed as ds in kvset_create (opt)
 * @nk:    number of kblk / vblk ids
 * @nv:
 * @dgen:  increments from 1 each call (first call is oldest kvset)
 * @ids[]: initd by mock_make_kvi
 */
struct mock_kvset {
    char                    tripwire[PAGE_SIZE * 3];
    struct kvset_list_entry entry;
    struct kvset_stats      stats;
    size_t                  alloc_sz;
    void *                  iter_data;
    int                     start;
    int                     ref;
    u64                     dgen;
    u64                     ids[];
};

enum val_mix { VMX_S32 = 1, VMX_BUF = 2, VMX_MIXED = 3 };

/*
 * struct nkv_tab - number of keys/values to generate
 */
struct nkv_tab {
    int          nkeys;
    int          key1;
    int          val1;
    enum val_mix vmix;
    int          be;
    u64          dgen;
};

/* Values for 'int be` member of struct nkv_tab */
#define KVDATA_BE_KEY true
#define KVDATA_INT_KEY false

/*
 * We cannot use the real kv_iterator, it is private in a .c file.
 * kv_iterator MUST be the first element in this struct.
 * This iterator traverses an array per kvset.
 */

struct mock_kv_iterator {
    struct kv_iterator kvi;
    char               tripwire[PAGE_SIZE * 3];
    struct mock_kvset *kvset;
    int                src;
    int                nextkey;
    void *             base;
    size_t             sz;
};

void
mock_kvset_set(void);
void
mock_kvset_unset(void);

void *
mock_vref_to_vdata(struct kv_iterator *kvi, uint vboff);

/*
 * These mock apis exist to faciliate test data creation.
 */

void
mock_kvset_data_reset(void);

struct kvset_meta;

merr_t
mock_make_kvi(struct kv_iterator **kvi, int src, struct kvs_rparams *rp, struct nkv_tab *nkv);

merr_t
mock_make_vblocks(struct kv_iterator **kvi, struct kvs_rparams *rp, int nv);

#endif
