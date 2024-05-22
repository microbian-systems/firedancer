#ifndef HEADER_fd_src_choreo_vote_fd_vote_h
#define HEADER_fd_src_choreo_vote_fd_vote_h

#include "../fd_choreo_base.h"
#include "../../flamenco/txn/fd_txn_generate.h"
#include "../../flamenco/runtime/fd_system_ids.h"

ulong
fd_vote_txn_generate(fd_vote_state_update_t *vote_update,
                     fd_pubkey_t *vote_acct_pubkey,
                     fd_pubkey_t *vote_auth_pubkey,
                     uchar* vote_acct_privkey,
                     uchar* vote_auth_privkey,
                     uchar out_txn_meta_buf [static FD_TXN_MAX_SZ],
                     uchar out_txn_buf [static FD_TXN_MTU]);

int
fd_vote_txn_parse(uchar txn_buf [static FD_TXN_MTU],
                  ulong txn_size,
                  fd_valloc_t valloc,
                  fd_vote_state_update_t *out_vote_update);

#endif
