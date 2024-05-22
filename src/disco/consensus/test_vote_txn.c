#include <sys/random.h>
#include "../../util/fd_util.h"
#include "../../choreo/fd_choreo.h"
#include "../../ballet/ed25519/fd_ed25519.h"
#include "../../flamenco/fd_flamenco.h"
#include "../../flamenco/txn/fd_txn_generate.h"
#include "../../flamenco/runtime/fd_system_ids.h"

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"

#define TEST_VOTE_TXN_MAGIC (0x7e58UL)

int
main( int argc, char ** argv ) {
  fd_boot( &argc, &argv );
  fd_flamenco_boot( &argc, &argv );

  /* Keys */
  fd_sha512_t sha[2];
  fd_pubkey_t vote_account_pubkey, vote_authority_pubkey;
  uchar vote_account_privkey[32], vote_authority_privkey[32];

  FD_TEST( 32UL == getrandom( vote_account_privkey, 32UL, 0 ) );
  FD_TEST( 32UL == getrandom( vote_authority_privkey, 32UL, 0 ) );
  FD_TEST( fd_ed25519_public_from_private( vote_account_pubkey.key, vote_account_privkey, &sha[0] ) );
  FD_TEST( fd_ed25519_public_from_private( vote_authority_pubkey.key, vote_authority_privkey, &sha[1] ) );

  /* Workspace */
  ulong page_cnt = 1;
  char * _page_sz = "gigantic";
  ulong  numa_idx = fd_shmem_numa_idx( 0 );
  fd_wksp_t * wksp = fd_wksp_new_anonymous(
      fd_cstr_to_shmem_page_sz( _page_sz ), page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

  /* Alloc */
  void * alloc_shmem =
      fd_wksp_alloc_laddr( wksp, fd_alloc_align(), fd_alloc_footprint(), TEST_VOTE_TXN_MAGIC );
  void *       alloc_shalloc = fd_alloc_new( alloc_shmem, TEST_VOTE_TXN_MAGIC );
  fd_alloc_t * alloc         = fd_alloc_join( alloc_shalloc, 0UL );
  fd_valloc_t  valloc        = fd_alloc_virtual( alloc );

  /* Create deque of fd_vote_lockout_t */
  ulong height = 32;
  void * mem = fd_wksp_alloc_laddr(
      wksp, deq_fd_vote_lockout_t_align(), deq_fd_vote_lockout_t_footprint( height ), TEST_VOTE_TXN_MAGIC );
  fd_vote_lockout_t * tower =
      deq_fd_vote_lockout_t_join( deq_fd_vote_lockout_t_new( mem, height ) );
  FD_TEST( tower );

  /* Insert lockouts into the tower */
  for (ulong i = 0; i < height; i++) {
      fd_vote_lockout_t lockout = { .slot = i, .confirmation_count = (uint)(height - i) };
      deq_fd_vote_lockout_t_push_head( tower, lockout );
  }

  /* Create vote_update with some dummy values */
  fd_vote_state_update_t vote_update;
  memset( &vote_update, 0, sizeof(fd_vote_state_update_t) );
  vote_update.lockouts = tower;
  vote_update.root = 100;
  vote_update.has_root = 1;
  FD_TEST( 32UL == getrandom( vote_update.hash.key, 32UL, 0 ) );
  static ulong now = 1715701506716580798UL;
  vote_update.timestamp = &now;

  /* Create the vote transaction */
  uchar txn_meta_buf[ FD_TXN_MAX_SZ ];
  uchar txn_buf [ FD_TXN_MTU ];
  ulong txn_size = fd_vote_txn_generate( &vote_update, &vote_account_pubkey, &vote_authority_pubkey, vote_account_privkey, vote_authority_privkey, txn_meta_buf, txn_buf);
  FD_LOG_NOTICE(("fd_vote_txn_generate: vote txn has %lu bytes", txn_size));

  /* Parse the transaction back to fd_txn_t */
  fd_vote_state_update_t parsed_vote_update;
  fd_vote_txn_parse(txn_buf, txn_size, valloc, &parsed_vote_update);
  FD_LOG_NOTICE((".root: %ld == %ld", vote_update.root, parsed_vote_update.root));
  FD_LOG_NOTICE((".has_root: %u == %u", vote_update.has_root, parsed_vote_update.has_root));
  FD_LOG_NOTICE((".timestamp: %lu == %lu", *vote_update.timestamp, *parsed_vote_update.timestamp));
  FD_LOG_NOTICE((".hash: %32J == %32J", vote_update.hash.key, parsed_vote_update.hash.key));
}
