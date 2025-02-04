/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2022 Micron Technology, Inc.  All rights reserved.
 */

#include <mtf/conditions.h>
#include <mtf/framework.h>
#include <mock/api.h>

#include <hse_util/inttypes.h>

#include <hse_ikvdb/cn.h>

#include <cn/route.h>
#include <cn/cn_tree_internal.h>

MTF_BEGIN_UTEST_COLLECTION(route_test)

MTF_DEFINE_UTEST(route_test, route_api_test)
{
    struct route_map *map;
    struct cn_tree_node tn;
    static const uint nodec = 16;
    struct route_node *rnodev[2 * nodec + 1], *rnode = NULL, *rnode2;
    char ekbuf[2 * nodec + 1][sizeof(rnode->rtn_keybuf)];
    uint eklen = 5, idx;
    char ekbuf_large[sizeof(rnode->rtn_keybuf) + 1];
    merr_t err;

    map = route_map_create(0);
    ASSERT_EQ(NULL, map);

    map = route_map_create(nodec * 2 + 1);
    ASSERT_NE(NULL, map);

    for (uint i = 0; i < 2 * nodec; i++) {
        uint64_t ekey;
        uint skip = 1;
        uint fmtarg = (i + 1) * skip - 1;

        ekey = cpu_to_be64(fmtarg);
        memcpy(ekbuf[i], (char *)&ekey + (sizeof(ekey) - eklen), eklen);
    }

    for (uint i = 0; i < 2 * nodec; i++) {
        char kbuf[sizeof(rnode->rtn_keybuf)];
        uint klen;

        rnodev[i] = route_map_insert(map, &tn, ekbuf[i], eklen);
        ASSERT_NE(NULL, rnodev[i]);

        ASSERT_EQ(true, route_node_isfirst(rnodev[0]));
        ASSERT_EQ(true, route_node_islast(rnodev[i]));

        rnode2 = route_map_last_node(map);
        ASSERT_EQ(rnode2, rnodev[i]);

        if (i > 0) {
            ASSERT_EQ(false, route_node_islast(rnodev[0]));
            ASSERT_EQ(false, route_node_isfirst(rnodev[i]));
        }

        ASSERT_EQ(&tn, route_node_tnode(rnodev[i]));

        route_node_keycpy(rnodev[i], kbuf, sizeof(rnode->rtn_keybuf), &klen);
        ASSERT_EQ(0, memcmp(kbuf, ekbuf[i], eklen));
        ASSERT_EQ(eklen, klen);
    }

    ASSERT_EQ(NULL, rnode);

    /* Delete odd numbered nodes */
    for (uint i = 1; i < 2 * nodec; i += 2) {
        rnode = route_map_lookup(map, ekbuf[i], eklen);
        ASSERT_EQ(rnode, rnodev[i]);

        route_map_delete(map, rnode);
    }

    /* Reinsert odd numbered nodes */
    for (uint i = 1; i < 2 * nodec; i += 2) {
        struct route_node *dup;

        rnodev[i] = route_node_alloc(map, &tn, ekbuf[i], eklen);
        ASSERT_NE(rnodev[i], NULL);

        dup = route_map_insert_by_node(map, rnodev[i]);
        ASSERT_EQ(dup, NULL);
    }

    /* Insert a node with large edge key when the node cache is empty */
    memset(ekbuf_large, 0xff, sizeof(ekbuf_large));
    rnode = route_map_insert(map, &tn, ekbuf_large, sizeof(ekbuf_large));
    ASSERT_NE(NULL, rnode);

    rnode2 = route_map_lookup(map, ekbuf_large, eklen);
    ASSERT_EQ(rnode, rnode2);

    memset(ekbuf_large, 0xf0, sizeof(ekbuf_large));
    err = route_node_key_modify(map, rnode, ekbuf_large, sizeof(ekbuf_large));
    ASSERT_EQ(0, err);

    rnode2 = route_map_lookup(map, ekbuf_large, eklen);
    ASSERT_EQ(rnode, rnode2);

    route_map_delete(map, rnode);

    idx = 5;
    rnode = route_map_lookup(map, ekbuf[idx], eklen); /* same as edge key */
    ASSERT_EQ(rnode, rnodev[idx]);

    ekbuf[idx][6] = 0xff;
    rnode = route_map_lookup(map, ekbuf[idx], eklen + 1); /* longer than edge key */
    ASSERT_EQ(rnode, rnodev[idx + 1]);

    ekbuf[idx][7] = 0xff;
    rnode = route_map_lookup(map, ekbuf[idx], eklen + 2);
    ASSERT_EQ(rnode, rnodev[idx + 1]);

    rnode = route_map_lookup(map, ekbuf[idx], eklen - 1); /* shorter than edge key */
    ASSERT_EQ(rnode, rnodev[0]);

    ekbuf[idx][eklen - 2] = 0xff;
    rnode = route_map_lookup(map, ekbuf[idx], eklen - 1);
    ASSERT_EQ(rnode, rnodev[2 * nodec - 1]);
    ekbuf[idx][eklen - 2] = 0x00;

    route_map_delete(NULL, rnodev[0]);
    route_map_delete(map, NULL);

    rnode = route_map_lookup(map, ekbuf[0], eklen);
    ASSERT_EQ(route_node_next(rnode), rnodev[1]);
    ASSERT_EQ(route_node_prev(rnode), NULL);

    rnode = route_map_lookup(map, ekbuf[nodec - 1], eklen);
    ASSERT_EQ(route_node_next(rnode), rnodev[nodec]);
    ASSERT_EQ(route_node_prev(rnode), rnodev[nodec - 2]);

    rnode = route_map_lookup(map, ekbuf[(2 * nodec) - 1], eklen);
    ASSERT_EQ(route_node_next(rnode), NULL);
    ASSERT_EQ(route_node_prev(rnode), rnodev[(2 * nodec) - 2]);

    for (uint i = 0; i < 2 * nodec; i++)
        route_map_delete(map, rnodev[i]);

    /* Insert a node with large edge key when the node cache is non-empty */
    memset(ekbuf_large, 0xff, sizeof(ekbuf_large));
    rnode = route_map_insert(map, &tn, ekbuf_large, sizeof(ekbuf_large));
    ASSERT_NE(NULL, rnode);

    rnode2 = route_map_lookup(map, ekbuf_large, eklen);
    ASSERT_EQ(rnode, rnode2);

    memset(ekbuf_large, 0xf0, sizeof(ekbuf_large));
    err = route_node_key_modify(map, rnode, ekbuf_large, sizeof(ekbuf_large));
    ASSERT_EQ(0, err);

    rnode2 = route_map_lookup(map, ekbuf_large, eklen);
    ASSERT_EQ(rnode, rnode2);

    route_map_delete(map, rnode);

    route_map_destroy(map);
}

MTF_END_UTEST_COLLECTION(route_test);
