#include "tiles.h"

#include <sys/socket.h> /* MSG_DONTWAIT needed before importing the net seccomp filter */
#include "generated/net_seccomp.h"
#include "../../../../waltz/quic/fd_quic.h"
#include "../../../../waltz/xdp/fd_xdp.h"
#include "../../../../waltz/xdp/fd_xsk_private.h"
#include "../../../../util/net/fd_ip4.h"
#include "../../../../waltz/ip/fd_ip.h"

#include <linux/unistd.h>

#define MAX_NET_INS (32UL)

typedef struct {
  fd_wksp_t * mem;
  ulong       chunk0;
  ulong       wmark;
} fd_net_in_ctx_t;

typedef struct {
  fd_frag_meta_t * mcache;
  ulong *          sync;
  ulong            depth;
  ulong            seq;

  fd_wksp_t * mem;
  ulong       chunk0;
  ulong       wmark;
  ulong       chunk;
} fd_net_out_ctx_t;

typedef struct {
  ulong xsk_aio_cnt;
  fd_xsk_aio_t * xsk_aio[ 2 ];

  ulong round_robin_cnt;
  ulong round_robin_id;

  const fd_aio_t * tx;
  const fd_aio_t * lo_tx;

  uchar frame[ FD_NET_MTU ];

  uint   src_ip_addr;
  uchar  src_mac_addr[6];

  ushort shred_listen_port;
  ushort quic_transaction_listen_port;
  ushort legacy_transaction_listen_port;
  ushort gossip_listen_port;
  ushort repair_intake_listen_port;
  ushort repair_serve_listen_port;

  ulong in_cnt;
  fd_net_in_ctx_t in[ MAX_NET_INS ];

  fd_net_out_ctx_t quic_out[1];
  fd_net_out_ctx_t shred_out[1];
  fd_net_out_ctx_t gossip_out[1];
  fd_net_out_ctx_t repair_out[1];

  fd_ip_t *   ip;
  long        ip_next_upd;
} fd_net_ctx_t;

typedef struct {
  fd_xsk_t * xsk;
  void *     xsk_aio;

  fd_xsk_t * lo_xsk;
  void *     lo_xsk_aio;

  fd_ip_t *  ip;
} fd_net_init_ctx_t;

FD_FN_CONST static inline ulong
scratch_align( void ) {
  return 4096UL;
}

FD_FN_PURE static inline ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  (void)tile;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof( fd_net_init_ctx_t ), sizeof( fd_net_init_ctx_t ) );
  l = FD_LAYOUT_APPEND( l, alignof( fd_net_ctx_t ),      sizeof( fd_net_ctx_t ) );
  l = FD_LAYOUT_APPEND( l, fd_aio_align(),               fd_aio_footprint() );
  l = FD_LAYOUT_APPEND( l, fd_xsk_align(),               fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) );
  l = FD_LAYOUT_APPEND( l, fd_xsk_aio_align(),           fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) );
  if( FD_UNLIKELY( strcmp( tile->net.interface, "lo" ) && !tile->kind_id ) ) {
    l = FD_LAYOUT_APPEND( l, fd_xsk_align(),     fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) );
    l = FD_LAYOUT_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) );
  }
  l = FD_LAYOUT_APPEND( l, fd_ip_align(), fd_ip_footprint( 0U, 0U ) );
  return FD_LAYOUT_FINI( l, scratch_align() );
}

FD_FN_CONST static inline void *
mux_ctx( void * scratch ) {
  ulong net_init = fd_ulong_align_up( (ulong)scratch, alignof( fd_net_init_ctx_t ) );
  return (void*)fd_ulong_align_up( net_init + sizeof( fd_net_init_ctx_t ), alignof( fd_net_ctx_t ) );
}

/* net_rx_aio_send is a callback invoked by aio when new data is
   received on an incoming xsk.  The xsk might be bound to any interface
   or ports, so the purpose of this callback is to determine if the
   packet might be a valid transaction, and whether it is QUIC or
   non-QUIC (raw UDP) before forwarding to the appropriate handler.

   This callback is supposed to return the number of packets in the
   batch which were successfully processed, but we always return
   batch_cnt since there is no logic in place to backpressure this far
   up the stack, and there is no sane way to "not handle" an incoming
   packet. */
static int
net_rx_aio_send( void *                    _ctx,
                 fd_aio_pkt_info_t const * batch,
                 ulong                     batch_cnt,
                 ulong *                   opt_batch_idx,
                 int                       flush ) {
  (void)flush;

  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;

  for( ulong i=0; i<batch_cnt; i++ ) {
    uchar const * packet = batch[i].buf;
    uchar const * packet_end = packet + batch[i].buf_sz;

    if( FD_UNLIKELY( batch[i].buf_sz > FD_NET_MTU ) )
      FD_LOG_ERR(( "received a UDP packet with a too large payload (%u)", batch[i].buf_sz ));

    uchar const * iphdr = packet + 14U;

    /* Filter for UDP/IPv4 packets. Test for ethtype and ipproto in 1
       branch */
    uint test_ethip = ( (uint)packet[12] << 16u ) | ( (uint)packet[13] << 8u ) | (uint)packet[23];
    if( FD_UNLIKELY( test_ethip!=0x080011 ) )
      FD_LOG_ERR(( "Firedancer received a packet from the XDP program that was either "
                   "not an IPv4 packet, or not a UDP packet. It is likely your XDP program "
                   "is not configured correctly." ));

    /* IPv4 is variable-length, so lookup IHL to find start of UDP */
    uint iplen = ( ( (uint)iphdr[0] ) & 0x0FU ) * 4U;
    uchar const * udp = iphdr + iplen;

    /* Ignore if UDP header is too short */
    if( FD_UNLIKELY( udp+8U > packet_end ) ) continue;

    /* Extract IP dest addr and UDP src/dest port */
    uint ip_srcaddr    =                  *(uint   *)( iphdr+12UL );
    ushort udp_srcport = fd_ushort_bswap( *(ushort *)( udp+0UL    ) );
    ushort udp_dstport = fd_ushort_bswap( *(ushort *)( udp+2UL    ) );

    ushort proto;
    fd_net_out_ctx_t * out;
    if(      FD_UNLIKELY( udp_dstport==ctx->shred_listen_port ) ) {
      proto = DST_PROTO_SHRED;
      out = ctx->shred_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->quic_transaction_listen_port ) ) {
      proto = DST_PROTO_TPU_QUIC;
      out = ctx->quic_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->legacy_transaction_listen_port ) ) {
      proto = DST_PROTO_TPU_UDP;
      out = ctx->quic_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->gossip_listen_port ) ) {
      proto = DST_PROTO_GOSSIP;
      out = ctx->gossip_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->repair_intake_listen_port ) ) {
      proto = DST_PROTO_REPAIR;
      out = ctx->repair_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->repair_serve_listen_port ) ) {
      proto = DST_PROTO_REPAIR;
      out = ctx->repair_out;
    } else {
      
      FD_LOG_ERR(( "Firedancer received a UDP packet on port %hu which was not expected. "
                   "Only the following ports should be configured to forward packets: "
                   "%hu, %hu, %hu, %hu, %hu, %hu (excluding any 0 ports, which can be ignored)."
                   "It is likely you changed the port configuration in your TOML file and "
                   "did not reload the XDP program. You can reload the program by running "
                   "`fdctl configure fini xdp && fdctl configure init xdp`.",
                   udp_dstport,
                   ctx->shred_listen_port,
                   ctx->quic_transaction_listen_port,
                   ctx->legacy_transaction_listen_port,
                   ctx->gossip_listen_port,
                   ctx->repair_intake_listen_port,
                   ctx->repair_serve_listen_port ));
    }

    fd_memcpy( fd_chunk_to_laddr( out->mem, out->chunk ), packet, batch[ i ].buf_sz );

    /* tile can decide how to partition based on src ip addr and src port */
    ulong sig = fd_disco_netmux_sig( ip_srcaddr, udp_srcport, 0U, proto, 14UL+8UL+iplen );

    ulong tspub  = (ulong)fd_frag_meta_ts_comp( fd_tickcount() );
    fd_mcache_publish( out->mcache, out->depth, out->seq, sig, out->chunk, batch[ i ].buf_sz, 0, 0, tspub );

    out->seq = fd_seq_inc( out->seq, 1UL );
    out->chunk = fd_dcache_compact_next( out->chunk, FD_NET_MTU, out->chunk0, out->wmark );
  }

  if( FD_LIKELY( opt_batch_idx ) ) {
    *opt_batch_idx = batch_cnt;
  }

  return FD_AIO_SUCCESS;
}

static void
before_credit( void * _ctx,
               fd_mux_context_t * mux ) {
  (void)mux;

  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;

  for( ulong i=0; i<ctx->xsk_aio_cnt; i++ ) {
    fd_xsk_aio_service( ctx->xsk_aio[i] );
  }
}

static void
during_housekeeping( void * _ctx ) {
  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;
  long now = fd_log_wallclock();
  if( FD_UNLIKELY( now > ctx->ip_next_upd ) ) {
    ctx->ip_next_upd = now + (long)60e9;
    fd_ip_arp_fetch( ctx->ip );
    fd_ip_route_fetch( ctx->ip );
  }
}

FD_FN_PURE static int
route_loopback( uint  tile_ip_addr,
                ulong sig ) {
  return fd_disco_netmux_sig_dst_ip( sig )==FD_IP4_ADDR(127,0,0,1) ||
    fd_disco_netmux_sig_dst_ip( sig )==tile_ip_addr;
}

static void
before_frag( void * _ctx,
             ulong  in_idx,
             ulong  seq,
             ulong  sig,
             int *  opt_filter ) {
  (void)in_idx;

  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;

  ulong proto = fd_disco_netmux_sig_proto( sig );
  if( FD_UNLIKELY( proto!=DST_PROTO_OUTGOING ) ) {
    *opt_filter = 1;
    return;
  }

  /* Round robin by sequence number for now, QUIC should be modified to
     echo the net tile index back so we can transmit on the same queue.

     127.0.0.1 packets for localhost must go out on net tile 0 which
     owns the loopback interface XSK, which only has 1 queue. */

  int handled_packet = 0;
  if( FD_UNLIKELY( route_loopback( ctx->src_ip_addr, sig ) ) ) {
    handled_packet = ctx->round_robin_id == 0;
  } else {
    handled_packet = (seq % ctx->round_robin_cnt) == ctx->round_robin_id;
  }

  if( FD_UNLIKELY( !handled_packet ) ) {
    *opt_filter = 1;
  }
}

static void
during_frag( void * _ctx,
             ulong in_idx,
             ulong seq,
             ulong sig,
             ulong chunk,
             ulong sz,
             int * opt_filter ) {
  (void)in_idx;
  (void)seq;
  (void)sig;
  (void)opt_filter;

  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;

  if( FD_UNLIKELY( chunk<ctx->in[ in_idx ].chunk0 || chunk>ctx->in[ in_idx ].wmark || sz>FD_NET_MTU ) )
    FD_LOG_ERR(( "chunk %lu %lu corrupt, not in range [%lu,%lu]", chunk, sz, ctx->in[ in_idx ].chunk0, ctx->in[ in_idx ].wmark ));

  uchar * src = (uchar *)fd_chunk_to_laddr( ctx->in[ in_idx ].mem, chunk );
  fd_memcpy( ctx->frame, src, sz ); // TODO: Change xsk_aio interface to eliminate this copy
}

static void
send_arp_probe( fd_net_ctx_t * ctx,
                uint           dst_ip_addr,
                uint           ifindex ) {
  uchar          arp_buf[FD_IP_ARP_SZ];
  ulong          arp_len = 0UL;

  uint           src_ip_addr  = ctx->src_ip_addr;
  uchar *        src_mac_addr = ctx->src_mac_addr;

  /* prepare arp table */
  int arp_table_rtn = fd_ip_update_arp_table( ctx->ip, dst_ip_addr, ifindex );

  if( FD_UNLIKELY( arp_table_rtn == FD_IP_SUCCESS ) ) {
    /* generate a probe */
    fd_ip_arp_gen_arp_probe( arp_buf, FD_IP_ARP_SZ, &arp_len, dst_ip_addr, fd_uint_bswap( src_ip_addr ), src_mac_addr );

    /* send the probe */
    fd_aio_pkt_info_t aio_buf = { .buf = arp_buf, .buf_sz = (ushort)arp_len };
    ctx->tx->send_func( ctx->xsk_aio[ 0 ], &aio_buf, 1, NULL, 1 );
  }
}

static void
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
  (void)opt_sig;
  (void)opt_chunk;
  (void)opt_tsorig;
  (void)opt_filter;
  (void)mux;

  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;

  fd_aio_pkt_info_t aio_buf = { .buf = ctx->frame, .buf_sz = (ushort)*opt_sz };
  if( FD_UNLIKELY( route_loopback( ctx->src_ip_addr, *opt_sig ) ) ) {
    ctx->lo_tx->send_func( ctx->xsk_aio[ 1 ], &aio_buf, 1, NULL, 1 );
  } else {
    /* extract dst ip */
    uint dst_ip = fd_uint_bswap( fd_disco_netmux_sig_dst_ip( *opt_sig ) );

    uint  next_hop    = 0U;
    uchar dst_mac[6]  = {0};
    uint  if_idx      = 0;

    /* route the packet */
    /*
     * determine the destination:
     *   same host
     *   same subnet
     *   other
     * determine the next hop
     *   localhost
     *   gateway
     *   subnet local host
     * determine the mac address of the next hop address
     *   and the local ipv4 and eth addresses */
    int rtn = fd_ip_route_ip_addr( dst_mac, &next_hop, &if_idx, ctx->ip, dst_ip );
    if( FD_UNLIKELY( rtn == FD_IP_PROBE_RQD ) ) {
      /* another fd_net instance might have already resolved this address
         so simply try another fetch */
      fd_ip_arp_fetch( ctx->ip );
      rtn = fd_ip_route_ip_addr( dst_mac, &next_hop, &if_idx, ctx->ip, dst_ip );
    }

    long now;
    switch( rtn ) {
      case FD_IP_PROBE_RQD:
        /* TODO possibly buffer some data while waiting for ARPs to complete */
        /* TODO rate limit ARPs */
        /* TODO add caching of ip_dst -> routing info */
        send_arp_probe( ctx, next_hop, if_idx );

        /* refresh tables */
        now = fd_log_wallclock();
        ctx->ip_next_upd = now + (long)200e3;
        break;
      case FD_IP_NO_ROUTE:
        /* cannot make progress here */
        break;
      case FD_IP_SUCCESS:
        /* set destination mac address */
        memcpy( ctx->frame, dst_mac, 6UL );

        /* set source mac address */
        memcpy( ctx->frame + 6UL, ctx->src_mac_addr, 6UL );

        ctx->tx->send_func( ctx->xsk_aio[ 0 ], &aio_buf, 1, NULL, 1 );
        break;
      case FD_IP_RETRY:
        /* refresh tables */
        now = fd_log_wallclock();
        ctx->ip_next_upd = now + (long)200e3;
        /* TODO consider buffering */
        break;
      case FD_IP_MULTICAST:
      case FD_IP_BROADCAST:
      default:
        /* should not occur in current use cases */
        break;
    }
  }

  *opt_filter = 1;
}

static void
privileged_init( fd_topo_t *      topo,
                 fd_topo_tile_t * tile,
                 void *           scratch ) {
  (void)topo;

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_init_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_init_ctx_t ), sizeof( fd_net_init_ctx_t ) );
  FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_ctx_t ),      sizeof( fd_net_ctx_t ) );
  FD_SCRATCH_ALLOC_APPEND( l, fd_aio_align(),               fd_aio_footprint() );

  /* Initialize XSK and xsk_aio which requires being privileged. */
  void * xsk = fd_xsk_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_align(), fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) ),
                           FD_NET_MTU,
                           tile->net.xdp_rx_queue_size,
                           tile->net.xdp_rx_queue_size,
                           tile->net.xdp_tx_queue_size,
                           tile->net.xdp_tx_queue_size,
                           tile->net.zero_copy );
  if( FD_UNLIKELY( !fd_xsk_bind( xsk, tile->net.app_name, tile->net.interface, (uint)tile->kind_id ) ) )
    FD_LOG_ERR(( "failed to bind xsk for net tile %lu", tile->kind_id ));

  ctx->xsk = fd_xsk_join( xsk );
  if( FD_UNLIKELY( !ctx->xsk ) ) FD_LOG_ERR(( "fd_xsk_join failed" ));

  ctx->xsk_aio = fd_xsk_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) ),
                                 tile->net.xdp_tx_queue_size,
                                 tile->net.xdp_aio_depth );
  if( FD_UNLIKELY( !ctx->xsk_aio ) ) FD_LOG_ERR(( "fd_xsk_aio_new failed" ));

  /* Networking tile at index 0 also binds to loopback (only queue 0 available on lo) */
  ctx->lo_xsk     = NULL;
  ctx->lo_xsk_aio = NULL;
  if( FD_UNLIKELY( strcmp( tile->net.interface, "lo" ) && !tile->kind_id ) ) {
    void * lo_xsk = fd_xsk_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_align(), fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) ),
                                FD_NET_MTU,
                                tile->net.xdp_rx_queue_size,
                                tile->net.xdp_rx_queue_size,
                                tile->net.xdp_tx_queue_size,
                                tile->net.xdp_tx_queue_size,
                                0 /* Not zero-copy */ );
    if( FD_UNLIKELY( !fd_xsk_bind( lo_xsk, tile->net.app_name, "lo", (uint)tile->kind_id ) ) )
      FD_LOG_ERR(( "failed to bind lo_xsk for net tile %lu", tile->kind_id ));

    ctx->lo_xsk = fd_xsk_join( lo_xsk );
    if( FD_UNLIKELY( !ctx->lo_xsk ) ) FD_LOG_ERR(( "fd_xsk_join failed" ));

    ctx->lo_xsk_aio = fd_xsk_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) ),
                                      tile->net.xdp_tx_queue_size,
                                      tile->net.xdp_aio_depth );
    if( FD_UNLIKELY( !ctx->lo_xsk_aio ) ) FD_LOG_ERR(( "fd_xsk_aio_new failed" ));
  }

  /* init fd_ip */
  ctx->ip = fd_ip_join( fd_ip_new( FD_SCRATCH_ALLOC_APPEND( l, fd_ip_align(), fd_ip_footprint( 0UL, 0UL ) ),
                                   0UL, 0UL ) );
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile,
                   void *           scratch ) {
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_init_ctx_t * init_ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_init_ctx_t ), sizeof( fd_net_init_ctx_t ) );
  fd_net_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_ctx_t ), sizeof( fd_net_ctx_t ) );
  fd_aio_t * net_rx_aio = fd_aio_join( fd_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_aio_align(), fd_aio_footprint() ), ctx, net_rx_aio_send ) );
  if( FD_UNLIKELY( !net_rx_aio ) ) FD_LOG_ERR(( "fd_aio_join failed" ));

  ctx->round_robin_cnt = fd_topo_tile_name_cnt( topo, tile->name );
  ctx->round_robin_id  = tile->kind_id;

  ctx->xsk_aio_cnt = 1;
  ctx->xsk_aio[ 0 ] = fd_xsk_aio_join( init_ctx->xsk_aio, init_ctx->xsk );
  if( FD_UNLIKELY( !ctx->xsk_aio[ 0 ] ) ) FD_LOG_ERR(( "fd_xsk_aio_join failed" ));
  fd_xsk_aio_set_rx( ctx->xsk_aio[ 0 ], net_rx_aio );
  ctx->tx = fd_xsk_aio_get_tx( init_ctx->xsk_aio );
  if( FD_UNLIKELY( init_ctx->lo_xsk ) ) {
    ctx->xsk_aio[ 1 ] = fd_xsk_aio_join( init_ctx->lo_xsk_aio, init_ctx->lo_xsk );
    if( FD_UNLIKELY( !ctx->xsk_aio[ 1 ] ) ) FD_LOG_ERR(( "fd_xsk_aio_join failed" ));
    fd_xsk_aio_set_rx( ctx->xsk_aio[ 1 ], net_rx_aio );
    ctx->lo_tx = fd_xsk_aio_get_tx( init_ctx->lo_xsk_aio );
    ctx->xsk_aio_cnt = 2;
  }

  ctx->src_ip_addr = tile->net.src_ip_addr;
  memcpy( ctx->src_mac_addr, tile->net.src_mac_addr, 6UL );

  ctx->shred_listen_port = tile->net.shred_listen_port;
  ctx->quic_transaction_listen_port = tile->net.quic_transaction_listen_port;
  ctx->legacy_transaction_listen_port = tile->net.legacy_transaction_listen_port;
  ctx->gossip_listen_port = tile->net.gossip_listen_port;
  ctx->repair_intake_listen_port = tile->net.repair_intake_listen_port;
  ctx->repair_serve_listen_port = tile->net.repair_serve_listen_port;

  /* Put a bound on chunks we read from the input, to make sure they
      are within in the data region of the workspace. */
  if( FD_UNLIKELY( !tile->in_cnt ) ) FD_LOG_ERR(( "net tile in link cnt is zero" ));
  if( FD_UNLIKELY( tile->in_cnt>MAX_NET_INS ) ) FD_LOG_ERR(( "net tile in link cnt %lu exceeds MAX_NET_INS %lu", tile->in_cnt, MAX_NET_INS ));
  FD_TEST( tile->in_cnt<=32 );
  for( ulong i=0UL; i<tile->in_cnt; i++ ) {
    fd_topo_link_t * link = &topo->links[ tile->in_link_id[ i ] ];
    if( FD_UNLIKELY( link->mtu!=FD_NET_MTU ) ) FD_LOG_ERR(( "net tile in link does not have a normal MTU" ));

    ctx->in[ i ].mem    = topo->workspaces[ topo->objs[ link->dcache_obj_id ].wksp_id ].wksp;
    ctx->in[ i ].chunk0 = fd_dcache_compact_chunk0( ctx->in[ i ].mem, link->dcache );
    ctx->in[ i ].wmark  = fd_dcache_compact_wmark( ctx->in[ i ].mem, link->dcache, link->mtu );
  }
  
  for( ulong i = 0; i < tile->out_cnt; i++ ) {
    fd_topo_link_t * out_link = &topo->links[ tile->out_link_id[ i  ] ];
    if( strcmp( out_link->name, "net_quic" ) == 0 ) {
      fd_topo_link_t * quic_out = out_link;
      ctx->quic_out->mcache = quic_out->mcache;
      ctx->quic_out->sync   = fd_mcache_seq_laddr( ctx->quic_out->mcache );
      ctx->quic_out->depth  = fd_mcache_depth( ctx->quic_out->mcache );
      ctx->quic_out->seq    = fd_mcache_seq_query( ctx->quic_out->sync );
      ctx->quic_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( quic_out->dcache ), quic_out->dcache );
      ctx->quic_out->mem    = topo->workspaces[ topo->objs[ quic_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->quic_out->wmark  = fd_dcache_compact_wmark ( ctx->quic_out->mem, quic_out->dcache, quic_out->mtu );
      ctx->quic_out->chunk  = ctx->quic_out->chunk0;
    } else if( strcmp( out_link->name, "net_shred" ) == 0 ) {
      fd_topo_link_t * shred_out = out_link;
      ctx->shred_out->mcache = shred_out->mcache;
      ctx->shred_out->sync   = fd_mcache_seq_laddr( ctx->shred_out->mcache );
      ctx->shred_out->depth  = fd_mcache_depth( ctx->shred_out->mcache );
      ctx->shred_out->seq    = fd_mcache_seq_query( ctx->shred_out->sync );
      ctx->shred_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( shred_out->dcache ), shred_out->dcache );
      ctx->shred_out->mem    = topo->workspaces[ topo->objs[ shred_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->shred_out->wmark  = fd_dcache_compact_wmark ( ctx->shred_out->mem, shred_out->dcache, shred_out->mtu );
      ctx->shred_out->chunk  = ctx->shred_out->chunk0;
    } else if( strcmp( out_link->name, "net_gossip" ) == 0 ) {
      fd_topo_link_t * gossip_out = out_link;
      ctx->gossip_out->mcache = gossip_out->mcache;
      ctx->gossip_out->sync   = fd_mcache_seq_laddr( ctx->gossip_out->mcache );
      ctx->gossip_out->depth  = fd_mcache_depth( ctx->gossip_out->mcache );
      ctx->gossip_out->seq    = fd_mcache_seq_query( ctx->gossip_out->sync );
      ctx->gossip_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( gossip_out->dcache ), gossip_out->dcache );
      ctx->gossip_out->mem    = topo->workspaces[ topo->objs[ gossip_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->gossip_out->wmark  = fd_dcache_compact_wmark ( ctx->gossip_out->mem, gossip_out->dcache, gossip_out->mtu );
      ctx->gossip_out->chunk  = ctx->gossip_out->chunk0;
    } else if( strcmp( out_link->name, "net_repair" ) == 0 ) {
      fd_topo_link_t * repair_out = out_link;
      ctx->repair_out->mcache = repair_out->mcache;
      ctx->repair_out->sync   = fd_mcache_seq_laddr( ctx->repair_out->mcache );
      ctx->repair_out->depth  = fd_mcache_depth( ctx->repair_out->mcache );
      ctx->repair_out->seq    = fd_mcache_seq_query( ctx->repair_out->sync );
      ctx->repair_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( repair_out->dcache ), repair_out->dcache );
      ctx->repair_out->mem    = topo->workspaces[ topo->objs[ repair_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->repair_out->wmark  = fd_dcache_compact_wmark ( ctx->repair_out->mem, repair_out->dcache, repair_out->mtu );
      ctx->repair_out->chunk  = ctx->repair_out->chunk0;
    } else {
      FD_LOG_ERR(( "unrecognized out link `%s`", out_link->name ));
    }
  }

  /* Check if any of the tiles we set a listen port for do not have an outlink. */
  if( FD_UNLIKELY( ctx->shred_listen_port!=0 && ctx->shred_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "shred listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->quic_transaction_listen_port!=0 && ctx->quic_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "quic transaction listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->legacy_transaction_listen_port!=0 && ctx->quic_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "legacy transaction listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->gossip_listen_port!=0 && ctx->gossip_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "gossip listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->repair_intake_listen_port!=0 && ctx->repair_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "repair intake port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->repair_serve_listen_port!=0 && ctx->repair_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "repair serve listen port set but no out link was found" ));
  }

  ctx->ip = init_ctx->ip;

  ulong scratch_top = FD_SCRATCH_ALLOC_FINI( l, 1UL );
  if( FD_UNLIKELY( scratch_top > (ulong)scratch + scratch_footprint( tile ) ) )
    FD_LOG_ERR(( "scratch overflow %lu %lu %lu", scratch_top - (ulong)scratch - scratch_footprint( tile ), scratch_top, (ulong)scratch + scratch_footprint( tile ) ));
}

static ulong
populate_allowed_seccomp( void *               scratch,
                          ulong                out_cnt,
                          struct sock_filter * out ) {
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_init_ctx_t * init_ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_init_ctx_t ), sizeof( fd_net_init_ctx_t ) );

  /* A bit of a hack, if there is no loopback XSK for this tile, we still need to pass
     two "allow" FD arguments to the net policy, so we just make them both the same. */
  int allow_fd2 = init_ctx->lo_xsk ? init_ctx->lo_xsk->xsk_fd : init_ctx->xsk->xsk_fd;
  FD_TEST( init_ctx->xsk->xsk_fd >= 0 && allow_fd2 >= 0 );
  int netlink_fd = fd_ip_netlink_get( init_ctx->ip )->fd;
  populate_sock_filter_policy_net( out_cnt, out, (uint)fd_log_private_logfile_fd(), (uint)init_ctx->xsk->xsk_fd, (uint)allow_fd2, (uint)netlink_fd );
  return sock_filter_policy_net_instr_cnt;
}

static ulong
populate_allowed_fds( void * scratch,
                      ulong  out_fds_cnt,
                      int *  out_fds ) {
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_init_ctx_t * init_ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_init_ctx_t ), sizeof( fd_net_init_ctx_t ) );

  if( FD_UNLIKELY( out_fds_cnt < 5 ) ) FD_LOG_ERR(( "out_fds_cnt %lu", out_fds_cnt ));

  ulong out_cnt = 0;
  out_fds[ out_cnt++ ] = 2; /* stderr */
  if( FD_LIKELY( -1!=fd_log_private_logfile_fd() ) )
    out_fds[ out_cnt++ ] = fd_log_private_logfile_fd(); /* logfile */
  out_fds[ out_cnt++ ] = init_ctx->xsk->xsk_fd;
  if( FD_UNLIKELY( init_ctx->lo_xsk ) )
    out_fds[ out_cnt++ ] = init_ctx->lo_xsk->xsk_fd;
  out_fds[ out_cnt++ ] = fd_ip_netlink_get( init_ctx->ip )->fd;
  return out_cnt;
}

fd_topo_run_tile_t fd_tile_net = {
  .name                     = "net",
  .mux_flags                = FD_MUX_FLAG_MANUAL_PUBLISH | FD_MUX_FLAG_COPY,
  .burst                    = 1UL,
  .mux_ctx                  = mux_ctx,
  .mux_before_credit        = before_credit,
  .mux_before_frag          = before_frag,
  .mux_during_frag          = during_frag,
  .mux_after_frag           = after_frag,
  .mux_during_housekeeping  = during_housekeeping,
  .populate_allowed_seccomp = populate_allowed_seccomp,
  .populate_allowed_fds     = populate_allowed_fds,
  .scratch_align            = scratch_align,
  .scratch_footprint        = scratch_footprint,
  .privileged_init          = privileged_init,
  .unprivileged_init        = unprivileged_init,
};
