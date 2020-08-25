/* -------------------------------------------------------------------------
 *
 * nodeBitmapIndexscan.cpp
 *	  Routines to support bitmapped index scans of relations
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/runtime/executor/nodeBitmapIndexscan.cpp
 *
 * -------------------------------------------------------------------------
 *
 * INTERFACE ROUTINES
 *		MultiExecBitmapIndexScan	scans a relation using index.
 *		ExecInitBitmapIndexScan		creates and initializes state info.
 *		ExecReScanBitmapIndexScan	prepares to rescan the plan.
 *		ExecEndBitmapIndexScan		releases all storage.
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "catalog/pg_partition_fn.h"
#include "executor/execdebug.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "optimizer/pruning.h"
#include "access/tableam.h"

static void ExecInitNextPartitionForBitmapIndexScan(BitmapIndexScanState* node);

/* ----------------------------------------------------------------
 *		MultiExecBitmapIndexScan(node)
 * ----------------------------------------------------------------
 */
Node* MultiExecBitmapIndexScan(BitmapIndexScanState* node)
{
    TIDBitmap* tbm = NULL;
    AbsIdxScanDesc scandesc;
    double nTuples = 0;
    bool doscan = false;

    /* must provide our own instrumentation support */
    if (node->ss.ps.instrument)
        InstrStartNode(node->ss.ps.instrument);

    /*
     * extract necessary information from index scan node
     */
    scandesc = node->biss_ScanDesc;

    /*
     * If we have runtime keys and they've not already been set up, do it now.
     * Array keys are also treated as runtime keys; note that if ExecReScan
     * returns with biss_RuntimeKeysReady still false, then there is an empty
     * array key so we should do nothing.
     */
    if (!node->biss_RuntimeKeysReady && (node->biss_NumRuntimeKeys != 0 || node->biss_NumArrayKeys != 0)) {
        if (node->ss.isPartTbl) {
            if (PointerIsValid(node->biss_IndexPartitionList)) {
                node->ss.ss_ReScan = true;

                ExecReScan((PlanState*)node);
                doscan = node->biss_RuntimeKeysReady;
            } else {
                doscan = false;
            }
        } else {
            ExecReScan((PlanState*)node);
            doscan = node->biss_RuntimeKeysReady;
        }
    } else {
        if (node->ss.isPartTbl && !PointerIsValid(node->biss_IndexPartitionList)) {
            doscan = false;
        } else {
            doscan = true;
        }
    }

    /*
     * Prepare the result bitmap.  Normally we just create a new one to pass
     * back; however, our parent node is allowed to store a pre-made one into
     * node->biss_result, in which case we just OR our tuple IDs into the
     * existing bitmap.  (This saves needing explicit UNION steps.)
     */
    if (node->biss_result) {
        tbm = node->biss_result;
        node->biss_result = NULL; /* reset for next time */
    } else {
        /* XXX should we use less than u_sess->attr.attr_memory.work_mem for this? */
        tbm = tbm_create(u_sess->attr.attr_memory.work_mem * 1024L);

        /* If bitmapscan uses global partition index, set tbm to global */
        if (RelationIsGlobalIndex(node->biss_RelationDesc)) {
            tbm_set_global(tbm, true);
        }
    }

    if (hbkt_idx_need_switch_bkt(scandesc, node->ss.ps.hbktScanSlot.currSlot)) {
        hbkt_idx_bitmapscan_switch_bucket(scandesc, node->ss.ps.hbktScanSlot.currSlot);
    }
    /*
     * Get TIDs from index and insert into bitmap
     */
    while (doscan) {
        nTuples += (double)abs_idx_getbitmap(scandesc, tbm);

        CHECK_FOR_INTERRUPTS();

        doscan = ExecIndexAdvanceArrayKeys(node->biss_ArrayKeys, node->biss_NumArrayKeys);
        if (doscan) /* reset index scan */
            abs_idx_rescan_local(node->biss_ScanDesc, node->biss_ScanKeys, node->biss_NumScanKeys, NULL, 0);
    }

    /* must provide our own instrumentation support */
    if (node->ss.ps.instrument)
        InstrStopNode(node->ss.ps.instrument, nTuples);

    return (Node*)tbm;
}

/* ----------------------------------------------------------------
 *		ExecReScanBitmapIndexScan(node)
 *
 *		Recalculates the values of any scan keys whose value depends on
 *		information known at runtime, then rescans the indexed relation.
 * ----------------------------------------------------------------
 */
void ExecReScanBitmapIndexScan(BitmapIndexScanState* node)
{
    ExprContext* econtext = node->biss_RuntimeContext;

    /*
     * Reset the runtime-key context so we don't leak memory as each outer
     * tuple is scanned.  Note this assumes that we will recalculate *all*
     * runtime keys on each call.
     */
    if (econtext != NULL)
        ResetExprContext(econtext);

    /*
     * If we are doing runtime key calculations (ie, any of the index key
     * values weren't simple Consts), compute the new key values.
     *
     * Array keys are also treated as runtime keys; note that if we return
     * with biss_RuntimeKeysReady still false, then there is an empty array
     * key so no index scan is needed.
     *
     * For recursive-stream rescan, if number of RuntimeKeys not euqal zero,
     * just return without rescan.
     */
    if (node->biss_NumRuntimeKeys != 0) {
        if (node->ss.ps.state->es_recursive_next_iteration) {
            node->biss_RuntimeKeysReady = false;
            return;
        }
        ExecIndexEvalRuntimeKeys(econtext, node->biss_RuntimeKeys, node->biss_NumRuntimeKeys);
    }

    if (node->biss_NumArrayKeys != 0)
        node->biss_RuntimeKeysReady = ExecIndexEvalArrayKeys(econtext, node->biss_ArrayKeys, node->biss_NumArrayKeys);
    else
        node->biss_RuntimeKeysReady = true;

    /*
     * deal with partitioned table
     */
    if (node->ss.isPartTbl) {
        /*
         * switch to the next partition for scaning
         */
        if (node->ss.ss_ReScan) {
            /* reset the rescan falg */
            node->ss.ss_ReScan = false;
        } else {
            if (!PointerIsValid(node->biss_IndexPartitionList)) {
                return;
            }
            /*
             * switch to the next partition for scaning
             */
			 Assert(node->biss_ScanDesc);

			 abs_idx_endscan(node->biss_ScanDesc);

             /*  initialize Scan for the next partition */
             ExecInitNextPartitionForBitmapIndexScan(node);
             /*
              * give up rescaning the index if there is no partition to scan
              */
        }
    }

    /* reset index scan */
    if (node->biss_RuntimeKeysReady)
        abs_idx_rescan_local(node->biss_ScanDesc, node->biss_ScanKeys, node->biss_NumScanKeys, NULL, 0);
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapIndexScan
 * ----------------------------------------------------------------
 */
void ExecEndBitmapIndexScan(BitmapIndexScanState* node)
{
    Relation indexRelationDesc;
    AbsIdxScanDesc indexScanDesc;

    /*
     * extract information from the node
     */
    indexRelationDesc = node->biss_RelationDesc;
    indexScanDesc = node->biss_ScanDesc;

    /*
     * Free the exprcontext ... now dead code, see ExecFreeExprContext
     */
#ifdef NOT_USED
    if (node->biss_RuntimeContext)
        FreeExprContext(node->biss_RuntimeContext, true);
#endif

    /*
     * close the index relation (no-op if we didn't open it)
     */
    if (indexScanDesc)
        abs_idx_endscan(indexScanDesc);

    /*
     * close the index relation (no-op if we didn't open it)
     * close the index relation if the relation is non-partitioned table
     * close the index partitions and table partitions if the relation is
     * non-partitioned table
     */
    if (node->ss.isPartTbl) {
        if (PointerIsValid(node->biss_IndexPartitionList)) {
            Assert(PointerIsValid(node->biss_CurrentIndexPartition));
            releaseDummyRelation(&(node->biss_CurrentIndexPartition));

            /* close index partition */
            releasePartitionList(node->biss_RelationDesc, &(node->biss_IndexPartitionList), NoLock);
        }
    }

    if (indexRelationDesc)
        index_close(indexRelationDesc, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapIndexScan
 *
 *		Initializes the index scan's state information.
 * ----------------------------------------------------------------
 */
BitmapIndexScanState* ExecInitBitmapIndexScan(BitmapIndexScan* node, EState* estate, int eflags)
{
    BitmapIndexScanState* indexstate = NULL;
    bool relistarget = false;

    /* check for unsupported flags */
    Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

    /*
     * create state structure
     */
    indexstate = makeNode(BitmapIndexScanState);
    indexstate->ss.ps.plan = (Plan*)node;
    indexstate->ss.ps.state = estate;
    indexstate->ss.isPartTbl = node->scan.isPartTbl;
    indexstate->ss.currentSlot = 0;
    indexstate->ss.partScanDirection = node->scan.partScanDirection;

    /* normally we don't make the result bitmap till runtime */
    indexstate->biss_result = NULL;

    /*
     * Miscellaneous initialization
     *
     * We do not need a standard exprcontext for this node, though we may
     * decide below to create a runtime-key exprcontext
     */
    /*
     * initialize child expressions
     *
     * We don't need to initialize targetlist or qual since neither are used.
     *
     * Note: we don't initialize all of the indexqual expression, only the
     * sub-parts corresponding to runtime keys (see below).
     */
    /*
     * We do not open or lock the base relation here.  We assume that an
     * ancestor BitmapHeapScan node is holding AccessShareLock (or better) on
     * the heap relation throughout the execution of the plan tree.
     */
    indexstate->ss.ss_currentRelation = NULL;
    indexstate->ss.ss_currentScanDesc = NULL;

    /*
     * If we are just doing EXPLAIN (ie, aren't going to run the plan), stop
     * here.  This allows an index-advisor plugin to EXPLAIN a plan containing
     * references to nonexistent indexes.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return indexstate;

    /*
     * Open the index relation.
     *
     * If the parent table is one of the target relations of the query, then
     * InitPlan already opened and write-locked the index, so we can avoid
     * taking another lock here.  Otherwise we need a normal reader's lock.
     */
    relistarget = ExecRelationIsTargetRelation(estate, node->scan.scanrelid);
    indexstate->biss_RelationDesc = index_open(node->indexid, relistarget ? NoLock : AccessShareLock);
    if (!IndexIsUsable(indexstate->biss_RelationDesc->rd_index)) {
        ereport(ERROR,
            (errcode(ERRCODE_INDEX_CORRUPTED),
                errmodule(MOD_EXECUTOR),
                errmsg("can't initialize bitmap index scans using unusable index \"%s\"",
                    RelationGetRelationName(indexstate->biss_RelationDesc))));
    }

    /*
     * Initialize index-specific scan state
     */
    indexstate->biss_RuntimeKeysReady = false;
    indexstate->biss_RuntimeKeys = NULL;
    indexstate->biss_NumRuntimeKeys = 0;

    /*
     * build the index scan keys from the index qualification
     */
    ExecIndexBuildScanKeys((PlanState*)indexstate,
        indexstate->biss_RelationDesc,
        node->indexqual,
        false,
        &indexstate->biss_ScanKeys,
        &indexstate->biss_NumScanKeys,
        &indexstate->biss_RuntimeKeys,
        &indexstate->biss_NumRuntimeKeys,
        &indexstate->biss_ArrayKeys,
        &indexstate->biss_NumArrayKeys);

    /*
     * If we have runtime keys or array keys, we need an ExprContext to
     * evaluate them. We could just create a "standard" plan node exprcontext,
     * but to keep the code looking similar to nodeIndexscan.c, it seems
     * better to stick with the approach of using a separate ExprContext.
     */
    if (indexstate->biss_NumRuntimeKeys != 0 || indexstate->biss_NumArrayKeys != 0) {
        ExprContext* stdecontext = indexstate->ss.ps.ps_ExprContext;

        ExecAssignExprContext(estate, &indexstate->ss.ps);
        indexstate->biss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
        indexstate->ss.ps.ps_ExprContext = stdecontext;
    } else {
        indexstate->biss_RuntimeContext = NULL;
    }

    /* get index partition list and table partition list */
    if (node->scan.isPartTbl) {
        indexstate->biss_ScanDesc = NULL;

        if (node->scan.itrs > 0) {
            Partition currentindex = NULL;
            Relation currentrel = NULL;

            currentrel = ExecOpenScanRelation(estate, node->scan.scanrelid);

            /* Initialize table partition and index partition */
            ExecInitPartitionForBitmapIndexScan(indexstate, estate, currentrel);

            /* get the first index partition */
            currentindex = (Partition)list_nth(indexstate->biss_IndexPartitionList, 0);
            indexstate->biss_CurrentIndexPartition = partitionGetRelation(indexstate->biss_RelationDesc, currentindex);

            ExecCloseScanRelation(currentrel);

            indexstate->biss_ScanDesc = abs_idx_beginscan_bitmap(indexstate->biss_CurrentIndexPartition,
                estate->es_snapshot,
                indexstate->biss_NumScanKeys,
                (ScanState*)indexstate);
        }
    } else {
        /*
         * Initialize scan descriptor.
         */
        indexstate->biss_ScanDesc = abs_idx_beginscan_bitmap(
            indexstate->biss_RelationDesc, estate->es_snapshot, indexstate->biss_NumScanKeys, (ScanState*)indexstate);
    }

    /*
     * If no run-time keys to calculate, go ahead and pass the scankeys to the
     * index AM.
     */
    if ((indexstate->biss_NumRuntimeKeys == 0 && indexstate->biss_NumArrayKeys == 0) &&
        PointerIsValid(indexstate->biss_ScanDesc))
        abs_idx_rescan_local(
            indexstate->biss_ScanDesc, indexstate->biss_ScanKeys, indexstate->biss_NumScanKeys, NULL, 0);

    if (!PointerIsValid(indexstate->biss_ScanDesc)) {
        indexstate->ss.ps.stubType = PST_Scan;
    }
    /*
     * all done.
     */
    return indexstate;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: Initialize the table partition and the index partition for
 *			: index sacn
 * Description	:
 * Input		:
 * Output	:
 * Notes		:
 */
static void ExecInitNextPartitionForBitmapIndexScan(BitmapIndexScanState* node)
{
    Partition currentindexpartition = NULL;
    Relation currentindexpartitionrel = NULL;
    BitmapIndexScan* plan = NULL;
    int paramno = -1;
    ParamExecData* param = NULL;

    plan = (BitmapIndexScan*)(node->ss.ps.plan);

    /* get partition sequnce */
    paramno = plan->scan.plan.paramno;
    param = &(node->ss.ps.state->es_param_exec_vals[paramno]);
    node->ss.currentSlot = (int)param->value;

    node->ss.ss_currentScanDesc = NULL;

    /* get the index partition for the special partitioned index and table partition */
    currentindexpartition = (Partition)list_nth(node->biss_IndexPartitionList, node->ss.currentSlot);

    /* construct a relation for the index partition */
    currentindexpartitionrel = partitionGetRelation(node->biss_RelationDesc, currentindexpartition);

    Assert(node->biss_CurrentIndexPartition);
    releaseDummyRelation(&(node->biss_CurrentIndexPartition));
    node->biss_CurrentIndexPartition = currentindexpartitionrel;

    /* Initialize scan descriptor. */
    node->biss_ScanDesc = abs_idx_beginscan_bitmap(
        node->biss_CurrentIndexPartition, node->ss.ps.state->es_snapshot, node->biss_NumScanKeys, (ScanState*)node);

    Assert(PointerIsValid(node->biss_ScanDesc));
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Input		:
 * Output	:
 * Notes		:
 */
void ExecInitPartitionForBitmapIndexScan(BitmapIndexScanState* indexstate, EState* estate, Relation rel)
{
    BitmapIndexScan* plan = NULL;

    plan = (BitmapIndexScan*)indexstate->ss.ps.plan;

    indexstate->biss_CurrentIndexPartition = NULL;
    indexstate->biss_IndexPartitionList = NIL;

    if (plan->scan.itrs > 0) {
        Oid indexid = plan->indexid;
        Partition indexpartition = NULL;
        bool relistarget = false;
        LOCKMODE lock;
        ListCell* cell = NULL;
        List* part_seqs = plan->scan.pruningInfo->ls_rangeSelectedPartitions;

        Assert(plan->scan.itrs == plan->scan.pruningInfo->ls_rangeSelectedPartitions->length);
        relistarget = ExecRelationIsTargetRelation(estate, plan->scan.scanrelid);
        lock = (relistarget ? RowExclusiveLock : AccessShareLock);
        indexstate->lockMode = lock;

        foreach (cell, part_seqs) {
            Oid tablepartitionid = InvalidOid;
            int partSeq = lfirst_int(cell);
            Oid indexpartitionid = InvalidOid;
            Partition tablePartition = NULL;
            List* partitionIndexOidList = NIL;

            /* get index partition list for the special index */
            tablepartitionid = getPartitionOidFromSequence(rel, partSeq);
            tablePartition = partitionOpen(rel, tablepartitionid, lock);

            partitionIndexOidList = PartitionGetPartIndexList(tablePartition);
            Assert(PointerIsValid(partitionIndexOidList));
            if (!PointerIsValid(partitionIndexOidList)) {
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmodule(MOD_EXECUTOR),
                        errmsg("no local indexes found for partition %s BitmapIndexScan",
                            PartitionGetPartitionName(tablePartition))));
            }
            indexpartitionid = searchPartitionIndexOid(indexid, partitionIndexOidList);
            list_free_ext(partitionIndexOidList);
            partitionClose(rel, tablePartition, NoLock);

            indexpartition = partitionOpen(indexstate->biss_RelationDesc, indexpartitionid, lock);
            if (indexpartition->pd_part->indisusable == false) {
                ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmodule(MOD_EXECUTOR),
                        errmsg("can't initialize bitmap index scans using unusable local index \"%s\" for partition",
                            PartitionGetPartitionName(indexpartition))));
            }
            /* add index partition to list for the following scan */
            indexstate->biss_IndexPartitionList = lappend(indexstate->biss_IndexPartitionList, indexpartition);
        }
    }
}
