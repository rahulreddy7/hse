/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2022 Micron Technology, Inc.  All rights reserved.
 */

#define MTF_MOCK_IMPL_csched_sp3

#include <bsd/string.h>

#include <hse/experimental.h>

#include <hse_util/alloc.h>
#include <hse_util/event_counter.h>
#include <hse_util/platform.h>
#include <hse_util/rest_api.h>
#include <hse_util/slab.h>

#include <hse_ikvdb/cn.h>
#include <hse_ikvdb/ikvdb.h>
#include <hse_ikvdb/kvdb_perfc.h>
#include <hse_ikvdb/csched.h>
#include <hse_ikvdb/sched_sts.h>
#include <hse_ikvdb/throttle.h>
#include <hse_ikvdb/kvdb_rparams.h>

#include "csched_sp3.h"
#include "csched_sp3_work.h"

#include "cn_tree_compact.h"
#include "cn_tree_internal.h"
#include "kvset.h"
#include "route.h"

struct mpool;

/*
 * The scheduler monitors multiple cn trees to determine what compaction jobs
 * to run and when to run them.
 *
 * Terminology
 * -----------
 *   r_node -- cn tree internal node
 *   l_node -- cn tree leaf node in
 *   r_xxx  -- something to do with internal nodes
 *   l_xxx  -- something to do with leaf nodes
 *
 * Why use "r_" instead of "i_"?  Two reasons.  First, use of "inode" might
 * cause confusion with files system inodes.  Second, "i" is virtually
 * indistinguishable from "l" in some fonts.  So we use "r".  The mnemonic for
 * "r" is "routing node" since, in a cn tree, internal nodes essentially form
 * a routing network to get data to leaf nodes.  The obvious overlap with "r"
 * for root node is unfortuntate, but not disastrous because, in a cn tree,
 * root nodes and internal nodes have almost identical treatment.
 *
 *
 * Threads
 * -------
 * There is one internal thread, referred to as the "monitor" thread,
 * started in sp3_create.
 *
 * There are several external threads that interact with the scheduler:
 *    - Ingest threads, created in c0
 *    - STS job threads, created in sts_create().
 *    - Open/Close threads (threads that call cn_open() and cn_close().
 *
 * Events
 * ------
 *    - Add / Remove trees (invoked by open/close threads)
 *    - Notify ingest complete (invoked ingest threads)
 *    - Notify compaction complete (invoked by job threads)
 *
 * Schedule Policy
 * ---------------
 * This scheduler manages the overall space amplification, which is often
 * referrred to as "space amp" or "samp".  Space amp is defined as:
 *
 *    samp = actual_kvdb_size / logical_kvdb_size
 *
 * where,
 *
 *    actual_kvdb_size = total media space used by kvdb
 *    logical_kvdb_size = sum of all key and value lengths after
 *                        eliminating duplicate and deleted entries
 *
 * We take some liberties in the computation of actual_kvdb_size:
 *    - We only count kblock and vblock capacities.
 *    - We do not count CNDB mlogs or any other mlogs.
 *    - We do not count mpool overhead.
 *
 * Let:
 *    R_SIZE = sum of all mblock sizes in all internal cn tree nodes
 *    L_SIZE = sum of all mblock sizes in all leaf cn tree nodes
 *    L_GOOD = estimated value of L_SIZE after full compaction
 *             of all leaf nodes.
 *
 * Our approach to controlling space amp is:
 *   - Track actual garbage in leaf nodes with hyperloglog.
 *   - Pretend internal nodes are 100% garbage (because we can't easily use
 *     hyperloglog on internal nodes).
 *   - Ensure L_SIZE is much larger than R_SIZE.
 *
 * We then compute the estimated space amp, SAMP_EST, as follows:
 *
 *    SAMP_EST = (L_SIZE + R_SIZE) / L_GOOD
 *
 * Due to the assumption that internal nodes are all garbage, SAMP_EST is an
 * upper bound on the actual space amp.  Let SAMP_MAX represent the maximum
 * allowed space amp.  This scheduler aims to keep SAMP_MAX >= SAMP_EST, or:
 *
 *    SAMP_MAX >= (L_SIZE + R_SIZE) / L_GOOD
 *
 * Note if R_SIZE decreases, SAMP_EST descreases.  The same is true if L_GOOD
 * increases.  This is how the scheduler manages space amp:
 *
 *    - If L_GOOD gets too small relative to L_SIZE, then compact leaf nodes.
 *    - If R_SIZE gets too large relative to L_SIZE, then spill internal nodes
 *      to leaves.
 *
 * Min/max for values L_GOOD:
 *    L_GOOD_MIN = L_SIZE / SAMP_MAX  // when R_SIZE == 0
 *    L_GOOD_MAX = L_SIZE             // no garbage in leaves
 *
 * Min/max for values R_SIZE:
 *    R_SIZE_MIN = 0                   // internal nodes empty
 *    R_SIZE_MAX = L_SIZE*(SAMP_MAX-1) // when L_GOOD == L_SIZE
 *
 * Summary:
 *    - Run-time parameters, with example values:
 *          csched_samp_max     - max space amp (1.5)
 *          csched_lo_th_pct  - space amp low water mark (25%)
 *          csched_hi_th_pct  - space amp low water mark (75%)
 *          csched_leaf_pct - percent data to keep in leaves (90%)
 *
 *    - Spill internal node data into leaf nodes to ensure:
 *          L_SIZE / (L_SIZE + R_SIZE) > csched_leaf_pct
 *
 *    - Compute samp high and low water marks (HWM, LWM) based on run-time
 *      parameters.
 *
 *    - If SAMP_EST exceeds HWM, enable leaf compaction to drive SAMP_EST to
 *      LWM. When it drops below LWM, disable leaf compaction.
 *
 *    - The scheduler also implements logic to limit individual node lengh and
 *      size because long nodes decrease query performance, and large nodes
 *      are hard to compact and spill.  This extra logic is not strictly
 *      required to manage space amp.
 */

#define CSCHED_SAMP_MAX_MIN  100
#define CSCHED_SAMP_MAX_MAX  999
#define CSCHED_LO_TH_PCT_MIN 5
#define CSCHED_LO_TH_PCT_MAX 95
#define CSCHED_HI_TH_PCT_MIN 5
#define CSCHED_HI_TH_PCT_MAX 95
#define CSCHED_LEAF_PCT_MIN  1
#define CSCHED_LEAF_PCT_MAX  99

struct sp3_qinfo {
    uint qjobs;
    uint qjobs_max;
};

/**
 * struct sp3 - kvdb scheduler policy
 * @ds:           to access mpool qos
 * @rp:           kvb run-time params
 * @sts:          short term scheduler
 * @running:      set to false by sp3_destroy
 * @sp_dlist_lock:  dirty-node/dirty-tree list lock
 * @sp_dlist_idx:   the current active dirty-node/dirty-tree list
 * @sp_dtree_listv: vector of lists of dirty trees with dirty nodes
 * @mon_tlist:    monitored trees
 * @spn_rlist:    list of all nodes ready for rspill
 * @spn_alist:    list of all nodes from all monitored trees
 * @new_tlist_lock: lock for list of new trees
 * @new_tlist:    list of new trees
 * @mon_lock:     mutex used with @mon_cv
 * @mon_signaled: set by sp3_monitor_wake()
 * @mon_cv:       monitor thread conditional var
 * @samp_reduce:  if true, compact while samp > LWM
 * @check_garbage_ns: used to stagger start of garbage jobs
 * @check_scatter_ns: used to stagger start of scatter jobs
 * @mon_wq:       monitor thread workqueue
 * @mon_work:     monitor thread work struct
 * @name:         name for logging and data tree
 */
struct sp3 {
    /* Accessed only by monitor thread */
    struct mpool            *ds;
    struct kvdb_rparams     *rp;
    struct sts              *sts;
    struct sp3_thresholds    thresh;
    struct throttle_sensor  *throttle_sensor_root;
    struct kvdb_health      *health;
    atomic_int               running;
    struct sp3_qinfo         qinfo[SP3_QNUM_MAX];

    struct rb_root rbt[wtype_MAX] HSE_L1D_ALIGNED;

    struct mutex     sp_dlist_lock HSE_L1D_ALIGNED;
    atomic_uint      sp_dlist_idx;
    struct list_head sp_dtree_listv[2];

    struct list_head mon_tlist HSE_L1D_ALIGNED;
    struct list_head spn_rlist;
    struct list_head spn_alist;
    atomic_int       sp_ingest_count;
    atomic_int       sp_prune_count;
    bool             sp_healthy;
    bool             idle;
    ulong            sp_ingest_ns;
    uint             sp_sval_min;
    uint             activity;
    uint             jobs_started;
    uint             jobs_finished;
    uint             jobs_max;
    uint             rr_wtype;
    u64              job_id;

    struct cn_compaction_work *wp;

    struct {
        /* mirror selected kvdb_rparams */
        u64 csched_samp_max;
        u64 csched_lo_th_pct;
        u64 csched_hi_th_pct;
        u64 csched_leaf_pct;
    } inputs;

    /* Working parameters, derived from kvdb_rparams mirrored
     * in 'struct inputs'.
     */
    uint samp_max;
    uint samp_hwm;
    uint samp_lwm;

    /* Current and target values for space amp and leaf percent.
     * Target refers the expected values after all active
     * compaction jobs finish.
     */
    bool samp_reduce;
    uint samp_curr;
    uint samp_targ;
    uint lpct_targ;

    uint64_t check_garbage_ns;
    uint64_t check_scatter_ns;
    u64 qos_log_ttl;

    /* Tree shape report */
    bool tree_shape_bad;

    struct cn_samp_stats samp;
    struct cn_samp_stats samp_wip;
    struct perfc_set     sched_pc;

    /* Accessed by monitor and infrequently by open/close threads */
    struct mutex     new_tlist_lock HSE_L1D_ALIGNED;
    struct list_head new_tlist;

    /* Accessed by monitor, open/close, ingest and jobs threads */
    struct mutex     mon_lock HSE_L1D_ALIGNED;
    bool             mon_signaled;
    struct cv        mon_cv;

    /* Accessed monitor and infrequently by job threads */
    struct mutex     work_list_lock HSE_L1D_ALIGNED;
    struct list_head work_list;

    u64  ucomp_prev_report_ns HSE_L1D_ALIGNED;
    bool ucomp_active;
    bool ucomp_canceled;

    /* The following fields are rarely touched.
     */
    struct workqueue_struct *mon_wq;
    struct work_struct mon_work;
    char name[];
};

/* cn_tree 2 sp3_tree */
#define tree2spt(_tree)     (&(_tree)->ct_sched.sp3t)
#define spt2tree(_spt)      container_of(_spt, struct cn_tree, ct_sched.sp3t)

/* Scale of kvdb rparms */
#define EXT_SCALE 100

/* Internal scale, to get better precision with scalar math.
 * ONE is defined simply for readability in
 * expressions such as '(1 + r) / r'.
 */
#define SCALE 10000
#define ONE   SCALE

/* Easy-ish access to run-time parameters */
#define debug_tree_life(_sp)   (csched_rp_dbg_tree_life((_sp)->rp))
#define debug_dirty_node(_sp)  (csched_rp_dbg_dirty_node((_sp)->rp))
#define debug_sched(_sp)       (csched_rp_dbg_sched((_sp)->rp))
#define debug_qos(_sp)         (csched_rp_dbg_qos((_sp)->rp))
#define debug_rbtree(_sp)      (csched_rp_dbg_rbtree((_sp)->rp))
#define debug_tree_shape(_sp)  (csched_rp_dbg_tree_shape((_sp)->rp))

static void
sp3_dirty_node(struct sp3 *sp, struct cn_tree_node *tn);

static inline bool
qfull(const struct sp3 *sp, uint qnum)
{
    const struct sp3_qinfo *qi = sp->qinfo + qnum;

    return qi->qjobs >= qi->qjobs_max;
}

static inline bool
qempty(const struct sp3 *sp, uint qnum)
{
    const struct sp3_qinfo *qi = sp->qinfo + qnum;

    return qi->qjobs == 0;
}

static inline uint
qthreads(struct sp3 *sp, uint qnum)
{
    const uint64_t rparam = sp->rp->csched_qthreads;

    return (rparam >> (qnum * 8)) & 0xff;
}

static inline double
safe_div(double numer, double denom)
{
    return denom == 0.0 ? 0.0 : numer / denom;
}

static inline double
scale2dbl(u64 samp)
{
    return (1.0 / SCALE) * samp;
}

static inline uint
samp_est(struct cn_samp_stats *s, uint scale)
{
    return scale * safe_div(s->i_alen + s->l_alen, s->i_alen + s->l_good);
}

static inline uint
samp_pct_leaves(struct cn_samp_stats *s, uint scale)
{
    return scale * safe_div(s->l_alen, s->i_alen + s->l_alen);
}

static inline uint
samp_pct_garbage(struct cn_samp_stats *s, uint scale)
{
    assert(s->l_alen >= s->l_good);

    return scale * safe_div(s->l_alen - s->l_good, s->l_alen);
}

static void
sp3_monitor_wake(struct sp3 *sp)
{
    /* Signal monitor thread (our cv_signal requres lock to be held). */
    mutex_lock(&sp->mon_lock);
    sp->mon_signaled = true;
    cv_signal(&sp->mon_cv);
    mutex_unlock(&sp->mon_lock);
}

static bool
sp3_tree_is_managed(struct cn_tree *tree)
{
    struct sp3_tree *spt = tree2spt(tree);

    /* Use link as indicator of scheduler's knowledge of tree.
     * Relies on use of list_del_init when removing items from list.
     */
    return spt->spt_tlink.next && !list_empty(&spt->spt_tlink);
}

static void
sp3_samp_target(struct sp3 *sp, struct cn_samp_stats *ss)
{
    ss->i_alen = sp->samp.i_alen + sp->samp_wip.i_alen;
    ss->l_alen = sp->samp.l_alen + sp->samp_wip.l_alen;
    ss->l_good = sp->samp.l_good + sp->samp_wip.l_good;
}

static void
sp3_log_progress(struct cn_compaction_work *w, struct cn_merge_stats *ms, bool final)
{
    struct cn_work_est *est = &w->cw_est;
    double              progress;
    double              vblk_read_efficiency;
    const char *        msg_type;
    u64                 qt, pt, bt, ct;

    if (final) {
        msg_type = "final";
        progress = 1.0 * ms->ms_keys_in / est->cwe_keys;
        qt = w->cw_t1_qtime ? (w->cw_t1_qtime - w->cw_t0_enqueue) / 1000 : 0;
        pt = w->cw_t2_prep ? (w->cw_t2_prep - w->cw_t1_qtime) / 1000 : 0;
        bt = w->cw_t3_build ? (w->cw_t3_build - w->cw_t2_prep) / 1000 : 0;
        ct = w->cw_t4_commit ? (w->cw_t4_commit - w->cw_t3_build) / 1000 : 0;

    } else {
        msg_type = "progress";
        progress = 1.0 * w->cw_stats.ms_keys_in / est->cwe_keys;
        qt = pt = bt = ct = 0;
    }

    vblk_read_efficiency =
        safe_div(1.0 * ms->ms_val_bytes_out, ms->ms_vblk_read1.op_size + ms->ms_vblk_read2.op_size);

    log_info(
        "type=%s job=%u comp=%s rule=%s "
        "cnid=%lu nodeid=%lu leaf=%u pct=%3.1f "
        "vrd_eff=%.3f "
        "kblk_alloc_ops=%ld kblk_alloc_sz=%ld "
        "kblk_alloc_ns=%ld kblk_write_ops=%ld kblk_write_sz=%ld "
        "kblk_write_ns=%ld vblk_alloc_ops=%ld vblk_alloc_sz=%ld "
        "vblk_alloc_ns=%ld vblk_write_ops=%ld vblk_write_sz=%ld "
        "vblk_write_ns=%ld vblk_read1_ops=%ld vblk_read1_sz=%ld "
        "vblk_read1_ns=%ld vblk_read1wait_ops=%ld vblk_read1wait_ns=%ld "
        "vblk_read2_ops=%ld vblk_read2_sz=%ld vblk_read2_ns=%ld "
        "vblk_read2wait_ops=%ld vblk_read2wait_ns=%ld "
        "kblk_write_ops=%ld kblk_write_sz=%ld kblk_write_ns=%ld "
        "kblk_readwait_ops=%ld kblk_readwait_ns=%ld "
        "vblk_dbl_reads=%ld "
        "queue_us=%lu prep_us=%lu build_us=%lu commit_us=%lu",
        msg_type, w->cw_job.sj_id, cn_action2str(w->cw_action), cn_rule2str(w->cw_rule),
        w->cw_tree->cnid, w->cw_node->tn_nodeid, (uint)cn_node_isleaf(w->cw_node), 100 * progress,
        vblk_read_efficiency,
        ms->ms_kblk_alloc.op_cnt, ms->ms_kblk_alloc.op_size,
        ms->ms_kblk_alloc.op_time, ms->ms_kblk_write.op_cnt, ms->ms_kblk_write.op_size,
        ms->ms_kblk_write.op_time, ms->ms_vblk_alloc.op_cnt, ms->ms_vblk_alloc.op_size,
        ms->ms_vblk_alloc.op_time, ms->ms_vblk_write.op_cnt, ms->ms_vblk_write.op_size,
        ms->ms_vblk_write.op_time, ms->ms_vblk_read1.op_cnt, ms->ms_vblk_read1.op_size,
        ms->ms_vblk_read1.op_time, ms->ms_vblk_read1_wait.op_cnt, ms->ms_vblk_read1_wait.op_time,
        ms->ms_vblk_read2.op_cnt, ms->ms_vblk_read2.op_size, ms->ms_vblk_read2.op_time,
        ms->ms_vblk_read2_wait.op_cnt, ms->ms_vblk_read2_wait.op_time,
        ms->ms_kblk_read.op_cnt, ms->ms_kblk_read.op_size, ms->ms_kblk_read.op_time,
        ms->ms_kblk_read_wait.op_cnt, ms->ms_kblk_read_wait.op_time,
        ms->ms_vblk_wasted_reads, qt, pt, bt, ct);
}

static void
sp3_refresh_samp(struct sp3 *sp)
{
    u64 samp, lwm, hwm, leaf, r;
    u64 good_max, good_min;
    u64 good_hwm, good_lwm;
    u64 samp_hwm, samp_lwm;
    u64 range;

    bool csched_samp_max_changed = sp->inputs.csched_samp_max != sp->rp->csched_samp_max,
         csched_lo_th_pct_changed = sp->inputs.csched_lo_th_pct != sp->rp->csched_lo_th_pct,
         csched_hi_th_pct_changed = sp->inputs.csched_hi_th_pct != sp->rp->csched_hi_th_pct,
         csched_leaf_pct_changed = sp->inputs.csched_leaf_pct != sp->rp->csched_leaf_pct;

    /* Early return if nothing changed */
    if (!csched_samp_max_changed && !csched_lo_th_pct_changed && !csched_hi_th_pct_changed &&
        !csched_leaf_pct_changed)
        return;

    if (csched_samp_max_changed) {
        const u64 new_val =
            clamp_t(u64, sp->rp->csched_samp_max, CSCHED_SAMP_MAX_MIN, CSCHED_SAMP_MAX_MAX);

        log_info("sp3 kvdb_rparam csched_samp_max changed from %lu to %lu",
                 (ulong)sp->inputs.csched_samp_max,
                 (ulong)new_val);
        sp->inputs.csched_samp_max = new_val;
    }
    if (csched_lo_th_pct_changed) {
        const u64 new_val =
            clamp_t(u64, sp->rp->csched_lo_th_pct, CSCHED_LO_TH_PCT_MIN, CSCHED_LO_TH_PCT_MAX);

        log_info("sp3 kvdb_rparam csched_lo_th_pct changed from %lu to %lu",
                 (ulong)sp->inputs.csched_lo_th_pct,
                 (ulong)new_val);
        sp->inputs.csched_lo_th_pct = new_val;
    }
    if (csched_hi_th_pct_changed) {
        const u64 new_val =
            clamp_t(u64, sp->rp->csched_hi_th_pct, CSCHED_HI_TH_PCT_MIN, CSCHED_HI_TH_PCT_MAX);

        log_info("sp3 kvdb_rparam csched_hi_th_pct changed from %lu to %lu",
                 (ulong)sp->inputs.csched_hi_th_pct,
                 (ulong)new_val);
        sp->inputs.csched_hi_th_pct = new_val;
    }
    if (csched_leaf_pct_changed) {
        const u64 new_val =
            clamp_t(u64, sp->rp->csched_leaf_pct, CSCHED_LEAF_PCT_MIN, CSCHED_LEAF_PCT_MAX);

        log_info("sp3 kvdb_rparam csched_leaf_pct changed from %lu to %lu",
                 (ulong)sp->inputs.csched_leaf_pct,
                 (ulong)new_val);
        sp->inputs.csched_leaf_pct = new_val;
    }

    log_info("sp3 new samp input params: samp %lu, lwm_pct %lu, hwm_pct %lu, leaf_pct %lu",
             (ulong)sp->inputs.csched_samp_max,
             (ulong)sp->inputs.csched_lo_th_pct,
             (ulong)sp->inputs.csched_hi_th_pct,
             (ulong)sp->inputs.csched_leaf_pct);

    /* Input params (from kvdb_rparams) are scaled up by 100.
     * Internally we scale up by SCALE (10000) to get more
     * resolution.  Multiply each input param by SCALE/100 to
     * convert to our internal scale factor.
     */
    samp = sp->inputs.csched_samp_max * SCALE / EXT_SCALE;
    lwm = sp->inputs.csched_lo_th_pct * SCALE / EXT_SCALE;
    hwm = sp->inputs.csched_hi_th_pct * SCALE / EXT_SCALE;
    leaf = sp->inputs.csched_leaf_pct * SCALE / EXT_SCALE;
    r = ONE - leaf;

    /* "Good" is the fraction of leaf data that is not garbage.
     * A value of 1.0 means no garbage, 0.30 means 70% garbage.
     * The max good value is 1.0.  The min good value is:
     *
     *   good_min = (1 + R) / S
     *
     * where R is the ratio of internal to leaf and S is the space
     * amp.  For example, if we aim to keep 90% of data in leaves,
     * and S=1.3, then R=0.1 (10% in non-leaves), and:
     *
     *   good_min =  1.1 / 1.3 = 0.84
     *
     * This means, w/ 10% of data in non-leaves, that we
     * assume is garbage, we can't let good drop below 84%,
     * alternatively, we can't get leaf garbage exceed 16%.
     *
     * The low and high water marks are precentages in the range
     * between good_min and good_max (1.0).
     */
    good_max = ONE;
    good_min = SCALE * (ONE + r) / samp;
    range = good_max - good_min;

    good_lwm = good_min + (ONE - lwm) * range / SCALE;
    good_hwm = good_min + (ONE - hwm) * range / SCALE;

    samp_lwm = SCALE * (ONE + r) / good_lwm;
    samp_hwm = SCALE * (ONE + r) / good_hwm;

    /* save in sp3 struct */
    sp->samp_lwm = samp_lwm;
    sp->samp_hwm = samp_hwm;
    sp->samp_max = samp;

    log_info("sp3 samp derived params:"
             " samp lo/hi/max: %.3f %.3f %.3f"
             " good/leaf ratio min/lo/hi: %.3f %.3f %.3f",
             scale2dbl(sp->samp_lwm),
             scale2dbl(sp->samp_hwm),
             scale2dbl(sp->samp_max),
             scale2dbl(good_min),
             scale2dbl(good_lwm),
             scale2dbl(good_hwm));
}

static void
sp3_refresh_thresholds(struct sp3 *sp)
{
    struct sp3_thresholds thresh = {};
    struct sp3_node *spn;
    uint64_t v;

    /* root node spill settings */
    v = sp->rp->csched_rspill_params;
    if (v) {
        thresh.rspill_runlen_max = (v >> 0) & 0xff;
        thresh.rspill_runlen_min = (v >> 8) & 0xff;
        thresh.rspill_wlen_max = ((v >> 16) & 0xffff) << 20;
    } else {
        thresh.rspill_runlen_max = SP3_RSPILL_RUNLEN_MAX_DEFAULT;
        thresh.rspill_runlen_min = SP3_RSPILL_RUNLEN_MIN_DEFAULT;
        thresh.rspill_wlen_max = SP3_RSPILL_WLEN_MAX_DEFAULT;
    }

    thresh.rspill_runlen_max = clamp_t(uint8_t, thresh.rspill_runlen_max,
                                       SP3_RSPILL_RUNLEN_MIN, SP3_RSPILL_RUNLEN_MAX);

    thresh.rspill_runlen_min = clamp_t(uint8_t, thresh.rspill_runlen_min,
                                       SP3_RSPILL_RUNLEN_MIN, thresh.rspill_runlen_max);

    thresh.rspill_wlen_max = clamp_t(size_t, thresh.rspill_wlen_max,
                                     SP3_RSPILL_WLEN_MIN, SP3_RSPILL_WLEN_MAX);

    /* leaf node compaction settings */
    v = sp->rp->csched_leaf_comp_params;
    if (v) {
        thresh.lcomp_runlen_max = (v >> 0) & 0xff;
        thresh.lcomp_join_pct = (v >> 16) & 0xff;
        thresh.lcomp_split_keys = ((v >> 24) & 0xff) << 22;
    } else {
        thresh.lcomp_runlen_max = SP3_LCOMP_RUNLEN_MAX_DEFAULT;
        thresh.lcomp_join_pct = SP3_LCOMP_JOIN_PCT_DEFAULT;
        thresh.lcomp_split_keys = SP3_LCOMP_SPLIT_KEYS_DEFAULT;
    }

    thresh.lcomp_runlen_max = clamp_t(uint, thresh.lcomp_runlen_max,
                                      SP3_LCOMP_RUNLEN_MAX_MIN, SP3_LCOMP_RUNLEN_MAX_MAX);

    thresh.lcomp_join_pct = clamp_t(uint, thresh.lcomp_join_pct,
                                    SP3_LCOMP_JOIN_PCT_MIN, SP3_LCOMP_JOIN_PCT_MAX);

    thresh.lcomp_split_keys = clamp_t(uint, thresh.lcomp_split_keys,
                                      SP3_LCOMP_SPLIT_KEYS_MIN, SP3_LCOMP_SPLIT_KEYS_MAX);

    /* leaf node length settings */
    v = sp->rp->csched_leaf_len_params;
    if (v) {
        thresh.llen_runlen_max = (v >> 0) & 0xff;
        thresh.llen_runlen_min = (v >> 8) & 0xff;
        thresh.llen_idlec = (v >> 24) & 0xff;
        thresh.llen_idlem = (v >> 32) & 0xff;
    } else {
        thresh.llen_runlen_max = SP3_LLEN_RUNLEN_MAX_DEFAULT;
        thresh.llen_runlen_min = SP3_LLEN_RUNLEN_MIN_DEFAULT;
        thresh.llen_idlec = SP3_LLEN_IDLEC_DEFAULT;
        thresh.llen_idlem = SP3_LLEN_IDLEM_DEFAULT;
    }

    thresh.llen_runlen_max = clamp_t(uint8_t, thresh.llen_runlen_max,
                                     SP3_LLEN_RUNLEN_MIN, SP3_LLEN_RUNLEN_MAX);

    thresh.llen_runlen_min = clamp_t(uint8_t, thresh.llen_runlen_min,
                                     SP3_LLEN_RUNLEN_MIN, thresh.llen_runlen_max);

    /* vgroup leaf-scatter remediation settings
     */
    thresh.lscat_runlen_max = sp->rp->csched_lscat_runlen_max;
    thresh.lscat_hwm = sp->rp->csched_lscat_hwm;

    thresh.split_cnt_max = qthreads(sp, SP3_QNUM_SPLIT);

    /* If thresholds have not changed there's nothing to do.  Otherwise, need to
     * recompute work trees.
     */
    if (!memcmp(&thresh, &sp->thresh, sizeof(thresh)))
        return;

    sp->thresh = thresh;

    list_for_each_entry(spn, &sp->spn_alist, spn_alink) {
        sp3_dirty_node(sp, spn2tn(spn));
    }

    log_info("sp3 thresholds: rspill: min/max/wlenmb %u/%u/%lu, lcomp: max/pct/keys %u/%u%%/%u,"
             " llen: min/max %u/%u, idlec: %u, idlem: %u, lscat: hwm/max %u/%u split %u",
             thresh.rspill_runlen_min, thresh.rspill_runlen_max, thresh.rspill_wlen_max >> 20,
             thresh.lcomp_runlen_max, thresh.lcomp_join_pct, thresh.lcomp_split_keys >> 20,
             thresh.llen_runlen_min, thresh.llen_runlen_max,
             thresh.llen_idlec, thresh.llen_idlem,
             thresh.lscat_hwm, thresh.lscat_runlen_max,
             thresh.split_cnt_max);
}

static void
sp3_refresh_worker_counts(struct sp3 *sp)
{
    sp->jobs_max = 0;

    for (size_t i = 0; i < NELEM(sp->qinfo); i++) {
        sp->qinfo[i].qjobs_max = qthreads(sp, i);
        sp->jobs_max += sp->qinfo[i].qjobs_max;
    }
}

static void
sp3_refresh_settings(struct sp3 *sp)
{
    sp3_refresh_samp(sp);
    sp3_refresh_worker_counts(sp);
    sp3_refresh_thresholds(sp);
}

/*****************************************************************
 *
 * SP3 user-initiated compaction (ucomp)
 *
 */

static void
sp3_ucomp_cancel(struct sp3 *sp)
{
    if (!sp->ucomp_active) {
        log_info("ignoring request to cancel user-initiated"
                 " compaction because there is no active request");
        return;
    }

    log_info("canceling user-initiated compaction");

    sp->ucomp_active = false;
    sp->ucomp_canceled = true;
}

static void
sp3_ucomp_start(struct sp3 *sp)
{
    if (sp->ucomp_active)
        log_info("restarting user-initiated compaction (was already active)");
    else
        log_info("starting user-initiated compaction");

    sp->ucomp_active = true;
    sp->ucomp_canceled = false;
    sp->samp_reduce = true;
}

static void
sp3_ucomp_report(struct sp3 *sp, bool final)
{
    uint curr = samp_est(&sp->samp, 100);

    if (final) {

        log_info("user-initiated compaction complete: space_amp %u.%02u",
                 curr / 100, curr % 100);

    } else {

        u64  started = sp->jobs_started;
        u64  finished = sp->jobs_finished;
        uint goal = sp->samp_lwm * 100 / SCALE;

        log_info("user-initiated compaction in progress:"
                 " jobs: active %lu, started %lu, finished %lu;"
                 " space_amp: current %u.%02u, goal %u.%02u;",
                 started - finished,
                 started,
                 finished,
                 curr / 100,
                 curr % 100,
                 goal / 100,
                 goal % 100);
    }
}

static void
sp3_ucomp_check(struct sp3 *sp)
{
    if (sp->ucomp_active) {

        bool completed = sp->idle || sp->samp_curr < sp->samp_lwm;
        u64  now = get_time_ns();
        bool report = now > sp->ucomp_prev_report_ns + 5 * NSEC_PER_SEC;

        if (completed) {
            sp->ucomp_active = false;
            sp->ucomp_canceled = false;
        }

        if (completed || report) {
            sp->ucomp_prev_report_ns = now;
            sp3_ucomp_report(sp, completed);
        }
    }
}

/*****************************************************************
 *
 * SP3 red-black trees
 *
 */

static void
sp3_rb_erase(struct rb_root *root, struct sp3_rbe *rbe)
{
    if (!RB_EMPTY_NODE(&rbe->rbe_node)) {
        rb_erase(&rbe->rbe_node, root);
        RB_CLEAR_NODE(&rbe->rbe_node);
    }
}

static void
sp3_rb_insert(struct rb_root *root, struct sp3_rbe *new_node)
{
    struct rb_node **link = &root->rb_node;
    struct rb_node * parent = 0;
    u64              weight = new_node->rbe_weight;

    assert(RB_EMPTY_NODE(&new_node->rbe_node));

    while (*link) {

        struct sp3_rbe *this;

        this = rb_entry(*link, struct sp3_rbe, rbe_node);
        parent = *link;

        if (weight > this->rbe_weight)
            link = &(*link)->rb_left;
        else if (weight < this->rbe_weight)
            link = &(*link)->rb_right;
        else {
            assert((u64)new_node != (u64)this);
            if ((u64)new_node > (u64)this)
                link = &(*link)->rb_left;
            else
                link = &(*link)->rb_right;
        }
    }

    rb_link_node(&new_node->rbe_node, parent, link);
    rb_insert_color(&new_node->rbe_node, root);
}

static void
sp3_node_init(struct sp3 *sp, struct sp3_node *spn)
{
    spn->spn_initialized = true;

    for (uint tx = 0; tx < NELEM(spn->spn_rbe); tx++)
        RB_CLEAR_NODE(&spn->spn_rbe[tx].rbe_node);

    INIT_LIST_HEAD(&spn->spn_rlink);
    INIT_LIST_HEAD(&spn->spn_alink);

    /* Append to list of all nodes from all managed trees.
     */
    list_add_tail(&spn->spn_alink, &sp->spn_alist);
}

static void
sp3_node_insert(struct sp3 *sp, struct sp3_node *spn, uint tx, u64 weight)
{
    struct rb_root *root = sp->rbt + tx;
    struct sp3_rbe *rbe = spn->spn_rbe + tx;

    assert(tx < NELEM(spn->spn_rbe));

    if (!RB_EMPTY_NODE(&rbe->rbe_node)) {
        if (rbe->rbe_weight == weight)
            return;

        sp3_rb_erase(root, rbe);
    }

    rbe->rbe_weight = weight;
    sp3_rb_insert(root, rbe);
}

static void
sp3_node_remove(struct sp3 *sp, struct sp3_node *spn, uint tx)
{
    struct rb_root *root = sp->rbt + tx;
    struct sp3_rbe *rbe = spn->spn_rbe + tx;

    sp3_rb_erase(root, rbe);
}

static void
sp3_node_unlink(struct sp3 *sp, struct sp3_node *spn)
{
    uint tx;

    for (tx = 0; tx < NELEM(spn->spn_rbe); tx++)
        sp3_rb_erase(sp->rbt + tx, spn->spn_rbe + tx);
}

static void
sp3_node_unlink_all(struct sp3 *sp, struct sp3_node *spn)
{
    assert(spn->spn_initialized);

    sp3_node_unlink(sp, spn);
    list_del_init(&spn->spn_rlink);
    list_del_init(&spn->spn_alink);
}

static void
sp3_dirty_node_locked(struct sp3 *sp, struct cn_tree_node *tn)
{
    const struct cn_node_stats *ns = &tn->tn_ns;
    struct cn_tree *tree = tn->tn_tree;
    struct sp3_node *spn = tn2spn(tn);
    uint64_t nkvsets_total, nkvsets;
    uint garbage = 0, jobs;
    uint scatter = 0;

    if (!spn->spn_initialized)
        return;

    ev_debug(1);
    jobs = atomic_read_acq(&tn->tn_busycnt);

    nkvsets_total = cn_ns_kvsets(ns);
    nkvsets = nkvsets_total - (jobs & 0xffffu);
    jobs >>= 16;

    /* We disallow scheduling more than one job of any given type on
     * any given leaf node (technically we could schedule more, but
     * the effects have proven deleterious in practice).
     *
     * Similarly, we never schedule more than three jobs on any given
     * root node (see CSCHED_QTHREADS_DEFAULT for default limits).
     */

    if (cn_node_isroot(tn)) {

        /* If this root node is ready to spill then ensure it's on the list
         * in FIFO order, retaining its current position if it's already on
         * the list.  List order is otherwise managed by sp3_check_roots().
         */
        if (nkvsets >= sp->thresh.rspill_runlen_min && jobs < 3) {
            if (list_empty(&spn->spn_rlink))
                list_add_tail(&spn->spn_rlink, &sp->spn_rlist);
        } else {
            list_del_init(&spn->spn_rlink);
        }
    } else {
        if (!tn->tn_route_node)
            abort();

        /* Node splits and joins are rare, but once a node has committed to split
         * or join it must be done as soon as possible as there could be rspill
         * threads waiting for the job to complete.  Hence, pending split/join
         * jobs prevent other compaction jobs from starting on the same node.
         */
        if (tn->tn_ss_splitting || tn->tn_ss_joining) {
            sp3_node_remove(sp, spn, wtype_length);
            sp3_node_remove(sp, spn, wtype_scatter);
            sp3_node_remove(sp, spn, wtype_garbage);
        } else if (nkvsets > 0 && jobs < 1) {
            const uint64_t keys_uniq = cn_ns_keys_uniq(ns);
            const uint64_t keys = cn_ns_keys(ns);
            const uint64_t tombs = cn_ns_tombs(ns);
            struct cn_tree_node *left;
            uint64_t weight;

            garbage = samp_pct_garbage(&tn->tn_samp, 100);
            scatter = cn_tree_node_scatter(tn);

            /* Leaf nodes sorted by vgroup scatter and garbage.
             */
            if (scatter > 0) {
                weight = ((uint64_t)scatter << 32) | garbage;

                sp3_node_insert(sp, spn, wtype_scatter, weight);
            } else {
                sp3_node_remove(sp, spn, wtype_scatter);
            }

            /* Leaf nodes sorted by number of kvsets.
             * We use inverse scatter as a secondary discriminant so as to
             * prefer scatter jobs over kcompactions when scatter is high.
             */
            if (nkvsets >= sp->thresh.llen_runlen_min) {
                weight = (nkvsets << 32) | (UINT32_MAX - scatter);

                if (nkvsets > sp->thresh.llen_runlen_max * 2) {
                    sp3_node_remove(sp, spn, wtype_scatter);
                    ev_debug(scatter > 0);
                }

                sp3_node_insert(sp, spn, wtype_length, weight);
            } else {
                sp3_node_remove(sp, spn, wtype_length);
            }

            /* Leaf nodes sorted by pct garbage.  We use alen as the secondary
             * discriminant to prefer nodes with higher total bytes of garbage.
             */
            if (tombs * 100 > keys_uniq * 95 || keys == 0) {
                garbage = 100;
                weight = ((uint64_t)garbage << 32) | (cn_ns_alen(ns) >> 20);

                /* Accelerate GC if the preponderance of keys are tombs,
                 * or if the node contains only ptombs (i.e., keys == 0).
                 * Use 95% to account for observed hlog estimation error,
                 * but would hlog-provided error bounds be better?
                 */
                sp3_node_unlink(sp, spn);
                sp3_node_insert(sp, spn, wtype_garbage, weight);
            } else if (garbage > 0) {
                weight = ((uint64_t)garbage << 32) | (cn_ns_alen(ns) >> 20);

                sp3_node_insert(sp, spn, wtype_garbage, weight);
            } else {
                sp3_node_remove(sp, spn, wtype_garbage);
            }

            /* Schedule a split if this node is splittable and there is
             * room in the tree for more nodes.  Splits prevent all other
             * potentially large compaction jobs as they could otherwise
             * delay the split far beyond the split thresholds.
             */
            if (sp3_work_splittable(tn, &sp->thresh) && tree->ct_fanout < CN_FANOUT_MAX) {
                if (keys > (32ul << 20))
                    sp3_node_remove(sp, spn, wtype_length);
                if (garbage < 100)
                    sp3_node_remove(sp, spn, wtype_garbage);
                sp3_node_remove(sp, spn, wtype_scatter);
                sp3_node_insert(sp, spn, wtype_split, keys);
                ev_debug(1);
            } else {
                sp3_node_remove(sp, spn, wtype_split);
            }

            /* Schedule a join if this node and it's left neighbor are
             * joinable.  Accelerate the join if the left node is empty.
             */
            left = sp3_work_joinable(tn, &sp->thresh);
            if (left) {
                weight = UINT64_MAX - cn_ns_kvsets(&left->tn_ns);

                if (cn_ns_kvsets(&left->tn_ns) == 0)
                    sp3_node_unlink(sp, spn);
                sp3_node_insert(sp, spn, wtype_join, weight);
                ev_debug(1);
            } else {
                sp3_node_remove(sp, spn, wtype_join);
            }

        } else if (nkvsets_total == 0) {
            struct cn_tree_node *left, *right;

            /* This node (tn) is empty, but sp3_work() cannot handle empty nodes,
             * therefore tn cannot be the anchor of a join.  So check to see if
             * the neighbor to the right can be the anchor, and let tn be the
             * left node of the join.
             */
            right = list_next_entry_or_null(tn, tn_link, &tree->ct_nodes);
            if (right) {
                left = sp3_work_joinable(right, &sp->thresh);
                if (left == tn) {
                    const uint64_t weight = UINT64_MAX - cn_ns_kvsets(&right->tn_ns);

                    sp3_node_insert(sp, tn2spn(right), wtype_join, weight);
                    ev_debug(1);
                }
            }
        }
    }

    /* Nodes sorted by idle check expiration time.
     * Time is a negative offset in 4-second intervals from
     * UINT32_MAX in order to work correctly with the rb-tree
     * weight comparator logic.
     */
    if (nkvsets >= sp->thresh.llen_idlec && sp->thresh.llen_idlem > 0 && jobs < 1) {
        uint64_t ttl = (sp->thresh.llen_idlem * 60) / 4;
        uint64_t weight;

        /* Reduce idle wait time if ptombs are present in the node.
         */
        if (cn_ns_ptombs(ns)) {
            ttl = 60 / 4;
            ev_debug(1);
        }

        weight = UINT32_MAX - (jclock_ns >> 32) - ttl;
        weight = (weight << 32) | nkvsets;

        sp3_node_insert(sp, spn, wtype_idle, weight);
    } else {
        sp3_node_remove(sp, spn, wtype_idle);
    }

    if (debug_dirty_node(sp)) {
        log_info(
            "cnid=%lu nodeid=%-2lu kvsets=%-2lu "
            "keys=%lu uniq=%lu tombs=%lu ptombs=%lu "
            "alen=%lu clen=%lu "
            "garbage=%u scatter=%u",
            tn->tn_tree->cnid, tn->tn_nodeid, nkvsets_total,
            cn_ns_keys(ns), cn_ns_keys_uniq(ns), cn_ns_tombs(ns), cn_ns_ptombs(ns),
            cn_ns_alen(ns), cn_ns_clen(ns),
            garbage, scatter);
    }
}

static void
sp3_dirty_node(struct sp3 *sp, struct cn_tree_node *tn)
{
    void *lock;

    rmlock_rlock(&tn->tn_tree->ct_lock, &lock);
    sp3_dirty_node_locked(sp, tn);
    rmlock_runlock(lock);
}

static void
sp3_process_workitem(struct sp3 *sp, struct cn_compaction_work *w)
{
    struct sp3_tree *spt = tree2spt(w->cw_tree);
    struct cn_samp_stats diff;

    assert(spt->spt_job_cnt > 0);
    assert(w->cw_qnum < SP3_QNUM_MAX);
    assert(sp->qinfo[w->cw_qnum].qjobs > 0);
    assert(sp->jobs_started > sp->jobs_finished);

    spt->spt_job_cnt--;

    sp->qinfo[w->cw_qnum].qjobs--;
    sp->jobs_finished++;

    cn_samp_diff(&diff, &w->cw_samp_post, &w->cw_samp_pre);

    sp->samp.r_alen += diff.r_alen;
    sp->samp.r_wlen += diff.r_wlen;
    sp->samp.i_alen += diff.i_alen;
    sp->samp.l_alen += diff.l_alen;
    sp->samp.l_good += diff.l_good;

    sp->samp_wip.i_alen -= w->cw_est.cwe_samp.i_alen;
    sp->samp_wip.l_alen -= w->cw_est.cwe_samp.l_alen;
    sp->samp_wip.l_good -= w->cw_est.cwe_samp.l_good;

    if (w->cw_action == CN_ACTION_SPILL) {
        struct cn_tree *tree = w->cw_tree;
        uint64_t dt;

        dt = (get_time_ns() - w->cw_t0_enqueue) / w->cw_kvset_cnt;
        if (tree->ct_rspill_dt == 0)
            dt *= 2;

        tree->ct_rspill_dt = (tree->ct_rspill_dt + dt) / 2;
    }

    if (w->cw_debug & (CW_DEBUG_PROGRESS | CW_DEBUG_FINAL))
        sp3_log_progress(w, &w->cw_stats, true);

    sts_job_done(&w->cw_job);
    free(w);
}

static void
sp3_process_ingest(struct sp3 *sp)
{
    struct cn_tree *tree;

    list_for_each_entry(tree, &sp->mon_tlist, ct_sched.sp3t.spt_tlink) {
        struct sp3_tree *spt = tree2spt(tree);
        long alen, wlen;

        if (atomic_read_acq(&sp->sp_ingest_count) == 0)
            break;

        /* [HSE_REVISIT] Given inopportune concurrency with sp3_op_notify_ingest()
         * there's a small window where alen and wlen could be acquired relatively
         * inconsistently.  The discrepancy will be reflected in samp until after
         * the next ingest in which we can acquire a stable view.
         */
        alen = atomic_read(&spt->spt_ingest_alen);
        wlen = atomic_read(&spt->spt_ingest_wlen);
        if (alen) {
            atomic_dec(&sp->sp_ingest_count);

            atomic_sub(&spt->spt_ingest_alen, alen);
            sp->samp.i_alen += alen;
            sp->samp.r_alen += alen;

            atomic_sub(&spt->spt_ingest_wlen, wlen);
            sp->samp.r_wlen += wlen;

            sp3_dirty_node(sp, tree->ct_root);
            sp->activity++;
        }
    }
}

static void
sp3_process_dirtylist(struct sp3 *sp)
{
    struct sp3_tree *spt, *spt_next;
    uint idx;

    ev_debug(1);

    /* Swap the active and stable dirty lists so that we can operate
     * on the stable lists without the lock.
     */
    mutex_lock(&sp->sp_dlist_lock);
    idx = sp->sp_dlist_idx++ % NELEM(sp->sp_dtree_listv);
    mutex_unlock(&sp->sp_dlist_lock);

    /* Process the list of dirty trees, each should contain at least one dirty node.
     */
    list_for_each_entry_safe(spt, spt_next, &sp->sp_dtree_listv[idx], spt_dtree_linkv[idx]) {
        struct cn_tree *tree = spt2tree(spt);
        struct cn_tree_node *tn, *tn_next;
        struct list_head joined;
        uint ndirty HSE_MAYBE_UNUSED;
        void *lock;

        /* Delete spt from the dirty tree list and reinit spt's dirty link so that
         * sp3_dirty_node_enqueue() can detect whether or not spt is on the list.
         */
        list_del_init(&spt->spt_dtree_linkv[idx]);
        ev_debug(1);

        INIT_LIST_HEAD(&joined);
        ndirty = 0;

        /* Verify that the action didn't dislodge the root node
         * from the head of the nodes list.
         */
        rmlock_rlock(&tree->ct_lock, &lock);
        assert(tree->ct_root == list_first_entry(&tree->ct_nodes, struct cn_tree_node, tn_link));

        list_for_each_entry_safe(tn, tn_next, &spt->spt_dnode_listv[idx], tn_dnode_linkv[idx]) {
            struct sp3_node *spn = tn2spn(tn);

            /* Delete tn from the dirty node list and reinit tn's dirty link so that
             * sp3_dirty_node_enqueue() can detect whether or not tn is on the list.
             */
            list_del_init(&tn->tn_dnode_linkv[idx]);
            ndirty++;
            ev_debug(1);

            /* If this leaf node has no route node pointer then it must be the
             * left node of a successful join, so we put it on the "joined" list
             * and will remove it from the tree it under the tree write lock.
             */
            if (cn_node_isleaf(tn) && !tn->tn_route_node) {
                list_add_tail(&tn->tn_dnode_linkv[idx], &joined);
                sp3_node_unlink_all(sp, spn);
                continue;
            }

            if (!sp->sp_healthy)
                continue;

            /* If this spn node is uninitialized then it must have been
             * created by a node split operation and this must be the
             * first time that csched has seen it.
             */
            if (!spn->spn_initialized)
                sp3_node_init(sp, spn);

            sp3_dirty_node_locked(sp, tn);
        }
        rmlock_runlock(lock);

        assert(ndirty > 0);

        if (list_empty(&joined))
            continue;

        /* Remove from the tree all the nodes that were on the left side of a join.
         */
        rmlock_wlock(&tree->ct_lock);
        list_for_each_entry(tn, &joined, tn_dnode_linkv[idx]) {
            struct cn_tree_node *right;

            /* Get the neighbor to the right then remove tn from the tree.
             */
            right = list_next_entry_or_null(tn, tn_link, &tree->ct_nodes);
            list_del(&tn->tn_link);
            tree->ct_fanout--;

            tn->tn_nodeid = UINT64_MAX;
            tn->tn_tree = NULL;

            /* Update the neighbor to the right to see if it can join
             * with it's new left neighbor.
             */
            if (right)
                sp3_dirty_node_locked(sp, right);
        }
        rmlock_wunlock(&tree->ct_lock);

        /* There shouldn't be any users of these tree nodes at this point, although
         * cursors and REST could still have references to the kvsets that used
         * to be in these nodes.
         */
        list_for_each_entry_safe(tn, tn_next, &joined, tn_dnode_linkv[idx])
            cn_node_free(tn);
    }

    ev_debug(1);
}

static void
sp3_process_worklist(struct sp3 *sp)
{
    struct cn_compaction_work *w, *next;
    struct list_head list;

    INIT_LIST_HEAD(&list);

    /* Move completed work from shared list to private list */
    mutex_lock(&sp->work_list_lock);
    list_splice_tail(&sp->work_list, &list);
    INIT_LIST_HEAD(&sp->work_list);
    mutex_unlock(&sp->work_list_lock);

    list_for_each_entry_safe(w, next, &list, cw_sched_link) {
        list_del(&w->cw_sched_link);
        sp3_process_workitem(sp, w);
        sp->activity++;
    }
}

static void
sp3_process_new_trees(struct sp3 *sp)
{
    struct cn_tree * tree, *tmp;
    struct list_head list;

    INIT_LIST_HEAD(&list);

    /* Move new trees from shared list to private list */
    mutex_lock(&sp->new_tlist_lock);
    list_splice_tail(&sp->new_tlist, &list);
    INIT_LIST_HEAD(&sp->new_tlist);
    mutex_unlock(&sp->new_tlist_lock);

    list_for_each_entry_safe(tree, tmp, &list, ct_sched.sp3t.spt_tlink) {
        struct sp3_tree *spt = tree2spt(tree);
        struct cn_tree_node *tn;
        void *lock;

        if (debug_tree_life(sp))
            log_info("sp3 acquire tree cnid %lu", (ulong)tree->cnid);

        rmlock_rlock(&tree->ct_lock, &lock);
        cn_tree_foreach_node(tn, tree) {
            sp3_node_init(sp, tn2spn(tn));
            sp3_dirty_node_locked(sp, tn);
        }
        rmlock_runlock(lock);

        sp->samp.r_alen += tree->ct_samp.r_alen;
        sp->samp.r_wlen += tree->ct_samp.r_wlen;
        sp->samp.i_alen += tree->ct_samp.i_alen;
        sp->samp.l_alen += tree->ct_samp.l_alen;
        sp->samp.l_good += tree->ct_samp.l_good;

        /* Move to the monitor's list. */
        list_del(&spt->spt_tlink);
        list_add(&spt->spt_tlink, &sp->mon_tlist);

        sp->activity++;
    }
}

static void
sp3_prune_trees(struct sp3 *sp)
{
    struct cn_tree *tree, *tmp;

    if (atomic_read_acq(&sp->sp_prune_count) == 0)
        return;

    list_for_each_entry_safe(tree, tmp, &sp->mon_tlist, ct_sched.sp3t.spt_tlink) {
        struct sp3_tree *spt = tree2spt(tree);
        struct cn_tree_node *tn;
        void *lock;
        bool busy;

        if (atomic_read(&spt->spt_enabled))
            continue;

        busy = (spt->spt_job_cnt > 0);

        /* Remove all this tree's nodes from the work queues to prevent
         * new jobs from starting, with the exception of pending split
         * and join jobs which must be allowed to complete.
         */
        rmlock_rlock(&tree->ct_lock, &lock);
        cn_tree_foreach_node(tn, tree) {
            struct sp3_node *spn = tn2spn(tn);

            if (tn->tn_ss_splitting || tn->tn_ss_joining) {
                log_info("waiting on %lu, %d", tn->tn_nodeid, tn->tn_ss_joining);
                busy = true;
                continue;
            }

            sp3_node_unlink_all(sp, spn);
        }
        rmlock_runlock(lock);

        if (busy)
            continue;

        if (debug_tree_life(sp))
            log_info("sp3 release tree cnid %lu", (ulong)tree->cnid);

        list_del_init(&spt->spt_tlink);

        if (sp->samp.i_alen >= tree->ct_samp.i_alen)
            sp->samp.i_alen -= tree->ct_samp.i_alen;
        if (sp->samp.r_alen >= tree->ct_samp.r_alen)
            sp->samp.r_alen -= tree->ct_samp.r_alen;
        if (sp->samp.r_wlen >= tree->ct_samp.r_wlen)
            sp->samp.r_wlen -= tree->ct_samp.r_wlen;
        if (sp->samp.l_alen >= tree->ct_samp.l_alen)
            sp->samp.l_alen -= tree->ct_samp.l_alen;
        if (sp->samp.l_good >= tree->ct_samp.l_good)
            sp->samp.l_good -= tree->ct_samp.l_good;

        cn_ref_put(tree->cn);

        sp->activity++;

        if (0 == atomic_dec_return(&sp->sp_prune_count))
            break;
    }
}

static void
sp3_dirty_node_enqueue(struct sp3 *sp, struct cn_tree_node *tn)
{
    struct sp3_tree *spt = tree2spt(tn->tn_tree);
    uint idx; /* active dirty-list index */

    mutex_lock(&sp->sp_dlist_lock);
    idx = sp->sp_dlist_idx % NELEM(sp->sp_dtree_listv);

    /* Append the tree node to the active dirty-node list if not already on it.
     * Append the spt tree to the active dirty-tree list if not already on it.
     */
    if (list_empty(&tn->tn_dnode_linkv[idx])) {
        list_add_tail(&tn->tn_dnode_linkv[idx], &spt->spt_dnode_listv[idx]);

        if (list_empty(&spt->spt_dtree_linkv[idx]))
            list_add_tail(&spt->spt_dtree_linkv[idx], &sp->sp_dtree_listv[idx]);
    }
    mutex_unlock(&sp->sp_dlist_lock);
}

/**
 * The follwing functions are callbacks used by compaction threads to notify
 * csched of various compaction related events.
 *
 * BEWARE! Most fields in sp are for private, single-threaded use by the
 * csched monitor thread and must NEVER be accessed by external threads.
 *
 * sp3_work_checkpoint() - notify csched of a completed incremental spill
 * sp3_work_complete() - notify csched of a completed compaction job
 * sp3_work_progress() - update csched with a compaction job's progress
 */

static void
sp3_work_checkpoint(struct cn_compaction_work *w)
{
    sp3_dirty_node_enqueue(w->cw_sched, w->cw_output_nodev[0]);
}

static void
sp3_work_complete(struct cn_compaction_work *w)
{
    struct sp3 *sp = w->cw_sched;

    if (w->cw_action == CN_ACTION_SPLIT) {
        if (w->cw_split.nodev[0])
            sp3_dirty_node_enqueue(sp, w->cw_split.nodev[0]);
        if (w->cw_split.nodev[1])
            sp3_dirty_node_enqueue(sp, w->cw_split.nodev[1]);
    } else if (w->cw_action == CN_ACTION_JOIN) {
        sp3_dirty_node_enqueue(sp, w->cw_join);
        sp3_dirty_node_enqueue(sp, w->cw_node);
    } else {
        sp3_dirty_node_enqueue(sp, w->cw_node);
    }

    mutex_lock(&sp->work_list_lock);
    list_add_tail(&w->cw_sched_link, &sp->work_list);
    mutex_unlock(&sp->work_list_lock);

    sp3_monitor_wake(sp);
}

static void
sp3_work_progress(struct cn_compaction_work *w)
{
    struct cn_merge_stats ms;

    if (!(w->cw_debug & CW_DEBUG_PROGRESS))
        return;

    /* compute change in merge stats from previous progress report */
    cn_merge_stats_diff(&ms, &w->cw_stats, &w->cw_stats_prev);
    memcpy(&w->cw_stats_prev, &w->cw_stats, sizeof(w->cw_stats_prev));

    sp3_log_progress(w, &ms, false);
}

static void
sp3_comp_thread_name(
    char *              buf,
    size_t              bufsz,
    enum cn_action      action,
    enum cn_rule        rule,
    uint64_t            nodeid)
{
    const char *a = "XX";
    const char *r = "XX";

    switch (action) {
    case CN_ACTION_NONE:
        a = "no";
        break;

    case CN_ACTION_COMPACT_K:
        a = "kc";
        break;

    case CN_ACTION_COMPACT_KV:
        a = "kv";
        break;

    case CN_ACTION_SPILL:
        a = "sp";
        break;

    case CN_ACTION_SPLIT:
        a = "ns";
        break;

    case CN_ACTION_JOIN:
        a = "nj";
        break;
    }

    switch (rule) {
    case CN_RULE_NONE:
        r = "xx";
        break;
    case CN_RULE_INGEST:
        r = "s0";
        break;
    case CN_RULE_RSPILL:
        r = "sr";
        break;
    case CN_RULE_TSPILL:
        r = "st";
        break;
    case CN_RULE_ZSPILL:
        r = "sz";
        break;
    case CN_RULE_SPLIT:
    case CN_RULE_LSPLIT:
    case CN_RULE_RSPLIT:
        r = "ns";
        break;
    case CN_RULE_GARBAGE:
        r = "gb";
        break;
    case CN_RULE_LENGTH_MIN:
        r = "ls";
        break;
    case CN_RULE_LENGTH_MAX:
        r = "ll";
        break;
    case CN_RULE_LENGTH_WLEN:
        r = "lw";
        break;
    case CN_RULE_LENGTH_VWLEN:
        r = "lv";
        break;
    case CN_RULE_LENGTH_CLEN:
        r = "lc";
        break;
    case CN_RULE_INDEX:
        r = "li";
        break;
    case CN_RULE_COMPC:
        r = "cc";
        break;
    case CN_RULE_IDLE_INDEX:
        r = "ii";
        break;
    case CN_RULE_IDLE_SIZE:
        r = "is";
        break;
    case CN_RULE_IDLE_TOMB:
        r = "it";
        break;
    case CN_RULE_SCATTERF:
        r = "fs";
        break;
    case CN_RULE_SCATTERP:
        r = "ps";
        break;
    case CN_RULE_JOIN:
        r = "nj";
        break;
    }

    snprintf(buf, bufsz, "hse_%s_%s_%lu", a, r, nodeid);
}

/* This function is the sts job-print callback which is invoked
 * with the sts run-queue lock held and hence must not block.
 * priv is a pointer to a 64-byte block for our private use,
 * zeroed before the first call.  job is set to NULL on the
 * last call to allow us to clean up any lingering state.
 */
static int
sp3_job_print(struct sts_job *job, void *priv, char *buf, size_t bufsz)
{
    struct cn_compaction_work *w = container_of(job, typeof(*w), cw_job);
    struct job_print_state {
        int jobwidth;
        bool hdr;
    } *jps = priv;
    int n = 0, m = 0;
    char tmbuf[32];
    ulong tm;

    if (!job) {
        return (jps->hdr) ? snprintf(buf, bufsz, "\n") : 0;
    }

    if (!jps->hdr) {
        jps->jobwidth = snprintf(NULL, 0, "%4u", sts_job_id_get(&w->cw_job) * 10);

        n = snprintf(buf, bufsz,
                     "%3s %5s %*s %7s %-7s"
                     " %2s %1s %5s %6s %6s %4s"
                     " %4s %5s %3s %3s %4s"
                     " %6s %6s %6s %6s"
                     " %8s %4s %s\n",
                     "ID", "NODE", jps->jobwidth, "JOB", "ACTION", "RULE",
                     "Q", "T", "KVSET", "ALEN", "CLEN", "PCAP",
                     "CC", "DGEN", "NH", "NK", "NV",
                     "RALEN", "IALEN", "LALEN", "LGOOD",
                     "WMESG", "TIME", "TNAME");

        if (n < 1 || n >= bufsz)
            return n;

        jps->hdr = true;
        bufsz -= n;
        buf += n;
    }

    tm = (jclock_ns - w->cw_t0_enqueue) / NSEC_PER_SEC;
    snprintf(tmbuf, sizeof(tmbuf), "%lu:%02lu", (tm / 60) % 60, tm % 60);

    m = snprintf(buf, bufsz,
                 "%3lu %5lu %*u %7s %-7s"
                 " %2u %1u %2u,%-2u %6lu %6lu %4u"
                 " %4u %5lu %3u %3u %4u"
                 " %6ld %6ld %6ld %6ld"
                 " %8.8s %4s %s\n",
                 w->cw_tree->cnid,
                 w->cw_node->tn_nodeid,
                 jps->jobwidth, sts_job_id_get(&w->cw_job),
                 cn_action2str(w->cw_action), cn_rule2str(w->cw_rule),
                 w->cw_qnum,
                 atomic_read(&w->cw_node->tn_busycnt) >> 16,
                 w->cw_kvset_cnt, (uint)cn_ns_kvsets(&w->cw_ns),
                 cn_ns_alen(&w->cw_ns) >> 20,
                 cn_ns_clen(&w->cw_ns) >> 20,
                 w->cw_ns.ns_pcap,
                 w->cw_compc,
                 w->cw_dgen_hi_min,
                 w->cw_nh, w->cw_nk, w->cw_nv,
                 w->cw_est.cwe_samp.r_alen >> 20,
                 w->cw_est.cwe_samp.i_alen >> 20,
                 w->cw_est.cwe_samp.l_alen >> 20,
                 w->cw_est.cwe_samp.l_good >> 20,
                 sts_job_wmesg_get(&w->cw_job),
                 tmbuf, w->cw_threadname);

    return (m < 1) ? m : (n + m);
}

static void
sp3_comp_slice_cb(struct sts_job *job)
{
    struct cn_compaction_work *w = container_of(job, typeof(*w), cw_job);

    cn_compact(w);

    sp3_work_complete(w);
}

static void
sp3_submit(struct sp3 *sp, struct cn_compaction_work *w, uint qnum)
{
    struct cn_tree_node *tn = w->cw_node;
    struct sp3_tree *    spt = tree2spt(w->cw_tree);

    assert(qnum < SP3_QNUM_MAX);

    sp3_comp_thread_name(
        w->cw_threadname,
        sizeof(w->cw_threadname),
        w->cw_action,
        w->cw_rule,
        tn->tn_nodeid);

    w->cw_iter_flags = kvset_iter_flag_fullscan;
    w->cw_io_workq = NULL;

    switch (csched_rp_kvset_iter(sp->rp)) {
        case csched_rp_kvset_iter_sync:
            /* synchronous mblock read */
            break;

        case csched_rp_kvset_iter_mcache:
            /* mache maps */
            w->cw_iter_flags |= kvset_iter_flag_mcache;
            break;

        case csched_rp_kvset_iter_async:
        default:
            /* async mblock read */
            w->cw_io_workq = cn_get_io_wq(w->cw_tree->cn);
            break;
    }

    /* Force compaction reads to use mcache if the value blocks for this node reside on
     * the pmem media class. This is not accurate if the mclass policy is changed during
     * subsequent kvs opens, which results in a mix of media classes for the k/vblocks
     * in this node. However, this is not a correctness issue and will recover on its own
     * after a series of compaction operations.
     */
    if (cn_tree_node_mclass(tn, HSE_MPOLICY_DTYPE_VALUE) == HSE_MCLASS_PMEM) {
        w->cw_iter_flags |= kvset_iter_flag_mcache;
        w->cw_io_workq = NULL;
    }

    w->cw_sched = sp;
    w->cw_checkpoint = sp3_work_checkpoint;
    w->cw_progress = sp3_work_progress;
    w->cw_prog_interval = nsecs_to_jiffies(NSEC_PER_SEC);
    w->cw_debug = csched_rp_dbg_comp(sp->rp);
    w->cw_qnum = qnum;

    sp->samp_wip.i_alen += w->cw_est.cwe_samp.i_alen;
    sp->samp_wip.l_alen += w->cw_est.cwe_samp.l_alen;
    sp->samp_wip.l_good += w->cw_est.cwe_samp.l_good;

    spt->spt_job_cnt++;

    assert(!qfull(sp, qnum));
    sp->qinfo[qnum].qjobs++;
    sp->jobs_started++;
    sp->job_id++;
    sp->activity++;

    sts_job_init(&w->cw_job, sp3_comp_slice_cb, sp->job_id);
    sts_job_submit(sp->sts, &w->cw_job);

    if (debug_sched(sp) || (w->cw_debug & CW_DEBUG_START)) {
        const struct cn_node_stats *ns = &w->cw_ns;
        const ulong hll_pct = cn_ns_keys(ns) ? ((100 * ns->ns_keys_uniq) / cn_ns_keys(ns)) : 0;
        const uint busycnt = atomic_read(&w->cw_node->tn_busycnt) >> 16;

        log_info(
            "job=%u jcnt=%u bcnt=%u qnum=%u reduce=%d "
            "cnid=%lu comp=%s rule=%s nodeid=%lu "
            "c_nk=%u c_nv=%u c_kvsets=%u "
            "nd_kvsets=%u nd_keys=%lu nd_hll%%=%lu nd_clen_mb=%lu "
            "samp=%u",
            w->cw_job.sj_id, spt->spt_job_cnt, busycnt, w->cw_qnum, sp->samp_reduce,
            w->cw_tree->cnid, cn_action2str(w->cw_action), cn_rule2str(w->cw_rule), w->cw_node->tn_nodeid,
            w->cw_nk, w->cw_nv, w->cw_kvset_cnt,
            cn_ns_kvsets(ns), cn_ns_keys(ns), hll_pct, cn_ns_clen(ns) >> MB_SHIFT,
            cn_ns_samp(ns));
    }
}

static bool
sp3_check_roots(struct sp3 *sp, uint qnum)
{
    struct sp3_node *spn, *next;
    uint debug;

    debug = csched_rp_dbg_comp(sp->rp);

    /* Each node on the rspill list had at least rspill_runlen_min kvsets
     * available when we scheduled this work request.
     */
    list_for_each_entry_safe(spn, next, &sp->spn_rlist, spn_rlink) {
        bool have_work;

        if (sp3_work(spn, wtype_root, &sp->thresh, debug, &sp->wp))
            return false;

        have_work = sp->wp->cw_action != CN_ACTION_NONE;
        if (have_work) {

            /* Move to end of list to prevent this node
             * from starving other nodes on the list.
             */
            if (!list_is_last(&spn->spn_rlink, &sp->spn_rlist)) {
                list_del(&spn->spn_rlink);
                list_add_tail(&spn->spn_rlink, &sp->spn_rlist);
            }

            sp3_submit(sp, sp->wp, qnum);
            sp->wp = NULL;
            return true;
        }

        if (sp->wp->cw_resched)
            continue;

        /* There are either too many active jobs or insufficient kvsets to start
         * a new job right now so drop this work request. sp3_dirty_node() will
         * re-assess the situation when the node composition changes.
         */
        list_del_init(&spn->spn_rlink);
    }

    return false;
}

static void
sp3_rb_dump(struct sp3 *sp, uint tx, uint count_max)
{
    struct rb_root *     root;
    struct rb_node *     rbn;
    struct sp3_rbe *     rbe;
    struct sp3_node *    spn;
    struct cn_tree_node *tn;
    uint                 count;

    if (tx >= NELEM(sp->rbt))
        return;

    /* spn_rbe must be first element in sp3_node struct in order for
     * '(void *)(rbe - tx)' to map rbe back to the sp3_node struct.
     */
    assert(offsetof(typeof(*spn), spn_rbe) == 0);

    count = 0;
    root = sp->rbt + tx;
    for (rbn = rb_first(root); rbn; rbn = rb_next(rbn)) {

        rbe = rb_entry(rbn, struct sp3_rbe, rbe_node);
        spn = (void *)(rbe - tx);
        tn = spn2tn(spn);

        log_info("cn_rbt rbt=%u item=%u weight=%lx cnid=%lu nodeid=%lu len=%u ialen_b=%ld "
            "lalen_b=%ld lgood_b=%ld lgarb_b=%ld", tx, count, rbe->rbe_weight,
            tn->tn_tree->cnid, tn->tn_nodeid, cn_ns_kvsets(&tn->tn_ns), tn->tn_samp.i_alen,
            tn->tn_samp.l_alen, tn->tn_samp.l_good, tn->tn_samp.l_alen - tn->tn_samp.l_good);

        if (count++ == count_max)
            break;
    }
}

static void
sp3_tree_shape_log(const struct cn_tree_node *tn, bool bad, const char *category)
{
    ulong hll_pct;
    const struct cn_node_stats *ns;

    if (!tn)
        return;

    ns = &tn->tn_ns;
    hll_pct = cn_ns_keys(ns) ? ((100 * ns->ns_keys_uniq) / cn_ns_keys(ns)) : 0;

    log_info("type=%s status=%s cnid=%lu nodeid=%lu "
        "nd_kvsets=%u nd_alen_mb=%lu nd_wlen_mb=%lu "
        "nd_clen_mb=%lu nd_hll%%=%lu nd_samp=%u",
        category, bad ? "bad" : "good", tn->tn_tree->cnid, tn->tn_nodeid,
        cn_ns_kvsets(ns), cn_ns_alen(ns) >> MB_SHIFT, cn_ns_wlen(ns) >> MB_SHIFT,
        cn_ns_clen(ns) >> MB_SHIFT, hll_pct, cn_ns_samp(ns));
}

/**
 * sp3_tree_shape_check() - report on tree shape
 * @sp: scheduler context
 *
 * Log a warning message if tree shape transitions from "good" to
 * "bad".  While shape is bad, periodically log messages providing
 * detail about shape and scheduler activity to assist
 * troubleshooting.
 *
 * Notes:
 * - There's nothing scientific about the thresholds for "bad tree"
 *   status, they were simply chosen to be high enough to hopefully
 *   not cause false alarms.
 * - The scheduler does not directly manage all these metrics, which
 *   means a tree might be flagged as bad and the scheduler won't
 *   purposefully fix it (e.g., there's no rule to directly limit the
 *   length of a leaf node).
 * - Largest internal node is not tracked because the scheduler
 *   doesn't manage internal nodes by size.
 */
static void
sp3_tree_shape_check(struct sp3 *sp)
{
    const uint rlen_thresh = 48;
    const uint llen_thresh = 20;
    const uint lsiz_thresh = 140;

    struct cn_tree_node *rlen_node = 0; /* longest root node */
    struct cn_tree_node *llen_node = 0; /* longest leaf node */
    struct cn_tree_node *lsiz_node = 0; /* largest leaf node */

    uint rlen = 0;
    uint llen = 0;
    uint lsiz = 0;
    uint lclen = 0;
    bool log = debug_tree_shape(sp);
    bool bad;

    struct cn_tree *tree;

    list_for_each_entry(tree, &sp->mon_tlist, ct_sched.sp3t.spt_tlink) {
        struct cn_tree_node *tn = tree->ct_root;
        uint8_t ekbuf[HSE_KVS_KEY_LEN_MAX];
        void *lock;
        uint len;

        rmlock_rlock(&tree->ct_lock, &lock);
        len = cn_ns_kvsets(&tn->tn_ns);

        if (!rlen_node || len > rlen) {
            rlen_node = tn;
            rlen = len;
        }

        cn_tree_foreach_leaf(tn, tree) {
            uint pcap = tn->tn_ns.ns_pcap;

            len = cn_ns_kvsets(&tn->tn_ns);

            if (!llen_node || len > llen) {
                llen_node = tn;
                llen = len;
            }

            if (!lsiz_node || pcap > lsiz) {
                lsiz_node = tn;
                lsiz = pcap;
                lclen = cn_ns_clen(&tn->tn_ns) >> 20;
            }
        }
        rmlock_runlock(lock);

        if (len > 0)
            continue;

        memset(ekbuf, -1, sizeof(ekbuf)); /* initialize max edge key */

        /* The primary node (i.e., the anchor node) of all compaction actions
         * must always contain at least one kvset, and in order to correctly
         * synchronize with incremental spill the right node of a join must
         * also always contain at least one kvset (whereas the left node of
         * a join may be empty).
         *
         * Hence, if the rightmost node in the tree is empty we cannot remove
         * it via the existing compaction apparatus.  So instead we look for
         * for and remove all rightmost empty nodes periodically here at the
         * end of each tree's shape check.
         */
        rmlock_wlock(&tree->ct_lock);
        tn = list_last_entry_or_null(&tree->ct_nodes, typeof(*tn), tn_link);

        while (tn && cn_ns_kvsets(&tn->tn_ns) == 0 && tree->ct_fanout > 1) {
            struct cn_tree_node *left = list_prev_entry(tn, tn_link);

            /* We can only remove the rightmost node if neither it nor
             * its left neighbor are undergoing a spill (required to
             * correctly coordinate with incremental spill).
             */
            mutex_lock(&tree->ct_ss_lock);
            if (tn->tn_ss_spilling || left->tn_ss_spilling) {
                tn = NULL;
            } else {
                struct route_map *map = tree->ct_route_map;
                merr_t err;

                err = route_node_key_modify(map, left->tn_route_node, ekbuf, sizeof(ekbuf));
                if (ev(err)) {
                    tn = NULL;
                } else {
                    route_map_delete(map, tn->tn_route_node);
                    tn->tn_route_node = NULL;

                    list_del(&tn->tn_link);
                    tree->ct_fanout--;
                    cn_node_free(tn);

                    tn = left;
                }
            }
            mutex_unlock(&tree->ct_ss_lock);
        }
        rmlock_wunlock(&tree->ct_lock);
    }

    bad = rlen > rlen_thresh || llen > llen_thresh || lsiz > lsiz_thresh;

    if (sp->tree_shape_bad != bad) {

        log_info("tree shape changed from %s (samp %.3f rlen %u llen %u lsize %um)",
                 bad ? "good to bad" : "bad to good",
                 scale2dbl(sp->samp_curr),
                 rlen, llen, lclen);

        sp->tree_shape_bad = bad;
        log = true; /* log details below */
    }

    if (log) {
        sp3_tree_shape_log(rlen_node, rlen > rlen_thresh, "longest_root");
        sp3_tree_shape_log(llen_node, llen > llen_thresh, "longest_leaf");
        sp3_tree_shape_log(lsiz_node, lsiz > lsiz_thresh, "largest_leaf");
    }
}

/* spn_rbe must be first element in sp3_node struct in order for
 * '(void *)(rbe - tx)' to map rbe back to the sp3_node struct.
 */
static_assert(offsetof(struct sp3_node, spn_rbe) == 0,
              "spn_rbe must be first field in struct sp3_node");

static bool
sp3_check_rb_tree(struct sp3 *sp, enum sp3_work_type wtype, uint64_t threshold, uint qnum)
{
    struct rb_root *root;
    struct rb_node *rbn;
    uint            debug;

    assert(wtype < NELEM(sp->rbt));

    debug = csched_rp_dbg_comp(sp->rp);

    root = sp->rbt + wtype;
    rbn = rb_first(root);

    while (rbn) {
        struct sp3_node *spn;
        struct sp3_rbe *rbe;
        bool have_work;

        rbe = rb_entry(rbn, struct sp3_rbe, rbe_node);
        spn = (void *)(rbe - wtype);

        if (rbe->rbe_weight < threshold)
            return false;

        if (sp3_work(spn, wtype, &sp->thresh, debug, &sp->wp))
            return false;

        have_work = sp->wp->cw_action != CN_ACTION_NONE;
        if (have_work) {
            sp3_node_remove(sp, spn, wtype);
            sp3_submit(sp, sp->wp, qnum);
            sp->wp = NULL;
            return true;
        }

        rbn = rb_next(rbn);

        if (sp->wp->cw_resched)
            continue;

        /* There are either too many active jobs or insufficient kvsets to start
         * a new job right now so drop this work request. sp3_dirty_node() will
         * re-assess the situation when the node composition changes.
         */
        sp3_node_remove(sp, spn, wtype);
    }

    return false;
}

static void
sp3_qos_check(struct sp3 *sp)
{
    struct cn_tree *tree;
    uint64_t rspill_dt_max;
    uint64_t clen_max;
    uint32_t rootmin, rootmax;
    uint sval, sleepers;

    if (!sp->throttle_sensor_root)
        return;

    rootmin = sp->thresh.rspill_runlen_min;
    rootmax = 0;
    rspill_dt_max = 0;
    clen_max = 0;
    sleepers = 0;
    sval = 0;

    list_for_each_entry(tree, &sp->mon_tlist, ct_sched.sp3t.spt_tlink) {
        uint32_t nk;

        nk = cn_ns_kvsets(&tree->ct_root->tn_ns) + 1;

        sleepers += atomic_read(&tree->ct_rspill_slp);

        if (nk > rootmin) {
            if (tree->ct_rspill_dt * (nk - rootmin) > rspill_dt_max * rootmax) {
                rspill_dt_max = tree->ct_rspill_dt;
                rootmax = nk - rootmin;
            }
        } else {
            if (cn_ns_clen(&tree->ct_root->tn_ns) > clen_max) {
                clen_max = cn_ns_clen(&tree->ct_root->tn_ns);
            }
        }
    }

    if (rspill_dt_max * rootmax > 0) {
        u64 K;
        u64 r = rootmax * 100;
        u64 secs = (rspill_dt_max * rootmax) / NSEC_PER_SEC;
        u64 min_lat = 16, max_lat = 80;

        /* Since, the throttling system's sensitivty to sensor values over 1000 is non-linear, the
         * sensor value is not incremented at a high rate once it gets over 1000.
         *
         * The mathematical function used here is:
         *
         *   sval = 3KR / (K + R)
         *
         * where,
         *   K is a parameter in the range [500, 600], and
         *   R is the root node length times a hundred
         *
         * The parameter K is determined based on the latency of a root spill, i.e. it's an
         * indicator of the available media bandwidth. K determines the root node length for which
         * the sensor value surpasses 1000. Lower the value of K, higher is this root node length.
         *
         * This was tested for extremes of slow and fast drives and a latency range of 16s to 80s
         * worked well. Map a latency of [16s, 80s] to the range [500, 600]:
         *
         *   K = (100 * secs / 64) + 475;
         */

        secs = clamp_t(u64, secs, min_lat, max_lat);
        K = ((100 * secs) + (475 * 64)) / 64;
        sval = (K * r * 3) / (K + r);

        if (rspill_dt_max > 1 && sval < sp->sp_sval_min) {
            sp->sp_sval_min = sval;
        }
    } else {
        if (clen_max > (1024ul << 20) && jclock_ns - sp->sp_ingest_ns < NSEC_PER_SEC * 60)
            sval = sp->sp_sval_min;
    }

    /* Clamp the sensor value to prevent wild oscillations in throughput as seen
     * by the application. Raise the clamp above THROTTLE_SENSOR_SCALE if there the
     * root list is excessively long or are any rspill jobs are asleep awaiting a
     * split or another spill to ensure the throttle can increase if need be...
     */
    if (rootmax > rootmin * 4 || sleepers > 0) {
        if (sval > THROTTLE_SENSOR_SCALE * 110 / 100) {
            sval = THROTTLE_SENSOR_SCALE * 110 / 100;
        }
    } else {
        if (sval > THROTTLE_SENSOR_SCALE * 90 / 100) {
            sval = THROTTLE_SENSOR_SCALE * 90 / 100;
        }
    }

    throttle_sensor_set(sp->throttle_sensor_root, sval);

    if (debug_qos(sp) && jclock_ns > sp->qos_log_ttl) {
        sp->qos_log_ttl = jclock_ns + NSEC_PER_SEC;

        log_info(
            "root_sensor=%u rootmax=%u rspill_dt_max=%lu "
            "samp_curr=%.3f samp_targ=%.3f lpct_targ=%.3f",
            sval, rootmax, rspill_dt_max,
            scale2dbl(sp->samp_curr), scale2dbl(sp->samp_targ), scale2dbl(sp->lpct_targ));
    }
}

/**
 * sp3_schedule() - try to schedule a single job
 */
static void
sp3_schedule(struct sp3 *sp)
{
    bool job = false;

    /* This log message should never be emitted (unless someone has reduced
     * csched_qthreads at run time).  Scheduling of new jobs will resume
     * after a sufficient number of jobs complete.
     */
    if (sp->jobs_started - sp->jobs_finished > sp->jobs_max) {
        log_warn("possible job queue accounting bug (%u - %u > %u)",
                 sp->jobs_started, sp->jobs_finished, sp->jobs_max);
        usleep(NSEC_PER_SEC / 3);
        return;
    }

    for (uint rr = 0; rr < wtype_MAX && !job; rr++) {
        uint rp_leaf_pct, qnum;
        uint64_t thresh;

        /* round robin between job types */
        sp->rr_wtype = (sp->rr_wtype + 1) % wtype_MAX;

        switch (sp->rr_wtype) {
        case wtype_root:
            qnum = SP3_QNUM_ROOT;
            if (qfull(sp, qnum))
                break;

            job = sp3_check_roots(sp, qnum);
            break;

        case wtype_length:
            qnum = SP3_QNUM_LENGTH;
            if (qfull(sp, qnum))
                break;

            job = sp3_check_rb_tree(sp, sp->rr_wtype, 0, qnum);
            break;

        case wtype_idle:
            qnum = SP3_QNUM_SHARED;
            if (qfull(sp, qnum))
                break;

            thresh = (UINT32_MAX - (jclock_ns >> 32)) << 32;

            job = sp3_check_rb_tree(sp, sp->rr_wtype, thresh, qnum);
            break;

        case wtype_garbage:
            qnum = SP3_QNUM_GARBAGE;
            if (!qempty(sp, qnum) && jclock_ns < sp->check_garbage_ns)
                break;

            if (qfull(sp, qnum)) {
                qnum = SP3_QNUM_SHARED;
                if (qfull(sp, qnum))
                    break;
            }

            /* convert rparam to internal scale */
            rp_leaf_pct = (uint)sp->inputs.csched_leaf_pct * SCALE / EXT_SCALE;

            /* Implements:
             *   - Leaf node space amp rule
             * Notes:
             *   - Check for garbage if ucomp is active OR samp_reduce mode is enabled
             *     and leaf percent is somewhat caught up (ie, current leaf pct (lpct_targ)
             *     is within 90% of rparam setting (rp_leaf_pct)).
             *   - When checking for garbage, if leaf percent is behind, then bump up
             *     the threshold so we don't waste write amp compacting nodes with
             *     low garbage (we'd rather wait for leaf_pct to catch up).
             *   - If neither ucomp nor samp_reduce is active then check for nodes
             *     with garbage above the per-node threshold (default 67%).
             */
            if (sp->samp_reduce && (100 * sp->lpct_targ > 90 * rp_leaf_pct)) {
                thresh = (sp->lpct_targ < rp_leaf_pct ? 10ul : 0ul) << 32;
            } else {
                thresh = (uint64_t)sp->rp->csched_gc_pct << 32;
            }

            job = sp3_check_rb_tree(sp, sp->rr_wtype, thresh, qnum);
            if (job)
                sp->check_garbage_ns = jclock_ns + NSEC_PER_SEC * 7;
            break;

        case wtype_scatter:
            qnum = SP3_QNUM_SCATTER;

            if (!qempty(sp, qnum) && jclock_ns < sp->check_scatter_ns)
                break;

            if (qfull(sp, qnum)) {
                qnum = SP3_QNUM_SHARED;
                if (qfull(sp, qnum))
                    break;
            }

            thresh = (uint64_t)sp->thresh.lscat_hwm << 32;

            job = sp3_check_rb_tree(sp, sp->rr_wtype, thresh, qnum);
            if (job)
                sp->check_scatter_ns = jclock_ns + NSEC_PER_SEC * 3;
            break;

        case wtype_split:
            qnum = SP3_QNUM_SPLIT;
            if (qfull(sp, qnum))
                break;

            job = sp3_check_rb_tree(sp, sp->rr_wtype, 0, qnum);
            break;

        case wtype_join:
            qnum = SP3_QNUM_SPLIT;
            if (qfull(sp, qnum))
                break;

            job = sp3_check_rb_tree(sp, sp->rr_wtype, 0, qnum);
            break;
        }
    }
}

/*
 * sp3_update_samp() - update internal space amp metrics
 *
 * Updates the following members of struct sp3:
 *
 *  sp->samp_curr
 *  sp->samp_targ
 *  sp->lpct_targ
 *  sp->lpct_throttle
 *  sp->samp_reduce
 *  sp->ucomp_*
 */
static void
sp3_update_samp(struct sp3 *sp)
{
    struct cn_samp_stats targ;

    sp3_samp_target(sp, &targ);
    sp->samp_targ = samp_est(&targ, SCALE);
    sp->lpct_targ = samp_pct_leaves(&targ, SCALE);

    sp->samp_curr = samp_est(&sp->samp, SCALE);

    sp3_ucomp_check(sp);

    /* Use low/high water marks to enable/disable garbage collection. */
    if (sp->samp_reduce) {
        if (sp->samp_targ < sp->samp_lwm) {
            sp->samp_reduce = false;
            log_info("sp3 expected samp %u below lwm %u, disable samp reduction",
                     sp->samp_targ * 100 / SCALE,
                     sp->samp_lwm * 100 / SCALE);
        }
    } else {
        if (sp->samp_targ > sp->samp_hwm) {
            sp->samp_reduce = true;
            log_info("sp3 expected samp %u above hwm %u, enable samp reduction",
                     sp->samp_targ * 100 / SCALE,
                     sp->samp_hwm * 100 / SCALE);
        }
    }
}

struct periodic_check {
    u64 interval;
    u64 next;
    u64 prev;
};

static void
sp3_monitor(struct work_struct *work)
{
    struct sp3 *sp = container_of(work, struct sp3, mon_work);

    struct periodic_check chk_qos     = { .interval = NSEC_PER_SEC / 3 };
    struct periodic_check chk_sched   = { .interval = NSEC_PER_SEC * 3 };
    struct periodic_check chk_refresh = { .interval = NSEC_PER_SEC * 10 };
    struct periodic_check chk_shape   = { .interval = NSEC_PER_SEC * 15 };
    u64 last_activity = 0;

    sp3_refresh_settings(sp);

    while (atomic_read(&sp->running)) {
        uint64_t now = get_time_ns();
        merr_t err;

        mutex_lock(&sp->mon_lock);
        end_stats_work();

        if (!sp->mon_signaled && now < chk_qos.next) {
            int timeout_ms = max_t(int, 10, (chk_qos.next - now) / USEC_PER_SEC);

            cv_timedwait(&sp->mon_cv, &sp->mon_lock, timeout_ms, "spmonslp");

            now = get_time_ns();
        }

        begin_stats_work();
        sp->mon_signaled = false;
        mutex_unlock(&sp->mon_lock);

        /* The following "process and prune" functions will increment
         * sp->activity to trigger a call (below) to sp3_schedule().
         */
        sp3_process_worklist(sp);
        sp3_process_dirtylist(sp);
        sp3_process_ingest(sp);
        sp3_process_new_trees(sp);
        sp3_prune_trees(sp);

        sp3_update_samp(sp);

        err = kvdb_health_check(sp->health, KVDB_HEALTH_FLAG_ALL);
        if (ev(err)) {
            if (sp->sp_healthy) {
                log_errx("KVDB %s is in bad health", err, sp->name);
                sp->sp_healthy = false;
            }
        }

        if (now > chk_sched.next || sp->activity) {
            if (sp->activity) {
                last_activity = now + NSEC_PER_SEC * 5;
                sp->activity = 0;
            }

            sp3_schedule(sp);

            chk_sched.next = now + chk_sched.interval;
        }

        if (now > chk_refresh.next) {
            sp3_refresh_settings(sp);
            chk_refresh.next = now + chk_refresh.interval;
        }

        if (now > chk_qos.next) {
            sp3_qos_check(sp);
            chk_qos.next = now + chk_qos.interval;
        }

        if (now > chk_shape.next) {
            sp3_tree_shape_check(sp);
            if (debug_rbtree(sp)) {
                for (uint tx = 0; tx < NELEM(sp->rbt); tx++)
                    sp3_rb_dump(sp, tx, 25);
            }
            chk_shape.next = now + chk_shape.interval;
        }

        sp->idle = now > last_activity && sp->jobs_started == sp->jobs_finished;
    }
}

/*****************************************************************
 *
 * External Entry Points
 *
 ****************************************************************/

void
sp3_throttle_sensor(struct csched *handle, struct throttle_sensor *sensor)
{
    struct sp3 *sp = (struct sp3 *)handle;

    if (!sp)
        return;

    sp->throttle_sensor_root = sensor;
}

void
sp3_compact_request(struct csched *handle, unsigned int flags)
{
    struct sp3 *sp = (struct sp3 *)handle;

    if (!sp)
        return;

    if (flags & HSE_KVDB_COMPACT_CANCEL) {
        sp3_ucomp_cancel(sp);
    } else if (flags & HSE_KVDB_COMPACT_SAMP_LWM) {
        sp3_ucomp_start(sp);
    } else {
        log_info("invalid user-initiated compaction request: flags 0x%x", flags);
    }
}

void
sp3_compact_status_get(struct csched *handle, struct hse_kvdb_compact_status *status)
{
    struct sp3 *sp = (struct sp3 *)handle;

    if (!sp)
        return;

    status->kvcs_active = sp->ucomp_active;
    status->kvcs_canceled = sp->ucomp_canceled;
    status->kvcs_samp_curr = samp_est(&sp->samp, 100);
    status->kvcs_samp_lwm = sp->samp_lwm * 100 / SCALE;
    status->kvcs_samp_hwm = sp->samp_hwm * 100 / SCALE;
}

/**
 * sp3_notify_ingest() - External API: notify ingest job has completed
 */
void
sp3_notify_ingest(struct csched *handle, struct cn_tree *tree, size_t alen, size_t wlen)
{
    struct sp3 *sp = (struct sp3 *)handle;
    struct sp3_tree *spt = tree2spt(tree);

    if (!sp)
        return;

    if (alen + wlen == 0)
        abort();

    atomic_add(&spt->spt_ingest_alen, alen);
    atomic_add(&spt->spt_ingest_wlen, wlen);
    atomic_inc_rel(&sp->sp_ingest_count);
    sp->sp_ingest_ns = jclock_ns;

    sp3_monitor_wake(sp);
}

static void
sp3_tree_init(struct sp3_tree *spt)
{
    memset(spt, 0, sizeof(*spt));
    INIT_LIST_HEAD(&spt->spt_tlink);
    atomic_set(&spt->spt_enabled, true);

    for (uint i = 0; i < NELEM(spt->spt_dnode_listv); ++i) {
        INIT_LIST_HEAD(&spt->spt_dnode_listv[i]);
        INIT_LIST_HEAD(&spt->spt_dtree_linkv[i]);
    }
}

/**
 * sp3_tree_add() - External API: add tree to scheduler
 */
void
sp3_tree_add(struct csched *handle, struct cn_tree *tree)
{
    struct sp3 *sp = (struct sp3 *)handle;
    struct sp3_tree *spt = tree2spt(tree);

    if (!sp)
        return;

    assert(!sp3_tree_is_managed(tree));

    if (debug_tree_life(sp))
        log_info("sp3 %s cnid %lu", __func__, (ulong)tree->cnid);

    cn_ref_get(tree->cn);

    sp3_tree_init(spt);

    mutex_lock(&sp->new_tlist_lock);
    list_add(&spt->spt_tlink, &sp->new_tlist);
    mutex_unlock(&sp->new_tlist_lock);

    sp3_monitor_wake(sp);
}

/**
 * sp3_tree_remove() - External API: remove tree from scheduler
 */
void
sp3_tree_remove(struct csched *handle, struct cn_tree *tree, bool cancel)
{
    struct sp3 *sp = (struct sp3 *)handle;
    struct sp3_tree *spt = tree2spt(tree);

    if (!sp || !sp3_tree_is_managed(tree))
        return;

    if (debug_tree_life(sp))
        log_info("sp3 %s cnid %lu", __func__, (ulong)tree->cnid);

    /* Disable scheduling for tree.  Monitor will remove the tree
     * out when no more jobs are pending.
     */
    atomic_set(&spt->spt_enabled, false);
    atomic_inc_rel(&sp->sp_prune_count);

    sp3_monitor_wake(sp);
}

/**
 * sp3_destroy() - External API: SP3 destructor
 */
void
sp3_destroy(struct csched *handle)
{
    struct sp3 *sp = (struct sp3 *)handle;
    uint        tx;

    if (!sp)
        return;

    /* Destroy shouldn't be invoked until all cn trees been removed and
     * all cn refs have been returned wih cn_ref_put.  If that is true
     * then we should have empty lists, rb trees, job counts, etc.
     */
    assert(list_empty(&sp->new_tlist));
    assert(list_empty(&sp->mon_tlist));
    assert(list_empty(&sp->work_list));

    for (tx = 0; tx < NELEM(sp->rbt); tx++)
        assert(!rb_first(sp->rbt + tx));

    atomic_set(&sp->running, 0);
    sp3_monitor_wake(sp);

    /* This is like a pthread_join for the monitor thread */
    destroy_workqueue(sp->mon_wq);

    sts_destroy(sp->sts);

    mutex_destroy(&sp->work_list_lock);
    mutex_destroy(&sp->new_tlist_lock);
    mutex_destroy(&sp->mon_lock);
    mutex_destroy(&sp->sp_dlist_lock);
    cv_destroy(&sp->mon_cv);

    perfc_free(&sp->sched_pc);
    free(sp->wp);
    free(sp);
}

/**
 * sp3_create() - External API: constructor
 */
merr_t
sp3_create(
    struct mpool *       ds,
    struct kvdb_rparams *rp,
    const char *         kvdb_alias,
    struct kvdb_health * health,
    struct csched      **handle)
{
    const char *restname = "csched";
    char group[128];
    struct sp3 *sp;
    merr_t      err;
    size_t      name_sz, alloc_sz;
    uint        tx;

    INVARIANT(rp && kvdb_alias && handle);

    /* Allocate cache aligned space for struct csched + sp->name */
    name_sz = strlen(restname) + strlen(kvdb_alias) + 2;
    alloc_sz = sizeof(*sp) + name_sz;
    alloc_sz = roundup(alloc_sz, __alignof__(*sp));

    sp = aligned_alloc(__alignof__(*sp), alloc_sz);
    if (ev(!sp))
        return merr(ENOMEM);

    memset(sp, 0, alloc_sz);
    sp->ds = ds;
    snprintf(sp->name, name_sz, "%s/%s", restname, kvdb_alias);

    sp->rp = rp;
    sp->health = health;

    mutex_init(&sp->new_tlist_lock);
    mutex_init(&sp->work_list_lock);

    mutex_init(&sp->mon_lock);
    cv_init(&sp->mon_cv);

    INIT_LIST_HEAD(&sp->mon_tlist);
    INIT_LIST_HEAD(&sp->new_tlist);
    INIT_LIST_HEAD(&sp->work_list);
    INIT_LIST_HEAD(&sp->spn_alist);
    INIT_LIST_HEAD(&sp->spn_rlist);

    for (tx = 0; tx < NELEM(sp->rbt); tx++)
        sp->rbt[tx] = RB_ROOT;

    atomic_set(&sp->running, 1);
    atomic_set(&sp->sp_ingest_count, 0);
    atomic_set(&sp->sp_prune_count, 0);

    mutex_init_adaptive(&sp->sp_dlist_lock);
    atomic_set(&sp->sp_dlist_idx, 0);
    for (uint i = 0; i < NELEM(sp->sp_dtree_listv); ++i)
        INIT_LIST_HEAD(&sp->sp_dtree_listv[i]);
    sp->sp_healthy = true;
    sp->sp_sval_min = THROTTLE_SENSOR_SCALE / 2;

    err = sts_create(sp->name, SP3_QNUM_MAX, sp3_job_print, &sp->sts);
    if (ev(err))
        goto err_exit;

    sp->mon_wq = alloc_workqueue("hse_sp3_monitor", 0, 1, 1);
    if (ev(!sp->mon_wq)) {
        err = merr(ENOMEM);
        goto err_exit;
    }

    snprintf(group, sizeof(group), "kvdb/%s", sp->name);

    INIT_WORK(&sp->mon_work, sp3_monitor);
    queue_work(sp->mon_wq, &sp->mon_work);

    *handle = (void *)sp;
    return 0;

err_exit:
    sts_destroy(sp->sts);

    mutex_destroy(&sp->work_list_lock);
    mutex_destroy(&sp->new_tlist_lock);
    mutex_destroy(&sp->mon_lock);
    cv_destroy(&sp->mon_cv);

    free(sp);

    return err;
}

#if HSE_MOCKING
#include "csched_sp3_ut_impl.i"
#endif /* HSE_MOCKING */
