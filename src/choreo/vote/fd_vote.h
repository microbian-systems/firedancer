#ifndef HEADER_fd_src_choreo_vote_fd_vote_h
#define HEADER_fd_src_choreo_vote_fd_vote_h

#include "../fd_choreo_base.h"

int
fd_vote_txn_generate(fd_pubkey_t *vote_acct_pubkey,
                     uchar* vote_acct_privkey,
                     fd_pubkey_t *vote_auth_pubkey,
                     uchar* vote_auth_privkey,
                     uchar out_txn_meta_buf [static FD_TXN_MAX_SZ],
                     uchar out_txn_buf [static FD_TXN_MTU]
                     );

#endif // FD_VOTE_H_
