/*
 * ethernet/arp.c: IP v4 ARP node
 *
 * Copyright (c) 2010 Cisco and/or its affiliates.
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

#include <vnet/ip/ip.h>
#include <vnet/ip/ip_neighbor.h>
#include <vnet/ip/ip6.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/ethernet/arp.h>
#include <vnet/l2/l2_input.h>
#include <vppinfra/mhash.h>
#include <vnet/fib/ip4_fib.h>
#include <vnet/fib/fib_entry_src.h>
#include <vnet/adj/adj_nbr.h>
#include <vnet/adj/adj_mcast.h>
#include <vnet/mpls/mpls.h>
#include <vnet/l2/feat_bitmap.h>

#include <vlibmemory/api.h>

/**
 * @file
 * @brief IPv4 ARP.
 *
 * This file contains code to manage the IPv4 ARP tables (IP Address
 * to MAC Address lookup).
 */


/**
 * @brief Per-interface ARP configuration and state
 */
typedef struct ethernet_arp_interface_t_
{
  /**
   * Hash table of ARP entries.
   * Since this hash table is per-interface, the key is only the IPv4 address.
   */
  uword *arp_entries;
  /**
   * Is ARP enabled on this interface
   */
  u32 enabled;
  /**
   * Is Proxy ARP enabled on this interface
   */
  u32 proxy_enabled;
} ethernet_arp_interface_t;

typedef struct
{
  ip4_address_t lo_addr;
  ip4_address_t hi_addr;
  u32 fib_index;
} ethernet_proxy_arp_t;

typedef struct
{
  u32 next_index;
  uword node_index;
  uword type_opaque;
  uword data;
  /* Used for arp event notification only */
  arp_change_event_cb_t data_callback;
  u32 pid;
} pending_resolution_t;

typedef struct
{
  /* Hash tables mapping name to opcode. */
  uword *opcode_by_name;

  /* lite beer "glean" adjacency handling */
  uword *pending_resolutions_by_address;
  pending_resolution_t *pending_resolutions;

  /* Mac address change notification */
  uword *mac_changes_by_address;
  pending_resolution_t *mac_changes;

  ethernet_arp_ip4_entry_t *ip4_entry_pool;

  /* ARP attack mitigation */
  u32 arp_delete_rotor;
  u32 limit_arp_cache_size;

  /** Per interface state */
  ethernet_arp_interface_t *ethernet_arp_by_sw_if_index;

  /* Proxy arp vector */
  ethernet_proxy_arp_t *proxy_arps;

  uword wc_ip4_arp_publisher_node;
  uword wc_ip4_arp_publisher_et;

  /* ARP feature arc index */
  u8 feature_arc_index;
} ethernet_arp_main_t;

static ethernet_arp_main_t ethernet_arp_main;

typedef struct
{
  u32 sw_if_index;
  ip4_address_t ip4;
  mac_address_t mac;
  ip_neighbor_flags_t nbr_flags;
  u32 flags;
#define ETHERNET_ARP_ARGS_REMOVE (1<<0)
#define ETHERNET_ARP_ARGS_FLUSH  (1<<1)
#define ETHERNET_ARP_ARGS_POPULATE  (1<<2)
#define ETHERNET_ARP_ARGS_WC_PUB  (1<<3)
} vnet_arp_set_ip4_over_ethernet_rpc_args_t;

static const u8 vrrp_prefix[] = { 0x00, 0x00, 0x5E, 0x00, 0x01 };

/* Node index for send_garp_na_process */
u32 send_garp_na_process_node_index;

static void
set_ip4_over_ethernet_rpc_callback (vnet_arp_set_ip4_over_ethernet_rpc_args_t
				    * a);

static u8 *
format_ethernet_arp_hardware_type (u8 * s, va_list * va)
{
  ethernet_arp_hardware_type_t h = va_arg (*va, ethernet_arp_hardware_type_t);
  char *t = 0;
  switch (h)
    {
#define _(n,f) case n: t = #f; break;
      foreach_ethernet_arp_hardware_type;
#undef _

    default:
      return format (s, "unknown 0x%x", h);
    }

  return format (s, "%s", t);
}

static u8 *
format_ethernet_arp_opcode (u8 * s, va_list * va)
{
  ethernet_arp_opcode_t o = va_arg (*va, ethernet_arp_opcode_t);
  char *t = 0;
  switch (o)
    {
#define _(f) case ETHERNET_ARP_OPCODE_##f: t = #f; break;
      foreach_ethernet_arp_opcode;
#undef _

    default:
      return format (s, "unknown 0x%x", o);
    }

  return format (s, "%s", t);
}

static uword
unformat_ethernet_arp_opcode_host_byte_order (unformat_input_t * input,
					      va_list * args)
{
  int *result = va_arg (*args, int *);
  ethernet_arp_main_t *am = &ethernet_arp_main;
  int x, i;

  /* Numeric opcode. */
  if (unformat (input, "0x%x", &x) || unformat (input, "%d", &x))
    {
      if (x >= (1 << 16))
	return 0;
      *result = x;
      return 1;
    }

  /* Named type. */
  if (unformat_user (input, unformat_vlib_number_by_name,
		     am->opcode_by_name, &i))
    {
      *result = i;
      return 1;
    }

  return 0;
}

static uword
unformat_ethernet_arp_opcode_net_byte_order (unformat_input_t * input,
					     va_list * args)
{
  int *result = va_arg (*args, int *);
  if (!unformat_user
      (input, unformat_ethernet_arp_opcode_host_byte_order, result))
    return 0;

  *result = clib_host_to_net_u16 ((u16) * result);
  return 1;
}

static u8 *
format_ethernet_arp_header (u8 * s, va_list * va)
{
  ethernet_arp_header_t *a = va_arg (*va, ethernet_arp_header_t *);
  u32 max_header_bytes = va_arg (*va, u32);
  u32 indent;
  u16 l2_type, l3_type;

  if (max_header_bytes != 0 && sizeof (a[0]) > max_header_bytes)
    return format (s, "ARP header truncated");

  l2_type = clib_net_to_host_u16 (a->l2_type);
  l3_type = clib_net_to_host_u16 (a->l3_type);

  indent = format_get_indent (s);

  s = format (s, "%U, type %U/%U, address size %d/%d",
	      format_ethernet_arp_opcode, clib_net_to_host_u16 (a->opcode),
	      format_ethernet_arp_hardware_type, l2_type,
	      format_ethernet_type, l3_type,
	      a->n_l2_address_bytes, a->n_l3_address_bytes);

  if (l2_type == ETHERNET_ARP_HARDWARE_TYPE_ethernet
      && l3_type == ETHERNET_TYPE_IP4)
    {
      s = format (s, "\n%U%U/%U -> %U/%U",
		  format_white_space, indent,
		  format_mac_address_t, &a->ip4_over_ethernet[0].mac,
		  format_ip4_address, &a->ip4_over_ethernet[0].ip4,
		  format_mac_address_t, &a->ip4_over_ethernet[1].mac,
		  format_ip4_address, &a->ip4_over_ethernet[1].ip4);
    }
  else
    {
      uword n2 = a->n_l2_address_bytes;
      uword n3 = a->n_l3_address_bytes;
      s = format (s, "\n%U%U/%U -> %U/%U",
		  format_white_space, indent,
		  format_hex_bytes, a->data + 0 * n2 + 0 * n3, n2,
		  format_hex_bytes, a->data + 1 * n2 + 0 * n3, n3,
		  format_hex_bytes, a->data + 1 * n2 + 1 * n3, n2,
		  format_hex_bytes, a->data + 2 * n2 + 1 * n3, n3);
    }

  return s;
}

u8 *
format_ethernet_arp_ip4_entry (u8 * s, va_list * va)
{
  vnet_main_t *vnm = va_arg (*va, vnet_main_t *);
  ethernet_arp_ip4_entry_t *e = va_arg (*va, ethernet_arp_ip4_entry_t *);
  vnet_sw_interface_t *si;

  if (!e)
    return format (s, "%=12s%=16s%=6s%=20s%=24s", "Time", "IP4",
		   "Flags", "Ethernet", "Interface");

  si = vnet_get_sw_interface (vnm, e->sw_if_index);

  return format (s, "%=12U%=16U%=6U%=20U%U",
		 format_vlib_time, vnm->vlib_main, e->time_last_updated,
		 format_ip4_address, &e->ip4_address,
		 format_ip_neighbor_flags, e->flags,
		 format_mac_address_t, &e->mac,
		 format_vnet_sw_interface_name, vnm, si);
}

typedef struct
{
  u8 packet_data[64];
} ethernet_arp_input_trace_t;

static u8 *
format_ethernet_arp_input_trace (u8 * s, va_list * va)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*va, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*va, vlib_node_t *);
  ethernet_arp_input_trace_t *t = va_arg (*va, ethernet_arp_input_trace_t *);

  s = format (s, "%U",
	      format_ethernet_arp_header,
	      t->packet_data, sizeof (t->packet_data));

  return s;
}

static u8 *
format_arp_term_input_trace (u8 * s, va_list * va)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*va, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*va, vlib_node_t *);
  ethernet_arp_input_trace_t *t = va_arg (*va, ethernet_arp_input_trace_t *);

  /* arp-term trace data saved is either arp or ip6/icmp6 packet:
     - for arp, the 1st 16-bit field is hw type of value of 0x0001.
     - for ip6, the first nibble has value of 6. */
  s = format (s, "%U", t->packet_data[0] == 0 ?
	      format_ethernet_arp_header : format_ip6_header,
	      t->packet_data, sizeof (t->packet_data));

  return s;
}

static void
arp_nbr_probe (ip_adjacency_t * adj)
{
  vnet_main_t *vnm = vnet_get_main ();
  ip4_main_t *im = &ip4_main;
  ip_interface_address_t *ia;
  ethernet_arp_header_t *h;
  vnet_hw_interface_t *hi;
  vnet_sw_interface_t *si;
  ip4_address_t *src;
  vlib_buffer_t *b;
  vlib_main_t *vm;
  u32 bi = 0;

  vm = vlib_get_main ();

  si = vnet_get_sw_interface (vnm, adj->rewrite_header.sw_if_index);

  if (!(si->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP))
    {
      return;
    }

  src =
    ip4_interface_address_matching_destination (im,
						&adj->sub_type.nbr.next_hop.
						ip4,
						adj->rewrite_header.
						sw_if_index, &ia);
  if (!src)
    {
      return;
    }

  h =
    vlib_packet_template_get_packet (vm, &im->ip4_arp_request_packet_template,
				     &bi);
  if (!h)
    return;

  hi = vnet_get_sup_hw_interface (vnm, adj->rewrite_header.sw_if_index);

  mac_address_from_bytes (&h->ip4_over_ethernet[0].mac, hi->hw_address);

  h->ip4_over_ethernet[0].ip4 = src[0];
  h->ip4_over_ethernet[1].ip4 = adj->sub_type.nbr.next_hop.ip4;

  b = vlib_get_buffer (vm, bi);
  vnet_buffer (b)->sw_if_index[VLIB_RX] =
    vnet_buffer (b)->sw_if_index[VLIB_TX] = adj->rewrite_header.sw_if_index;

  /* Add encapsulation string for software interface (e.g. ethernet header). */
  vnet_rewrite_one_header (adj[0], h, sizeof (ethernet_header_t));
  vlib_buffer_advance (b, -adj->rewrite_header.data_bytes);

  {
    vlib_frame_t *f = vlib_get_frame_to_node (vm, hi->output_node_index);
    u32 *to_next = vlib_frame_vector_args (f);
    to_next[0] = bi;
    f->n_vectors = 1;
    vlib_put_frame_to_node (vm, hi->output_node_index, f);
  }
}

static void
arp_mk_complete (adj_index_t ai, ethernet_arp_ip4_entry_t * e)
{
  adj_nbr_update_rewrite
    (ai, ADJ_NBR_REWRITE_FLAG_COMPLETE,
     ethernet_build_rewrite (vnet_get_main (),
			     e->sw_if_index,
			     adj_get_link_type (ai), &e->mac));
}

static void
arp_mk_incomplete (adj_index_t ai)
{
  ip_adjacency_t *adj = adj_get (ai);

  adj_nbr_update_rewrite
    (ai,
     ADJ_NBR_REWRITE_FLAG_INCOMPLETE,
     ethernet_build_rewrite (vnet_get_main (),
			     adj->rewrite_header.sw_if_index,
			     VNET_LINK_ARP,
			     VNET_REWRITE_FOR_SW_INTERFACE_ADDRESS_BROADCAST));
}

static ethernet_arp_ip4_entry_t *
arp_entry_find (ethernet_arp_interface_t * eai, const ip4_address_t * addr)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e = NULL;
  uword *p;

  if (NULL != eai->arp_entries)
    {
      p = hash_get (eai->arp_entries, addr->as_u32);
      if (!p)
	return (NULL);

      e = pool_elt_at_index (am->ip4_entry_pool, p[0]);
    }

  return (e);
}

static adj_walk_rc_t
arp_mk_complete_walk (adj_index_t ai, void *ctx)
{
  ethernet_arp_ip4_entry_t *e = ctx;

  arp_mk_complete (ai, e);

  return (ADJ_WALK_RC_CONTINUE);
}

static adj_walk_rc_t
arp_mk_incomplete_walk (adj_index_t ai, void *ctx)
{
  arp_mk_incomplete (ai);

  return (ADJ_WALK_RC_CONTINUE);
}

static int
arp_is_enabled (ethernet_arp_main_t * am, u32 sw_if_index)
{
  if (vec_len (am->ethernet_arp_by_sw_if_index) <= sw_if_index)
    return 0;

  return (am->ethernet_arp_by_sw_if_index[sw_if_index].enabled);
}

static void
arp_enable (ethernet_arp_main_t * am, u32 sw_if_index)
{
  if (arp_is_enabled (am, sw_if_index))
    return;

  vec_validate (am->ethernet_arp_by_sw_if_index, sw_if_index);

  am->ethernet_arp_by_sw_if_index[sw_if_index].enabled = 1;

  vnet_feature_enable_disable ("arp", "arp-reply", sw_if_index, 1, NULL, 0);
  vnet_feature_enable_disable ("arp", "arp-disabled", sw_if_index, 0, NULL,
			       0);
}

static int
vnet_arp_flush_ip4_over_ethernet_internal (vnet_main_t * vnm,
					   vnet_arp_set_ip4_over_ethernet_rpc_args_t
					   * args);

static void
arp_disable (ethernet_arp_main_t * am, u32 sw_if_index)
{
  ethernet_arp_interface_t *eai;
  ethernet_arp_ip4_entry_t *e;
  u32 i, *to_delete = 0;
  hash_pair_t *pair;

  if (!arp_is_enabled (am, sw_if_index))
    return;

  vnet_feature_enable_disable ("arp", "arp-disabled", sw_if_index, 1, NULL,
			       0);
  vnet_feature_enable_disable ("arp", "arp-reply", sw_if_index, 0, NULL, 0);

  eai = &am->ethernet_arp_by_sw_if_index[sw_if_index];


  /* *INDENT-OFF* */
  hash_foreach_pair (pair, eai->arp_entries,
  ({
    e = pool_elt_at_index(am->ip4_entry_pool,
                          pair->value[0]);
    vec_add1 (to_delete, e - am->ip4_entry_pool);
  }));
  /* *INDENT-ON* */

  for (i = 0; i < vec_len (to_delete); i++)
    {
      e = pool_elt_at_index (am->ip4_entry_pool, to_delete[i]);

      vnet_arp_set_ip4_over_ethernet_rpc_args_t delme = {
	.ip4.as_u32 = e->ip4_address.as_u32,
	.sw_if_index = e->sw_if_index,
	.flags = ETHERNET_ARP_ARGS_FLUSH,
      };
      mac_address_copy (&delme.mac, &e->mac);

      vnet_arp_flush_ip4_over_ethernet_internal (vnet_get_main (), &delme);
    }

  vec_free (to_delete);

  eai->enabled = 0;
}

void
arp_update_adjacency (vnet_main_t * vnm, u32 sw_if_index, u32 ai)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_interface_t *arp_int;
  ethernet_arp_ip4_entry_t *e;
  ip_adjacency_t *adj;

  adj = adj_get (ai);

  arp_enable (am, sw_if_index);
  arp_int = &am->ethernet_arp_by_sw_if_index[sw_if_index];
  e = arp_entry_find (arp_int, &adj->sub_type.nbr.next_hop.ip4);

  switch (adj->lookup_next_index)
    {
    case IP_LOOKUP_NEXT_GLEAN:
      adj_glean_update_rewrite (ai);
      break;
    case IP_LOOKUP_NEXT_ARP:
      if (NULL != e)
	{
	  adj_nbr_walk_nh4 (sw_if_index,
			    &e->ip4_address, arp_mk_complete_walk, e);
	}
      else
	{
	  /*
	   * no matching ARP entry.
	   * construct the rewrite required to for an ARP packet, and stick
	   * that in the adj's pipe to smoke.
	   */
	  adj_nbr_update_rewrite
	    (ai,
	     ADJ_NBR_REWRITE_FLAG_INCOMPLETE,
	     ethernet_build_rewrite
	     (vnm,
	      sw_if_index,
	      VNET_LINK_ARP,
	      VNET_REWRITE_FOR_SW_INTERFACE_ADDRESS_BROADCAST));

	  /*
	   * since the FIB has added this adj for a route, it makes sense it
	   * may want to forward traffic sometime soon. Let's send a
	   * speculative ARP. just one. If we were to do periodically that
	   * wouldn't be bad either, but that's more code than i'm prepared to
	   * write at this time for relatively little reward.
	   */
	  arp_nbr_probe (adj);
	}
      break;
    case IP_LOOKUP_NEXT_BCAST:
      adj_nbr_update_rewrite (ai,
			      ADJ_NBR_REWRITE_FLAG_COMPLETE,
			      ethernet_build_rewrite
			      (vnm,
			       sw_if_index,
			       VNET_LINK_IP4,
			       VNET_REWRITE_FOR_SW_INTERFACE_ADDRESS_BROADCAST));
      break;
    case IP_LOOKUP_NEXT_MCAST:
      {
	/*
	 * Construct a partial rewrite from the known ethernet mcast dest MAC
	 */
	u8 *rewrite;
	u8 offset;

	rewrite = ethernet_build_rewrite (vnm,
					  sw_if_index,
					  adj->ia_link,
					  ethernet_ip4_mcast_dst_addr ());
	offset = vec_len (rewrite) - 2;

	/*
	 * Complete the remaining fields of the adj's rewrite to direct the
	 * complete of the rewrite at switch time by copying in the IP
	 * dst address's bytes.
	 * Offset is 2 bytes into the MAC destination address.
	 */
	adj_mcast_update_rewrite (ai, rewrite, offset);

	break;
      }
    case IP_LOOKUP_NEXT_DROP:
    case IP_LOOKUP_NEXT_PUNT:
    case IP_LOOKUP_NEXT_LOCAL:
    case IP_LOOKUP_NEXT_REWRITE:
    case IP_LOOKUP_NEXT_MCAST_MIDCHAIN:
    case IP_LOOKUP_NEXT_MIDCHAIN:
    case IP_LOOKUP_NEXT_ICMP_ERROR:
    case IP_LOOKUP_N_NEXT:
      ASSERT (0);
      break;
    }
}

static void
arp_adj_fib_add (ethernet_arp_ip4_entry_t * e, u32 fib_index)
{
  fib_prefix_t pfx = {
    .fp_len = 32,
    .fp_proto = FIB_PROTOCOL_IP4,
    .fp_addr.ip4 = e->ip4_address,
  };

  e->fib_entry_index =
    fib_table_entry_path_add (fib_index, &pfx, FIB_SOURCE_ADJ,
			      FIB_ENTRY_FLAG_ATTACHED,
			      DPO_PROTO_IP4, &pfx.fp_addr,
			      e->sw_if_index, ~0, 1, NULL,
			      FIB_ROUTE_PATH_FLAG_NONE);
  fib_table_lock (fib_index, FIB_PROTOCOL_IP4, FIB_SOURCE_ADJ);
}

static void
arp_adj_fib_remove (ethernet_arp_ip4_entry_t * e, u32 fib_index)
{
  if (FIB_NODE_INDEX_INVALID != e->fib_entry_index)
    {
      fib_prefix_t pfx = {
	.fp_len = 32,
	.fp_proto = FIB_PROTOCOL_IP4,
	.fp_addr.ip4 = e->ip4_address,
      };
      u32 fib_index;

      fib_index = ip4_fib_table_get_index_for_sw_if_index (e->sw_if_index);

      fib_table_entry_path_remove (fib_index, &pfx,
				   FIB_SOURCE_ADJ,
				   DPO_PROTO_IP4,
				   &pfx.fp_addr,
				   e->sw_if_index, ~0, 1,
				   FIB_ROUTE_PATH_FLAG_NONE);
      fib_table_unlock (fib_index, FIB_PROTOCOL_IP4, FIB_SOURCE_ADJ);
    }
}

static ethernet_arp_ip4_entry_t *
force_reuse_arp_entry (void)
{
  ethernet_arp_ip4_entry_t *e;
  ethernet_arp_main_t *am = &ethernet_arp_main;
  u32 count = 0;
  u32 index = pool_next_index (am->ip4_entry_pool, am->arp_delete_rotor);
  if (index == ~0)		/* Try again from elt 0 */
    index = pool_next_index (am->ip4_entry_pool, index);

  /* Find a non-static random entry to free up for reuse */
  do
    {
      if ((count++ == 100) || (index == ~0))
	return NULL;		/* give up after 100 entries */
      e = pool_elt_at_index (am->ip4_entry_pool, index);
      am->arp_delete_rotor = index;
      index = pool_next_index (am->ip4_entry_pool, index);
    }
  while (e->flags & IP_NEIGHBOR_FLAG_STATIC);

  /* Remove ARP entry from its interface and update fib */
  hash_unset
    (am->ethernet_arp_by_sw_if_index[e->sw_if_index].arp_entries,
     e->ip4_address.as_u32);
  arp_adj_fib_remove
    (e, ip4_fib_table_get_index_for_sw_if_index (e->sw_if_index));
  adj_nbr_walk_nh4 (e->sw_if_index,
		    &e->ip4_address, arp_mk_incomplete_walk, e);
  return e;
}

static int
vnet_arp_set_ip4_over_ethernet_internal (vnet_main_t * vnm,
					 vnet_arp_set_ip4_over_ethernet_rpc_args_t
					 * args)
{
  ethernet_arp_ip4_entry_t *e = 0;
  ethernet_arp_main_t *am = &ethernet_arp_main;
  vlib_main_t *vm = vlib_get_main ();
  int make_new_arp_cache_entry = 1;
  uword *p;
  pending_resolution_t *pr, *mc;
  ethernet_arp_interface_t *arp_int;
  u32 sw_if_index = args->sw_if_index;

  arp_enable (am, sw_if_index);

  arp_int = &am->ethernet_arp_by_sw_if_index[sw_if_index];

  if (NULL != arp_int->arp_entries)
    {
      p = hash_get (arp_int->arp_entries, args->ip4.as_u32);
      if (p)
	{
	  e = pool_elt_at_index (am->ip4_entry_pool, p[0]);

	  /* Refuse to over-write static arp. */
	  if (!(args->nbr_flags & IP_NEIGHBOR_FLAG_STATIC) &&
	      (e->flags & IP_NEIGHBOR_FLAG_STATIC))
	    {
	      /* if MAC address match, still check to send event */
	      if (mac_address_equal (&e->mac, &args->mac))
		goto check_customers;
	      return -2;
	    }
	  make_new_arp_cache_entry = 0;
	}
    }

  if (make_new_arp_cache_entry)
    {
      if (am->limit_arp_cache_size &&
	  pool_elts (am->ip4_entry_pool) >= am->limit_arp_cache_size)
	{
	  e = force_reuse_arp_entry ();
	  if (NULL == e)
	    return -2;
	}
      else
	pool_get (am->ip4_entry_pool, e);

      if (NULL == arp_int->arp_entries)
	arp_int->arp_entries = hash_create (0, sizeof (u32));

      hash_set (arp_int->arp_entries, args->ip4.as_u32,
		e - am->ip4_entry_pool);

      e->sw_if_index = sw_if_index;
      e->ip4_address = args->ip4;
      e->fib_entry_index = FIB_NODE_INDEX_INVALID;
      mac_address_copy (&e->mac, &args->mac);

      if (!(args->nbr_flags & IP_NEIGHBOR_FLAG_NO_FIB_ENTRY))
	{
	  arp_adj_fib_add (e,
			   ip4_fib_table_get_index_for_sw_if_index
			   (e->sw_if_index));
	}
      else
	{
	  e->flags |= IP_NEIGHBOR_FLAG_NO_FIB_ENTRY;
	}
    }
  else
    {
      /*
       * prevent a DoS attack from the data-plane that
       * spams us with no-op updates to the MAC address
       */
      if (mac_address_equal (&e->mac, &args->mac))
	{
	  e->time_last_updated = vlib_time_now (vm);
	  goto check_customers;
	}

      /* Update ethernet address. */
      mac_address_copy (&e->mac, &args->mac);
    }

  /* Update time stamp and flags. */
  e->time_last_updated = vlib_time_now (vm);
  if (args->nbr_flags & IP_NEIGHBOR_FLAG_STATIC)
    {
      e->flags &= ~IP_NEIGHBOR_FLAG_DYNAMIC;
      e->flags |= IP_NEIGHBOR_FLAG_STATIC;
    }
  else
    {
      e->flags &= ~IP_NEIGHBOR_FLAG_STATIC;
      e->flags |= IP_NEIGHBOR_FLAG_DYNAMIC;
    }

  adj_nbr_walk_nh4 (sw_if_index, &e->ip4_address, arp_mk_complete_walk, e);

check_customers:
  /* Customer(s) waiting for this address to be resolved? */
  p = hash_get (am->pending_resolutions_by_address, args->ip4.as_u32);
  if (p)
    {
      u32 next_index;
      next_index = p[0];

      while (next_index != (u32) ~ 0)
	{
	  pr = pool_elt_at_index (am->pending_resolutions, next_index);
	  vlib_process_signal_event (vm, pr->node_index,
				     pr->type_opaque, pr->data);
	  next_index = pr->next_index;
	  pool_put (am->pending_resolutions, pr);
	}

      hash_unset (am->pending_resolutions_by_address, args->ip4.as_u32);
    }

  /* Customer(s) requesting ARP event for this address? */
  p = hash_get (am->mac_changes_by_address, args->ip4.as_u32);
  if (p)
    {
      u32 next_index;
      next_index = p[0];

      while (next_index != (u32) ~ 0)
	{
	  int rv = 1;
	  mc = pool_elt_at_index (am->mac_changes, next_index);

	  /* Call the user's data callback, return 1 to suppress dup events */
	  if (mc->data_callback)
	    rv = (mc->data_callback) (mc->data, &args->mac, sw_if_index, 0);

	  /*
	   * Signal the resolver process, as long as the user
	   * says they want to be notified
	   */
	  if (rv == 0)
	    vlib_process_signal_event (vm, mc->node_index,
				       mc->type_opaque, mc->data);
	  next_index = mc->next_index;
	}
    }

  return 0;
}

void
vnet_register_ip4_arp_resolution_event (vnet_main_t * vnm,
					void *address_arg,
					uword node_index,
					uword type_opaque, uword data)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ip4_address_t *address = address_arg;
  uword *p;
  pending_resolution_t *pr;

  pool_get (am->pending_resolutions, pr);

  pr->next_index = ~0;
  pr->node_index = node_index;
  pr->type_opaque = type_opaque;
  pr->data = data;
  pr->data_callback = 0;

  p = hash_get (am->pending_resolutions_by_address, address->as_u32);
  if (p)
    {
      /* Insert new resolution at the head of the list */
      pr->next_index = p[0];
      hash_unset (am->pending_resolutions_by_address, address->as_u32);
    }

  hash_set (am->pending_resolutions_by_address, address->as_u32,
	    pr - am->pending_resolutions);
}

int
vnet_add_del_ip4_arp_change_event (vnet_main_t * vnm,
				   arp_change_event_cb_t data_callback,
				   u32 pid,
				   void *address_arg,
				   uword node_index,
				   uword type_opaque, uword data, int is_add)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ip4_address_t *address = address_arg;

  /* Try to find an existing entry */
  u32 *first = (u32 *) hash_get (am->mac_changes_by_address, address->as_u32);
  u32 *p = first;
  pending_resolution_t *mc;
  while (p && *p != ~0)
    {
      mc = pool_elt_at_index (am->mac_changes, *p);
      if (mc->node_index == node_index && mc->type_opaque == type_opaque
	  && mc->pid == pid)
	break;
      p = &mc->next_index;
    }

  int found = p && *p != ~0;
  if (is_add)
    {
      if (found)
	return VNET_API_ERROR_ENTRY_ALREADY_EXISTS;

      pool_get (am->mac_changes, mc);
      /* *INDENT-OFF* */
      *mc = (pending_resolution_t)
      {
        .next_index = ~0,
        .node_index = node_index,
        .type_opaque = type_opaque,
        .data = data,
        .data_callback = data_callback,
        .pid = pid,
      };
      /* *INDENT-ON* */

      /* Insert new resolution at the end of the list */
      u32 new_idx = mc - am->mac_changes;
      if (p)
	p[0] = new_idx;
      else
	hash_set (am->mac_changes_by_address, address->as_u32, new_idx);
    }
  else
    {
      if (!found)
	return VNET_API_ERROR_NO_SUCH_ENTRY;

      /* Clients may need to clean up pool entries, too */
      if (data_callback)
	/* no new mac addrs */
	(data_callback) (mc->data, NULL, ~0, NULL);

      /* Remove the entry from the list and delete the entry */
      *p = mc->next_index;
      pool_put (am->mac_changes, mc);

      /* Remove from hash if we deleted the last entry */
      if (*p == ~0 && p == first)
	hash_unset (am->mac_changes_by_address, address->as_u32);
    }
  return 0;
}

/* Either we drop the packet or we send a reply to the sender. */
typedef enum
{
  ARP_REPLY_NEXT_DROP,
  ARP_REPLY_NEXT_REPLY_TX,
  ARP_REPLY_N_NEXT,
} arp_reply_next_t;

#define foreach_ethernet_arp_error					\
  _ (replies_sent, "ARP replies sent")					\
  _ (l2_type_not_ethernet, "L2 type not ethernet")			\
  _ (l3_type_not_ip4, "L3 type not IP4")				\
  _ (l3_src_address_not_local, "IP4 source address not local to subnet") \
  _ (l3_dst_address_not_local, "IP4 destination address not local to subnet") \
  _ (l3_dst_address_unset, "IP4 destination address is unset")          \
  _ (l3_src_address_is_local, "IP4 source address matches local interface") \
  _ (l3_src_address_learned, "ARP request IP4 source address learned")  \
  _ (replies_received, "ARP replies received")				\
  _ (opcode_not_request, "ARP opcode not request")                      \
  _ (proxy_arp_replies_sent, "Proxy ARP replies sent")			\
  _ (l2_address_mismatch, "ARP hw addr does not match L2 frame src addr") \
  _ (gratuitous_arp, "ARP probe or announcement dropped") \
  _ (interface_no_table, "Interface is not mapped to an IP table") \
  _ (interface_not_ip_enabled, "Interface is not IP enabled") \
  _ (unnumbered_mismatch, "RX interface is unnumbered to different subnet") \

typedef enum
{
#define _(sym,string) ETHERNET_ARP_ERROR_##sym,
  foreach_ethernet_arp_error
#undef _
    ETHERNET_ARP_N_ERROR,
} ethernet_arp_reply_error_t;

static int
arp_unnumbered (vlib_buffer_t * p0,
		u32 input_sw_if_index, u32 conn_sw_if_index)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *vim = &vnm->interface_main;
  vnet_sw_interface_t *si;

  /* verify that the input interface is unnumbered to the connected.
   * the connected interface is the interface on which the subnet is
   * configured */
  si = &vim->sw_interfaces[input_sw_if_index];

  if (!(si->flags & VNET_SW_INTERFACE_FLAG_UNNUMBERED &&
	(si->unnumbered_sw_if_index == conn_sw_if_index)))
    {
      /* the input interface is not unnumbered to the interface on which
       * the sub-net is configured that covers the ARP request.
       * So this is not the case for unnumbered.. */
      return 0;
    }

  return !0;
}

static u32
arp_learn (vnet_main_t * vnm,
	   ethernet_arp_main_t * am, u32 sw_if_index,
	   const ethernet_arp_ip4_over_ethernet_address_t * addr)
{
  vnet_arp_set_ip4_over_ethernet (vnm, sw_if_index, addr, 0);
  return (ETHERNET_ARP_ERROR_l3_src_address_learned);
}

typedef enum arp_input_next_t_
{
  ARP_INPUT_NEXT_DROP,
  ARP_INPUT_NEXT_DISABLED,
  ARP_INPUT_N_NEXT,
} arp_input_next_t;

static uword
arp_input (vlib_main_t * vm, vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  u32 n_left_from, next_index, *from, *to_next, n_left_to_next;
  ethernet_arp_main_t *am = &ethernet_arp_main;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  if (node->flags & VLIB_NODE_FLAG_TRACE)
    vlib_trace_frame_buffers_only (vm, node, from, frame->n_vectors,
				   /* stride */ 1,
				   sizeof (ethernet_arp_input_trace_t));

  while (n_left_from > 0)
    {
      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  const ethernet_arp_header_t *arp0;
	  arp_input_next_t next0;
	  vlib_buffer_t *p0;
	  u32 pi0, error0;

	  pi0 = to_next[0] = from[0];
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  p0 = vlib_get_buffer (vm, pi0);
	  arp0 = vlib_buffer_get_current (p0);

	  error0 = ETHERNET_ARP_ERROR_replies_sent;
	  next0 = ARP_INPUT_NEXT_DROP;

	  error0 =
	    (arp0->l2_type !=
	     clib_net_to_host_u16 (ETHERNET_ARP_HARDWARE_TYPE_ethernet) ?
	     ETHERNET_ARP_ERROR_l2_type_not_ethernet : error0);
	  error0 =
	    (arp0->l3_type !=
	     clib_net_to_host_u16 (ETHERNET_TYPE_IP4) ?
	     ETHERNET_ARP_ERROR_l3_type_not_ip4 : error0);
	  error0 =
	    (0 == arp0->ip4_over_ethernet[0].ip4.as_u32 ?
	     ETHERNET_ARP_ERROR_l3_dst_address_unset : error0);

	  if (ETHERNET_ARP_ERROR_replies_sent == error0)
	    {
	      next0 = ARP_INPUT_NEXT_DISABLED;
	      vnet_feature_arc_start (am->feature_arc_index,
				      vnet_buffer (p0)->sw_if_index[VLIB_RX],
				      &next0, p0);
	    }
	  else
	    p0->error = node->errors[error0];

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, pi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return frame->n_vectors;
}

typedef enum arp_disabled_next_t_
{
  ARP_DISABLED_NEXT_DROP,
  ARP_DISABLED_N_NEXT,
} arp_disabled_next_t;

#define foreach_arp_disabled_error					\
  _ (DISABLED, "ARP Disabled on this interface")                    \

typedef enum
{
#define _(sym,string) ARP_DISABLED_ERROR_##sym,
  foreach_arp_disabled_error
#undef _
    ARP_DISABLED_N_ERROR,
} arp_disabled_error_t;

static char *arp_disabled_error_strings[] = {
#define _(sym,string) string,
  foreach_arp_disabled_error
#undef _
};

static uword
arp_disabled (vlib_main_t * vm,
	      vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  u32 n_left_from, next_index, *from, *to_next, n_left_to_next;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  if (node->flags & VLIB_NODE_FLAG_TRACE)
    vlib_trace_frame_buffers_only (vm, node, from, frame->n_vectors,
				   /* stride */ 1,
				   sizeof (ethernet_arp_input_trace_t));

  while (n_left_from > 0)
    {
      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  arp_disabled_next_t next0 = ARP_DISABLED_NEXT_DROP;
	  vlib_buffer_t *p0;
	  u32 pi0, error0;

	  next0 = ARP_DISABLED_NEXT_DROP;
	  error0 = ARP_DISABLED_ERROR_DISABLED;

	  pi0 = to_next[0] = from[0];
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  p0 = vlib_get_buffer (vm, pi0);
	  p0->error = node->errors[error0];

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, pi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return frame->n_vectors;
}

static_always_inline u32
arp_mk_reply (vnet_main_t * vnm,
	      vlib_buffer_t * p0,
	      u32 sw_if_index0,
	      const ip4_address_t * if_addr0,
	      ethernet_arp_header_t * arp0, ethernet_header_t * eth_rx)
{
  vnet_hw_interface_t *hw_if0;
  u8 *rewrite0, rewrite0_len;
  ethernet_header_t *eth_tx;
  u32 next0;

  /* Send a reply.
     An adjacency to the sender is not always present,
     so we use the interface to build us a rewrite string
     which will contain all the necessary tags. */
  rewrite0 = ethernet_build_rewrite (vnm, sw_if_index0,
				     VNET_LINK_ARP, eth_rx->src_address);
  rewrite0_len = vec_len (rewrite0);

  /* Figure out how much to rewind current data from adjacency. */
  vlib_buffer_advance (p0, -rewrite0_len);
  eth_tx = vlib_buffer_get_current (p0);

  vnet_buffer (p0)->sw_if_index[VLIB_TX] = sw_if_index0;
  hw_if0 = vnet_get_sup_hw_interface (vnm, sw_if_index0);

  /* Send reply back through input interface */
  vnet_buffer (p0)->sw_if_index[VLIB_TX] = sw_if_index0;
  next0 = ARP_REPLY_NEXT_REPLY_TX;

  arp0->opcode = clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_reply);

  arp0->ip4_over_ethernet[1] = arp0->ip4_over_ethernet[0];

  mac_address_from_bytes (&arp0->ip4_over_ethernet[0].mac,
			  hw_if0->hw_address);
  clib_mem_unaligned (&arp0->ip4_over_ethernet[0].ip4.data_u32, u32) =
    if_addr0->data_u32;

  /* Hardware must be ethernet-like. */
  ASSERT (vec_len (hw_if0->hw_address) == 6);

  /* the rx nd tx ethernet headers wil overlap in the case
   * when we received a tagged VLAN=0 packet, but we are sending
   * back untagged */
  clib_memcpy_fast (eth_tx, rewrite0, vec_len (rewrite0));
  vec_free (rewrite0);

  return (next0);
}

enum arp_dst_fib_type
{
  ARP_DST_FIB_NONE,
  ARP_DST_FIB_ADJ,
  ARP_DST_FIB_CONN
};

/*
 * we're looking for FIB sources that indicate the destination
 * is attached. There may be interposed DPO prior to the one
 * we are looking for
 */
static enum arp_dst_fib_type
arp_dst_fib_check (const fib_node_index_t fei, fib_entry_flag_t * flags)
{
  const fib_entry_t *entry = fib_entry_get (fei);
  const fib_entry_src_t *entry_src;
  fib_source_t src;
  /* *INDENT-OFF* */
  FOR_EACH_SRC_ADDED(entry, entry_src, src,
  ({
    *flags = fib_entry_get_flags_for_source (fei, src);
    if (fib_entry_is_sourced (fei, FIB_SOURCE_ADJ))
        return ARP_DST_FIB_ADJ;
      else if (FIB_ENTRY_FLAG_CONNECTED & *flags)
        return ARP_DST_FIB_CONN;
  }))
  /* *INDENT-ON* */

  return ARP_DST_FIB_NONE;
}

static uword
arp_reply (vlib_main_t * vm, vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 n_left_from, next_index, *from, *to_next;
  u32 n_replies_sent = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  if (node->flags & VLIB_NODE_FLAG_TRACE)
    vlib_trace_frame_buffers_only (vm, node, from, frame->n_vectors,
				   /* stride */ 1,
				   sizeof (ethernet_arp_input_trace_t));

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  vlib_buffer_t *p0;
	  ethernet_arp_header_t *arp0;
	  ethernet_header_t *eth_rx;
	  const ip4_address_t *if_addr0;
	  u32 pi0, error0, next0, sw_if_index0, conn_sw_if_index0, fib_index0;
	  u8 dst_is_local0, is_vrrp_reply0;
	  fib_node_index_t dst_fei, src_fei;
	  const fib_prefix_t *pfx0;
	  fib_entry_flag_t src_flags, dst_flags;

	  pi0 = from[0];
	  to_next[0] = pi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  p0 = vlib_get_buffer (vm, pi0);
	  arp0 = vlib_buffer_get_current (p0);
	  /* Fill in ethernet header. */
	  eth_rx = ethernet_buffer_get_header (p0);

	  next0 = ARP_REPLY_NEXT_DROP;
	  error0 = ETHERNET_ARP_ERROR_replies_sent;
	  sw_if_index0 = vnet_buffer (p0)->sw_if_index[VLIB_RX];

	  /* Check that IP address is local and matches incoming interface. */
	  fib_index0 = ip4_fib_table_get_index_for_sw_if_index (sw_if_index0);
	  if (~0 == fib_index0)
	    {
	      error0 = ETHERNET_ARP_ERROR_interface_no_table;
	      goto drop;

	    }

	  {
	    /*
	     * we're looking for FIB entries that indicate the source
	     * is attached. There may be more specific non-attached
	     * routes that match the source, but these do not influence
	     * whether we respond to an ARP request, i.e. they do not
	     * influence whether we are the correct way for the sender
	     * to reach us, they only affect how we reach the sender.
	     */
	    fib_entry_t *src_fib_entry;
	    const fib_prefix_t *pfx;
	    fib_entry_src_t *src;
	    fib_source_t source;
	    int attached;
	    int mask;

	    mask = 32;
	    attached = 0;

	    do
	      {
		src_fei = ip4_fib_table_lookup (ip4_fib_get (fib_index0),
						&arp0->
						ip4_over_ethernet[0].ip4,
						mask);
		src_fib_entry = fib_entry_get (src_fei);

		/*
		 * It's possible that the source that provides the
		 * flags we need, or the flags we must not have,
		 * is not the best source, so check then all.
		 */
                /* *INDENT-OFF* */
                FOR_EACH_SRC_ADDED(src_fib_entry, src, source,
                ({
                  src_flags = fib_entry_get_flags_for_source (src_fei, source);

                  /* Reject requests/replies with our local interface
                     address. */
                  if (FIB_ENTRY_FLAG_LOCAL & src_flags)
                    {
                      error0 = ETHERNET_ARP_ERROR_l3_src_address_is_local;
                      /*
                       * When VPP has an interface whose address is also
                       * applied to a TAP interface on the host, then VPP's
                       * TAP interface will be unnumbered  to the 'real'
                       * interface and do proxy ARP from the host.
                       * The curious aspect of this setup is that ARP requests
                       * from the host will come from the VPP's own address.
                       * So don't drop immediately here, instead go see if this
                       * is a proxy ARP case.
                       */
                      goto next_feature;
                    }
                  /* A Source must also be local to subnet of matching
                   * interface address. */
                  if ((FIB_ENTRY_FLAG_ATTACHED & src_flags) ||
                      (FIB_ENTRY_FLAG_CONNECTED & src_flags))
                    {
                      attached = 1;
                      break;
                    }
                  /*
                   * else
                   *  The packet was sent from an address that is not
                   *  connected nor attached i.e. it is not from an
                   *  address that is covered by a link's sub-net,
                   *  nor is it a already learned host resp.
                   */
                }));
                /* *INDENT-ON* */

		/*
		 * shorter mask lookup for the next iteration.
		 */
		pfx = fib_entry_get_prefix (src_fei);
		mask = pfx->fp_len - 1;

		/*
		 * continue until we hit the default route or we find
		 * the attached we are looking for. The most likely
		 * outcome is we find the attached with the first source
		 * on the first lookup.
		 */
	      }
	    while (!attached &&
		   !fib_entry_is_sourced (src_fei, FIB_SOURCE_DEFAULT_ROUTE));

	    if (!attached)
	      {
		/*
		 * the matching route is a not attached, i.e. it was
		 * added as a result of routing, rather than interface/ARP
		 * configuration. If the matching route is not a host route
		 * (i.e. a /32)
		 */
		error0 = ETHERNET_ARP_ERROR_l3_src_address_not_local;
		goto drop;
	      }
	  }

	  dst_fei = ip4_fib_table_lookup (ip4_fib_get (fib_index0),
					  &arp0->ip4_over_ethernet[1].ip4,
					  32);
	  switch (arp_dst_fib_check (dst_fei, &dst_flags))
	    {
	    case ARP_DST_FIB_ADJ:
	      /*
	       * We matched an adj-fib on ths source subnet (a /32 previously
	       * added as a result of ARP). If this request is a gratuitous
	       * ARP, then learn from it.
	       * The check for matching an adj-fib, is to prevent hosts
	       * from spamming us with gratuitous ARPS that might otherwise
	       * blow our ARP cache
	       */
	      if (arp0->ip4_over_ethernet[0].ip4.as_u32 ==
		  arp0->ip4_over_ethernet[1].ip4.as_u32)
		error0 = arp_learn (vnm, am, sw_if_index0,
				    &arp0->ip4_over_ethernet[0]);
	      goto drop;
	    case ARP_DST_FIB_CONN:
	      /* destination is connected, continue to process */
	      break;
	    case ARP_DST_FIB_NONE:
	      /* destination is not connected, stop here */
	      error0 = ETHERNET_ARP_ERROR_l3_dst_address_not_local;
	      goto next_feature;
	    }

	  dst_is_local0 = (FIB_ENTRY_FLAG_LOCAL & dst_flags);
	  pfx0 = fib_entry_get_prefix (dst_fei);
	  if_addr0 = &pfx0->fp_addr.ip4;

	  is_vrrp_reply0 =
	    ((arp0->opcode ==
	      clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_reply))
	     &&
	     (!memcmp
	      (arp0->ip4_over_ethernet[0].mac.bytes, vrrp_prefix,
	       sizeof (vrrp_prefix))));

	  /* Trash ARP packets whose ARP-level source addresses do not
	     match their L2-frame-level source addresses, unless it's
	     a reply from a VRRP virtual router */
	  if (!ethernet_mac_address_equal
	      (eth_rx->src_address,
	       arp0->ip4_over_ethernet[0].mac.bytes) && !is_vrrp_reply0)
	    {
	      error0 = ETHERNET_ARP_ERROR_l2_address_mismatch;
	      goto drop;
	    }

	  /* Learn or update sender's mapping only for replies to addresses
	   * that are local to the subnet */
	  if (arp0->opcode ==
	      clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_reply))
	    {
	      if (dst_is_local0)
		error0 = arp_learn (vnm, am, sw_if_index0,
				    &arp0->ip4_over_ethernet[0]);
	      else
		/* a reply for a non-local destination could be a GARP.
		 * GARPs for hosts we know were handled above, so this one
		 * we drop */
		error0 = ETHERNET_ARP_ERROR_l3_dst_address_not_local;

	      goto next_feature;
	    }
	  else if (arp0->opcode ==
		   clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_request) &&
		   (dst_is_local0 == 0))
	    {
	      goto next_feature;
	    }

	  /* Honor unnumbered interface, if any */
	  conn_sw_if_index0 = fib_entry_get_resolving_interface (dst_fei);
	  if (sw_if_index0 != conn_sw_if_index0 ||
	      sw_if_index0 != fib_entry_get_resolving_interface (src_fei))
	    {
	      /*
	       * The interface the ARP is sent to or was received on is not the
	       * interface on which the covering prefix is configured.
	       * Maybe this is a case for unnumbered.
	       */
	      if (!arp_unnumbered (p0, sw_if_index0, conn_sw_if_index0))
		{
		  error0 = ETHERNET_ARP_ERROR_unnumbered_mismatch;
		  goto drop;
		}
	    }
	  if (arp0->ip4_over_ethernet[0].ip4.as_u32 ==
	      arp0->ip4_over_ethernet[1].ip4.as_u32)
	    {
	      error0 = ETHERNET_ARP_ERROR_gratuitous_arp;
	      goto drop;
	    }

	  next0 = arp_mk_reply (vnm, p0, sw_if_index0,
				if_addr0, arp0, eth_rx);

	  /* We are going to reply to this request, so, in the absence of
	     errors, learn the sender */
	  if (!error0)
	    error0 = arp_learn (vnm, am, sw_if_index0,
				&arp0->ip4_over_ethernet[1]);

	  n_replies_sent += 1;
	  goto enqueue;

	next_feature:
	  vnet_feature_next (&next0, p0);
	  goto enqueue;

	drop:
	  p0->error = node->errors[error0];

	enqueue:
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, pi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_error_count (vm, node->node_index,
		    ETHERNET_ARP_ERROR_replies_sent, n_replies_sent);

  return frame->n_vectors;
}

static uword
arp_proxy (vlib_main_t * vm, vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 n_left_from, next_index, *from, *to_next;
  u32 n_arp_replies_sent = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  if (node->flags & VLIB_NODE_FLAG_TRACE)
    vlib_trace_frame_buffers_only (vm, node, from, frame->n_vectors,
				   /* stride */ 1,
				   sizeof (ethernet_arp_input_trace_t));

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  vlib_buffer_t *p0;
	  ethernet_arp_header_t *arp0;
	  ethernet_header_t *eth_rx;
	  ip4_address_t proxy_src;
	  u32 pi0, error0, next0, sw_if_index0, fib_index0;
	  u8 is_request0;
	  ethernet_proxy_arp_t *pa;

	  pi0 = from[0];
	  to_next[0] = pi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  p0 = vlib_get_buffer (vm, pi0);
	  arp0 = vlib_buffer_get_current (p0);
	  /* Fill in ethernet header. */
	  eth_rx = ethernet_buffer_get_header (p0);

	  is_request0 = arp0->opcode
	    == clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_request);

	  error0 = ETHERNET_ARP_ERROR_replies_sent;
	  sw_if_index0 = vnet_buffer (p0)->sw_if_index[VLIB_RX];
	  next0 = ARP_REPLY_NEXT_DROP;

	  fib_index0 = ip4_fib_table_get_index_for_sw_if_index (sw_if_index0);
	  if (~0 == fib_index0)
	    {
	      error0 = ETHERNET_ARP_ERROR_interface_no_table;
	    }

	  if (0 == error0 && is_request0)
	    {
	      u32 this_addr = clib_net_to_host_u32
		(arp0->ip4_over_ethernet[1].ip4.as_u32);

	      vec_foreach (pa, am->proxy_arps)
	      {
		u32 lo_addr = clib_net_to_host_u32 (pa->lo_addr.as_u32);
		u32 hi_addr = clib_net_to_host_u32 (pa->hi_addr.as_u32);

		/* an ARP request hit in the proxy-arp table? */
		if ((this_addr >= lo_addr && this_addr <= hi_addr) &&
		    (fib_index0 == pa->fib_index))
		  {
		    proxy_src.as_u32 =
		      arp0->ip4_over_ethernet[1].ip4.data_u32;

		    /*
		     * change the interface address to the proxied
		     */
		    n_arp_replies_sent++;

		    next0 =
		      arp_mk_reply (vnm, p0, sw_if_index0, &proxy_src, arp0,
				    eth_rx);
		  }
	      }
	    }
	  else
	    {
	      p0->error = node->errors[error0];
	    }

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, pi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_error_count (vm, node->node_index,
		    ETHERNET_ARP_ERROR_replies_sent, n_arp_replies_sent);

  return frame->n_vectors;
}

static char *ethernet_arp_error_strings[] = {
#define _(sym,string) string,
  foreach_ethernet_arp_error
#undef _
};

/* *INDENT-OFF* */

VLIB_REGISTER_NODE (arp_input_node, static) =
{
  .function = arp_input,
  .name = "arp-input",
  .vector_size = sizeof (u32),
  .n_errors = ETHERNET_ARP_N_ERROR,
  .error_strings = ethernet_arp_error_strings,
  .n_next_nodes = ARP_INPUT_N_NEXT,
  .next_nodes = {
    [ARP_INPUT_NEXT_DROP] = "error-drop",
    [ARP_INPUT_NEXT_DISABLED] = "arp-disabled",
  },
  .format_buffer = format_ethernet_arp_header,
  .format_trace = format_ethernet_arp_input_trace,
};

VLIB_REGISTER_NODE (arp_disabled_node, static) =
{
  .function = arp_disabled,
  .name = "arp-disabled",
  .vector_size = sizeof (u32),
  .n_errors = ARP_DISABLED_N_ERROR,
  .error_strings = arp_disabled_error_strings,
  .n_next_nodes = ARP_DISABLED_N_NEXT,
  .next_nodes = {
    [ARP_INPUT_NEXT_DROP] = "error-drop",
  },
  .format_buffer = format_ethernet_arp_header,
  .format_trace = format_ethernet_arp_input_trace,
};

VLIB_REGISTER_NODE (arp_reply_node, static) =
{
  .function = arp_reply,
  .name = "arp-reply",
  .vector_size = sizeof (u32),
  .n_errors = ETHERNET_ARP_N_ERROR,
  .error_strings = ethernet_arp_error_strings,
  .n_next_nodes = ARP_REPLY_N_NEXT,
  .next_nodes = {
    [ARP_REPLY_NEXT_DROP] = "error-drop",
    [ARP_REPLY_NEXT_REPLY_TX] = "interface-output",
  },
  .format_buffer = format_ethernet_arp_header,
  .format_trace = format_ethernet_arp_input_trace,
};

VLIB_REGISTER_NODE (arp_proxy_node, static) =
{
  .function = arp_proxy,
  .name = "arp-proxy",
  .vector_size = sizeof (u32),
  .n_errors = ETHERNET_ARP_N_ERROR,
  .error_strings = ethernet_arp_error_strings,
  .n_next_nodes = ARP_REPLY_N_NEXT,
  .next_nodes = {
    [ARP_REPLY_NEXT_DROP] = "error-drop",
    [ARP_REPLY_NEXT_REPLY_TX] = "interface-output",
  },
  .format_buffer = format_ethernet_arp_header,
  .format_trace = format_ethernet_arp_input_trace,
};

/* Built-in ARP rx feature path definition */
VNET_FEATURE_ARC_INIT (arp_feat, static) =
{
  .arc_name = "arp",
  .start_nodes = VNET_FEATURES ("arp-input"),
  .last_in_arc = "error-drop",
  .arc_index_ptr = &ethernet_arp_main.feature_arc_index,
};

VNET_FEATURE_INIT (arp_reply_feat_node, static) =
{
  .arc_name = "arp",
  .node_name = "arp-reply",
  .runs_before = VNET_FEATURES ("arp-disabled"),
};

VNET_FEATURE_INIT (arp_proxy_feat_node, static) =
{
  .arc_name = "arp",
  .node_name = "arp-proxy",
  .runs_after = VNET_FEATURES ("arp-reply"),
  .runs_before = VNET_FEATURES ("arp-disabled"),
};

VNET_FEATURE_INIT (arp_disabled_feat_node, static) =
{
  .arc_name = "arp",
  .node_name = "arp-disabled",
  .runs_before = VNET_FEATURES ("error-drop"),
};

VNET_FEATURE_INIT (arp_drop_feat_node, static) =
{
  .arc_name = "arp",
  .node_name = "error-drop",
  .runs_before = 0,	/* last feature */
};

/* *INDENT-ON* */

static int
ip4_arp_entry_sort (void *a1, void *a2)
{
  ethernet_arp_ip4_entry_t *e1 = a1;
  ethernet_arp_ip4_entry_t *e2 = a2;

  int cmp;
  vnet_main_t *vnm = vnet_get_main ();

  cmp = vnet_sw_interface_compare (vnm, e1->sw_if_index, e2->sw_if_index);
  if (!cmp)
    cmp = ip4_address_compare (&e1->ip4_address, &e2->ip4_address);
  return cmp;
}

ethernet_arp_ip4_entry_t *
ip4_neighbors_pool (void)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  return am->ip4_entry_pool;
}

ethernet_arp_ip4_entry_t *
ip4_neighbor_entries (u32 sw_if_index)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *n, *ns = 0;

  /* *INDENT-OFF* */
  pool_foreach (n, am->ip4_entry_pool, ({
    if (sw_if_index != ~0 && n->sw_if_index != sw_if_index)
      continue;
    vec_add1 (ns, n[0]);
  }));
  /* *INDENT-ON* */

  if (ns)
    vec_sort_with_function (ns, ip4_arp_entry_sort);
  return ns;
}

static clib_error_t *
show_ip4_arp (vlib_main_t * vm,
	      unformat_input_t * input, vlib_cli_command_t * cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e, *es;
  ethernet_proxy_arp_t *pa;
  clib_error_t *error = 0;
  u32 sw_if_index;

  /* Filter entries by interface if given. */
  sw_if_index = ~0;
  (void) unformat_user (input, unformat_vnet_sw_interface, vnm, &sw_if_index);

  es = ip4_neighbor_entries (sw_if_index);
  if (es)
    {
      vlib_cli_output (vm, "%U", format_ethernet_arp_ip4_entry, vnm, 0);
      vec_foreach (e, es)
      {
	vlib_cli_output (vm, "%U", format_ethernet_arp_ip4_entry, vnm, e);
      }
      vec_free (es);
    }

  if (vec_len (am->proxy_arps))
    {
      vlib_cli_output (vm, "Proxy arps enabled for:");
      vec_foreach (pa, am->proxy_arps)
      {
	vlib_cli_output (vm, "Fib_index %d   %U - %U ",
			 pa->fib_index,
			 format_ip4_address, &pa->lo_addr,
			 format_ip4_address, &pa->hi_addr);
      }
    }

  return error;
}

/*?
 * Display all the IPv4 ARP entries.
 *
 * @cliexpar
 * Example of how to display the IPv4 ARP table:
 * @cliexstart{show ip arp}
 *    Time      FIB        IP4       Flags      Ethernet              Interface
 *    346.3028   0       6.1.1.3            de:ad:be:ef:ba:be   GigabitEthernet2/0/0
 *   3077.4271   0       6.1.1.4       S    de:ad:be:ef:ff:ff   GigabitEthernet2/0/0
 *   2998.6409   1       6.2.2.3            de:ad:be:ef:00:01   GigabitEthernet2/0/0
 * Proxy arps enabled for:
 * Fib_index 0   6.0.0.1 - 6.0.0.11
 * @cliexend
 ?*/
/* *INDENT-OFF* */
VLIB_CLI_COMMAND (show_ip4_arp_command, static) = {
  .path = "show ip arp",
  .function = show_ip4_arp,
  .short_help = "show ip arp",
};
/* *INDENT-ON* */

typedef struct
{
  pg_edit_t l2_type, l3_type;
  pg_edit_t n_l2_address_bytes, n_l3_address_bytes;
  pg_edit_t opcode;
  struct
  {
    pg_edit_t mac;
    pg_edit_t ip4;
  } ip4_over_ethernet[2];
} pg_ethernet_arp_header_t;

static inline void
pg_ethernet_arp_header_init (pg_ethernet_arp_header_t * p)
{
  /* Initialize fields that are not bit fields in the IP header. */
#define _(f) pg_edit_init (&p->f, ethernet_arp_header_t, f);
  _(l2_type);
  _(l3_type);
  _(n_l2_address_bytes);
  _(n_l3_address_bytes);
  _(opcode);
  _(ip4_over_ethernet[0].mac);
  _(ip4_over_ethernet[0].ip4);
  _(ip4_over_ethernet[1].mac);
  _(ip4_over_ethernet[1].ip4);
#undef _
}

uword
unformat_pg_arp_header (unformat_input_t * input, va_list * args)
{
  pg_stream_t *s = va_arg (*args, pg_stream_t *);
  pg_ethernet_arp_header_t *p;
  u32 group_index;

  p = pg_create_edit_group (s, sizeof (p[0]), sizeof (ethernet_arp_header_t),
			    &group_index);
  pg_ethernet_arp_header_init (p);

  /* Defaults. */
  pg_edit_set_fixed (&p->l2_type, ETHERNET_ARP_HARDWARE_TYPE_ethernet);
  pg_edit_set_fixed (&p->l3_type, ETHERNET_TYPE_IP4);
  pg_edit_set_fixed (&p->n_l2_address_bytes, 6);
  pg_edit_set_fixed (&p->n_l3_address_bytes, 4);

  if (!unformat (input, "%U: %U/%U -> %U/%U",
		 unformat_pg_edit,
		 unformat_ethernet_arp_opcode_net_byte_order, &p->opcode,
		 unformat_pg_edit,
		 unformat_mac_address_t, &p->ip4_over_ethernet[0].mac,
		 unformat_pg_edit,
		 unformat_ip4_address, &p->ip4_over_ethernet[0].ip4,
		 unformat_pg_edit,
		 unformat_mac_address_t, &p->ip4_over_ethernet[1].mac,
		 unformat_pg_edit,
		 unformat_ip4_address, &p->ip4_over_ethernet[1].ip4))
    {
      /* Free up any edits we may have added. */
      pg_free_edit_group (s);
      return 0;
    }
  return 1;
}

clib_error_t *
ip4_set_arp_limit (u32 arp_limit)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;

  am->limit_arp_cache_size = arp_limit;
  return 0;
}

/**
 * @brief Control Plane hook to remove an ARP entry
 */
int
vnet_arp_unset_ip4_over_ethernet (vnet_main_t * vnm,
				  u32 sw_if_index,
				  const
				  ethernet_arp_ip4_over_ethernet_address_t *
				  a)
{
  vnet_arp_set_ip4_over_ethernet_rpc_args_t args = {
    .sw_if_index = sw_if_index,
    .flags = ETHERNET_ARP_ARGS_REMOVE,
    .ip4 = a->ip4,
    .mac = a->mac,
  };

  vl_api_rpc_call_main_thread (set_ip4_over_ethernet_rpc_callback,
			       (u8 *) & args, sizeof (args));
  return 0;
}

/**
 * @brief publish wildcard arp event
 * @param sw_if_index The interface on which the ARP entries are acted
 */
static int
vnet_arp_wc_publish (u32 sw_if_index,
		     const ethernet_arp_ip4_over_ethernet_address_t * a)
{
  vnet_arp_set_ip4_over_ethernet_rpc_args_t args = {
    .flags = ETHERNET_ARP_ARGS_WC_PUB,
    .sw_if_index = sw_if_index,
    .ip4 = a->ip4,
    .mac = a->mac,
  };

  vl_api_rpc_call_main_thread (set_ip4_over_ethernet_rpc_callback,
			       (u8 *) & args, sizeof (args));
  return 0;
}

static void
vnet_arp_wc_publish_internal (vnet_main_t * vnm,
			      vnet_arp_set_ip4_over_ethernet_rpc_args_t *
			      args)
{
  vlib_main_t *vm = vlib_get_main ();
  ethernet_arp_main_t *am = &ethernet_arp_main;
  uword ni = am->wc_ip4_arp_publisher_node;
  uword et = am->wc_ip4_arp_publisher_et;

  if (ni == (uword) ~ 0)
    return;
  wc_arp_report_t *r =
    vlib_process_signal_event_data (vm, ni, et, 1, sizeof *r);
  r->ip.as_u32 = args->ip4.as_u32;
  r->sw_if_index = args->sw_if_index;
  mac_address_copy (&r->mac, &args->mac);
}

void
wc_arp_set_publisher_node (uword node_index, uword event_type)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  am->wc_ip4_arp_publisher_node = node_index;
  am->wc_ip4_arp_publisher_et = event_type;
}

static void
arp_entry_free (ethernet_arp_interface_t * eai, ethernet_arp_ip4_entry_t * e);

static int
vnet_arp_flush_ip4_over_ethernet_internal (vnet_main_t * vnm,
					   vnet_arp_set_ip4_over_ethernet_rpc_args_t
					   * args)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e;
  ethernet_arp_interface_t *eai;

  if (vec_len (am->ethernet_arp_by_sw_if_index) <= args->sw_if_index)
    return 0;

  eai = &am->ethernet_arp_by_sw_if_index[args->sw_if_index];

  e = arp_entry_find (eai, &args->ip4);

  if (NULL != e)
    {
      adj_nbr_walk_nh4 (e->sw_if_index,
			&e->ip4_address, arp_mk_incomplete_walk, e);

      /*
       * The difference between flush and unset, is that an unset
       * means delete for static and dynamic entries. A flush
       * means delete only for dynamic. Flushing is what the DP
       * does in response to interface events. unset is only done
       * by the control plane.
       */
      if (e->flags & IP_NEIGHBOR_FLAG_STATIC)
	{
	  e->flags &= ~IP_NEIGHBOR_FLAG_DYNAMIC;
	}
      else if (e->flags & IP_NEIGHBOR_FLAG_DYNAMIC)
	{
	  arp_entry_free (eai, e);
	}
    }
  return (0);
}

/*
 * arp_add_del_interface_address
 *
 * callback when an interface address is added or deleted
 */
static void
arp_enable_disable_interface (ip4_main_t * im,
			      uword opaque, u32 sw_if_index, u32 is_enable)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;

  if (is_enable)
    arp_enable (am, sw_if_index);
  else
    arp_disable (am, sw_if_index);
}

/*
 * arp_add_del_interface_address
 *
 * callback when an interface address is added or deleted
 */
static void
arp_add_del_interface_address (ip4_main_t * im,
			       uword opaque,
			       u32 sw_if_index,
			       ip4_address_t * address,
			       u32 address_length,
			       u32 if_address_index, u32 is_del)
{
  /*
   * Flush the ARP cache of all entries covered by the address
   * that is being removed.
   */
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e;

  if (vec_len (am->ethernet_arp_by_sw_if_index) <= sw_if_index)
    return;

  if (is_del)
    {
      ethernet_arp_interface_t *eai;
      u32 i, *to_delete = 0;
      hash_pair_t *pair;

      eai = &am->ethernet_arp_by_sw_if_index[sw_if_index];

      /* *INDENT-OFF* */
      hash_foreach_pair (pair, eai->arp_entries,
      ({
	e = pool_elt_at_index(am->ip4_entry_pool,
			      pair->value[0]);
	if (ip4_destination_matches_route (im, &e->ip4_address,
					   address, address_length))
	  {
	    vec_add1 (to_delete, e - am->ip4_entry_pool);
	  }
      }));
      /* *INDENT-ON* */

      for (i = 0; i < vec_len (to_delete); i++)
	{
	  e = pool_elt_at_index (am->ip4_entry_pool, to_delete[i]);

	  vnet_arp_set_ip4_over_ethernet_rpc_args_t delme = {
	    .ip4.as_u32 = e->ip4_address.as_u32,
	    .sw_if_index = e->sw_if_index,
	    .flags = ETHERNET_ARP_ARGS_FLUSH,
	  };
	  mac_address_copy (&delme.mac, &e->mac);

	  vnet_arp_flush_ip4_over_ethernet_internal (vnet_get_main (),
						     &delme);
	}

      vec_free (to_delete);
    }
}

static void
arp_table_bind (ip4_main_t * im,
		uword opaque,
		u32 sw_if_index, u32 new_fib_index, u32 old_fib_index)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_interface_t *eai;
  ethernet_arp_ip4_entry_t *e;
  hash_pair_t *pair;

  /*
   * the IP table that the interface is bound to has changed.
   * reinstall all the adj fibs.
   */

  if (vec_len (am->ethernet_arp_by_sw_if_index) <= sw_if_index)
    return;

  eai = &am->ethernet_arp_by_sw_if_index[sw_if_index];

  /* *INDENT-OFF* */
  hash_foreach_pair (pair, eai->arp_entries,
  ({
    e = pool_elt_at_index(am->ip4_entry_pool,
                          pair->value[0]);
    /*
     * remove the adj-fib from the old table and add to the new
     */
    arp_adj_fib_remove(e, old_fib_index);
    arp_adj_fib_add(e, new_fib_index);
  }));
  /* *INDENT-ON* */

}

static clib_error_t *
ethernet_arp_init (vlib_main_t * vm)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ip4_main_t *im = &ip4_main;
  pg_node_t *pn;

  ethernet_register_input_type (vm, ETHERNET_TYPE_ARP, arp_input_node.index);

  pn = pg_get_node (arp_input_node.index);
  pn->unformat_edit = unformat_pg_arp_header;

  am->opcode_by_name = hash_create_string (0, sizeof (uword));
#define _(o) hash_set_mem (am->opcode_by_name, #o, ETHERNET_ARP_OPCODE_##o);
  foreach_ethernet_arp_opcode;
#undef _

  /* $$$ configurable */
  am->limit_arp_cache_size = 50000;

  am->pending_resolutions_by_address = hash_create (0, sizeof (uword));
  am->mac_changes_by_address = hash_create (0, sizeof (uword));
  am->wc_ip4_arp_publisher_node = (uword) ~ 0;

  /* don't trace ARP error packets */
  {
    vlib_node_runtime_t *rt =
      vlib_node_get_runtime (vm, arp_input_node.index);

#define _(a,b)                                  \
    vnet_pcap_drop_trace_filter_add_del         \
        (rt->errors[ETHERNET_ARP_ERROR_##a],    \
         1 /* is_add */);
    foreach_ethernet_arp_error
#undef _
  }

  ip4_add_del_interface_address_callback_t cb;
  cb.function = arp_add_del_interface_address;
  cb.function_opaque = 0;
  vec_add1 (im->add_del_interface_address_callbacks, cb);

  ip4_enable_disable_interface_callback_t cbe;
  cbe.function = arp_enable_disable_interface;
  cbe.function_opaque = 0;
  vec_add1 (im->enable_disable_interface_callbacks, cbe);

  ip4_table_bind_callback_t cbt;
  cbt.function = arp_table_bind;
  cbt.function_opaque = 0;
  vec_add1 (im->table_bind_callbacks, cbt);

  return 0;
}
/* *INDENT-OFF* */
VLIB_INIT_FUNCTION (ethernet_arp_init) =
{
  .runs_after = VLIB_INITS("ethernet_init"),
};
/* *INDENT-ON* */

static void
arp_entry_free (ethernet_arp_interface_t * eai, ethernet_arp_ip4_entry_t * e)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;

  arp_adj_fib_remove
    (e, ip4_fib_table_get_index_for_sw_if_index (e->sw_if_index));
  hash_unset (eai->arp_entries, e->ip4_address.as_u32);
  pool_put (am->ip4_entry_pool, e);
}

static inline int
vnet_arp_unset_ip4_over_ethernet_internal (vnet_main_t * vnm,
					   vnet_arp_set_ip4_over_ethernet_rpc_args_t
					   * args)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e;
  ethernet_arp_interface_t *eai;

  if (vec_len (am->ethernet_arp_by_sw_if_index) <= args->sw_if_index)
    return 0;

  eai = &am->ethernet_arp_by_sw_if_index[args->sw_if_index];

  e = arp_entry_find (eai, &args->ip4);

  if (NULL != e)
    {
      adj_nbr_walk_nh4 (e->sw_if_index,
			&e->ip4_address, arp_mk_incomplete_walk, e);
      arp_entry_free (eai, e);
    }

  return 0;
}


static int
vnet_arp_populate_ip4_over_ethernet_internal (vnet_main_t * vnm,
					      vnet_arp_set_ip4_over_ethernet_rpc_args_t
					      * args)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e;
  ethernet_arp_interface_t *eai;

  arp_enable (am, args->sw_if_index);
  eai = &am->ethernet_arp_by_sw_if_index[args->sw_if_index];

  e = arp_entry_find (eai, &args->ip4);

  if (NULL != e)
    {
      adj_nbr_walk_nh4 (e->sw_if_index,
			&e->ip4_address, arp_mk_complete_walk, e);
    }
  return (0);
}

static void
set_ip4_over_ethernet_rpc_callback (vnet_arp_set_ip4_over_ethernet_rpc_args_t
				    * a)
{
  vnet_main_t *vm = vnet_get_main ();
  ASSERT (vlib_get_thread_index () == 0);

  if (a->flags & ETHERNET_ARP_ARGS_REMOVE)
    vnet_arp_unset_ip4_over_ethernet_internal (vm, a);
  else if (a->flags & ETHERNET_ARP_ARGS_FLUSH)
    vnet_arp_flush_ip4_over_ethernet_internal (vm, a);
  else if (a->flags & ETHERNET_ARP_ARGS_POPULATE)
    vnet_arp_populate_ip4_over_ethernet_internal (vm, a);
  else if (a->flags & ETHERNET_ARP_ARGS_WC_PUB)
    vnet_arp_wc_publish_internal (vm, a);
  else
    vnet_arp_set_ip4_over_ethernet_internal (vm, a);
}

/**
 * @brief Invoked when the interface's admin state changes
 */
static clib_error_t *
ethernet_arp_sw_interface_up_down (vnet_main_t * vnm,
				   u32 sw_if_index, u32 flags)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e;
  u32 i, *to_update = 0;

  /* *INDENT-OFF* */
  pool_foreach (e, am->ip4_entry_pool,
  ({
    if (e->sw_if_index == sw_if_index)
      vec_add1 (to_update,
		e - am->ip4_entry_pool);
  }));
  /* *INDENT-ON* */

  for (i = 0; i < vec_len (to_update); i++)
    {
      e = pool_elt_at_index (am->ip4_entry_pool, to_update[i]);

      vnet_arp_set_ip4_over_ethernet_rpc_args_t update_me = {
	.ip4.as_u32 = e->ip4_address.as_u32,
	.sw_if_index = e->sw_if_index,
      };
      mac_address_copy (&update_me.mac, &e->mac);

      if (flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
	{
	  update_me.flags = ETHERNET_ARP_ARGS_POPULATE;
	  vnet_arp_populate_ip4_over_ethernet_internal (vnm, &update_me);
	}
      else
	{
	  update_me.flags = ETHERNET_ARP_ARGS_FLUSH;
	  vnet_arp_flush_ip4_over_ethernet_internal (vnm, &update_me);
	}
    }
  vec_free (to_update);

  return 0;
}

VNET_SW_INTERFACE_ADMIN_UP_DOWN_FUNCTION (ethernet_arp_sw_interface_up_down);

static void
increment_ip4_and_mac_address (ethernet_arp_ip4_over_ethernet_address_t * a)
{
  u8 old;
  int i;

  for (i = 3; i >= 0; i--)
    {
      old = a->ip4.as_u8[i];
      a->ip4.as_u8[i] += 1;
      if (old < a->ip4.as_u8[i])
	break;
    }

  for (i = 5; i >= 0; i--)
    {
      old = a->mac.bytes[i];
      a->mac.bytes[i] += 1;
      if (old < a->mac.bytes[i])
	break;
    }
}

int
vnet_arp_set_ip4_over_ethernet (vnet_main_t * vnm,
				u32 sw_if_index,
				const ethernet_arp_ip4_over_ethernet_address_t
				* a, ip_neighbor_flags_t flags)
{
  vnet_arp_set_ip4_over_ethernet_rpc_args_t args = {
    .sw_if_index = sw_if_index,
    .nbr_flags = flags,
    .flags = 0,
    .ip4.as_u32 = a->ip4.as_u32,
    .mac = a->mac,
  };

  vl_api_rpc_call_main_thread (set_ip4_over_ethernet_rpc_callback,
			       (u8 *) & args, sizeof (args));
  return 0;
}

void
proxy_arp_walk (proxy_arp_walk_t cb, void *data)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_proxy_arp_t *pa;

  vec_foreach (pa, am->proxy_arps)
  {
    if (!cb (&pa->lo_addr, &pa->hi_addr, pa->fib_index, data))
      break;
  }
}

int
vnet_proxy_arp_enable_disable (vnet_main_t * vnm, u32 sw_if_index, u8 enable)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_interface_t *eai;

  vec_validate (am->ethernet_arp_by_sw_if_index, sw_if_index);

  eai = &am->ethernet_arp_by_sw_if_index[sw_if_index];

  if (enable)
    {
      if (!eai->proxy_enabled)
	{
	  vnet_feature_enable_disable ("arp", "arp-proxy",
				       sw_if_index, 1, NULL, 0);
	}
      eai->proxy_enabled = 1;
    }
  else
    {
      if (eai->proxy_enabled)
	{
	  vnet_feature_enable_disable ("arp", "arp-proxy",
				       sw_if_index, 0, NULL, 0);
	}
      eai->proxy_enabled = 0;
    }

  return (0);
}

int
vnet_proxy_arp_add_del (ip4_address_t * lo_addr,
			ip4_address_t * hi_addr, u32 fib_index, int is_del)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_proxy_arp_t *pa;
  u32 found_at_index = ~0;

  vec_foreach (pa, am->proxy_arps)
  {
    if (pa->lo_addr.as_u32 == lo_addr->as_u32 &&
	pa->hi_addr.as_u32 == hi_addr->as_u32 && pa->fib_index == fib_index)
      {
	found_at_index = pa - am->proxy_arps;
	break;
      }
  }

  if (found_at_index != ~0)
    {
      /* Delete, otherwise it's already in the table */
      if (is_del)
	vec_delete (am->proxy_arps, 1, found_at_index);
      return 0;
    }
  /* delete, no such entry */
  if (is_del)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  /* add, not in table */
  vec_add2 (am->proxy_arps, pa, 1);
  pa->lo_addr.as_u32 = lo_addr->as_u32;
  pa->hi_addr.as_u32 = hi_addr->as_u32;
  pa->fib_index = fib_index;
  return 0;
}

void
proxy_arp_intfc_walk (proxy_arp_intf_walk_t cb, void *data)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_interface_t *eai;

  vec_foreach (eai, am->ethernet_arp_by_sw_if_index)
  {
    if (eai->proxy_enabled)
      cb (eai - am->ethernet_arp_by_sw_if_index, data);
  }
}

/*
 * Remove any proxy arp entries associated with the
 * specified fib.
 */
int
vnet_proxy_arp_fib_reset (u32 fib_id)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_proxy_arp_t *pa;
  u32 *entries_to_delete = 0;
  u32 fib_index;
  int i;

  fib_index = fib_table_find (FIB_PROTOCOL_IP4, fib_id);
  if (~0 == fib_index)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  vec_foreach (pa, am->proxy_arps)
  {
    if (pa->fib_index == fib_index)
      {
	vec_add1 (entries_to_delete, pa - am->proxy_arps);
      }
  }

  for (i = 0; i < vec_len (entries_to_delete); i++)
    {
      vec_delete (am->proxy_arps, 1, entries_to_delete[i]);
    }

  vec_free (entries_to_delete);

  return 0;
}

static clib_error_t *
ip_arp_add_del_command_fn (vlib_main_t * vm,
			   unformat_input_t * input, vlib_cli_command_t * cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 sw_if_index;
  ethernet_arp_ip4_over_ethernet_address_t lo_addr, hi_addr, addr;
  int addr_valid = 0;
  int is_del = 0;
  int count = 1;
  u32 fib_index = 0;
  u32 fib_id;
  int is_proxy = 0;
  ip_neighbor_flags_t flags;

  flags = IP_NEIGHBOR_FLAG_NONE;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      /* set ip arp TenGigE1/1/0/1 1.2.3.4 aa:bb:... or aabb.ccdd... */
      if (unformat (input, "%U %U %U",
		    unformat_vnet_sw_interface, vnm, &sw_if_index,
		    unformat_ip4_address, &addr.ip4,
		    unformat_mac_address_t, &addr.mac))
	addr_valid = 1;

      else if (unformat (input, "delete") || unformat (input, "del"))
	is_del = 1;

      else if (unformat (input, "static"))
	flags |= IP_NEIGHBOR_FLAG_STATIC;

      else if (unformat (input, "no-fib-entry"))
	flags |= IP_NEIGHBOR_FLAG_NO_FIB_ENTRY;

      else if (unformat (input, "count %d", &count))
	;

      else if (unformat (input, "fib-id %d", &fib_id))
	{
	  fib_index = fib_table_find (FIB_PROTOCOL_IP4, fib_id);

	  if (~0 == fib_index)
	    return clib_error_return (0, "fib ID %d doesn't exist\n", fib_id);
	}

      else if (unformat (input, "proxy %U - %U",
			 unformat_ip4_address, &lo_addr.ip4,
			 unformat_ip4_address, &hi_addr.ip4))
	is_proxy = 1;
      else
	break;
    }

  if (is_proxy)
    {
      (void) vnet_proxy_arp_add_del (&lo_addr.ip4, &hi_addr.ip4,
				     fib_index, is_del);
      return 0;
    }

  if (addr_valid)
    {
      int i;

      for (i = 0; i < count; i++)
	{
	  if (is_del == 0)
	    {
	      uword event_type, *event_data = 0;

	      /* Park the debug CLI until the arp entry is installed */
	      vnet_register_ip4_arp_resolution_event
		(vnm, &addr.ip4, vlib_current_process (vm),
		 1 /* type */ , 0 /* data */ );

	      vnet_arp_set_ip4_over_ethernet (vnm, sw_if_index, &addr, flags);

	      vlib_process_wait_for_event (vm);
	      event_type = vlib_process_get_events (vm, &event_data);
	      vec_reset_length (event_data);
	      if (event_type != 1)
		clib_warning ("event type %d unexpected", event_type);
	    }
	  else
	    vnet_arp_unset_ip4_over_ethernet (vnm, sw_if_index, &addr);

	  increment_ip4_and_mac_address (&addr);
	}
    }
  else
    {
      return clib_error_return (0, "unknown input `%U'",
				format_unformat_error, input);
    }

  return 0;
}

/* *INDENT-OFF* */
/*?
 * Add or delete IPv4 ARP cache entries.
 *
 * @note 'set ip arp' options (e.g. delete, static, 'fib-id <id>',
 * 'count <number>', 'interface ip4_addr mac_addr') can be added in
 * any order and combination.
 *
 * @cliexpar
 * @parblock
 * Add or delete IPv4 ARP cache entries as follows. MAC Address can be in
 * either aa:bb:cc:dd:ee:ff format or aabb.ccdd.eeff format.
 * @cliexcmd{set ip arp GigabitEthernet2/0/0 6.0.0.3 dead.beef.babe}
 * @cliexcmd{set ip arp delete GigabitEthernet2/0/0 6.0.0.3 de:ad:be:ef:ba:be}
 *
 * To add or delete an IPv4 ARP cache entry to or from a specific fib
 * table:
 * @cliexcmd{set ip arp fib-id 1 GigabitEthernet2/0/0 6.0.0.3 dead.beef.babe}
 * @cliexcmd{set ip arp fib-id 1 delete GigabitEthernet2/0/0 6.0.0.3 dead.beef.babe}
 *
 * Add or delete IPv4 static ARP cache entries as follows:
 * @cliexcmd{set ip arp static GigabitEthernet2/0/0 6.0.0.3 dead.beef.babe}
 * @cliexcmd{set ip arp static delete GigabitEthernet2/0/0 6.0.0.3 dead.beef.babe}
 *
 * For testing / debugging purposes, the 'set ip arp' command can add or
 * delete multiple entries. Supply the 'count N' parameter:
 * @cliexcmd{set ip arp count 10 GigabitEthernet2/0/0 6.0.0.3 dead.beef.babe}
 * @endparblock
 ?*/
VLIB_CLI_COMMAND (ip_arp_add_del_command, static) = {
  .path = "set ip arp",
  .short_help =
  "set ip arp [del] <intfc> <ip-address> <mac-address> [static] [no-fib-entry] [count <count>] [fib-id <fib-id>] [proxy <lo-addr> - <hi-addr>]",
  .function = ip_arp_add_del_command_fn,
};
/* *INDENT-ON* */

static clib_error_t *
set_int_proxy_arp_command_fn (vlib_main_t * vm,
			      unformat_input_t *
			      input, vlib_cli_command_t * cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 sw_if_index;
  int enable = 0;

  sw_if_index = ~0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_vnet_sw_interface,
		    vnm, &sw_if_index))
	;
      else if (unformat (input, "enable") || unformat (input, "on"))
	enable = 1;
      else if (unformat (input, "disable") || unformat (input, "off"))
	enable = 0;
      else
	break;
    }

  if (~0 == sw_if_index)
    return clib_error_return (0, "unknown input '%U'",
			      format_unformat_error, input);

  vnet_proxy_arp_enable_disable (vnm, sw_if_index, enable);

  return 0;
}

/* *INDENT-OFF* */
/*?
 * Enable proxy-arp on an interface. The vpp stack will answer ARP
 * requests for the indicated address range. Multiple proxy-arp
 * ranges may be provisioned.
 *
 * @note Proxy ARP as a technology is infamous for blackholing traffic.
 * Also, the underlying implementation has not been performance-tuned.
 * Avoid creating an unnecessarily large set of ranges.
 *
 * @cliexpar
 * To enable proxy arp on a range of addresses, use:
 * @cliexcmd{set ip arp proxy 6.0.0.1 - 6.0.0.11}
 * Append 'del' to delete a range of proxy ARP addresses:
 * @cliexcmd{set ip arp proxy 6.0.0.1 - 6.0.0.11 del}
 * You must then specifically enable proxy arp on individual interfaces:
 * @cliexcmd{set interface proxy-arp GigabitEthernet0/8/0 enable}
 * To disable proxy arp on an individual interface:
 * @cliexcmd{set interface proxy-arp GigabitEthernet0/8/0 disable}
 ?*/
VLIB_CLI_COMMAND (set_int_proxy_enable_command, static) = {
  .path = "set interface proxy-arp",
  .short_help =
  "set interface proxy-arp <intfc> [enable|disable]",
  .function = set_int_proxy_arp_command_fn,
};
/* *INDENT-ON* */


/*
 * ARP/ND Termination in a L2 Bridge Domain based on IP4/IP6 to MAC
 * hash tables mac_by_ip4 and mac_by_ip6 for each BD.
 */
typedef enum
{
  ARP_TERM_NEXT_L2_OUTPUT,
  ARP_TERM_NEXT_DROP,
  ARP_TERM_N_NEXT,
} arp_term_next_t;

u32 arp_term_next_node_index[32];

static uword
arp_term_l2bd (vlib_main_t * vm,
	       vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  l2input_main_t *l2im = &l2input_main;
  u32 n_left_from, next_index, *from, *to_next;
  u32 n_replies_sent = 0;
  u16 last_bd_index = ~0;
  l2_bridge_domain_t *last_bd_config = 0;
  l2_input_config_t *cfg0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  vlib_buffer_t *p0;
	  ethernet_header_t *eth0;
	  ethernet_arp_header_t *arp0;
	  ip6_header_t *iph0;
	  u8 *l3h0;
	  u32 pi0, error0, next0, sw_if_index0;
	  u16 ethertype0;
	  u16 bd_index0;
	  u32 ip0;
	  u8 *macp0;

	  pi0 = from[0];
	  to_next[0] = pi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  p0 = vlib_get_buffer (vm, pi0);
	  // Terminate only local (SHG == 0) ARP
	  if (vnet_buffer (p0)->l2.shg != 0)
	    goto next_l2_feature;

	  eth0 = vlib_buffer_get_current (p0);
	  l3h0 = (u8 *) eth0 + vnet_buffer (p0)->l2.l2_len;
	  ethertype0 = clib_net_to_host_u16 (*(u16 *) (l3h0 - 2));
	  arp0 = (ethernet_arp_header_t *) l3h0;

	  if (ethertype0 != ETHERNET_TYPE_ARP)
	    goto check_ip6_nd;

	  if ((arp0->opcode !=
	       clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_request)) &&
	      (arp0->opcode !=
	       clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_reply)))
	    goto check_ip6_nd;

	  /* Must be ARP request/reply packet here */
	  if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			     (p0->flags & VLIB_BUFFER_IS_TRACED)))
	    {
	      u8 *t0 = vlib_add_trace (vm, node, p0,
				       sizeof (ethernet_arp_input_trace_t));
	      clib_memcpy_fast (t0, l3h0,
				sizeof (ethernet_arp_input_trace_t));
	    }

	  error0 = 0;
	  error0 =
	    (arp0->l2_type !=
	     clib_net_to_host_u16 (ETHERNET_ARP_HARDWARE_TYPE_ethernet)
	     ? ETHERNET_ARP_ERROR_l2_type_not_ethernet : error0);
	  error0 =
	    (arp0->l3_type !=
	     clib_net_to_host_u16 (ETHERNET_TYPE_IP4) ?
	     ETHERNET_ARP_ERROR_l3_type_not_ip4 : error0);

	  sw_if_index0 = vnet_buffer (p0)->sw_if_index[VLIB_RX];

	  if (error0)
	    goto drop;

	  /* Trash ARP packets whose ARP-level source addresses do not
	     match, or if requester address is mcast */
	  if (PREDICT_FALSE
	      (!ethernet_mac_address_equal (eth0->src_address,
					    arp0->ip4_over_ethernet[0].
					    mac.bytes))
	      || ethernet_address_cast (arp0->ip4_over_ethernet[0].mac.bytes))
	    {
	      /* VRRP virtual MAC may be different to SMAC in ARP reply */
	      if (!ethernet_mac_address_equal
		  (arp0->ip4_over_ethernet[0].mac.bytes, vrrp_prefix))
		{
		  error0 = ETHERNET_ARP_ERROR_l2_address_mismatch;
		  goto drop;
		}
	    }
	  if (PREDICT_FALSE
	      (ip4_address_is_multicast (&arp0->ip4_over_ethernet[0].ip4)))
	    {
	      error0 = ETHERNET_ARP_ERROR_l3_src_address_not_local;
	      goto drop;
	    }

	  /* Check if anyone want ARP request events for L2 BDs */
	  {
	    ethernet_arp_main_t *am = &ethernet_arp_main;
	    if (am->wc_ip4_arp_publisher_node != (uword) ~ 0)
	      vnet_arp_wc_publish (sw_if_index0, &arp0->ip4_over_ethernet[0]);
	  }

	  /* lookup BD mac_by_ip4 hash table for MAC entry */
	  ip0 = arp0->ip4_over_ethernet[1].ip4.as_u32;
	  bd_index0 = vnet_buffer (p0)->l2.bd_index;
	  if (PREDICT_FALSE ((bd_index0 != last_bd_index)
			     || (last_bd_index == (u16) ~ 0)))
	    {
	      last_bd_index = bd_index0;
	      last_bd_config = vec_elt_at_index (l2im->bd_configs, bd_index0);
	    }
	  macp0 = (u8 *) hash_get (last_bd_config->mac_by_ip4, ip0);

	  if (PREDICT_FALSE (!macp0))
	    goto next_l2_feature;	/* MAC not found */
	  if (PREDICT_FALSE (arp0->ip4_over_ethernet[0].ip4.as_u32 ==
			     arp0->ip4_over_ethernet[1].ip4.as_u32))
	    goto next_l2_feature;	/* GARP */

	  /* MAC found, send ARP reply -
	     Convert ARP request packet to ARP reply */
	  arp0->opcode = clib_host_to_net_u16 (ETHERNET_ARP_OPCODE_reply);
	  arp0->ip4_over_ethernet[1] = arp0->ip4_over_ethernet[0];
	  arp0->ip4_over_ethernet[0].ip4.as_u32 = ip0;
	  mac_address_from_bytes (&arp0->ip4_over_ethernet[0].mac, macp0);
	  clib_memcpy_fast (eth0->dst_address, eth0->src_address, 6);
	  clib_memcpy_fast (eth0->src_address, macp0, 6);
	  n_replies_sent += 1;

	output_response:
	  /* For BVI, need to use l2-fwd node to send ARP reply as
	     l2-output node cannot output packet to BVI properly */
	  cfg0 = vec_elt_at_index (l2im->configs, sw_if_index0);
	  if (PREDICT_FALSE (cfg0->bvi))
	    {
	      vnet_buffer (p0)->l2.feature_bitmap |= L2INPUT_FEAT_FWD;
	      vnet_buffer (p0)->sw_if_index[VLIB_RX] = 0;
	      goto next_l2_feature;
	    }

	  /* Send ARP/ND reply back out input interface through l2-output */
	  vnet_buffer (p0)->sw_if_index[VLIB_TX] = sw_if_index0;
	  next0 = ARP_TERM_NEXT_L2_OUTPUT;
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next, pi0,
					   next0);
	  continue;

	check_ip6_nd:
	  /* IP6 ND event notification or solicitation handling to generate
	     local response instead of flooding */
	  iph0 = (ip6_header_t *) l3h0;
	  if (PREDICT_FALSE (ethertype0 == ETHERNET_TYPE_IP6 &&
			     iph0->protocol == IP_PROTOCOL_ICMP6 &&
			     !ip6_address_is_unspecified
			     (&iph0->src_address)))
	    {
	      sw_if_index0 = vnet_buffer (p0)->sw_if_index[VLIB_RX];
	      if (vnet_ip6_nd_term
		  (vm, node, p0, eth0, iph0, sw_if_index0,
		   vnet_buffer (p0)->l2.bd_index))
		goto output_response;
	    }

	next_l2_feature:
	  {
	    next0 = vnet_l2_feature_next (p0, arp_term_next_node_index,
					  L2INPUT_FEAT_ARP_TERM);
	    vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					     to_next, n_left_to_next,
					     pi0, next0);
	    continue;
	  }

	drop:
	  if (0 == arp0->ip4_over_ethernet[0].ip4.as_u32 ||
	      (arp0->ip4_over_ethernet[0].ip4.as_u32 ==
	       arp0->ip4_over_ethernet[1].ip4.as_u32))
	    {
	      error0 = ETHERNET_ARP_ERROR_gratuitous_arp;
	    }
	  next0 = ARP_TERM_NEXT_DROP;
	  p0->error = node->errors[error0];

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next, pi0,
					   next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_error_count (vm, node->node_index,
		    ETHERNET_ARP_ERROR_replies_sent, n_replies_sent);
  return frame->n_vectors;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (arp_term_l2bd_node, static) = {
  .function = arp_term_l2bd,
  .name = "arp-term-l2bd",
  .vector_size = sizeof (u32),
  .n_errors = ETHERNET_ARP_N_ERROR,
  .error_strings = ethernet_arp_error_strings,
  .n_next_nodes = ARP_TERM_N_NEXT,
  .next_nodes = {
    [ARP_TERM_NEXT_L2_OUTPUT] = "l2-output",
    [ARP_TERM_NEXT_DROP] = "error-drop",
  },
  .format_buffer = format_ethernet_arp_header,
  .format_trace = format_arp_term_input_trace,
};
/* *INDENT-ON* */

clib_error_t *
arp_term_init (vlib_main_t * vm)
{
  // Initialize the feature next-node indexes
  feat_bitmap_init_next_nodes (vm,
			       arp_term_l2bd_node.index,
			       L2INPUT_N_FEAT,
			       l2input_get_feat_names (),
			       arp_term_next_node_index);
  return 0;
}

VLIB_INIT_FUNCTION (arp_term_init);

void
change_arp_mac (u32 sw_if_index, ethernet_arp_ip4_entry_t * e)
{
  if (e->sw_if_index == sw_if_index)
    {
      adj_nbr_walk_nh4 (e->sw_if_index,
			&e->ip4_address, arp_mk_complete_walk, e);
    }
}

void
ethernet_arp_change_mac (u32 sw_if_index)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;
  ethernet_arp_ip4_entry_t *e;
  adj_index_t ai;

  /* *INDENT-OFF* */
  pool_foreach (e, am->ip4_entry_pool,
  ({
    change_arp_mac (sw_if_index, e);
  }));
  /* *INDENT-ON* */

  ai = adj_glean_get (FIB_PROTOCOL_IP4, sw_if_index);

  if (ADJ_INDEX_INVALID != ai)
    adj_glean_update_rewrite (ai);
}

void
send_ip4_garp (vlib_main_t * vm, u32 sw_if_index)
{
  ip4_main_t *i4m = &ip4_main;
  ip4_address_t *ip4_addr = ip4_interface_first_address (i4m, sw_if_index, 0);

  send_ip4_garp_w_addr (vm, ip4_addr, sw_if_index);
}

void
send_ip4_garp_w_addr (vlib_main_t * vm,
		      const ip4_address_t * ip4_addr, u32 sw_if_index)
{
  ip4_main_t *i4m = &ip4_main;
  vnet_main_t *vnm = vnet_get_main ();
  u8 *rewrite, rewrite_len;
  vnet_hw_interface_t *hi = vnet_get_sup_hw_interface (vnm, sw_if_index);

  if (ip4_addr)
    {
      clib_warning ("Sending GARP for IP4 address %U on sw_if_idex %d",
		    format_ip4_address, ip4_addr, sw_if_index);

      /* Form GARP packet for output - Gratuitous ARP is an ARP request packet
         where the interface IP/MAC pair is used for both source and request
         MAC/IP pairs in the request */
      u32 bi = 0;
      ethernet_arp_header_t *h = vlib_packet_template_get_packet
	(vm, &i4m->ip4_arp_request_packet_template, &bi);

      if (!h)
	return;

      mac_address_from_bytes (&h->ip4_over_ethernet[0].mac, hi->hw_address);
      mac_address_from_bytes (&h->ip4_over_ethernet[1].mac, hi->hw_address);
      h->ip4_over_ethernet[0].ip4 = ip4_addr[0];
      h->ip4_over_ethernet[1].ip4 = ip4_addr[0];

      /* Setup MAC header with ARP Etype and broadcast DMAC */
      vlib_buffer_t *b = vlib_get_buffer (vm, bi);
      rewrite =
	ethernet_build_rewrite (vnm, sw_if_index, VNET_LINK_ARP,
				VNET_REWRITE_FOR_SW_INTERFACE_ADDRESS_BROADCAST);
      rewrite_len = vec_len (rewrite);
      vlib_buffer_advance (b, -rewrite_len);
      ethernet_header_t *e = vlib_buffer_get_current (b);
      clib_memcpy_fast (e->dst_address, rewrite, rewrite_len);
      vec_free (rewrite);

      /* Send GARP packet out the specified interface */
      vnet_buffer (b)->sw_if_index[VLIB_RX] =
	vnet_buffer (b)->sw_if_index[VLIB_TX] = sw_if_index;
      vlib_frame_t *f = vlib_get_frame_to_node (vm, hi->output_node_index);
      u32 *to_next = vlib_frame_vector_args (f);
      to_next[0] = bi;
      f->n_vectors = 1;
      vlib_put_frame_to_node (vm, hi->output_node_index, f);
    }
}

/*
 * Remove any arp entries associated with the specified interface
 */
static clib_error_t *
vnet_arp_delete_sw_interface (vnet_main_t * vnm, u32 sw_if_index, u32 is_add)
{
  ethernet_arp_main_t *am = &ethernet_arp_main;

  if (!is_add && sw_if_index != ~0)
    {
      ethernet_arp_ip4_entry_t *e;
      /* *INDENT-OFF* */
      pool_foreach (e, am->ip4_entry_pool, ({
        if (e->sw_if_index != sw_if_index)
          continue;
        vnet_arp_set_ip4_over_ethernet_rpc_args_t args = {
          .sw_if_index = sw_if_index,
          .ip4 = e->ip4_address,
        };
        vnet_arp_unset_ip4_over_ethernet_internal (vnm, &args);
      }));
      /* *INDENT-ON* */
      arp_disable (am, sw_if_index);
    }
  else if (is_add)
    {
      vnet_feature_enable_disable ("arp", "arp-disabled",
				   sw_if_index, 1, NULL, 0);
    }

  return (NULL);
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (vnet_arp_delete_sw_interface);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
