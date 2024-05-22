/* Data structure which is passed through replay_notif link */

#define FD_REPLAY_SAVED_TYPE 0x29FE5131U

struct __attribute__((aligned(64UL))) fd_replay_notif_msg {
  union {
    struct {
      fd_pubkey_t acct_id;
      fd_hash_t funk_xid;
    } acct_saved;
  };
  uint type;
};
typedef struct fd_replay_notif_msg fd_replay_notif_msg_t;

/* MTU on replay_notif link is 128 */
FD_STATIC_ASSERT( sizeof(fd_replay_notif_msg_t) <= 128UL, fd_replay_notif_msg );
