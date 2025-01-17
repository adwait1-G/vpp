/*
 * Copyright (c) 2016-2019 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _vnet_tcp_h_
#define _vnet_tcp_h_

#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vnet/tcp/tcp_packet.h>
#include <vnet/tcp/tcp_timer.h>
#include <vnet/session/transport.h>
#include <vnet/session/session.h>
#include <vnet/tcp/tcp_debug.h>

#define TCP_TICK 0.001			/**< TCP tick period (s) */
#define THZ (u32) (1/TCP_TICK)		/**< TCP tick frequency */
#define TCP_TSTAMP_RESOLUTION TCP_TICK	/**< Time stamp resolution */
#define TCP_PAWS_IDLE 24 * 24 * 60 * 60 * THZ /**< 24 days */
#define TCP_FIB_RECHECK_PERIOD	1 * THZ	/**< Recheck every 1s */
#define TCP_MAX_OPTION_SPACE 40
#define TCP_CC_DATA_SZ 24
#define TCP_MAX_GSO_SZ 65536
#define TCP_RXT_MAX_BURST 10

#define TCP_DUPACK_THRESHOLD 	3
#define TCP_IW_N_SEGMENTS 	10
#define TCP_ALWAYS_ACK		1	/**< On/off delayed acks */
#define TCP_USE_SACKS		1	/**< Disable only for testing */

/** TCP FSM state definitions as per RFC793. */
#define foreach_tcp_fsm_state   \
  _(CLOSED, "CLOSED")           \
  _(LISTEN, "LISTEN")           \
  _(SYN_SENT, "SYN_SENT")       \
  _(SYN_RCVD, "SYN_RCVD")       \
  _(ESTABLISHED, "ESTABLISHED") \
  _(CLOSE_WAIT, "CLOSE_WAIT")   \
  _(FIN_WAIT_1, "FIN_WAIT_1")   \
  _(LAST_ACK, "LAST_ACK")       \
  _(CLOSING, "CLOSING")         \
  _(FIN_WAIT_2, "FIN_WAIT_2")   \
  _(TIME_WAIT, "TIME_WAIT")

typedef enum _tcp_state
{
#define _(sym, str) TCP_STATE_##sym,
  foreach_tcp_fsm_state
#undef _
  TCP_N_STATES
} tcp_state_t;

format_function_t format_tcp_state;
format_function_t format_tcp_flags;
format_function_t format_tcp_sacks;
format_function_t format_tcp_rcv_sacks;

/** TCP timers */
#define foreach_tcp_timer               \
  _(RETRANSMIT, "RETRANSMIT")           \
  _(DELACK, "DELAYED ACK")              \
  _(PERSIST, "PERSIST")                 \
  _(WAITCLOSE, "WAIT CLOSE")            \
  _(RETRANSMIT_SYN, "RETRANSMIT SYN")   \

typedef enum _tcp_timers
{
#define _(sym, str) TCP_TIMER_##sym,
  foreach_tcp_timer
#undef _
  TCP_N_TIMERS
} tcp_timers_e;

typedef void (timer_expiration_handler) (u32 index);

extern timer_expiration_handler tcp_timer_delack_handler;
extern timer_expiration_handler tcp_timer_retransmit_handler;
extern timer_expiration_handler tcp_timer_persist_handler;
extern timer_expiration_handler tcp_timer_retransmit_syn_handler;

#define TCP_TIMER_HANDLE_INVALID ((u32) ~0)

#define TCP_TIMER_TICK		0.1		/**< Timer tick in seconds */
#define TCP_TO_TIMER_TICK       TCP_TICK*10	/**< Factor for converting
						     ticks to timer ticks */

#define TCP_RTO_MAX 60 * THZ	/* Min max RTO (60s) as per RFC6298 */
#define TCP_RTO_MIN 0.2 * THZ	/* Min RTO (200ms) - lower than standard */
#define TCP_RTT_MAX 30 * THZ	/* 30s (probably too much) */
#define TCP_RTO_SYN_RETRIES 3	/* SYN retries without doubling RTO */
#define TCP_RTO_INIT 1 * THZ	/* Initial retransmit timer */
#define TCP_RTO_BOFF_MAX 8	/* Max number of retries before reset */
#define TCP_ESTABLISH_TIME (60 * THZ)	/* Connection establish timeout */

/** Connection configuration flags */
#define foreach_tcp_cfg_flag 			\
  _(RATE_SAMPLE, "Rate sampling")		\
  _(NO_CSUM_OFFLOAD, "No csum offload")    	\
  _(NO_TSO, "TSO off")				\
  _(TSO, "TSO")					\

typedef enum tcp_cfg_flag_bits_
{
#define _(sym, str) TCP_CFG_F_##sym##_BIT,
  foreach_tcp_cfg_flag
#undef _
  TCP_CFG_N_FLAG_BITS
} tcp_cfg_flag_bits_e;

typedef enum tcp_cfg_flag_
{
#define _(sym, str) TCP_CFG_F_##sym = 1 << TCP_CFG_F_##sym##_BIT,
  foreach_tcp_cfg_flag
#undef _
  TCP_CFG_N_FLAGS
} tcp_cfg_flags_e;

/** TCP connection flags */
#define foreach_tcp_connection_flag             \
  _(SNDACK, "Send ACK")                         \
  _(FINSNT, "FIN sent")				\
  _(RECOVERY, "Recovery")                    	\
  _(FAST_RECOVERY, "Fast Recovery")		\
  _(DCNT_PENDING, "Disconnect pending")		\
  _(HALF_OPEN_DONE, "Half-open completed")	\
  _(FINPNDG, "FIN pending")			\
  _(RXT_PENDING, "Retransmit pending")		\
  _(FRXT_FIRST, "Retransmit first")		\
  _(DEQ_PENDING, "Dequeue pending ")		\
  _(PSH_PENDING, "PSH pending")			\
  _(FINRCVD, "FIN received")			\
  _(ZERO_RWND_SENT, "Zero RWND sent")		\

typedef enum tcp_connection_flag_bits_
{
#define _(sym, str) TCP_CONN_##sym##_BIT,
  foreach_tcp_connection_flag
#undef _
  TCP_CONN_N_FLAG_BITS
} tcp_connection_flag_bits_e;

typedef enum tcp_connection_flag_
{
#define _(sym, str) TCP_CONN_##sym = 1 << TCP_CONN_##sym##_BIT,
  foreach_tcp_connection_flag
#undef _
  TCP_CONN_N_FLAGS
} tcp_connection_flags_e;

#define TCP_SCOREBOARD_TRACE (0)
#define TCP_MAX_SACK_BLOCKS 256	/**< Max number of SACK blocks stored */
#define TCP_INVALID_SACK_HOLE_INDEX ((u32)~0)

typedef struct _scoreboard_trace_elt
{
  u32 start;
  u32 end;
  u32 ack;
  u32 snd_una_max;
  u32 group;
} scoreboard_trace_elt_t;

typedef struct _sack_scoreboard_hole
{
  u32 next;		/**< Index for next entry in linked list */
  u32 prev;		/**< Index for previous entry in linked list */
  u32 start;		/**< Start sequence number */
  u32 end;		/**< End sequence number */
  u8 is_lost;		/**< Mark hole as lost */
} sack_scoreboard_hole_t;

typedef struct _sack_scoreboard
{
  sack_scoreboard_hole_t *holes;	/**< Pool of holes */
  u32 head;				/**< Index of first entry */
  u32 tail;				/**< Index of last entry */
  u32 sacked_bytes;			/**< Number of bytes sacked in sb */
  u32 last_sacked_bytes;		/**< Number of bytes last sacked */
  u32 last_bytes_delivered;		/**< Sack bytes delivered to app */
  u32 rxt_sacked;			/**< Rxt bytes last delivered */
  u32 high_sacked;			/**< Highest byte sacked (fack) */
  u32 high_rxt;				/**< Highest retransmitted sequence */
  u32 rescue_rxt;			/**< Rescue sequence number */
  u32 lost_bytes;			/**< Bytes lost as per RFC6675 */
  u32 last_lost_bytes;			/**< Number of bytes last lost */
  u32 cur_rxt_hole;			/**< Retransmitting from this hole */
  u8 is_reneging;

#if TCP_SCOREBOARD_TRACE
  scoreboard_trace_elt_t *trace;
#endif

} sack_scoreboard_t;

#if TCP_SCOREBOARD_TRACE
#define tcp_scoreboard_trace_add(_tc, _ack) 				\
{									\
    static u64 _group = 0;						\
    sack_scoreboard_t *_sb = &_tc->sack_sb;				\
    sack_block_t *_sack, *_sacks;					\
    scoreboard_trace_elt_t *_elt;					\
    int i;								\
    _group++;								\
    _sacks = _tc->rcv_opts.sacks;					\
    for (i = 0; i < vec_len (_sacks); i++) 				\
      {									\
	_sack = &_sacks[i];						\
	vec_add2 (_sb->trace, _elt, 1);					\
	_elt->start = _sack->start;					\
	_elt->end = _sack->end;						\
	_elt->ack = _elt->end == _ack ? _ack : 0;			\
	_elt->snd_una_max = _elt->end == _ack ? _tc->snd_una_max : 0;	\
	_elt->group = _group;						\
      }									\
}
#else
#define tcp_scoreboard_trace_add(_tc, _ack)
#endif

sack_scoreboard_hole_t *scoreboard_next_rxt_hole (sack_scoreboard_t * sb,
						  sack_scoreboard_hole_t *
						  start, u8 have_sent_1_smss,
						  u8 * can_rescue,
						  u8 * snd_limited);
sack_scoreboard_hole_t *scoreboard_get_hole (sack_scoreboard_t * sb,
					     u32 index);

sack_scoreboard_hole_t *scoreboard_next_hole (sack_scoreboard_t * sb,
					      sack_scoreboard_hole_t * hole);
sack_scoreboard_hole_t *scoreboard_prev_hole (sack_scoreboard_t * sb,
					      sack_scoreboard_hole_t * hole);
sack_scoreboard_hole_t *scoreboard_first_hole (sack_scoreboard_t * sb);
sack_scoreboard_hole_t *scoreboard_last_hole (sack_scoreboard_t * sb);

void scoreboard_clear (sack_scoreboard_t * sb);
void scoreboard_clear_reneging (sack_scoreboard_t * sb, u32 start, u32 end);
void scoreboard_init (sack_scoreboard_t * sb);
void scoreboard_init_rxt (sack_scoreboard_t * sb, u32 snd_una);
u8 *format_tcp_scoreboard (u8 * s, va_list * args);

#define TCP_BTS_INVALID_INDEX	((u32)~0)

typedef enum tcp_bts_flags_
{
  TCP_BTS_IS_RXT = 1,
  TCP_BTS_IS_APP_LIMITED = 1 << 1,
  TCP_BTS_IS_SACKED = 1 << 2,
  TCP_BTS_IS_RXT_LOST = 1 << 3,
} __clib_packed tcp_bts_flags_t;

typedef struct tcp_bt_sample_
{
  u32 next;			/**< Next sample index in list */
  u32 prev;			/**< Previous sample index in list */
  u32 min_seq;			/**< Min seq number in sample */
  u32 max_seq;			/**< Max seq number. Set for rxt samples */
  u64 delivered;		/**< Total delivered bytes for sample */
  f64 delivered_time;		/**< Delivered time when sample taken */
  f64 tx_time;			/**< Transmit time for the burst */
  f64 first_tx_time;		/**< Connection first tx time at tx */
  tcp_bts_flags_t flags;	/**< Sample flag */
} tcp_bt_sample_t;

typedef struct tcp_rate_sample_
{
  u64 prior_delivered;		/**< Delivered of sample used for rate, i.e.,
				     total bytes delivered at prior_time */
  f64 prior_time;		/**< Delivered time of sample used for rate */
  f64 interval_time;		/**< Time to ack the bytes delivered */
  f64 rtt_time;			/**< RTT for sample */
  u32 delivered;		/**< Bytes delivered in interval_time */
  u32 acked_and_sacked;		/**< Bytes acked + sacked now */
  u32 lost;			/**< Bytes lost now */
  tcp_bts_flags_t flags;	/**< Rate sample flags from bt sample */
} tcp_rate_sample_t;

typedef struct tcp_byte_tracker_
{
  tcp_bt_sample_t *samples;	/**< Pool of samples */
  rb_tree_t sample_lookup;	/**< Rbtree for sample lookup by min_seq */
  u32 head;			/**< Head of samples linked list */
  u32 tail;			/**< Tail of samples linked list */
  u32 last_ooo;			/**< Cached last ooo sample */
} tcp_byte_tracker_t;

typedef enum _tcp_cc_algorithm_type
{
  TCP_CC_NEWRENO,
  TCP_CC_CUBIC,
  TCP_CC_LAST = TCP_CC_CUBIC
} tcp_cc_algorithm_type_e;

typedef struct _tcp_cc_algorithm tcp_cc_algorithm_t;

typedef enum _tcp_cc_ack_t
{
  TCP_CC_ACK,
  TCP_CC_DUPACK,
  TCP_CC_PARTIALACK
} tcp_cc_ack_t;

typedef enum tcp_cc_event_
{
  TCP_CC_EVT_START_TX,
} tcp_cc_event_t;

/*
 * As per RFC4898 tcpEStatsStackSoftErrors
 */
typedef struct tcp_errors_
{
  u32 below_data_wnd;	/**< All data in seg is below snd_una */
  u32 above_data_wnd;	/**< Some data in segment is above snd_wnd */
  u32 below_ack_wnd;	/**< Acks for data below snd_una */
  u32 above_ack_wnd;	/**< Acks for data not sent */
} tcp_errors_t;

typedef struct _tcp_connection
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);
  transport_connection_t connection;  /**< Common transport data. First! */

  u8 state;			/**< TCP state as per tcp_state_t */
  u8 cfg_flags;			/**< Connection configuration flags */
  u16 flags;			/**< Connection flags (see tcp_conn_flags_e) */
  u32 timers[TCP_N_TIMERS];	/**< Timer handles into timer wheel */

  u64 segs_in;		/** RFC4022/4898 tcpHCInSegs/tcpEStatsPerfSegsIn */
  u64 bytes_in;		/** RFC4898 tcpEStatsPerfHCDataOctetsIn */
  u64 segs_out;		/** RFC4898 tcpEStatsPerfSegsOut */
  u64 bytes_out;	/** RFC4898 tcpEStatsPerfHCDataOctetsOut */

  /** Send sequence variables RFC793 */
  u32 snd_una;		/**< oldest unacknowledged sequence number */
  u32 snd_una_max;	/**< newest unacknowledged sequence number + 1*/
  u32 snd_wnd;		/**< send window */
  u32 snd_wl1;		/**< seq number used for last snd.wnd update */
  u32 snd_wl2;		/**< ack number used for last snd.wnd update */
  u32 snd_nxt;		/**< next seq number to be sent */
  u16 snd_mss;		/**< Effective send max seg (data) size */

  u64 data_segs_in;	/** RFC4898 tcpEStatsPerfDataSegsIn */
  u64 data_segs_out;	/** RFC4898 tcpEStatsPerfDataSegsOut */

  /** Receive sequence variables RFC793 */
  u32 rcv_nxt;		/**< next sequence number expected */
  u32 rcv_wnd;		/**< receive window we expect */

  u32 rcv_las;		/**< rcv_nxt at last ack sent/rcv_wnd update */
  u32 iss;		/**< initial sent sequence */
  u32 irs;		/**< initial remote sequence */

  /* Options */
  u8 snd_opts_len;		/**< Tx options len */
  u8 rcv_wscale;		/**< Window scale to advertise to peer */
  u8 snd_wscale;		/**< Window scale to use when sending */
  u32 tsval_recent;		/**< Last timestamp received */
  u32 tsval_recent_age;		/**< When last updated tstamp_recent*/
  tcp_options_t snd_opts;	/**< Tx options for connection */
  tcp_options_t rcv_opts;	/**< Rx options for connection */

  sack_block_t *snd_sacks;	/**< Vector of SACKs to send. XXX Fixed size? */
  u8 snd_sack_pos;		/**< Position in vec of first block to send */
  sack_block_t *snd_sacks_fl;	/**< Vector for building new list */
  sack_scoreboard_t sack_sb;	/**< SACK "scoreboard" that tracks holes */

  u16 rcv_dupacks;	/**< Number of recent DUPACKs received */
  u32 dupacks_in;	/**< RFC4898 tcpEStatsStackDupAcksIn*/
  u8 pending_dupacks;	/**< Number of DUPACKs to be sent */
  u32 dupacks_out;	/**< RFC4898 tcpEStatsPathDupAcksOut */

  /* Congestion control */
  u32 cwnd;		/**< Congestion window */
  u32 cwnd_acc_bytes;	/**< Bytes accumulated for cwnd increment */
  u32 ssthresh;		/**< Slow-start threshold */
  u32 prev_ssthresh;	/**< ssthresh before congestion */
  u32 prev_cwnd;	/**< ssthresh before congestion */
  u32 bytes_acked;	/**< Bytes acknowledged by current segment */
  u32 burst_acked;	/**< Bytes acknowledged in current burst */
  u32 snd_rxt_bytes;	/**< Retransmitted bytes during current cc event */
  u32 snd_rxt_ts;	/**< Timestamp when first packet is retransmitted */
  u32 prr_delivered;	/**< RFC6937 bytes delivered during current event */
  u32 prr_start;	/**< snd_una when prr starts */
  u32 rxt_delivered;	/**< Rxt bytes delivered during current cc event */
  u32 rxt_head;		/**< snd_una last time we re rxted the head */
  u32 tsecr_last_ack;	/**< Timestamp echoed to us in last healthy ACK */
  u32 snd_congestion;	/**< snd_una_max when congestion is detected */
  u32 tx_fifo_size;	/**< Tx fifo size. Used to constrain cwnd */
  tcp_cc_algorithm_t *cc_algo;	/**< Congestion control algorithm */
  u8 cc_data[TCP_CC_DATA_SZ];	/**< Congestion control algo private data */

  u32 fr_occurences;	/**< fast-retransmit occurrences RFC4898
			     tcpEStatsStackFastRetran */
  u32 tr_occurences;	/**< timer-retransmit occurrences */
  u64 bytes_retrans;	/**< RFC4898 tcpEStatsPerfOctetsRetrans */
  u64 segs_retrans;	/**< RFC4898 tcpEStatsPerfSegsRetrans*/

  /* RTT and RTO */
  u32 rto;		/**< Retransmission timeout */
  u32 rto_boff;		/**< Index for RTO backoff */
  u32 srtt;		/**< Smoothed RTT */
  u32 rttvar;		/**< Smoothed mean RTT difference. Approximates variance */
  u32 rtt_seq;		/**< Sequence number for tracked ACK */
  f64 rtt_ts;		/**< Timestamp for tracked ACK */
  f64 mrtt_us;		/**< High precision mrtt from tracked acks */

  u32 psh_seq;		/**< Add psh header for seg that includes this */
  u32 next_node_index;	/**< Can be used to control next node in output */
  u32 next_node_opaque;	/**< Opaque to pass to next node */
  u32 limited_transmit;	/**< snd_nxt when limited transmit starts */
  u32 sw_if_index;	/**< Interface for the connection */

  /* Delivery rate estimation */
  u64 delivered;		/**< Total bytes delivered to peer */
  u64 app_limited;		/**< Delivered when app-limited detected */
  f64 delivered_time;		/**< Time last bytes were acked */
  f64 first_tx_time;		/**< Send time for recently delivered/sent */
  tcp_byte_tracker_t *bt;	/**< Tx byte tracker */

  tcp_errors_t errors;	/**< Soft connection errors */

  f64 start_ts;		/**< Timestamp when connection initialized */
  u32 last_fib_check;	/**< Last time we checked fib route for peer */
  u16 mss;		/**< Our max seg size that includes options */
  u32 timestamp_delta;	/**< Offset for timestamp */
  u32 ipv6_flow_label;	/**< flow label for ipv6 header */
} tcp_connection_t;

/* *INDENT-OFF* */
struct _tcp_cc_algorithm
{
  const char *name;
  uword (*unformat_cfg) (unformat_input_t * input);
  void (*init) (tcp_connection_t * tc);
  void (*cleanup) (tcp_connection_t * tc);
  void (*rcv_ack) (tcp_connection_t * tc, tcp_rate_sample_t *rs);
  void (*rcv_cong_ack) (tcp_connection_t * tc, tcp_cc_ack_t ack,
			tcp_rate_sample_t *rs);
  void (*congestion) (tcp_connection_t * tc);
  void (*loss) (tcp_connection_t * tc);
  void (*recovered) (tcp_connection_t * tc);
  void (*undo_recovery) (tcp_connection_t * tc);
  void (*event) (tcp_connection_t *tc, tcp_cc_event_t evt);
  u64 (*get_pacing_rate) (tcp_connection_t *tc);
};
/* *INDENT-ON* */

#define tcp_fastrecovery_on(tc) (tc)->flags |= TCP_CONN_FAST_RECOVERY
#define tcp_fastrecovery_off(tc) (tc)->flags &= ~TCP_CONN_FAST_RECOVERY
#define tcp_recovery_on(tc) (tc)->flags |= TCP_CONN_RECOVERY
#define tcp_recovery_off(tc) (tc)->flags &= ~TCP_CONN_RECOVERY
#define tcp_in_fastrecovery(tc) ((tc)->flags & TCP_CONN_FAST_RECOVERY)
#define tcp_in_recovery(tc) ((tc)->flags & (TCP_CONN_RECOVERY))
#define tcp_in_slowstart(tc) (tc->cwnd < tc->ssthresh)
#define tcp_disconnect_pending(tc) ((tc)->flags & TCP_CONN_DCNT_PENDING)
#define tcp_disconnect_pending_on(tc) ((tc)->flags |= TCP_CONN_DCNT_PENDING)
#define tcp_disconnect_pending_off(tc) ((tc)->flags &= ~TCP_CONN_DCNT_PENDING)
#define tcp_fastrecovery_first(tc) ((tc)->flags & TCP_CONN_FRXT_FIRST)
#define tcp_fastrecovery_first_on(tc) ((tc)->flags |= TCP_CONN_FRXT_FIRST)
#define tcp_fastrecovery_first_off(tc) ((tc)->flags &= ~TCP_CONN_FRXT_FIRST)

#define tcp_in_cong_recovery(tc) ((tc)->flags & 		\
	  (TCP_CONN_FAST_RECOVERY | TCP_CONN_RECOVERY))

#define tcp_csum_offload(tc) (!((tc)->cfg_flags & TCP_CFG_F_NO_CSUM_OFFLOAD))

always_inline void
tcp_cong_recovery_off (tcp_connection_t * tc)
{
  tc->flags &= ~(TCP_CONN_FAST_RECOVERY | TCP_CONN_RECOVERY);
  tcp_fastrecovery_first_off (tc);
}

#define tcp_zero_rwnd_sent(tc) ((tc)->flags & TCP_CONN_ZERO_RWND_SENT)
#define tcp_zero_rwnd_sent_on(tc) (tc)->flags |= TCP_CONN_ZERO_RWND_SENT
#define tcp_zero_rwnd_sent_off(tc) (tc)->flags &= ~TCP_CONN_ZERO_RWND_SENT

typedef enum _tcp_error
{
#define tcp_error(n,s) TCP_ERROR_##n,
#include <vnet/tcp/tcp_error.def>
#undef tcp_error
  TCP_N_ERROR,
} tcp_error_t;

typedef struct _tcp_lookup_dispatch
{
  u8 next, error;
} tcp_lookup_dispatch_t;

typedef struct tcp_worker_ctx_
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);
  /** worker time */
  u32 time_now;

  /** worker timer wheel */
  tw_timer_wheel_16t_2w_512sl_t timer_wheel;

  /** tx buffer free list */
  u32 *tx_buffers;

  /** tx frames for ip 4/6 lookup nodes */
  vlib_frame_t *ip_lookup_tx_frames[2];

  /** vector of pending ack dequeues */
  u32 *pending_deq_acked;

  /** vector of pending disconnect notifications */
  u32 *pending_disconnects;

  /** convenience pointer to this thread's vlib main */
  vlib_main_t *vm;

    CLIB_CACHE_LINE_ALIGN_MARK (cacheline1);

  /** cached 'on the wire' options for bursts */
  u8 cached_opts[40];

} tcp_worker_ctx_t;

typedef struct tcp_iss_seed_
{
  u64 first;
  u64 second;
} tcp_iss_seed_t;

typedef struct tcp_configuration_
{
  /** Max rx fifo size for a session (in bytes). It is used in to compute the
   *  rfc 7323 window scaling factor */
  u32 max_rx_fifo;

  /** Min rx fifo for a session (in bytes) */
  u32 min_rx_fifo;

  /** Default MTU to be used when establishing connections */
  u16 default_mtu;

  /** Initial CWND multiplier, which multiplies MSS to determine initial CWND.
   *  Set 0 to determine the initial CWND by another way */
  u16 initial_cwnd_multiplier;

  /** Enable tx pacing for new connections */
  u8 enable_tx_pacing;

  /** Allow use of TSO whenever available */
  u8 allow_tso;

  /** Set if csum offloading is enabled */
  u8 csum_offload;

  /** Default congestion control algorithm type */
  tcp_cc_algorithm_type_e cc_algo;

  /** Min rwnd, as number of snd_mss segments, for update ack to be sent after
   * a zero rwnd advertisement */
  u32 rwnd_min_update_ack;

  /** Delayed ack time (disabled) */
  u16 delack_time;

  /** Timer ticks to wait for close from app */
  u16 closewait_time;

  /** Timer ticks to wait in time-wait. Also known as 2MSL */
  u16 timewait_time;

  /** Timer ticks to wait in fin-wait1 to send fin and rcv fin-ack */
  u16 finwait1_time;

  /** Timer ticks to wait in last ack for ack */
  u16 lastack_time;

  /** Timer ticks to wait in fin-wait2 for fin */
  u16 finwait2_time;

  /** Timer ticks to wait in closing for fin ack */
  u16 closing_time;

  /** Timer ticks to wait before cleaning up the connection */
  u16 cleanup_time;

  /** Number of preallocated connections */
  u32 preallocated_connections;

  /** Number of preallocated half-open connections */
  u32 preallocated_half_open_connections;

  /** Vectors of src addresses. Optional unless one needs > 63K active-opens */
  ip4_address_t *ip4_src_addrs;
  ip6_address_t *ip6_src_addrs;

  /** Fault-injection. Debug only */
  f64 buffer_fail_fraction;
} tcp_configuration_t;

typedef struct _tcp_main
{
  /* Per-worker thread tcp connection pools */
  tcp_connection_t **connections;

  /* Pool of listeners. */
  tcp_connection_t *listener_pool;

  /** Dispatch table by state and flags */
  tcp_lookup_dispatch_t dispatch_table[TCP_N_STATES][64];

  u8 log2_tstamp_clocks_per_tick;
  f64 tstamp_ticks_per_clock;

  /** per-worker context */
  tcp_worker_ctx_t *wrk_ctx;

  /** Pool of half-open connections on which we've sent a SYN */
  tcp_connection_t *half_open_connections;
  clib_spinlock_t half_open_lock;

  /** vlib buffer size */
  u32 bytes_per_buffer;

  /** Seed used to generate random iss */
  tcp_iss_seed_t iss_seed;

  /** Congestion control algorithms registered */
  tcp_cc_algorithm_t *cc_algos;

  /** Hash table of cc algorithms by name */
  uword *cc_algo_by_name;

  /** Last cc algo registered */
  tcp_cc_algorithm_type_e cc_last_type;

  /** Flag that indicates if stack is on or off */
  u8 is_enabled;

  /** Flag that indicates if v4 punting is enabled */
  u8 punt_unknown4;

  /** Flag that indicates if v6 punting is enabled */
  u8 punt_unknown6;

  /** Rotor for v4 source addresses */
  u32 last_v4_addr_rotor;

  /** Rotor for v6 source addresses */
  u32 last_v6_addr_rotor;

  /** Protocol configuration */
  tcp_configuration_t cfg;
} tcp_main_t;

extern tcp_main_t tcp_main;
extern vlib_node_registration_t tcp4_input_node;
extern vlib_node_registration_t tcp6_input_node;
extern vlib_node_registration_t tcp4_output_node;
extern vlib_node_registration_t tcp6_output_node;
extern vlib_node_registration_t tcp4_established_node;
extern vlib_node_registration_t tcp6_established_node;
extern vlib_node_registration_t tcp4_syn_sent_node;
extern vlib_node_registration_t tcp6_syn_sent_node;
extern vlib_node_registration_t tcp4_rcv_process_node;
extern vlib_node_registration_t tcp6_rcv_process_node;
extern vlib_node_registration_t tcp4_listen_node;
extern vlib_node_registration_t tcp6_listen_node;

#define tcp_cfg tcp_main.cfg
#define tcp_node_index(node_id, is_ip4) 				\
  ((is_ip4) ? tcp4_##node_id##_node.index : tcp6_##node_id##_node.index)

always_inline tcp_main_t *
vnet_get_tcp_main ()
{
  return &tcp_main;
}

always_inline tcp_worker_ctx_t *
tcp_get_worker (u32 thread_index)
{
  return &tcp_main.wrk_ctx[thread_index];
}

always_inline tcp_header_t *
tcp_buffer_hdr (vlib_buffer_t * b)
{
  ASSERT ((signed) b->current_data >= (signed) -VLIB_BUFFER_PRE_DATA_SIZE);
  return (tcp_header_t *) (b->data + b->current_data
			   + vnet_buffer (b)->tcp.hdr_offset);
}

#if (VLIB_BUFFER_TRACE_TRAJECTORY)
#define tcp_trajectory_add_start(b, start)			\
{								\
    (*vlib_buffer_trace_trajectory_cb) (b, start);		\
}
#else
#define tcp_trajectory_add_start(b, start)
#endif

clib_error_t *vnet_tcp_enable_disable (vlib_main_t * vm, u8 is_en);

void tcp_punt_unknown (vlib_main_t * vm, u8 is_ip4, u8 is_add);

always_inline tcp_connection_t *
tcp_connection_get (u32 conn_index, u32 thread_index)
{
  if (PREDICT_FALSE
      (pool_is_free_index (tcp_main.connections[thread_index], conn_index)))
    return 0;
  return pool_elt_at_index (tcp_main.connections[thread_index], conn_index);
}

always_inline tcp_connection_t *
tcp_connection_get_if_valid (u32 conn_index, u32 thread_index)
{
  if (tcp_main.connections[thread_index] == 0)
    return 0;
  if (pool_is_free_index (tcp_main.connections[thread_index], conn_index))
    return 0;
  return pool_elt_at_index (tcp_main.connections[thread_index], conn_index);
}

always_inline tcp_connection_t *
tcp_get_connection_from_transport (transport_connection_t * tconn)
{
  return (tcp_connection_t *) tconn;
}

always_inline void
tcp_connection_set_state (tcp_connection_t * tc, tcp_state_t state)
{
  tc->state = state;
  TCP_EVT (TCP_EVT_STATE_CHANGE, tc);
}

void tcp_connection_close (tcp_connection_t * tc);
void tcp_connection_cleanup (tcp_connection_t * tc);
void tcp_connection_del (tcp_connection_t * tc);
int tcp_half_open_connection_cleanup (tcp_connection_t * tc);
tcp_connection_t *tcp_connection_alloc (u8 thread_index);
tcp_connection_t *tcp_connection_alloc_w_base (u8 thread_index,
					       tcp_connection_t * base);
void tcp_connection_free (tcp_connection_t * tc);
void tcp_connection_reset (tcp_connection_t * tc);
int tcp_configure_v4_source_address_range (vlib_main_t * vm,
					   ip4_address_t * start,
					   ip4_address_t * end, u32 table_id);
int tcp_configure_v6_source_address_range (vlib_main_t * vm,
					   ip6_address_t * start,
					   ip6_address_t * end, u32 table_id);
void tcp_api_reference (void);
u8 *format_tcp_connection (u8 * s, va_list * args);

always_inline tcp_connection_t *
tcp_listener_get (u32 tli)
{
  return pool_elt_at_index (tcp_main.listener_pool, tli);
}

always_inline tcp_connection_t *
tcp_half_open_connection_get (u32 conn_index)
{
  tcp_connection_t *tc = 0;
  clib_spinlock_lock_if_init (&tcp_main.half_open_lock);
  if (!pool_is_free_index (tcp_main.half_open_connections, conn_index))
    tc = pool_elt_at_index (tcp_main.half_open_connections, conn_index);
  clib_spinlock_unlock_if_init (&tcp_main.half_open_lock);
  return tc;
}

void tcp_make_fin (tcp_connection_t * tc, vlib_buffer_t * b);
void tcp_make_synack (tcp_connection_t * ts, vlib_buffer_t * b);
void tcp_send_reset_w_pkt (tcp_connection_t * tc, vlib_buffer_t * pkt,
			   u32 thread_index, u8 is_ip4);
void tcp_send_reset (tcp_connection_t * tc);
void tcp_send_syn (tcp_connection_t * tc);
void tcp_send_synack (tcp_connection_t * tc);
void tcp_send_fin (tcp_connection_t * tc);
void tcp_send_ack (tcp_connection_t * tc);
void tcp_update_burst_snd_vars (tcp_connection_t * tc);
void tcp_update_rto (tcp_connection_t * tc);
void tcp_flush_frames_to_output (tcp_worker_ctx_t * wrk);
void tcp_send_window_update_ack (tcp_connection_t * tc);

void tcp_program_ack (tcp_connection_t * tc);
void tcp_program_dupack (tcp_connection_t * tc);
void tcp_program_retransmit (tcp_connection_t * tc);

/*
 * Rate estimation
 */

/**
 * Byte tracker initialize
 *
 * @param tc	connection for which the byte tracker should be allocated and
 * 		initialized
 */
void tcp_bt_init (tcp_connection_t * tc);
/**
 * Byte tracker cleanup
 *
 * @param tc	connection for which the byte tracker should be cleaned up
 */
void tcp_bt_cleanup (tcp_connection_t * tc);
/**
 * Flush byte tracker samples
 *
 * @param tc	tcp connection for which samples should be flushed
 */
void tcp_bt_flush_samples (tcp_connection_t * tc);
/**
 * Track a tcp tx burst
 *
 * @param tc	tcp connection
 */
void tcp_bt_track_tx (tcp_connection_t * tc, u32 len);
/**
 * Track a tcp retransmission
 *
 * @param tc	tcp connection
 * @param start	start sequence number
 * @param end	end sequence number
 */
void tcp_bt_track_rxt (tcp_connection_t * tc, u32 start, u32 end);
/**
 * Generate a delivery rate sample from recently acked bytes
 *
 * @param tc	tcp connection
 * @param rs	resulting rate sample
 */
void tcp_bt_sample_delivery_rate (tcp_connection_t * tc,
				  tcp_rate_sample_t * rs);
/**
 * Check if sample to be generated is app limited
 *
 * @param tc	tcp connection
 */
void tcp_bt_check_app_limited (tcp_connection_t * tc);
/**
 * Check if the byte tracker is in sane state
 *
 * Should be used only for testing
 *
 * @param bt	byte tracker
 */
int tcp_bt_is_sane (tcp_byte_tracker_t * bt);
u8 *format_tcp_bt (u8 * s, va_list * args);

always_inline u32
tcp_end_seq (tcp_header_t * th, u32 len)
{
  return th->seq_number + tcp_is_syn (th) + tcp_is_fin (th) + len;
}

/* Modulo arithmetic for TCP sequence numbers */
#define seq_lt(_s1, _s2) ((i32)((_s1)-(_s2)) < 0)
#define seq_leq(_s1, _s2) ((i32)((_s1)-(_s2)) <= 0)
#define seq_gt(_s1, _s2) ((i32)((_s1)-(_s2)) > 0)
#define seq_geq(_s1, _s2) ((i32)((_s1)-(_s2)) >= 0)
#define seq_max(_s1, _s2) (seq_gt((_s1), (_s2)) ? (_s1) : (_s2))

/* Modulo arithmetic for timestamps */
#define timestamp_lt(_t1, _t2) ((i32)((_t1)-(_t2)) < 0)
#define timestamp_leq(_t1, _t2) ((i32)((_t1)-(_t2)) <= 0)

/**
 * Our estimate of the number of bytes that have left the network
 */
always_inline u32
tcp_bytes_out (const tcp_connection_t * tc)
{
  if (tcp_opts_sack_permitted (&tc->rcv_opts))
    return tc->sack_sb.sacked_bytes + tc->sack_sb.lost_bytes;
  else
    return clib_min (tc->rcv_dupacks * tc->snd_mss,
		     tc->snd_nxt - tc->snd_una);
}

/**
 * Our estimate of the number of bytes in flight (pipe size)
 */
always_inline u32
tcp_flight_size (const tcp_connection_t * tc)
{
  int flight_size;

  flight_size = (int) (tc->snd_nxt - tc->snd_una) - tcp_bytes_out (tc)
    + tc->snd_rxt_bytes - tc->rxt_delivered;

  ASSERT (flight_size >= 0);

  return flight_size;
}

/**
 * Initial cwnd as per RFC5681
 */
always_inline u32
tcp_initial_cwnd (const tcp_connection_t * tc)
{
  if (tcp_cfg.initial_cwnd_multiplier > 0)
    return tcp_cfg.initial_cwnd_multiplier * tc->snd_mss;

  if (tc->snd_mss > 2190)
    return 2 * tc->snd_mss;
  else if (tc->snd_mss > 1095)
    return 3 * tc->snd_mss;
  else
    return 4 * tc->snd_mss;
}

/*
 * Accumulate acked bytes for cwnd increase
 *
 * Once threshold bytes are accumulated, snd_mss bytes are added
 * to the cwnd.
 */
always_inline void
tcp_cwnd_accumulate (tcp_connection_t * tc, u32 thresh, u32 bytes)
{
  tc->cwnd_acc_bytes += bytes;
  if (tc->cwnd_acc_bytes >= thresh)
    {
      u32 inc = tc->cwnd_acc_bytes / thresh;
      tc->cwnd_acc_bytes -= inc * thresh;
      tc->cwnd += inc * tc->snd_mss;
      tc->cwnd = clib_min (tc->cwnd, tc->tx_fifo_size);
    }
}

always_inline u32
tcp_loss_wnd (const tcp_connection_t * tc)
{
  /* Whatever we have in flight + the packet we're about to send */
  return tcp_flight_size (tc) + tc->snd_mss;
}

always_inline u32
tcp_available_snd_wnd (const tcp_connection_t * tc)
{
  return clib_min (tc->cwnd, tc->snd_wnd);
}

always_inline u32
tcp_available_output_snd_space (const tcp_connection_t * tc)
{
  u32 available_wnd = tcp_available_snd_wnd (tc);
  int flight_size = (int) (tc->snd_nxt - tc->snd_una);

  if (available_wnd <= flight_size)
    return 0;

  return available_wnd - flight_size;
}

/**
 * Estimate of how many bytes we can still push into the network
 */
always_inline u32
tcp_available_cc_snd_space (const tcp_connection_t * tc)
{
  u32 available_wnd = tcp_available_snd_wnd (tc);
  u32 flight_size = tcp_flight_size (tc);

  if (available_wnd <= flight_size)
    return 0;

  return available_wnd - flight_size;
}

always_inline u8
tcp_is_lost_fin (tcp_connection_t * tc)
{
  if ((tc->flags & TCP_CONN_FINSNT) && (tc->snd_una_max - tc->snd_una == 1))
    return 1;
  return 0;
}

u32 tcp_snd_space (tcp_connection_t * tc);
int tcp_fastrecovery_prr_snd_space (tcp_connection_t * tc);

fib_node_index_t tcp_lookup_rmt_in_fib (tcp_connection_t * tc);

/* Made public for unit testing only */
void tcp_update_sack_list (tcp_connection_t * tc, u32 start, u32 end);
u32 tcp_sack_list_bytes (tcp_connection_t * tc);

always_inline u32
tcp_time_now (void)
{
  return tcp_main.wrk_ctx[vlib_get_thread_index ()].time_now;
}

always_inline u32
tcp_time_now_w_thread (u32 thread_index)
{
  return tcp_main.wrk_ctx[thread_index].time_now;
}

/**
 * Generate timestamp for tcp connection
 */
always_inline u32
tcp_tstamp (tcp_connection_t * tc)
{
  return (tcp_main.wrk_ctx[tc->c_thread_index].time_now -
	  tc->timestamp_delta);
}

always_inline f64
tcp_time_now_us (u32 thread_index)
{
  return transport_time_now (thread_index);
}

always_inline u32
tcp_set_time_now (tcp_worker_ctx_t * wrk)
{
  wrk->time_now = clib_cpu_time_now () * tcp_main.tstamp_ticks_per_clock;
  return wrk->time_now;
}

u32 tcp_session_push_header (transport_connection_t * tconn,
			     vlib_buffer_t * b);
int tcp_session_custom_tx (void *conn, u32 max_burst_size);

void tcp_connection_timers_init (tcp_connection_t * tc);
void tcp_connection_timers_reset (tcp_connection_t * tc);
void tcp_init_snd_vars (tcp_connection_t * tc);
void tcp_connection_init_vars (tcp_connection_t * tc);
void tcp_connection_tx_pacer_update (tcp_connection_t * tc);
void tcp_connection_tx_pacer_reset (tcp_connection_t * tc, u32 window,
				    u32 start_bucket);

always_inline void
tcp_cc_rcv_ack (tcp_connection_t * tc, tcp_rate_sample_t * rs)
{
  tc->cc_algo->rcv_ack (tc, rs);
  tc->tsecr_last_ack = tc->rcv_opts.tsecr;
}

static inline void
tcp_cc_rcv_cong_ack (tcp_connection_t * tc, tcp_cc_ack_t ack_type,
		     tcp_rate_sample_t * rs)
{
  tc->cc_algo->rcv_cong_ack (tc, ack_type, rs);
}

static inline void
tcp_cc_congestion (tcp_connection_t * tc)
{
  tc->cc_algo->congestion (tc);
}

static inline void
tcp_cc_loss (tcp_connection_t * tc)
{
  tc->cc_algo->loss (tc);
}

static inline void
tcp_cc_recovered (tcp_connection_t * tc)
{
  tc->cc_algo->recovered (tc);
}

static inline void
tcp_cc_undo_recovery (tcp_connection_t * tc)
{
  if (tc->cc_algo->undo_recovery)
    tc->cc_algo->undo_recovery (tc);
}

static inline void
tcp_cc_event (tcp_connection_t * tc, tcp_cc_event_t evt)
{
  if (tc->cc_algo->event)
    tc->cc_algo->event (tc, evt);
}

static inline u64
tcp_cc_get_pacing_rate (tcp_connection_t * tc)
{
  if (tc->cc_algo->get_pacing_rate)
    return tc->cc_algo->get_pacing_rate (tc);

  f64 srtt = clib_min ((f64) tc->srtt * TCP_TICK, tc->mrtt_us);

  /* TODO should constrain to interface's max throughput but
   * we don't have link speeds for sw ifs ..*/
  return ((f64) tc->cwnd / srtt);
}

always_inline void
tcp_timer_set (tcp_connection_t * tc, u8 timer_id, u32 interval)
{
  ASSERT (tc->c_thread_index == vlib_get_thread_index ());
  ASSERT (tc->timers[timer_id] == TCP_TIMER_HANDLE_INVALID);
  tc->timers[timer_id] =
    tw_timer_start_16t_2w_512sl (&tcp_main.
				 wrk_ctx[tc->c_thread_index].timer_wheel,
				 tc->c_c_index, timer_id, interval);
}

always_inline void
tcp_timer_reset (tcp_connection_t * tc, u8 timer_id)
{
  ASSERT (tc->c_thread_index == vlib_get_thread_index ());
  if (tc->timers[timer_id] == TCP_TIMER_HANDLE_INVALID)
    return;

  tw_timer_stop_16t_2w_512sl (&tcp_main.
			      wrk_ctx[tc->c_thread_index].timer_wheel,
			      tc->timers[timer_id]);
  tc->timers[timer_id] = TCP_TIMER_HANDLE_INVALID;
}

always_inline void
tcp_timer_update (tcp_connection_t * tc, u8 timer_id, u32 interval)
{
  ASSERT (tc->c_thread_index == vlib_get_thread_index ());
  if (tc->timers[timer_id] != TCP_TIMER_HANDLE_INVALID)
    tw_timer_update_16t_2w_512sl (&tcp_main.
				  wrk_ctx[tc->c_thread_index].timer_wheel,
				  tc->timers[timer_id], interval);
  else
    tc->timers[timer_id] =
      tw_timer_start_16t_2w_512sl (&tcp_main.
				   wrk_ctx[tc->c_thread_index].timer_wheel,
				   tc->c_c_index, timer_id, interval);
}

always_inline void
tcp_retransmit_timer_set (tcp_connection_t * tc)
{
  ASSERT (tc->snd_una != tc->snd_una_max);
  tcp_timer_set (tc, TCP_TIMER_RETRANSMIT,
		 clib_max (tc->rto * TCP_TO_TIMER_TICK, 1));
}

always_inline void
tcp_retransmit_timer_reset (tcp_connection_t * tc)
{
  tcp_timer_reset (tc, TCP_TIMER_RETRANSMIT);
}

always_inline void
tcp_retransmit_timer_force_update (tcp_connection_t * tc)
{
  tcp_timer_update (tc, TCP_TIMER_RETRANSMIT,
		    clib_max (tc->rto * TCP_TO_TIMER_TICK, 1));
}

always_inline void
tcp_persist_timer_set (tcp_connection_t * tc)
{
  /* Reuse RTO. It's backed off in handler */
  tcp_timer_set (tc, TCP_TIMER_PERSIST,
		 clib_max (tc->rto * TCP_TO_TIMER_TICK, 1));
}

always_inline void
tcp_persist_timer_update (tcp_connection_t * tc)
{
  u32 interval;

  if (seq_leq (tc->snd_una, tc->snd_congestion + tc->burst_acked))
    interval = 1;
  else
    interval = clib_max (tc->rto * TCP_TO_TIMER_TICK, 1);

  tcp_timer_update (tc, TCP_TIMER_PERSIST, interval);
}

always_inline void
tcp_persist_timer_reset (tcp_connection_t * tc)
{
  tcp_timer_reset (tc, TCP_TIMER_PERSIST);
}

always_inline void
tcp_retransmit_timer_update (tcp_connection_t * tc)
{
  if (tc->snd_una == tc->snd_nxt)
    {
      tcp_retransmit_timer_reset (tc);
      if (tc->snd_wnd < tc->snd_mss)
	tcp_persist_timer_update (tc);
    }
  else
    tcp_timer_update (tc, TCP_TIMER_RETRANSMIT,
		      clib_max (tc->rto * TCP_TO_TIMER_TICK, 1));
}

always_inline u8
tcp_timer_is_active (tcp_connection_t * tc, tcp_timers_e timer)
{
  return tc->timers[timer] != TCP_TIMER_HANDLE_INVALID;
}

#define tcp_validate_txf_size(_tc, _a) 					\
  ASSERT(_tc->state != TCP_STATE_ESTABLISHED 				\
	 || transport_max_tx_dequeue (&_tc->connection) >= _a)

void tcp_rcv_sacks (tcp_connection_t * tc, u32 ack);
u8 *tcp_scoreboard_replay (u8 * s, tcp_connection_t * tc, u8 verbose);

/**
 * Register exiting cc algo type
 */
void tcp_cc_algo_register (tcp_cc_algorithm_type_e type,
			   const tcp_cc_algorithm_t * vft);

/**
 * Register new cc algo type
 */
tcp_cc_algorithm_type_e tcp_cc_algo_new_type (const tcp_cc_algorithm_t * vft);
tcp_cc_algorithm_t *tcp_cc_algo_get (tcp_cc_algorithm_type_e type);

static inline void *
tcp_cc_data (tcp_connection_t * tc)
{
  return (void *) tc->cc_data;
}

void newreno_rcv_cong_ack (tcp_connection_t * tc, tcp_cc_ack_t ack_type,
			   tcp_rate_sample_t * rs);

/**
 * Push TCP header to buffer
 *
 * @param vm - vlib_main
 * @param b - buffer to write the header to
 * @param sp_net - source port net order
 * @param dp_net - destination port net order
 * @param seq - sequence number net order
 * @param ack - ack number net order
 * @param tcp_hdr_opts_len - header and options length in bytes
 * @param flags - header flags
 * @param wnd - window size
 *
 * @return - pointer to start of TCP header
 */
always_inline void *
vlib_buffer_push_tcp_net_order (vlib_buffer_t * b, u16 sp, u16 dp, u32 seq,
				u32 ack, u8 tcp_hdr_opts_len, u8 flags,
				u16 wnd)
{
  tcp_header_t *th;

  th = vlib_buffer_push_uninit (b, tcp_hdr_opts_len);

  th->src_port = sp;
  th->dst_port = dp;
  th->seq_number = seq;
  th->ack_number = ack;
  th->data_offset_and_reserved = (tcp_hdr_opts_len >> 2) << 4;
  th->flags = flags;
  th->window = wnd;
  th->checksum = 0;
  th->urgent_pointer = 0;
  vnet_buffer (b)->l4_hdr_offset = (u8 *) th - b->data;
  b->flags |= VNET_BUFFER_F_L4_HDR_OFFSET_VALID;
  return th;
}

/**
 * Push TCP header to buffer
 *
 * @param b - buffer to write the header to
 * @param sp_net - source port net order
 * @param dp_net - destination port net order
 * @param seq - sequence number host order
 * @param ack - ack number host order
 * @param tcp_hdr_opts_len - header and options length in bytes
 * @param flags - header flags
 * @param wnd - window size
 *
 * @return - pointer to start of TCP header
 */
always_inline void *
vlib_buffer_push_tcp (vlib_buffer_t * b, u16 sp_net, u16 dp_net, u32 seq,
		      u32 ack, u8 tcp_hdr_opts_len, u8 flags, u16 wnd)
{
  return vlib_buffer_push_tcp_net_order (b, sp_net, dp_net,
					 clib_host_to_net_u32 (seq),
					 clib_host_to_net_u32 (ack),
					 tcp_hdr_opts_len, flags,
					 clib_host_to_net_u16 (wnd));
}

#endif /* _vnet_tcp_h_ */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
