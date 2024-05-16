#include "../../fdctl.h"
#include "../../../../disco/poh/fd_poh_tile.h"

#include "../../../../ballet/pack/fd_pack.h"
#include "../../../../ballet/sha256/fd_sha256.h"
#include "../../../../disco/topo/fd_pod_format.h"
#include "../../../../disco/shred/fd_shredder.h"
#include "../../../../disco/shred/fd_stake_ci.h"
#include "../../../../disco/bank/fd_bank_abi.h"
#include "../../../../disco/keyguard/fd_keyload.h"
#include "../../../../disco/metrics/generated/fd_metrics_poh.h"
#include "../../../../flamenco/leaders/fd_leaders.h"

/* The PoH recorder is implemented in Firedancer but for now needs to
   work with Solana Labs, so we have a locking scheme for them to
   co-operate.

   This is because the PoH tile lives in the Solana Labs memory address
   space and their version of concurrency is locking the PoH recorder
   and reading arbitrary fields.

   So we allow them to lock the PoH tile, although with a very bad (for
   them) locking scheme.  By default, the tile has full and exclusive
   access to the data.  If part of Solana Labs wishes to read/write they
   can either,

     1. Rewrite their concurrency to message passing based on mcache
        (preferred, but not feasible).
     2. Signal to the tile they wish to acquire the lock, by setting
        fd_poh_waiting_lock to 1.

   During housekeeping, the tile will check if there is the waiting lock
   is set to 1, and if so, set the returned lock to 1, indicating to the
   waiter that they may now proceed.

   When the waiter is done reading and writing, they restore the
   returned lock value back to zero, and the POH tile continues with its
   day. */

typedef struct {
  fd_poh_tile_ctx_t * poh_tile_ctx;

  /* If we currently are the leader according the clock AND we have
     received the leader bank for the slot from the replay stage,
     this value will be non-NULL.

     Note that we might be inside our leader slot, but not have a bank
     yet, in which case this will still be NULL.

     It will be NULL for a brief race period between consecutive leader
     slots, as we ping-pong back to replay stage waiting for a new bank.

     Solana Labs refers to this as the "working bank". */
  void const * current_leader_bank;

  /* The Solana Labs client needs to be notified when the leader changes,
     so that they can resume the replay stage if it was suspended waiting. */
  void * signal_leader_change;

  ulong bank_cnt;

  ulong stake_in_idx;

  ulong pack_in_idx;

  fd_pubkey_t identity_key;

  /* These are temporarily set in during_frag so they can be used in
     after_frag once the frag has been validated as not overrun. */
  uchar _txns[ USHORT_MAX ];
  fd_microblock_trailer_t * _microblock_trailer;

  fd_poh_tile_in_ctx_t bank_in[ 32 ];
  fd_poh_tile_in_ctx_t stake_in;
  fd_poh_tile_in_ctx_t pack_in;

} fd_poh_ctx_t;

/* The PoH tile needs to interact with the Solana Labs address space to
   do certain operations that Firedancer hasn't reimplemented yet, a.k.a
   transaction execution.  We have Solana Labs export some wrapper
   functions that we call into during regular tile execution.  These do
   not need any locking, since they are called serially from the single
   PoH tile. */

extern void fd_ext_bank_acquire( void const * bank );
extern void fd_ext_bank_release( void const * bank );
extern void fd_ext_poh_signal_leader_change( void * sender );
extern void fd_ext_poh_register_tick( void const * bank, uchar const * hash );

/* fd_ext_poh_initialize is called by Solana Labs on startup to
   initialize the PoH tile with some static configuration, and the
   initial reset slot and hash which it retrieves from a snapshot.

   This function is called by some random Solana Labs thread, but
   it blocks booting of the PoH tile.  The tile will spin until it
   determines that this initialization has happened.

   signal_leader_change is an opaque Rust object that is used to
   tell the replay stage that the leader has changed.  It is a
   Box::into_raw(Arc::increment_strong(crossbeam::Sender)), so it
   has infinite lifetime unless this C code releases the refcnt.

   It can be used with `fd_ext_poh_signal_leader_change` which
   will just issue a nonblocking send on the channel. */

void
fd_poh_initialize( fd_poh_ctx_t * ctx,
                       double        hashcnt_duration_ns, /* See clock comments above, will be 500ns for mainnet-beta. */
                       ulong         hashcnt_per_tick,    /* See clock comments above, will be 12,500 for mainnet-beta. */
                       ulong         ticks_per_slot,      /* See clock comments above, will almost always be 64. */
                       ulong         tick_height,         /* The counter (height) of the tick to start hashing on top of. */
                       uchar const * last_entry_hash,     /* Points to start of a 32 byte region of memory, the hash itself at the tick height. */
                       void *        signal_leader_change /* See comment above. */ ) {

  fd_poh_tile_initialize( ctx->poh_tile_ctx, hashcnt_duration_ns, hashcnt_per_tick, ticks_per_slot, tick_height,
      last_entry_hash );

  ctx->signal_leader_change = signal_leader_change;
}

static void
publish_became_leader( fd_poh_ctx_t * ctx,
                       ulong          slot ) {
  fd_poh_tile_publish_became_leader( ctx->poh_tile_ctx, ctx->current_leader_bank, slot );
}

/* The PoH tile knows when it should become leader by waiting for its
   leader slot (with the operating system clock).  This function is so
   that when it becomes the leader, it can be told what the leader bank
   is by the replay stage.  See the notes in the long comment above for
   more on how this works. */

FD_FN_UNUSED static void
fd_poh_begin_leader( fd_poh_ctx_t * ctx,
                     void const *   bank,
                     ulong          slot ) {
  FD_TEST( !ctx->current_leader_bank );

  fd_poh_tile_begin_leader( ctx->poh_tile_ctx, slot );

  ctx->current_leader_bank = bank;

  /* We need to register ticks on the bank for all of the ticks that
     were skipped. */
  for( fd_poh_tile_skipped_hashcnt_iter_t iter = fd_poh_tile_skipped_hashcnt_iter_init( ctx->poh_tile_ctx );
       !fd_poh_tile_skipped_hashcnt_iter_done( ctx->poh_tile_ctx, iter );
       iter = fd_poh_tile_skipped_hashcnt_iter_next( ctx->poh_tile_ctx, iter ) ) {
    /* The "hash" value we provide doesn't matter for all but the
       oldest 150 slots, since only the most recent 150 slots are
       saved in the sysvar.  The value provided for those is a
       dummy value, but we keep the same calculation for
       simplicity.  Also the value provided for ticks that are not
       on a slot boundary doesn't matter, since the blockhash will
       be ignored. */
    if( FD_UNLIKELY( fd_poh_tile_skipped_hashcnt_iter_is_slot_boundary( ctx->poh_tile_ctx, iter ) ) ) {
      fd_ext_poh_register_tick( ctx->current_leader_bank, fd_poh_tile_skipped_hashcnt_iter_slot_hash( ctx->poh_tile_ctx, iter ) );
    } else {
      /* If it's not a slot boundary, the actual blockhash doesn't
         matter -- it won't be used for anything, but we still need
         to register the tick to make the bank tick counter correct. */
      uchar ignored[ 32 ];
      fd_ext_poh_register_tick( ctx->current_leader_bank, ignored );
    }
  }

  publish_became_leader( ctx, slot );
}

void
no_longer_leader( fd_poh_ctx_t * ctx ) {
  if( FD_UNLIKELY( ctx->current_leader_bank ) ) fd_ext_bank_release( ctx->current_leader_bank );
  ctx->current_leader_bank = NULL;

  fd_poh_tile_no_longer_leader( ctx->poh_tile_ctx );

  FD_COMPILER_MFENCE();
  fd_ext_poh_signal_leader_change( ctx->signal_leader_change );
}

/* fd_ext_poh_reset is called by the Solana Labs client when a slot on
   the active fork has finished a block and we need to reset our PoH to
   be ticking on top of the block it produced. */

 void
fd_poh_reset( fd_poh_ctx_t * ctx,
              ulong         completed_bank_slot, /* The slot that successfully produced a block */
              uchar const * reset_blockhash      /* Thsh of the e halast tick in the produced block */ ) {
  int leader_before_reset = fd_poh_tile_reset( ctx->poh_tile_ctx, completed_bank_slot, reset_blockhash );
  
  /* No longer have a leader bank if we are reset. Replay stage will
    call back again to give us a new one if we should become leader
    for the reset slot.

    The order is important here, ctx->hashcnt must be updated before
    calling no_longer_leader. */
  if( FD_UNLIKELY( leader_before_reset ) ) {
    no_longer_leader( ctx );
  }
}

FD_FN_CONST static inline ulong
scratch_align( void ) {
  return 128UL;
}

FD_FN_PURE static inline ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  (void)tile;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof( fd_poh_ctx_t ), sizeof( fd_poh_ctx_t ) );
  l = FD_LAYOUT_APPEND( l, fd_poh_tile_align(), fd_poh_tile_footprint() );
  return FD_LAYOUT_FINI( l, scratch_align() );
}

FD_FN_CONST static inline void *
mux_ctx( void * scratch ) {
  return (void*)fd_ulong_align_up( (ulong)scratch, alignof( fd_poh_ctx_t ) );
}

static inline void
after_credit( void *             _ctx,
              fd_mux_context_t * mux ) {
  fd_poh_ctx_t * ctx = (fd_poh_ctx_t *)_ctx;

  int is_leader       = fd_poh_tile_is_leader( ctx->poh_tile_ctx );
  int hashes_produced = fd_poh_tile_do_hashing( ctx->poh_tile_ctx, is_leader );
  
  if( !hashes_produced ) {
    /* No hashes were produced, nothing to do. */
    return;
  }

  if( FD_UNLIKELY( fd_poh_tile_has_become_leader( ctx->poh_tile_ctx, is_leader ) ) ) {
    /* We were not leader but beame leader... we don't want to do any
       other hashing until we get the leader bank from the replay
       stage. */
    return;
  }

  if( FD_UNLIKELY( fd_poh_tile_has_ticked_while_leader( ctx->poh_tile_ctx, is_leader ) ) ) {
    /* We ticked while leader... tell the leader bank. */
    fd_ext_poh_register_tick( ctx->current_leader_bank, ctx->poh_tile_ctx->hash );

    /* And send an empty microblock (a tick) to the shred tile. */
    fd_poh_tile_publish_tick( ctx->poh_tile_ctx, mux );
  }

  fd_poh_tile_process_skipped_slot( ctx->poh_tile_ctx, is_leader );

  if( FD_UNLIKELY( fd_poh_tile_is_no_longer_leader( ctx->poh_tile_ctx, is_leader ) ) ) {
    no_longer_leader( ctx );
  }
}

static inline void
during_housekeeping( void * _ctx ) {
  fd_poh_ctx_t * ctx = (fd_poh_ctx_t *)_ctx;

  fd_poh_tile_during_housekeeping( ctx->poh_tile_ctx );
}

static void
before_frag( void * _ctx,
             ulong  in_idx,
             ulong  seq,
             ulong  sig,
             int *  opt_filter ) {
  (void)seq;

  fd_poh_ctx_t * ctx = (fd_poh_ctx_t *)_ctx;
  if( FD_UNLIKELY( in_idx==ctx->pack_in_idx ) ) {
    if( FD_LIKELY( fd_disco_poh_sig_pkt_type( sig )==POH_PKT_TYPE_DONE_PACKING ||
                  fd_disco_poh_sig_pkt_type( sig )==POH_PKT_TYPE_MICROBLOCK ) ) {
      ulong slot = fd_disco_poh_sig_slot( sig );

      /* The following sequence is possible...
      
          1. We become leader in slot 10
          2. While leader, we switch to a fork that is on slot 8, where we are leader
          3. We get the in-flight microblocks for slot 10

        These in-flight microblocks need to be dropped, so we check
        against the hashcnt high water mark (last_hashcnt) rather than the current
        hashcnt here when determining what to drop.

        We know if the slot is lower than the high water mark it's from a stale
        leader slot, because we will not become leader for the same slot twice
        even if we are reset back in time (to prevent duplicate blocks). */
      if( FD_UNLIKELY( slot<fd_poh_tile_get_last_slot( ctx->poh_tile_ctx ) ) ) *opt_filter = 1;
      return;
    }
  }
}

static inline void
during_frag( void * _ctx,
             ulong  in_idx,
             ulong  seq,
             ulong  sig,
             ulong  chunk,
             ulong  sz,
             int *  opt_filter ) {
  (void)seq;
  (void)sig;
  (void)opt_filter;

  fd_poh_ctx_t * ctx = (fd_poh_ctx_t *)_ctx;

  if( FD_UNLIKELY( in_idx==ctx->stake_in_idx ) ) {
    if( FD_UNLIKELY( chunk<ctx->stake_in.chunk0 || chunk>ctx->stake_in.wmark ) )
      FD_LOG_ERR(( "chunk %lu %lu corrupt, not in range [%lu,%lu]", chunk, sz,
            ctx->stake_in.chunk0, ctx->stake_in.wmark ));

    uchar const * dcache_entry = fd_chunk_to_laddr_const( ctx->stake_in.mem, chunk );
    fd_stake_ci_stake_msg_init( ctx->poh_tile_ctx->stake_ci, dcache_entry );
    return;
  } else if( FD_UNLIKELY( in_idx==ctx->pack_in_idx ) ) {
    /* We now know the real amount of microblocks published, so set an
       exact bound for once we receive them. */
    if( fd_disco_poh_sig_pkt_type( sig )==POH_PKT_TYPE_DONE_PACKING ) {
      FD_TEST( ctx->poh_tile_ctx->microblocks_lower_bound<=ctx->poh_tile_ctx->max_microblocks_per_slot );
      fd_done_packing_t const * done_packing = fd_chunk_to_laddr( ctx->pack_in.mem, chunk );
      FD_LOG_INFO(( "done_packing(slot=%lu,seen_microblocks=%lu,microblocks_in_slot=%lu)",
                    fd_poh_tile_get_slot( ctx->poh_tile_ctx ),
                    ctx->poh_tile_ctx->microblocks_lower_bound,
                    done_packing->microblocks_in_slot ));
      ctx->poh_tile_ctx->microblocks_lower_bound += ctx->poh_tile_ctx->max_microblocks_per_slot - done_packing->microblocks_in_slot;
    }
    *opt_filter = 1;
    return;
  } else {
    if( FD_UNLIKELY( chunk<ctx->bank_in[ in_idx ].chunk0 || chunk>ctx->bank_in[ in_idx ].wmark || sz>USHORT_MAX ) )
      FD_LOG_ERR(( "chunk %lu %lu corrupt, not in range [%lu,%lu]", chunk, sz, ctx->bank_in[ in_idx ].chunk0, ctx->bank_in[ in_idx ].wmark ));

    uchar * src = (uchar *)fd_chunk_to_laddr( ctx->bank_in[ in_idx ].mem, chunk );

    fd_memcpy( ctx->_txns, src, sz-sizeof(fd_microblock_trailer_t) );
    ctx->_microblock_trailer = (fd_microblock_trailer_t*)(src+sz-sizeof(fd_microblock_trailer_t));
  }
}

static inline void
after_frag( void *             _ctx,
            ulong              in_idx,
            ulong              seq,
            ulong *            opt_sig,
            ulong *            opt_chunk,
            ulong *            opt_sz,
            ulong *            opt_tsorig,
            int *              opt_filter,
            fd_mux_context_t * mux ) {
  (void)in_idx;
  (void)seq;
  (void)opt_chunk;
  (void)opt_tsorig;
  (void)opt_filter;

  fd_poh_ctx_t * ctx = (fd_poh_ctx_t *)_ctx;

  if( FD_UNLIKELY( in_idx==ctx->stake_in_idx ) ) {
    fd_stake_ci_stake_msg_fini( ctx->poh_tile_ctx->stake_ci );
    fd_poh_tile_stake_update( ctx->poh_tile_ctx );

    /* Nothing to do if we transition into being leader, since it
       will just get picked up by the regular tick loop. */
    return;
  }

  if( FD_UNLIKELY( !ctx->poh_tile_ctx->microblocks_lower_bound ) ) {
    double tick_per_ns = fd_tempo_tick_per_ns( NULL );
    fd_histf_sample( ctx->poh_tile_ctx->first_microblock_delay, (ulong)((double)(fd_log_wallclock()-ctx->poh_tile_ctx->reset_slot_start_ns)/tick_per_ns) );
  }

  ulong target_slot = fd_disco_poh_sig_slot( *opt_sig );

  ulong current_slot = fd_poh_tile_get_slot( ctx->poh_tile_ctx );
  ulong leader_slot = fd_poh_tile_get_next_leader_slot( ctx->poh_tile_ctx );
  if( FD_UNLIKELY( target_slot!=leader_slot || target_slot!=current_slot ) )
    FD_LOG_ERR(( "packed too early or late target_slot=%lu, current_slot=%lu", target_slot, current_slot ));

  FD_TEST( ctx->current_leader_bank );
  FD_TEST( ctx->poh_tile_ctx->microblocks_lower_bound<ctx->poh_tile_ctx->max_microblocks_per_slot );
  ctx->poh_tile_ctx->microblocks_lower_bound += 1UL;

  ulong txn_cnt = (*opt_sz-sizeof(fd_microblock_trailer_t))/sizeof(fd_txn_p_t);
  fd_txn_p_t * txns = (fd_txn_p_t *)(ctx->_txns);
  ulong executed_txn_cnt = 0UL;
  for( ulong i=0; i<txn_cnt; i++ ) { executed_txn_cnt += !!(txns[ i ].flags & FD_TXN_P_FLAGS_EXECUTE_SUCCESS); }

  /* We don't publish transactions that fail to execute.  If all the
     transctions failed to execute, the microblock would be empty, causing
     solana labs to think it's a tick and complain.  Instead we just skip
     the microblock and don't hash or update the hashcnt. */
  if( FD_UNLIKELY( !executed_txn_cnt ) ) return;

  ulong hashcnt_delta = fd_poh_tile_mixin( ctx->poh_tile_ctx, ctx->_microblock_trailer->hash );

  /* The hashing loop above will never leave us exactly one away from
     crossing a tick boundary, so this increment will never cause the
     current tick (or the slot) to change, except in low power mode
     for development, in which case we do need to register the tick
     with the leader bank.  We don't need to publish the tick since
     sending the microblock below is the publishing action. */
  if( FD_UNLIKELY( fd_poh_tile_is_at_tick_boundary( ctx->poh_tile_ctx ) ) ) {
    fd_ext_poh_register_tick( ctx->current_leader_bank, ctx->poh_tile_ctx->hash );
    if( FD_UNLIKELY( fd_poh_tile_is_no_longer_leader_simple( ctx->poh_tile_ctx ) ) ) {
      /* We ticked while leader and are no longer leader... transition
         the state machine. */
      no_longer_leader( ctx );
    }
  }

  fd_poh_tile_publish_microblock( ctx->poh_tile_ctx, mux, *opt_sig, target_slot, hashcnt_delta, (fd_txn_p_t *)ctx->_txns, txn_cnt );
}

static void
privileged_init( fd_topo_t *      topo,
                 fd_topo_tile_t * tile,
                 void *           scratch ) {
  (void)topo;

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_poh_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_poh_ctx_t ), sizeof( fd_poh_ctx_t ) );

  if( FD_UNLIKELY( !strcmp( tile->poh.identity_key_path, "" ) ) )
    FD_LOG_ERR(( "identity_key_path not set" ));

  const uchar * identity_key = fd_keyload_load( tile->poh.identity_key_path, /* pubkey only: */ 1 );
  fd_memcpy( ctx->identity_key.uc, identity_key, 32UL );
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile,
                   void *           scratch ) {
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_poh_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_poh_ctx_t ), sizeof( fd_poh_ctx_t ) );

#define NONNULL( x ) (__extension__({                                        \
      __typeof__((x)) __x = (x);                                             \
      if( FD_UNLIKELY( !__x ) ) FD_LOG_ERR(( #x " was unexpectedly NULL" )); \
      __x; }))

  // TODO: scratch alloc needs fixing!
  fd_poh_tile_unprivileged_init( topo, tile, scratch );

  ctx->current_leader_bank = NULL;
  ctx->signal_leader_change = NULL;

  ulong poh_shred_obj_id = fd_pod_query_ulong( topo->props, "poh_shred", ULONG_MAX );
  FD_TEST( poh_shred_obj_id!=ULONG_MAX );

  FD_TEST( tile->out_cnt==4UL );

  ctx->bank_cnt = tile->in_cnt-2UL;
  ctx->stake_in_idx = tile->in_cnt-2UL;
  ctx->pack_in_idx = tile->in_cnt-1UL;
  for( ulong i=0; i<tile->in_cnt-1; i++ ) {
    fd_topo_link_t * link = &topo->links[ tile->in_link_id[ i ] ];
    fd_topo_wksp_t * link_wksp = &topo->workspaces[ topo->objs[ link->dcache_obj_id ].wksp_id ];

    ctx->bank_in[ i ].mem    = link_wksp->wksp;
    ctx->bank_in[ i ].chunk0 = fd_dcache_compact_chunk0( ctx->bank_in[i].mem, link->dcache );
    ctx->bank_in[ i ].wmark  = fd_dcache_compact_wmark ( ctx->bank_in[i].mem, link->dcache, link->mtu );
  }

  ctx->stake_in.mem    = topo->workspaces[ topo->objs[ topo->links[ tile->in_link_id[ ctx->stake_in_idx ] ].dcache_obj_id ].wksp_id ].wksp;
  ctx->stake_in.chunk0 = fd_dcache_compact_chunk0( ctx->stake_in.mem, topo->links[ tile->in_link_id[ ctx->stake_in_idx ] ].dcache );
  ctx->stake_in.wmark  = fd_dcache_compact_wmark ( ctx->stake_in.mem, topo->links[ tile->in_link_id[ ctx->stake_in_idx ] ].dcache, topo->links[ tile->in_link_id[ ctx->stake_in_idx ] ].mtu );

  ctx->pack_in.mem    = topo->workspaces[ topo->objs[ topo->links[ tile->in_link_id[ ctx->pack_in_idx ] ].dcache_obj_id ].wksp_id ].wksp;
  ctx->pack_in.chunk0 = fd_dcache_compact_chunk0( ctx->stake_in.mem, topo->links[ tile->in_link_id[ ctx->pack_in_idx ] ].dcache );
  ctx->pack_in.wmark  = fd_dcache_compact_wmark ( ctx->stake_in.mem, topo->links[ tile->in_link_id[ ctx->pack_in_idx ] ].dcache, topo->links[ tile->in_link_id[ ctx->pack_in_idx ] ].mtu );

  ulong scratch_top = FD_SCRATCH_ALLOC_FINI( l, 1UL );
  if( FD_UNLIKELY( scratch_top > (ulong)scratch + scratch_footprint( tile ) ) )
    FD_LOG_ERR(( "scratch overflow %lu %lu %lu", scratch_top - (ulong)scratch - scratch_footprint( tile ), scratch_top, (ulong)scratch + scratch_footprint( tile ) ));
}

static long
lazy( fd_topo_tile_t * tile ) {
  (void)tile;
  /* See explanation in fd_pack */
  return 128L * 300L;
}

fd_topo_run_tile_t fd_tile_poh_int = {
  .name                     = "pohi",
  .mux_flags                = FD_MUX_FLAG_COPY | FD_MUX_FLAG_MANUAL_PUBLISH,
  .burst                    = 3UL, /* One tick, one microblock, and one leader update. */
  .mux_ctx                  = mux_ctx,
  .mux_after_credit         = after_credit,
  .mux_during_housekeeping  = during_housekeeping,
  .mux_before_frag          = before_frag,
  .mux_during_frag          = during_frag,
  .mux_after_frag           = after_frag,
  .lazy                     = lazy,
  .populate_allowed_seccomp = NULL,
  .populate_allowed_fds     = NULL,
  .scratch_align            = scratch_align,
  .scratch_footprint        = scratch_footprint,
  .privileged_init          = privileged_init,
  .unprivileged_init        = unprivileged_init,
};
