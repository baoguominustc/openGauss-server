/* --------------------------------------------------------------------
 * bgworker.cpp
 *      POSTGRES pluggable background workers implementation
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/gausskernel/process/postmaster/bgworker.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/parallel.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "tcop/autonomous.h"
#include "utils/ascii.h"
#include "utils/ps_status.h"
#include "utils/postinit.h"
#include "access/xact.h"
#include "utils/memtrack.h"

/*
 * BackgroundWorkerSlots exist in shared memory and can be accessed (via
 * the BackgroundWorkerArray) by both the postmaster and by regular backends.
 * However, the postmaster cannot take locks, even spinlocks, because this
 * might allow it to crash or become wedged if shared memory gets corrupted.
 * Such an outcome is intolerable.  Therefore, we need a lockless protocol
 * for coordinating access to this data.
 *
 * The 'in_use' flag is used to hand off responsibility for the slot between
 * the postmaster and the rest of the system.  When 'in_use' is false,
 * the postmaster will ignore the slot entirely, except for the 'in_use' flag
 * itself, which it may read.  In this state, regular backends may modify the
 * slot.  Once a backend sets 'in_use' to true, the slot becomes the
 * responsibility of the postmaster.  Regular backends may no longer modify it,
 * but the postmaster may examine it.  Thus, a backend initializing a slot
 * must fully initialize the slot - and insert a write memory barrier - before
 * marking it as in use.
 *
 * As an exception, however, even when the slot is in use, regular backends
 * may set the 'terminate' flag for a slot, telling the postmaster not
 * to restart it.  Once the background worker is no longer running, the slot
 * will be released for reuse.
 *
 * In addition to coordinating with the postmaster, backends modifying this
 * data structure must coordinate with each other.  Since they can take locks,
 * this is straightforward: any backend wishing to manipulate a slot must
 * take BackgroundWorkerLock in exclusive mode.  Backends wishing to read
 * data that might get concurrently modified by other backends should take
 * this lock in shared mode.  No matter what, backends reading this data
 * structure must be able to tolerate concurrent modifications by the
 * postmaster.
 */
typedef struct BackgroundWorkerSlot {
    bool in_use;
    bool terminate;
    ThreadId pid; /* InvalidPid = not started yet; 0 = dead */
    uint64 generation; /* incremented when slot is recycled */
    BackgroundWorker worker;
} BackgroundWorkerSlot;

/*
 * In order to limit the total number of parallel workers (according to
 * max_parallel_workers GUC), we maintain the number of active parallel
 * workers.  Since the postmaster cannot take locks, two variables are used for
 * this purpose: the number of registered parallel workers (modified by the
 * backends, protected by BackgroundWorkerLock) and the number of terminated
 * parallel workers (modified only by the postmaster, lockless).  The active
 * number of parallel workers is the number of registered workers minus the
 * terminated ones.  These counters can of course overflow, but it's not
 * important here since the subtraction will still give the right number.
 */
typedef struct BackgroundWorkerArray {
    int total_slots;
    uint32 parallel_register_count; // For extension only
    uint32 parallel_terminate_count; // For extension only
    BackgroundWorkerSlot slot[FLEXIBLE_ARRAY_MEMBER];
} BackgroundWorkerArray;

/*
 * List of internal background worker entry points.  We need this for
 * reasons explained in LookupBackgroundWorkerFunction(), below.
 */
static const struct {
    const char *fn_name;
    bgworker_main_type fn_addr;
} InternalBGWorkers[] = {
    {
        "autonomous_worker_main",
        autonomous_worker_main
    },
    {
        "ParallelWorkerMain",
        ParallelWorkerMain
    }
};

/* Private functions. */
static bgworker_main_type LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname);

/*
 * Calculate shared memory needed.
 */
Size BackgroundWorkerShmemSize(void)
{
    Size size;

    /* Array of workers is variably sized. */
    size = offsetof(BackgroundWorkerArray, slot);
    size = add_size(size, mul_size((Size)g_instance.attr.attr_storage.max_background_workers,
                                   sizeof(BackgroundWorkerSlot)));

    return size;
}

/*
 * Initialize shared memory.
 */
void BackgroundWorkerShmemInit(void)
{
    bool found;

    t_thrd.bgworker_cxt.background_worker_data = (BackgroundWorkerArray*)ShmemInitStruct("Background Worker Data",
        BackgroundWorkerShmemSize(),
        &found);
    if (!IsUnderPostmaster) {
        slist_iter siter;
        int slotno = 0;

        t_thrd.bgworker_cxt.background_worker_data->total_slots = g_instance.attr.attr_storage.max_background_workers;
        t_thrd.bgworker_cxt.background_worker_data->parallel_register_count = 0;
        t_thrd.bgworker_cxt.background_worker_data->parallel_terminate_count = 0;

        /*
         * Copy contents of worker list into shared memory.  Record the shared
         * memory slot assigned to each worker.  This ensures a 1-to-1
         * correspondence between the postmaster's private list and the array
         * in shared memory.
         */
        slist_foreach(siter, &t_thrd.bgworker_cxt.background_worker_list) {
            BackgroundWorkerSlot *slot = &t_thrd.bgworker_cxt.background_worker_data->slot[slotno];
            RegisteredBgWorker *rw;

            rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
            Assert(slotno < g_instance.attr.attr_storage.max_background_workers);
            slot->in_use = true;
            slot->terminate = false;
            slot->pid = InvalidPid;
            slot->generation = 0;
            rw->rw_shmem_slot = slotno;
            rw->rw_worker.bgw_notify_pid = 0; /* might be reinit after crash */
            int ss_rc = memcpy_s(&slot->worker, sizeof(BackgroundWorker), &rw->rw_worker, sizeof(BackgroundWorker));
            securec_check(ss_rc, "\0", "\0");
            ++slotno;
        }

        /*
         * Mark any remaining slots as not in use.
         */
        while (slotno < g_instance.attr.attr_storage.max_background_workers) {
            BackgroundWorkerSlot *slot = &t_thrd.bgworker_cxt.background_worker_data->slot[slotno];

            slot->in_use = false;
            ++slotno;
        }
    } else {
        Assert(found);
    }
}

/*
 * Search the postmaster's backend-private list of RegisteredBgWorker objects
 * for the one that maps to the given slot number.
 */
static RegisteredBgWorker* FindRegisteredWorkerBySlotNumber(int slotno)
{
    slist_iter  siter;

    slist_foreach(siter, &t_thrd.bgworker_cxt.background_worker_list) {
        RegisteredBgWorker *rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
        if (rw->rw_shmem_slot == slotno) {
            return rw;
        }
    }

    return NULL;
}

/*
 * Notice changes to shared memory made by other backends.  This code
 * runs in the postmaster, so we must be very careful not to assume that
 * shared memory contents are sane.  Otherwise, a rogue backend could take
 * out the postmaster.
 */
void BackgroundWorkerStateChange(void)
{
    int slotno;

    /*
     * The total number of slots stored in shared memory should match our
     * notion of max_background_workers.  If it does not, something is very
     * wrong.  Further down, we always refer to this value as
     * max_background_workers, in case shared memory gets corrupted while we're
     * looping.
     */
    if (g_instance.attr.attr_storage.max_background_workers !=
        t_thrd.bgworker_cxt.background_worker_data->total_slots) {
        elog(LOG,
             "inconsistent background worker state (max_background_workers=%d, total_slots=%d",
             g_instance.attr.attr_storage.max_background_workers,
             t_thrd.bgworker_cxt.background_worker_data->total_slots);
        return;
    }

    /*
     * Iterate through slots, looking for newly-registered workers or workers
     * who must die.
     */
    for (slotno = 0; slotno < g_instance.attr.attr_storage.max_background_workers; ++slotno) {
        BackgroundWorkerSlot *slot = &t_thrd.bgworker_cxt.background_worker_data->slot[slotno];
        RegisteredBgWorker *rw = NULL;

        if (!slot->in_use) {
            continue;
        }

        /*
         * Make sure we don't see the in_use flag before the updated slot
         * contents.
         */
        pg_read_barrier();

        /* See whether we already know about this worker. */
        rw = FindRegisteredWorkerBySlotNumber(slotno);
        if (rw != NULL) {
            /*
             * In general, the worker data can't change after it's initially
             * registered.  However, someone can set the terminate flag.
             */
            if (slot->terminate && !rw->rw_terminate) {
                rw->rw_terminate = true;
                if (rw->rw_pid != 0) {
                    if (gs_signal_send(rw->rw_pid, SIGTERM) != 0) {
                        ereport(WARNING,
                            (errmsg("sending SIGTERM to %lu failed", rw->rw_pid)));
                    }
                } else {
                    /* Report never-started, now-terminated worker as dead. */
                    ReportBackgroundWorkerPID(rw);
                }
            }
            continue;
        }

        /*
         * If the worker is marked for termination, we don't need to add it to
         * the registered workers list; we can just free the slot. However, if
         * bgw_notify_pid is set, the process that registered the worker may
         * need to know that we've processed the terminate request, so be sure
         * to signal it.
         */
        if (slot->terminate) {
            /*
             * We need a memory barrier here to make sure that the load of
             * bgw_notify_pid and the update of parallel_terminate_count
             * complete before the store to in_use.
             */
            ThreadId notify_pid = slot->worker.bgw_notify_pid;
            if ((slot->worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0) {
                t_thrd.bgworker_cxt.background_worker_data->parallel_terminate_count++;
            }
            pg_memory_barrier();
            slot->pid = 0;
            slot->in_use = false;
            if (notify_pid != 0) {
                if (gs_signal_send(notify_pid, SIGUSR1) != 0) {
                    ereport(WARNING,
                        (errmsg("sending SIGUSR1 to %lu failed", notify_pid)));
                }
            }

            continue;
        }

        /*
         * Copy the registration data into the registered workers list.
         */
        rw = (RegisteredBgWorker*)malloc(sizeof(RegisteredBgWorker));
        if (rw == NULL) {
            ereport(LOG,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                    errmsg("out of memory")));
            return;
        }

        /*
         * Copy strings in a paranoid way.  If shared memory is corrupted, the
         * source data might not even be NUL-terminated.
         */
        ascii_safe_strlcpy(rw->rw_worker.bgw_name,
                           slot->worker.bgw_name, BGW_MAXLEN);
        ascii_safe_strlcpy(rw->rw_worker.bgw_type,
                           slot->worker.bgw_type, BGW_MAXLEN);
        ascii_safe_strlcpy(rw->rw_worker.bgw_library_name,
                           slot->worker.bgw_library_name, BGW_MAXLEN);
        ascii_safe_strlcpy(rw->rw_worker.bgw_function_name,
                           slot->worker.bgw_function_name, BGW_MAXLEN);

        /*
         * Copy various fixed-size fields.
         *
         * flags, start_time, and restart_time are examined by the postmaster,
         * but nothing too bad will happen if they are corrupted.  The
         * remaining fields will only be examined by the child process.  It
         * might crash, but we won't.
         */
        rw->rw_worker.bgw_flags = slot->worker.bgw_flags;
        rw->rw_worker.bgw_start_time = slot->worker.bgw_start_time;
        rw->rw_worker.bgw_restart_time = slot->worker.bgw_restart_time;
        rw->rw_worker.bgw_main_arg = slot->worker.bgw_main_arg;
        int ss_rc = memcpy_s(rw->rw_worker.bgw_extra, BGW_EXTRALEN, slot->worker.bgw_extra, BGW_EXTRALEN);
        securec_check(ss_rc, "\0", "\0");

        /*
         * Copy the PID to be notified about state changes, but only if the
         * postmaster knows about a backend with that PID.  It isn't an error
         * if the postmaster doesn't know about the PID, because the backend
         * that requested the worker could have died (or been killed) just
         * after doing so.  Nonetheless, at least until we get some experience
         * with how this plays out in the wild, log a message at a relative
         * high debug level.
         */
        rw->rw_worker.bgw_notify_pid = slot->worker.bgw_notify_pid;
        if (!PostmasterMarkPIDForWorkerNotify(rw->rw_worker.bgw_notify_pid)) {
            elog(DEBUG1, "worker notification PID %lu is not valid",
                 rw->rw_worker.bgw_notify_pid);
            rw->rw_worker.bgw_notify_pid = 0;
        }

        /* Initialize postmaster bookkeeping. */
        rw->rw_backend = NULL;
        rw->rw_pid = 0;
        rw->rw_child_slot = 0;
        rw->rw_crashed_at = 0;
        rw->rw_shmem_slot = slotno;
        rw->rw_terminate = false;

        /* Log it! */
        ereport(DEBUG1,
            (errmsg("registering background worker \"%s\"",
                rw->rw_worker.bgw_name)));

        slist_push_head(&t_thrd.bgworker_cxt.background_worker_list, &rw->rw_lnode);
    }
}

/*
 * Forget about a background worker that's no longer needed.
 *
 * The worker must be identified by passing an slist_mutable_iter that
 * points to it.  This convention allows deletion of workers during
 * searches of the worker list, and saves having to search the list again.
 *
 * This function must be invoked only in the postmaster.
 */
void ForgetBackgroundWorker(slist_mutable_iter *cur)
{
    RegisteredBgWorker *rw = NULL;
    BackgroundWorkerSlot *slot = NULL;

    rw = slist_container(RegisteredBgWorker, rw_lnode, cur->cur);

    Assert(rw->rw_shmem_slot < g_instance.attr.attr_storage.max_background_workers);
    slot = &t_thrd.bgworker_cxt.background_worker_data->slot[rw->rw_shmem_slot];
    if ((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0) {
        t_thrd.bgworker_cxt.background_worker_data->parallel_terminate_count++;
    }

    slot->in_use = false;

    ereport(DEBUG1,
        (errmsg("unregistering background worker \"%s\"",
            rw->rw_worker.bgw_name)));

    slist_delete_current(cur);
    free(rw);
}

/*
 * Report the PID of a newly-launched background worker in shared memory.
 *
 * This function should only be called from the postmaster.
 */
void ReportBackgroundWorkerPID(const RegisteredBgWorker *rw)
{
    BackgroundWorkerSlot *slot;

    Assert(rw->rw_shmem_slot < g_instance.attr.attr_storage.max_background_workers);
    slot = &t_thrd.bgworker_cxt.background_worker_data->slot[rw->rw_shmem_slot];
    slot->pid = rw->rw_pid;
    ereport(LOG,
        (errmsg("ReportBackgroundWorkerPID slot: %d, pid: %lu, bgw_notify_pid: %lu",
            rw->rw_shmem_slot, slot->pid, rw->rw_worker.bgw_notify_pid)));

    if (rw->rw_worker.bgw_notify_pid != 0) {
        int ret = gs_signal_send(rw->rw_worker.bgw_notify_pid, SIGUSR1);
        ereport(LOG,
            (errmsg("ReportBackgroundWorkerPID send SIGUSR1 to bgw_notify_pid: %lu, ret: %d",
                rw->rw_worker.bgw_notify_pid, ret)));
    }
}

/*
 * Report that the PID of a background worker is now zero because a
 * previously-running background worker has exited.
 *
 * This function should only be called from the postmaster.
 */
void ReportBackgroundWorkerExit(slist_mutable_iter *cur)
{
    RegisteredBgWorker *rw = slist_container(RegisteredBgWorker, rw_lnode, cur->cur);

    Assert(rw->rw_shmem_slot < g_instance.attr.attr_storage.max_background_workers);
    BackgroundWorkerSlot *slot = &t_thrd.bgworker_cxt.background_worker_data->slot[rw->rw_shmem_slot];
    slot->pid = rw->rw_pid;
    ThreadId notify_pid = rw->rw_worker.bgw_notify_pid;

    /*
     * If this worker is slated for deregistration, do that before notifying
     * the process which started it.  Otherwise, if that process tries to
     * reuse the slot immediately, it might not be available yet.  In theory
     * that could happen anyway if the process checks slot->pid at just the
     * wrong moment, but this makes the window narrower.
     */
    if (rw->rw_terminate ||
        rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART) {
        ForgetBackgroundWorker(cur);
    }

    if (notify_pid != 0) {
        int ret = gs_signal_send(notify_pid, SIGUSR1);
        ereport(LOG,
            (errmsg("ReportBackgroundWorkerExit send SIGUSR1 to bgw_notify_pid: %lu, ret: %d",
                notify_pid, ret)));
    }
}

/*
 * Cancel SIGUSR1 notifications for a PID belonging to an exiting backend.
 *
 * This function should only be called from the postmaster.
 */
void BackgroundWorkerStopNotifications(ThreadId pid)
{
    slist_iter  siter;

    slist_foreach(siter, &t_thrd.bgworker_cxt.background_worker_list)
    {
        RegisteredBgWorker *rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
        if (rw->rw_worker.bgw_notify_pid == pid) {
            rw->rw_worker.bgw_notify_pid = 0;
        }
    }
}

/*
 * Reset background worker crash state.
 *
 * We assume that, after a crash-and-restart cycle, background workers without
 * the never-restart flag should be restarted immediately, instead of waiting
 * for bgw_restart_time to elapse.
 */
void ResetBackgroundWorkerCrashTimes(void)
{
    slist_mutable_iter iter;

    slist_foreach_modify(iter, &t_thrd.bgworker_cxt.background_worker_list)
    {
        RegisteredBgWorker *rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);

        if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART) {
            /*
             * Workers marked BGW_NEVER_RESTART shouldn't get relaunched after
             * the crash, so forget about them.  (If we wait until after the
             * crash to forget about them, and they are parallel workers,
             * parallel_terminate_count will get incremented after we've
             * already zeroed parallel_register_count, which would be bad.)
             */
            ForgetBackgroundWorker(&iter);
        } else {
            /*
             * The accounting which we do via parallel_register_count and
             * parallel_terminate_count would get messed up if a worker marked
             * parallel could survive a crash and restart cycle. All such
             * workers should be marked BGW_NEVER_RESTART, and thus control
             * should never reach this branch.
             */
            Assert((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) == 0);

            /*
             * Allow this worker to be restarted immediately after we finish
             * resetting.
             */
            rw->rw_crashed_at = 0;
        }
    }
}

#ifdef EXEC_BACKEND
/*
 * In EXEC_BACKEND mode, return address of the corresponding slot in
 * shared memory.
 */
void* GetBackgroundWorkerShmAddr(int slotno)
{
    Assert(slotno < t_thrd.bgworker_cxt.background_worker_data->total_slots);
    return (void*)&t_thrd.bgworker_cxt.background_worker_data->slot[slotno];
}

/*
 * In EXEC_BACKEND mode, workers use this to retrieve their details from
 * shared memory.
 */
BackgroundWorker* BackgroundWorkerEntry(const BackgroundWorkerSlot* bgWorkerSlotShmAddr)
{
    static THR_LOCAL BackgroundWorker myEntry;

    Assert(bgWorkerSlotShmAddr != NULL);
    Assert(bgWorkerSlotShmAddr->in_use);

    /* must copy this in case we don't intend to retain shmem access */
    int ss_rc = memcpy_s(&myEntry, sizeof(myEntry), &bgWorkerSlotShmAddr->worker, sizeof(myEntry));
    securec_check(ss_rc, "\0", "\0");
    return &myEntry;
}
#endif

/*
 * Complain about the BackgroundWorker definition using error level elevel.
 * Return true if it looks ok, false if not (unless elevel >= ERROR, in
 * which case we won't return at all in the not-OK case).
 */
static bool SanityCheckBackgroundWorker(BackgroundWorker *worker, int elevel)
{
    /* sanity check for flags */
    if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION) {
        if (!(worker->bgw_flags & BGWORKER_SHMEM_ACCESS)) {
            ereport(elevel,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("background worker \"%s\": must attach to shared memory in order to request a database connection",
                            worker->bgw_name)));
            return false;
        }

        if (worker->bgw_start_time == BgWorkerStart_PostmasterStart) {
            ereport(elevel,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("background worker \"%s\": cannot request database access if starting at postmaster start",
                            worker->bgw_name)));
            return false;
        }

        /* XXX other checks? */
    }

    if ((worker->bgw_restart_time < 0 &&
        worker->bgw_restart_time != BGW_NEVER_RESTART) ||
        (worker->bgw_restart_time > USECS_PER_DAY / 1000))  {
        ereport(elevel,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("background worker \"%s\": invalid restart interval",
                    worker->bgw_name)));
        return false;
    }

    /*
     * Parallel workers may not be configured for restart, because the
     * parallel_register_count/parallel_terminate_count accounting can't
     * handle parallel workers lasting through a crash-and-restart cycle.
     */
    if (worker->bgw_restart_time != BGW_NEVER_RESTART &&
        (worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0) {
        ereport(elevel,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("background worker \"%s\": parallel workers may not be configured for restart",
                    worker->bgw_name)));
        return false;
    }

    /*
     * If bgw_type is not filled in, use bgw_name.
     */
    if (strcmp(worker->bgw_type, "") == 0) {
        int rd = strncpy_s(worker->bgw_type, BGW_MAXLEN, worker->bgw_name, BGW_MAXLEN);
        securec_check(rd, "\0", "\0");
    }

    return true;
}

static void bgworker_quickdie(SIGNAL_ARGS)
{
    /*
     * We DO NOT want to run proc_exit() or atexit() callbacks -- we're here
     * because shared memory may be corrupted, so we don't want to try to
     * clean up our transaction.  Just nail the windows shut and get out of
     * town.  The callbacks wouldn't be safe to run from a signal handler,
     * anyway.
     *
     * Note we do _exit(2) not _exit(0).  This is to force the postmaster into
     * a system reset cycle if someone sends a manual SIGQUIT to a random
     * backend.  This is necessary precisely because we don't clean up our
     * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
     * should ensure the postmaster sees this as a crash, too, but no harm in
     * being doubly sure.)
     */
    _exit(2);
}

/*
 * Standard SIGTERM handler for background workers
 */
static void bgworker_die(SIGNAL_ARGS)
{
    (void)gs_signal_setmask(&t_thrd.libpq_cxt.BlockSig, NULL);
    t_thrd.bgworker_cxt.worker_shutdown_requested = true;
    t_thrd.postgres_cxt.whereToSendOutput = DestNone;
    if (t_thrd.proc)
        SetLatch(&t_thrd.proc->procLatch);
    ereport(WARNING,
        (errcode(ERRCODE_ADMIN_SHUTDOWN),
            errmsg("terminating background worker \"%s\" due to administrator command",
                t_thrd.bgworker_cxt.my_bgworker_entry->bgw_type)));
}

/*
 * Standard SIGUSR1 handler for unconnected workers
 *
 * Here, we want to make sure an unconnected worker will at least heed
 * latch activity.
 */
static void bgworker_sigusr1_handler(SIGNAL_ARGS)
{
    int save_errno = errno;

    latch_sigusr1_handler();

    errno = save_errno;
}

/*
 * Start a new background worker
 *
 * This is the main entry point for background worker, to be called from
 * postmaster.
 */
void StartBackgroundWorker(void* bgWorkerSlotShmAddr)
{
    sigjmp_buf local_sigjmp_buf;
    t_thrd.bgworker_cxt.my_bgworker_entry = BackgroundWorkerEntry((BackgroundWorkerSlot *)bgWorkerSlotShmAddr);
    BackgroundWorker *worker = t_thrd.bgworker_cxt.my_bgworker_entry;
    bgworker_main_type entrypt;

    knl_thread_set_name("BgWorker");
    /*
     * Create memory context and buffer used for RowDescription messages. As
     * SendRowDescriptionMessage(), via exec_describe_statement_message(), is
     * frequently executed for ever single statement, we don't want to
     * allocate a separate buffer every time.
     */
    t_thrd.mem_cxt.row_desc_mem_cxt = AllocSetContextCreate(t_thrd.top_mem_cxt,
        "RowDescriptionContext",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    MemoryContext old_mc = MemoryContextSwitchTo(t_thrd.mem_cxt.row_desc_mem_cxt);
    initStringInfo(&(*t_thrd.postgres_cxt.row_description_buf));
    (void)MemoryContextSwitchTo(old_mc);
    
    t_thrd.mem_cxt.mask_password_mem_cxt = AllocSetContextCreate(t_thrd.top_mem_cxt,
        "MaskPasswordCtx",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);

    if (worker == NULL) {
        ereport(FATAL,
            (errmsg("unable to find bgworker entry")));
    }

    t_thrd.bgworker_cxt.is_background_worker = true;
    t_thrd.bgworker_cxt.worker_shutdown_requested = false;
    /* Identify myself via ps */
    init_ps_display(worker->bgw_name, "", "", "");

    SetProcessingMode(InitProcessing);

    /*
     * Set up signal handlers.
     */
    if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION) {
        /*
         * SIGINT is used to signal canceling the current action
         */
        (void)gspqsignal(SIGINT, StatementCancelHandler);
        (void)gspqsignal(SIGUSR1, procsignal_sigusr1_handler);
        (void)gspqsignal(SIGFPE, FloatExceptionHandler);

        /* XXX Any other handlers needed here? */
    } else {
        (void)gspqsignal(SIGINT, SIG_IGN);
        (void)gspqsignal(SIGUSR1, bgworker_sigusr1_handler);
        (void)gspqsignal(SIGFPE, SIG_IGN);
    }
    (void)gspqsignal(SIGTERM, bgworker_die);
    (void)gspqsignal(SIGHUP, SIG_IGN);

    (void)gspqsignal(SIGQUIT, bgworker_quickdie);
    (void)gspqsignal(SIGALRM, handle_sig_alarm);

    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR2, SIG_IGN);
    (void)gspqsignal(SIGCHLD, SIG_DFL);

    (void)gs_signal_unblock_sigusr2();
    if (IsUnderPostmaster) {
        /* We allow SIGQUIT (quickdie) at all times */
        (void)sigdelset(&t_thrd.libpq_cxt.BlockSig, SIGQUIT);
    }

    gs_signal_setmask(&t_thrd.libpq_cxt.BlockSig, NULL); /* block everything except SIGQUIT */

    /*
     * If an exception is encountered, processing resumes here.
     *
     * See notes in postgres.c about the design of this coding.
     */
    if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
        /* Since not using PG_TRY, must reset error stack by hand */
        t_thrd.log_cxt.error_context_stack = NULL;

        /* Prevent interrupts while cleaning up */
        HOLD_INTERRUPTS();

        /* output the memory tracking information when error happened */
        MemoryTrackingOutputFile();
        
        /* Report the error to the server log */
        EmitErrorReport();

        AbortCurrentTransaction();
        /*
         * Do we need more cleanup here?  For shmem-connected bgworkers, we
         * will call InitProcess below, which will install ProcKill as exit
         * callback.  That will take care of releasing locks, etc.
         */

        /* and go away */
        proc_exit(1);
    }

    /* We can now handle ereport(ERROR) */
    t_thrd.log_cxt.PG_exception_stack = &local_sigjmp_buf;

    /*
     * If the background worker request shared memory access, set that up now;
     * else, detach all shared memory segments.
     */
    if (worker->bgw_flags & BGWORKER_SHMEM_ACCESS) {
        /*
         * Early initialization.  Some of this could be useful even for
         * background workers that aren't using shared memory, but they can
         * call the individual startup routines for those subsystems if
         * needed.
         */
        BaseInit();

        /*
         * Create a per-backend PGPROC struct in shared memory, except in the
         * EXEC_BACKEND case where this was done in SubPostmasterMain. We must
         * do this before we can use LWLocks (and in the EXEC_BACKEND case we
         * already had to do some stuff with LWLocks).
         */
#ifndef EXEC_BACKEND
        InitProcess();
#endif
    }

    /* Initialize the memory tracking information */
    MemoryTrackingInit();

    /*
     * Look up the entry point function, loading its library if necessary.
     */
    entrypt = LookupBackgroundWorkerFunction(worker->bgw_library_name,
                                             worker->bgw_function_name);

    /*
     * Note that in normal processes, we would call InitPostgres here.  For a
     * worker, however, we don't know what database to connect to, yet; so we
     * need to wait until the user code does it via
     * BackgroundWorkerInitializeConnection().
     */

    /*
     * Now invoke the user-defined worker code
     */
    entrypt(worker->bgw_main_arg);

    /* ... and if it returns, we're done */
    proc_exit(0);
}

/*
 * Register a new static background worker.
 *
 * This can only be called directly from postmaster or in the _PG_init
 * function of a module library that's loaded by shared_preload_libraries;
 * otherwise it will have no effect.
 */
void RegisterBackgroundWorker(BackgroundWorker *worker)
{
    RegisteredBgWorker *rw;
    static THR_LOCAL int    numworkers = 0;

    if (!IsUnderPostmaster) {
        ereport(DEBUG1,
            (errmsg("registering background worker \"%s\"", worker->bgw_name)));
    }

    if (!u_sess->misc_cxt.process_shared_preload_libraries_in_progress &&
        strcmp(worker->bgw_library_name, "postgres") != 0) {
        if (!IsUnderPostmaster) {
            ereport(LOG,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("background worker \"%s\": must be registered in shared_preload_libraries",
                        worker->bgw_name)));
        }
        return;
    }

    if (!SanityCheckBackgroundWorker(worker, LOG)) {
        return;
    }

    if (worker->bgw_notify_pid != 0) {
        ereport(LOG,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("background worker \"%s\": only dynamic background workers can request notification",
                    worker->bgw_name)));
        return;
    }

    /*
     * Enforce maximum number of workers.  Note this is overly restrictive: we
     * could allow more non-shmem-connected workers, because these don't count
     * towards the MAX_BACKENDS limit elsewhere.  For now, it doesn't seem
     * important to relax this restriction.
     */
    if (++numworkers > g_instance.attr.attr_storage.max_background_workers) {
        ereport(LOG,
            (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
                 errmsg("too many background workers"),
                 errdetail_plural("Up to %d background worker can be registered with the current settings.",
                                  "Up to %d background workers can be registered with the current settings.",
                                  g_instance.attr.attr_storage.max_background_workers,
                                  g_instance.attr.attr_storage.max_background_workers),
                 errhint("Consider increasing the configuration parameter \"max_background_workers\".")));
        return;
    }

    /*
     * Copy the registration data into the registered workers list.
     */
    rw = (RegisteredBgWorker*)malloc(sizeof(RegisteredBgWorker));
    if (rw == NULL) {
        ereport(LOG,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory")));
        return;
    }

    rw->rw_worker = *worker;
    rw->rw_backend = NULL;
    rw->rw_pid = 0;
    rw->rw_child_slot = 0;
    rw->rw_crashed_at = 0;
    rw->rw_terminate = false;

    slist_push_head(&t_thrd.bgworker_cxt.background_worker_list, &rw->rw_lnode);
}

/*
 * Register a new background worker from a regular backend.
 *
 * Returns true on success and false on failure.  Failure typically indicates
 * that no background worker slots are currently available.
 *
 * If handle != NULL, we'll set *handle to a pointer that can subsequently
 * be used as an argument to GetBackgroundWorkerPid().  The caller can
 * free this pointer using pfree(), if desired.
 */
bool RegisterDynamicBackgroundWorker(BackgroundWorker *worker,
    BackgroundWorkerHandle **handle)
{
    int         slotno;
    bool        success = false;
    bool        parallel;
    uint64      generation = 0;

    /*
     * We can't register dynamic background workers from the postmaster. If
     * this is a standalone backend, we're the only process and can't start
     * any more.  In a multi-process environment, it might be theoretically
     * possible, but we don't currently support it due to locking
     * considerations; see comments on the BackgroundWorkerSlot data
     * structure.
     */
    if (!IsUnderPostmaster) {
        return false;
    }

    if (!SanityCheckBackgroundWorker(worker, ERROR)) {
        return false;
    }

    parallel = (worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0;

    (void)LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);

    /*
     * If this is a parallel worker, check whether there are already too many
     * parallel workers; if so, don't register another one.  Our view of
     * parallel_terminate_count may be slightly stale, but that doesn't really
     * matter: we would have gotten the same result if we'd arrived here
     * slightly earlier anyway.  There's no help for it, either, since the
     * postmaster must not take locks; a memory barrier wouldn't guarantee
     * anything useful.
     */
    if (parallel && (int)(t_thrd.bgworker_cxt.background_worker_data->parallel_register_count -
        t_thrd.bgworker_cxt.background_worker_data->parallel_terminate_count) >=
        g_instance.shmem_cxt.max_parallel_workers) {
        Assert(t_thrd.bgworker_cxt.background_worker_data->parallel_register_count -
            t_thrd.bgworker_cxt.background_worker_data->parallel_terminate_count <=
            MAX_PARALLEL_WORKER_LIMIT);
        LWLockRelease(BackgroundWorkerLock);
        return false;
    }

    /*
     * Look for an unused slot.  If we find one, grab it.
     */
    for (slotno = 0; slotno < t_thrd.bgworker_cxt.background_worker_data->total_slots; ++slotno) {
        BackgroundWorkerSlot *slot = &t_thrd.bgworker_cxt.background_worker_data->slot[slotno];

        if (!slot->in_use) {
            int ss_rc = memcpy_s(&slot->worker, sizeof(BackgroundWorker), worker, sizeof(BackgroundWorker));
            securec_check(ss_rc, "\0", "\0");
            slot->pid = InvalidPid; /* indicates not started yet */
            slot->generation++;
            slot->terminate = false;
            generation = slot->generation;
            if (parallel)
                t_thrd.bgworker_cxt.background_worker_data->parallel_register_count++;

            /*
             * Make sure postmaster doesn't see the slot as in use before it
             * sees the new contents.
             */
            pg_write_barrier();

            slot->in_use = true;
            success = true;
            break;
        }
    }

    LWLockRelease(BackgroundWorkerLock);

    /* If we found a slot, tell the postmaster to notice the change. */
    if (success) {
        SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);
    }

    /*
     * If we found a slot and the user has provided a handle, initialize it.
     */
    if (success && handle) {
        *handle = (BackgroundWorkerHandle*)palloc(sizeof(BackgroundWorkerHandle));
        (*handle)->slot = slotno;
        (*handle)->generation = generation;
    }

    return success;
}

/*
 * Get the PID of a dynamically-registered background worker.
 *
 * If the worker is determined to be running, the return value will be
 * BGWH_STARTED and *pidp will get the PID of the worker process.  If the
 * postmaster has not yet attempted to start the worker, the return value will
 * be BGWH_NOT_YET_STARTED.  Otherwise, the return value is BGWH_STOPPED.
 *
 * BGWH_STOPPED can indicate either that the worker is temporarily stopped
 * (because it is configured for automatic restart and exited non-zero),
 * or that the worker is permanently stopped (because it exited with exit
 * code 0, or was not configured for automatic restart), or even that the
 * worker was unregistered without ever starting (either because startup
 * failed and the worker is not configured for automatic restart, or because
 * TerminateBackgroundWorker was used before the worker was successfully
 * started).
 */
BgwHandleStatus GetBackgroundWorkerPid(const BackgroundWorkerHandle *handle, ThreadId *pidp)
{
    ThreadId pid = InvalidPid;

    Assert(handle->slot < g_instance.attr.attr_storage.max_background_workers);
    BackgroundWorkerSlot* slot = &t_thrd.bgworker_cxt.background_worker_data->slot[handle->slot];

    /*
     * We could probably arrange to synchronize access to data using memory
     * barriers only, but for now, let's just keep it simple and grab the
     * lock.  It seems unlikely that there will be enough traffic here to
     * result in meaningful contention.
     */
    (void)LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

    /*
     * The generation number can't be concurrently changed while we hold the
     * lock.  The pid, which is updated by the postmaster, can change at any
     * time, but we assume such changes are atomic.  So the value we read
     * won't be garbage, but it might be out of date by the time the caller
     * examines it (but that's unavoidable anyway).
     *
     * The in_use flag could be in the process of changing from true to false,
     * but if it is already false then it can't change further.
     */
    if (handle->generation != slot->generation || !slot->in_use) {
        pid = 0;
    } else {
        pid = slot->pid;
    }

    /* All done. */
    LWLockRelease(BackgroundWorkerLock);

    ereport(DEBUG1,
        (errmsg("GetBackgroundWorkerPid slot: %d, pid: %lu",
            handle->slot, pid)));
    if (pid == 0) {
        return BGWH_STOPPED;
    } else if (pid == InvalidPid) {
        return BGWH_NOT_YET_STARTED;
    }
    *pidp = pid;
    return BGWH_STARTED;
}

/*
 * Wait for a background worker to start up.
 *
 * This is like GetBackgroundWorkerPid(), except that if the worker has not
 * yet started, we wait for it to do so; thus, BGWH_NOT_YET_STARTED is never
 * returned.  However, if the postmaster has died, we give up and return
 * BGWH_POSTMASTER_DIED, since it that case we know that startup will not
 * take place.
 */
BgwHandleStatus WaitForBackgroundWorkerStartup(const BackgroundWorkerHandle *handle, ThreadId *pidp)
{
    BgwHandleStatus status;
    int rc;
    volatile knl_thrd_context* localThrd = &t_thrd;

    for (;;) {
        ThreadId pid = 0;

        CHECK_FOR_INTERRUPTS();

        status = GetBackgroundWorkerPid(handle, &pid);
        ereport(LOG,
            (errmsg("WaitForBackgroundWorkerStartup slot: %d, pid: %lu, status: %u, mypid: %lu",
                handle->slot, pid, status, t_thrd.proc_cxt.MyProcPid)));
        ereport(LOG,
            (errmsg("WaitForBackgroundWorkerStartup addr: %p", localThrd)));
        if (status == BGWH_STARTED) {
            *pidp = pid;
        }
        if (status != BGWH_NOT_YET_STARTED) {
            break;
        }

        rc = WaitLatch(&t_thrd.proc->procLatch,
                       WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
        if (rc & WL_POSTMASTER_DEATH) {
            status = BGWH_POSTMASTER_DIED;
            break;
        }

        ResetLatch(&t_thrd.proc->procLatch);
    }

    return status;
}

/*
 * Wait for a background worker to stop.
 *
 * If the worker hasn't yet started, or is running, we wait for it to stop
 * and then return BGWH_STOPPED.  However, if the postmaster has died, we give
 * up and return BGWH_POSTMASTER_DIED, because it's the postmaster that
 * notifies us when a worker's state changes.
 */
BgwHandleStatus WaitForBackgroundWorkerShutdown(const BackgroundWorkerHandle *handle)
{
    BgwHandleStatus status;
    int rc;

    for (;;) {
        ThreadId pid = InvalidPid;

        CHECK_FOR_INTERRUPTS();

        status = GetBackgroundWorkerPid(handle, &pid);
        if (status == BGWH_STOPPED) {
            break;
        }

        rc = WaitLatch(&t_thrd.proc->procLatch,
                       WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
        if (rc & WL_POSTMASTER_DEATH) {
            status = BGWH_POSTMASTER_DIED;
            break;
        }

        ResetLatch(&t_thrd.proc->procLatch);
    }

    return status;
}

/*
 * Instruct the postmaster to terminate a background worker.
 *
 * Note that it's safe to do this without regard to whether the worker is
 * still running, or even if the worker may already have existed and been
 * unregistered.
 */
void TerminateBackgroundWorker(const BackgroundWorkerHandle *handle)
{
    bool signal_postmaster = false;

    Assert( handle->slot >= 0 && handle->slot < g_instance.attr.attr_storage.max_background_workers);
    BackgroundWorkerSlot* slot = &t_thrd.bgworker_cxt.background_worker_data->slot[handle->slot];

    /* Set terminate flag in shared memory, unless slot has been reused. */
    (void)LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);
    if (handle->generation == slot->generation) {
        slot->terminate = true;
        signal_postmaster = true;
    }
    LWLockRelease(BackgroundWorkerLock);

    /* Make sure the postmaster notices the change to shared memory. */
    if (signal_postmaster) {
        SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);
    }
}

void StopBackgroundWorker() 
{
    TerminateBackgroundWorker(&(t_thrd.autonomous_cxt.handle));
    (void)WaitForBackgroundWorkerShutdown(&(t_thrd.autonomous_cxt.handle));
    // reset handle of autonomous_cxt
    t_thrd.autonomous_cxt.handle.slot = -1;
    t_thrd.autonomous_cxt.handle.generation = 0;
}

/*
 * Look up (and possibly load) a bgworker entry point function.
 *
 * For functions contained in the core code, we use library name "postgres"
 * and consult the InternalBGWorkers array.  External functions are
 * looked up, and loaded if necessary, using load_external_function().
 *
 * The point of this is to pass function names as strings across process
 * boundaries.  We can't pass actual function addresses because of the
 * possibility that the function has been loaded at a different address
 * in a different process.  This is obviously a hazard for functions in
 * loadable libraries, but it can happen even for functions in the core code
 * on platforms using EXEC_BACKEND (e.g., Windows).
 *
 * At some point it might be worthwhile to get rid of InternalBGWorkers[]
 * in favor of applying load_external_function() for core functions too;
 * but that raises portability issues that are not worth addressing now.
 */
static bgworker_main_type LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname)
{
    /*
     * If the function is to be loaded from postgres itself, search the
     * InternalBGWorkers array.
     */
    if (strcmp(libraryname, "postgres") == 0) {
        size_t i;
        for (i = 0; i < lengthof(InternalBGWorkers); i++) {
            if (strcmp(InternalBGWorkers[i].fn_name, funcname) == 0) {
                return InternalBGWorkers[i].fn_addr;
            }
        }

        /* We can only reach this by programming error. */
        elog(ERROR, "internal function \"%s\" not found", funcname);
    }

    /* Otherwise load from external library. */
    return (bgworker_main_type)
        load_external_function(libraryname, (char*)funcname, true, true).user_fn;
}

/*
 * Given a PID, get the bgw_type of the background worker.  Returns NULL if
 * not a valid background worker.
 *
 * The return value is in static memory belonging to this function, so it has
 * to be used before calling this function again.  This is so that the caller
 * doesn't have to worry about the background worker locking protocol.
 */
const char* GetBackgroundWorkerTypeByPid(ThreadId pid)
{
    int         slotno;
    bool        found = false;
    static THR_LOCAL char result[BGW_MAXLEN];

    (void)LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

    for (slotno = 0; slotno < t_thrd.bgworker_cxt.background_worker_data->total_slots; slotno++) {
        BackgroundWorkerSlot *slot = &t_thrd.bgworker_cxt.background_worker_data->slot[slotno];

        if (slot->pid > 0 && slot->pid == pid) {
            int rd = strncpy_s(result, BGW_MAXLEN, slot->worker.bgw_type, BGW_MAXLEN);
            securec_check(rd, "\0", "\0");
            found = true;
            break;
        }
    }

    LWLockRelease(BackgroundWorkerLock);

    if (!found) {
        return NULL;
    }

    return result;
}

/*
 * Connect background worker to a database.
 */
void BackgroundWorkerInitializeConnection(const char *dbname, const char *username, uint32 flags)
{
    BackgroundWorker *worker = t_thrd.bgworker_cxt.my_bgworker_entry;

    /* XXX is this the right errcode? */
    if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)) {
        ereport(FATAL,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("database connection requirement not indicated during registration")));
    }

    t_thrd.proc_cxt.PostInit->SetDatabaseAndUser(dbname, InvalidOid, username, InvalidOid);
    t_thrd.proc_cxt.PostInit->InitBackendWorker();

    /* it had better not gotten out of "init" mode yet */
    if (!IsInitProcessingMode()) {
        ereport(ERROR,
            (errmsg("invalid processing mode in background worker")));
    }
    SetProcessingMode(NormalProcessing);
}

/*
 * Connect background worker to a database using OIDs.
 */
void BackgroundWorkerInitializeConnectionByOid(Oid dboid, Oid useroid, uint32 flags)
{
    BackgroundWorker *worker = t_thrd.bgworker_cxt.my_bgworker_entry;

    /* XXX is this the right errcode? */
    if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)) {
        ereport(FATAL,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("database connection requirement not indicated during registration")));
    }

    t_thrd.proc_cxt.PostInit->SetDatabaseAndUser(NULL, dboid, NULL, useroid);
    t_thrd.proc_cxt.PostInit->InitBackendWorker();

    /* it had better not gotten out of "init" mode yet */
    if (!IsInitProcessingMode()) {
        ereport(ERROR,
            (errmsg("invalid processing mode in background worker")));
    }
    SetProcessingMode(NormalProcessing);
}

/*
 * Block/unblock signals in a background worker
 */
void BackgroundWorkerBlockSignals(void)
{
    (void)gs_signal_setmask(&t_thrd.libpq_cxt.BlockSig, NULL);
}

void BackgroundWorkerUnblockSignals(void)
{
    (void)gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
}

