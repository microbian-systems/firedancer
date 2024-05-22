#include "fd_vote.h"

ulong
fd_vote_txn_generate(fd_vote_state_update_t *vote_update,
                     fd_pubkey_t *vote_acct_pubkey,
                     fd_pubkey_t *vote_auth_pubkey,
                     uchar* vote_acct_privkey,
                     uchar* vote_auth_privkey,
                     uchar out_txn_meta_buf [static FD_TXN_MAX_SZ],
                     uchar out_txn_buf [static FD_TXN_MTU]
                     ) {
  /* Create the transaction base */
  fd_pubkey_t pubkeys[2], vote_program_pubkey;
  memcpy( pubkeys, vote_acct_pubkey, sizeof(fd_pubkey_t) );
  memcpy( pubkeys + 1, vote_auth_pubkey, sizeof(fd_pubkey_t) );
  memcpy( &vote_program_pubkey, &fd_solana_vote_program_id, sizeof(fd_pubkey_t) );

  fd_txn_accounts_t vote_txn_accounts;
  vote_txn_accounts.signature_cnt         = 2;
  vote_txn_accounts.readonly_signed_cnt   = 0;
  vote_txn_accounts.readonly_unsigned_cnt = 1;
  vote_txn_accounts.acct_cnt              = 3;
  vote_txn_accounts.signers_w             = pubkeys;              /* 2 pubkeys: vote account, vote authority */
  vote_txn_accounts.signers_r             = NULL;                 /* 0 pubkey */
  vote_txn_accounts.non_signers_w         = NULL;                 /* 0 pubkey  */
  vote_txn_accounts.non_signers_r         = &vote_program_pubkey; /* 1 pubkey: vote program */
  FD_TEST( fd_txn_base_generate( out_txn_meta_buf, out_txn_buf, 2, &vote_txn_accounts, NULL ) );

  /* Add the vote instruction */
  fd_vote_instruction_t vote_instr;
  uchar vote_instr_buf[FD_TXN_MTU];
  vote_instr.discriminant = fd_vote_instruction_enum_update_vote_state;
  vote_instr.inner.update_vote_state = *vote_update;
  fd_bincode_encode_ctx_t encode = { .data = vote_instr_buf, .dataend = (vote_instr_buf + FD_TXN_MTU) };
  fd_vote_instruction_encode ( &vote_instr, &encode );
  ushort vote_instr_size = (ushort)fd_vote_instruction_size( &vote_instr );
  uchar instr_accounts[2];
  instr_accounts[0] = 0;
  instr_accounts[1] = 1;
  ulong txn_size = fd_txn_add_instr(out_txn_meta_buf, out_txn_buf, 2, instr_accounts, 2, vote_instr_buf, vote_instr_size);

  /* Add signatures */
  fd_sha512_t sha;
  fd_txn_t * txn_meta = (fd_txn_t *) out_txn_meta_buf;
  fd_ed25519_sign( /* sig */ out_txn_buf + txn_meta->signature_off,
                   /* msg */ out_txn_buf + txn_meta->message_off,
                   /* sz  */ txn_size - txn_meta->message_off,
                   /* public_key  */ vote_acct_pubkey->key,
                   /* private_key */ vote_acct_privkey,
                   &sha);
  fd_ed25519_sign( /* sig */ out_txn_buf + txn_meta->signature_off,
                   /* msg */ out_txn_buf + txn_meta->message_off,
                   /* sz  */ txn_size - txn_meta->message_off,
                   /* public_key  */ vote_auth_pubkey->key,
                   /* private_key */ vote_auth_privkey,
                   &sha);

  return txn_size;
}
