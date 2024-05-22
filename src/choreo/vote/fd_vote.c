#include "fd_vote.h"


int
fd_vote_txn_generate(fd_pubkey_t *vote_acct_pubkey,
                     uchar* vote_acct_privkey,
                     fd_pubkey_t *vote_auth_pubkey,
                     uchar* vote_auth_privkey,
                     uchar out_txn_meta_buf [static FD_TXN_MAX_SZ],
                     uchar out_txn_buf [static FD_TXN_MTU]
                     ) {
  FD_LOG_WARNING(("Within fd_vote_txn_generate"));
}
