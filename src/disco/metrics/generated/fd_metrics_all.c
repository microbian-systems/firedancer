/* THIS FILE IS GENERATED BY gen_metrics.py. DO NOT HAND EDIT. */
#include "fd_metrics_all.h"

const fd_metrics_meta_t FD_METRICS_ALL[FD_METRICS_ALL_TOTAL] = {
    DECLARE_METRIC( TILE_PID, GAUGE ),
    DECLARE_METRIC( TILE_TID, GAUGE ),
    DECLARE_METRIC( TILE_CONTEXT_SWITCH_INVOLUNTARY_COUNT, COUNTER ),
    DECLARE_METRIC( TILE_CONTEXT_SWITCH_VOLUNTARY_COUNT, COUNTER ),
    DECLARE_METRIC( TILE_STATUS, GAUGE ),
    DECLARE_METRIC( TILE_HEARTBEAT, GAUGE ),
    DECLARE_METRIC( TILE_IN_BACKPRESSURE, GAUGE ),
    DECLARE_METRIC( TILE_BACKPRESSURE_COUNT, COUNTER ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, CAUGHT_UP_HOUSEKEEPING ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, PROCESSING_HOUSEKEEPING ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, BACKPRESSURE_HOUSEKEEPING ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, CAUGHT_UP_PREFRAG ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, PROCESSING_PREFRAG ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, BACKPRESSURE_PREFRAG ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, CAUGHT_UP_POSTFRAG ),
    DECLARE_METRIC_ENUM( TILE_REGIME_DURATION_NANOS, COUNTER, TILE_REGIME, PROCESSING_POSTFRAG ),
};

const fd_metrics_meta_t FD_METRICS_ALL_LINK_IN[FD_METRICS_ALL_LINK_IN_TOTAL] = {
    DECLARE_METRIC( LINK_CONSUMED_COUNT, COUNTER ),
    DECLARE_METRIC( LINK_CONSUMED_SIZE_BYTES, COUNTER ),
    DECLARE_METRIC( LINK_FILTERED_COUNT, COUNTER ),
    DECLARE_METRIC( LINK_FILTERED_SIZE_BYTES, COUNTER ),
    DECLARE_METRIC( LINK_OVERRUN_POLLING_COUNT, COUNTER ),
    DECLARE_METRIC( LINK_OVERRUN_POLLING_FRAG_COUNT, COUNTER ),
    DECLARE_METRIC( LINK_OVERRUN_READING_COUNT, COUNTER ),
    DECLARE_METRIC( LINK_OVERRUN_READING_FRAG_COUNT, COUNTER ),
};

const fd_metrics_meta_t FD_METRICS_ALL_LINK_OUT[FD_METRICS_ALL_LINK_OUT_TOTAL] = {
    DECLARE_METRIC( LINK_SLOW_COUNT, COUNTER ),
};

const char * FD_METRICS_TILE_KIND_NAMES[FD_METRICS_TILE_KIND_CNT] = {
    "net",
    "quic",
    "bundle",
    "verify",
    "dedup",
    "resolv",
    "pack",
    "bank",
    "poh",
    "shred",
    "store",
    "replay",
    "storei",
    "gossip",
    "netlnk",
    "sock",
};

const ulong FD_METRICS_TILE_KIND_SIZES[FD_METRICS_TILE_KIND_CNT] = {
    FD_METRICS_NET_TOTAL,
    FD_METRICS_QUIC_TOTAL,
    FD_METRICS_BUNDLE_TOTAL,
    FD_METRICS_VERIFY_TOTAL,
    FD_METRICS_DEDUP_TOTAL,
    FD_METRICS_RESOLV_TOTAL,
    FD_METRICS_PACK_TOTAL,
    FD_METRICS_BANK_TOTAL,
    FD_METRICS_POH_TOTAL,
    FD_METRICS_SHRED_TOTAL,
    FD_METRICS_STORE_TOTAL,
    FD_METRICS_REPLAY_TOTAL,
    FD_METRICS_STOREI_TOTAL,
    FD_METRICS_GOSSIP_TOTAL,
    FD_METRICS_NETLNK_TOTAL,
    FD_METRICS_SOCK_TOTAL,
};
const fd_metrics_meta_t * FD_METRICS_TILE_KIND_METRICS[FD_METRICS_TILE_KIND_CNT] = {
    FD_METRICS_NET,
    FD_METRICS_QUIC,
    FD_METRICS_BUNDLE,
    FD_METRICS_VERIFY,
    FD_METRICS_DEDUP,
    FD_METRICS_RESOLV,
    FD_METRICS_PACK,
    FD_METRICS_BANK,
    FD_METRICS_POH,
    FD_METRICS_SHRED,
    FD_METRICS_STORE,
    FD_METRICS_REPLAY,
    FD_METRICS_STOREI,
    FD_METRICS_GOSSIP,
    FD_METRICS_NETLNK,
    FD_METRICS_SOCK,
};
