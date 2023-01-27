#ifndef HEADER_fd_quic_conn_id_h
#define HEADER_fd_quic_conn_id_h

#include "../../util/fd_util_base.h"
#include <string.h>
#include <stdint.h>

/* use a global seed initialized at runtime
   should help avoid collision attacks */
extern uint64_t fd_quic_conn_id_hash_seed;


/* TODO move this into more reasonable place */
#define FD_QUIC_MAX_CONN_ID_SZ 20

/* max number of connection ids per connection */
#define FD_QUIC_MAX_CONN_ID_PER_CONN 4

/* Firedancer connection ids will sized thus */
#define FD_QUIC_CONN_ID_SZ 8

/* pad fd_quic_conn_id struct */
#define FD_QUIC_CONN_ID_PAD (24 - 1 - FD_QUIC_MAX_CONN_ID_SZ)

// have to support variable length connection ids
// in various parts of the protocol
struct fd_quic_conn_id {
  uint8_t sz;
  uchar conn_id[FD_QUIC_MAX_CONN_ID_SZ];

  /* explicitly pad for alignment */
  uchar pad[FD_QUIC_CONN_ID_PAD];
};
typedef struct fd_quic_conn_id fd_quic_conn_id_t;

/* Defines a NULL connection id
   Used as a NULL key in hash maps
   Note that the QUIC protocol supports zero-length connection ids.
   Hence, an all-zero fd_quic_conn_id_t wouldn't work as a NULL key */
#define FD_QUIC_CONN_ID_NULL ((fd_quic_conn_id_t){ .sz = 0xff })

/* define some functions for using fd_quic_conn_id as a key */

/* is this an invalid connection id */
#define FD_QUIC_CONN_ID_INVAL(CONN_ID) ((CONN_ID).sz > FD_QUIC_MAX_CONN_ID_SZ)

/* are these connection ids the same connection id
   for this to work properly, all unused bytes are set to zero */
#define FD_QUIC_CONN_ID_EQUAL(LHS,RHS) \
  (memcmp(&(LHS),&(RHS),sizeof(fd_quic_conn_id_t))==0)

/* hash function for connection ids */
#define FD_QUIC_CONN_ID_HASH(CONN_ID) ((uint)fd_hash(fd_quic_conn_id_hash_seed,&(CONN_ID),sizeof(fd_quic_conn_id_t)))

typedef struct fd_quic_endpoint fd_quic_endpoint_t;

struct fd_quic_endpoint {
  fd_quic_conn_id_t conn_id;

  /* current ip addr and port used by the peer
     stored host endian */
  uint32_t cur_ip_addr;
  uint16_t cur_udp_port;
};

#endif

