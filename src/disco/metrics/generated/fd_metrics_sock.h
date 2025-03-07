/* THIS FILE IS GENERATED BY gen_metrics.py. DO NOT HAND EDIT. */

#include "../fd_metrics_base.h"
#include "fd_metrics_enums.h"

#define FD_METRICS_COUNTER_SOCK_SYSCALLS_SENDMMSG_OFF  (16UL)
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_SENDMMSG_NAME "sock_syscalls_sendmmsg"
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_SENDMMSG_TYPE (FD_METRICS_TYPE_COUNTER)
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_SENDMMSG_DESC "Number of sendmmsg syscalls dispatched"
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_SENDMMSG_CVT  (FD_METRICS_CONVERTER_NONE)

#define FD_METRICS_COUNTER_SOCK_SYSCALLS_RECVMMSG_OFF  (17UL)
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_RECVMMSG_NAME "sock_syscalls_recvmmsg"
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_RECVMMSG_TYPE (FD_METRICS_TYPE_COUNTER)
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_RECVMMSG_DESC "Number of recvmsg syscalls dispatched"
#define FD_METRICS_COUNTER_SOCK_SYSCALLS_RECVMMSG_CVT  (FD_METRICS_CONVERTER_NONE)

#define FD_METRICS_COUNTER_SOCK_RX_PKT_CNT_OFF  (18UL)
#define FD_METRICS_COUNTER_SOCK_RX_PKT_CNT_NAME "sock_rx_pkt_cnt"
#define FD_METRICS_COUNTER_SOCK_RX_PKT_CNT_TYPE (FD_METRICS_TYPE_COUNTER)
#define FD_METRICS_COUNTER_SOCK_RX_PKT_CNT_DESC "Number of packets received"
#define FD_METRICS_COUNTER_SOCK_RX_PKT_CNT_CVT  (FD_METRICS_CONVERTER_NONE)

#define FD_METRICS_COUNTER_SOCK_TX_PKT_CNT_OFF  (19UL)
#define FD_METRICS_COUNTER_SOCK_TX_PKT_CNT_NAME "sock_tx_pkt_cnt"
#define FD_METRICS_COUNTER_SOCK_TX_PKT_CNT_TYPE (FD_METRICS_TYPE_COUNTER)
#define FD_METRICS_COUNTER_SOCK_TX_PKT_CNT_DESC "Number of packets sent"
#define FD_METRICS_COUNTER_SOCK_TX_PKT_CNT_CVT  (FD_METRICS_CONVERTER_NONE)

#define FD_METRICS_COUNTER_SOCK_TX_DROP_CNT_OFF  (20UL)
#define FD_METRICS_COUNTER_SOCK_TX_DROP_CNT_NAME "sock_tx_drop_cnt"
#define FD_METRICS_COUNTER_SOCK_TX_DROP_CNT_TYPE (FD_METRICS_TYPE_COUNTER)
#define FD_METRICS_COUNTER_SOCK_TX_DROP_CNT_DESC "Number of packets failed to send"
#define FD_METRICS_COUNTER_SOCK_TX_DROP_CNT_CVT  (FD_METRICS_CONVERTER_NONE)

#define FD_METRICS_SOCK_TOTAL (5UL)
extern const fd_metrics_meta_t FD_METRICS_SOCK[FD_METRICS_SOCK_TOTAL];
