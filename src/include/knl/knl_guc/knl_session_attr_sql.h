/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 * 
 * knl_session_attr_sql.h
 *   Data struct to store knl_session_attr_sql variables.
 *
 *   When anyone try to added variable in this file, which means add a guc
 *   variable, there are several rules needed to obey:
 *
 *   add variable to struct 'knl_@level@_attr_@group@'
 *
 *   @level@:
 *   1. instance: the level of guc variable is PGC_POSTMASTER.
 *   2. session: the other level of guc variable.
 *
 *   @group@: sql, storage, security, network, memory, resource, common
 *   select the group according to the type of guc variable.
 * 
 * IDENTIFICATION
 *        src/include/knl/knl_guc/knl_session_attr_sql.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef SRC_INCLUDE_KNL_KNL_SESSION_ATTR_SQL
#define SRC_INCLUDE_KNL_KNL_SESSION_ATTR_SQL

#include "knl/knl_guc/knl_guc_common.h"

typedef struct knl_session_attr_sql {
    bool enable_fast_numeric;
    bool enable_global_stats;
    bool enable_hdfs_predicate_pushdown;
    bool enable_absolute_tablespace;
    bool enable_hadoop_env;
    bool enable_valuepartition_pruning;
    bool enable_constraint_optimization;
    bool enable_bloom_filter;
    bool enable_codegen;
    bool enable_codegen_print;
    bool enable_sonic_optspill;
    bool enable_sonic_hashjoin;
    bool enable_sonic_hashagg;
    bool enable_upsert_to_merge;
    bool enable_csqual_pushdown;
    bool enable_change_hjcost;
    bool enable_seqscan;
    bool enable_indexscan;
    bool enable_indexonlyscan;
    bool enable_bitmapscan;
    bool force_bitmapand;
    bool enable_parallel_ddl;
    bool enable_tidscan;
    bool enable_sort;
    bool enable_compress_spill;
    bool enable_hashagg;
    bool enable_material;
    bool enable_nestloop;
    bool enable_mergejoin;
    bool enable_hashjoin;
    bool enable_parallel_append;
    bool enable_index_nestloop;
    bool enable_nodegroup_debug;
    bool enable_partitionwise;
    bool enable_remotejoin;
    bool enable_fast_query_shipping;
    bool enable_compress_hll;
    bool enable_remotegroup;
    bool enable_remotesort;
    bool enable_remotelimit;
    bool gtm_backup_barrier;
    bool enable_stream_operator;
    bool enable_stream_concurrent_update;
    bool enable_vector_engine;
    bool enable_force_vector_engine;
    bool enable_random_datanode;
    bool enable_fstream;
    bool enable_geqo;
    bool restart_after_crash;
    bool enable_early_free;
    bool enable_kill_query;
    bool log_duration;
    bool Debug_print_parse;
    bool Debug_print_rewritten;
    bool Debug_print_plan;
    bool Debug_pretty_print;
    bool enable_analyze_check;
    bool enable_autoanalyze;
    bool SQL_inheritance;
    bool Transform_null_equals;
    bool check_function_bodies;
    bool Array_nulls;
    bool default_with_oids;
#ifdef DEBUG_BOUNDED_SORT
    bool optimize_bounded_sort;
#endif
    bool escape_string_warning;
    bool standard_conforming_strings;
    bool enable_light_proxy;
    bool enable_pbe_optimization;
    bool enable_cluster_resize;
    bool lo_compat_privileges;
    bool quote_all_identifiers;
    bool enforce_a_behavior;
    bool enable_slot_log;
    bool convert_string_to_digit;
    bool agg_redistribute_enhancement;
    bool enable_broadcast;
    bool ngram_punctuation_ignore;
    bool ngram_grapsymbol_ignore;
    bool enable_fast_allocate;
    bool td_compatible_truncation;
    bool enable_upgrade_merge_lock_mode;
    bool acceleration_with_compute_pool;
    bool enable_extrapolation_stats;
    bool enable_trigger_shipping;
    bool enable_agg_pushdown_for_cooperation_analysis;
    bool enable_online_ddl_waitlock;
    bool show_acce_estimate_detail;
    bool enable_prevent_job_task_startup;
    int from_collapse_limit;
    int join_collapse_limit;
    int geqo_threshold;
    int Geqo_effort;
    int Geqo_pool_size;
    int Geqo_generations;
    int g_default_log2m;
    int g_default_regwidth;
    int g_default_sparseon;
    int g_max_sparse;
    int g_planCacheMode;
    int cost_param;
    int schedule_splits_threshold;
    int hashagg_table_size;
    int statement_mem;
    int statement_max_mem;
    int temp_file_limit;
    int effective_cache_size;
    int best_agg_plan;
    int query_dop_tmp;
    int plan_mode_seed;
    int codegen_cost_threshold;
    int acce_min_datasize_per_thread;
    int max_cn_temp_file_size;
    int default_statistics_target;
    int min_parallel_table_scan_size;
    int min_parallel_index_scan_size;
    /* Memory Limit user could set in session */
    int FencedUDFMemoryLimit;
    int64 g_default_expthresh;
    double seq_page_cost;
    double random_page_cost;
    double cpu_tuple_cost;
    double allocate_mem_cost;
    double cpu_index_tuple_cost;
    double cpu_operator_cost;
    double parallel_tuple_cost;
    double parallel_setup_cost;
    double stream_multiple;
    double cursor_tuple_fraction;
    double Geqo_selection_bias;
    double Geqo_seed;
    double phony_random_seed;
    char* expected_computing_nodegroup;
    char* default_storage_nodegroup;
    char* inlist2join_optmode;
    char* behavior_compat_string;
    char* connection_info;
    char* retry_errcode_list;
    /* the vmoptions to start JVM */
    char* pljava_vmoptions;
    int backslash_quote;
    int constraint_exclusion;
    int rewrite_rule;
    int sql_compatibility;  /* reference to DB_Compatibility */
    int guc_explain_perf_mode;
    int skew_strategy_store;
    int codegen_strategy;

    bool enable_unshipping_log;
    /*
     * enable to support "with-recursive" stream plan
     */
    bool enable_stream_recursive;
    bool enable_save_datachanged_timestamp;
    int max_recursive_times;
    /* Table skewness warning rows, range from 0 to INT_MAX*/
    int table_skewness_warning_rows;
    /* Table skewness warning threshold, range from 0 to 1, 0 indicates feature disabled*/
    double table_skewness_warning_threshold;
    bool enable_opfusion;
    bool enable_beta_opfusion;
    bool enable_beta_nestloop_fusion;
    bool parallel_leader_participation;
    int opfusion_debug_mode;
    int single_shard_stmt;
    int force_parallel_mode;
    int max_parallel_workers_per_gather;
    int max_parallel_maintenance_workers;
} knl_session_attr_sql;

#endif /* SRC_INCLUDE_KNL_KNL_SESSION_ATTR_SQL */
