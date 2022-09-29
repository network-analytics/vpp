/*
 * delayprobe.c - ipfix probe plugin
 *
 * Copyright (c) 2016 Cisco and/or its affiliates.
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

/**
 * @file
 * @brief Per-packet IPFIX flow record generator plugin
 *
 * This file implements vpp plugin registration mechanics,
 * debug CLI, and binary API handling.
 */

#include <vnet/vnet.h>
#include <vpp/app/version.h>
#include <vnet/plugin/plugin.h>
#include <vnet/udp/udp_local.h>
#include <delayprobe/delayprobe.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

/* define message IDs */
#include <delayprobe/delayprobe.api_enum.h>
#include <delayprobe/delayprobe.api_types.h>

delayprobe_main_t delayprobe_main;
static vlib_node_registration_t delayprobe_timer_node;
uword delayprobe_walker_process (vlib_main_t *vm, vlib_node_runtime_t *rt,
				 vlib_frame_t *f);

#define REPLY_MSG_ID_BASE fm->msg_id_base
#include <vlibapi/api_helper_macros.h>

/* Define the per-interface configurable features */
/* *INDENT-OFF* */
VNET_FEATURE_INIT (delayprobe_input_ip6_srh_unicast, static) = {
  .arc_name = "ip6-unicast",
  .node_name = "delayprobe-input-srh-ip6",
  .runs_before = VNET_FEATURES ("ip6-lookup"),
};
VNET_FEATURE_INIT (delayprobe_input_ip6_srh_multicast, static) = {
  .arc_name = "ip6-multicast",
  .node_name = "delayprobe-input-srh-ip6",
  .runs_before = VNET_FEATURES ("ip6-mfib-forward-lookup"),
};
/* *INDENT-ON* */

/* Macro to finish up custom dump fns */
#define vl_print(handle, ...) vlib_cli_output (handle, __VA_ARGS__)
#define FINISH                                                                \
  vec_add1 (s, 0);                                                            \
  vl_print (handle, (char *) s);                                              \
  vec_free (s);                                                               \
  return handle;

static inline ipfix_field_specifier_t *
delayprobe_template_ip6_srh_fields (ipfix_field_specifier_t *f)
{
#define delayprobe_template_ip6_srh_field_count() 11
  /* srh sourceIpv6Address, TLV type 27, 16 octets */
  f->e_id_length =
    ipfix_e_id_length (0 /* enterprise */, sourceIPv6Address, 16);
  f++;
  /* srh srhActiveSegmentIPv6, TLV type 28, 16 octets */
  f->e_id_length =
    ipfix_e_id_length (0 /* enterprise */, srhActiveSegmentIPv6, 16);
  f++;
  /* srh srhSegmentEndpointBehavior, TLV type 28, 2 octets */
  f->e_id_length =
    ipfix_e_id_length (0 /* enterprise */, srhSegmentEndpointBehavior, 2);
  f++;
  /* srhSegmentIPv6sLeft, TLV type 506, 1 octets */
  f->e_id_length =
    ipfix_e_id_length (0 /* enterprise */, srhSegmentIPv6sLeft, 1);
  f++;
  /* srhFlagsIPv6, TLV type 500, 1 octets */
  f->e_id_length = ipfix_e_id_length (0 /* enterprise */, srhFlagsIPv6, 1);
  f++;
  /* srhTagIPv6, TLV type 501, 2 octets */
  f->e_id_length = ipfix_e_id_length (0 /* enterprise */, srhTagIPv6, 2);
  f++;
  /* srhSegmentIPv6BasicList, TLV type 504, N octets */
  f->e_id_length =
    ipfix_e_id_length (0 /* enterprise */, srhSegmentIPv6BasicList,
		       16 * FLOW_SRH_MAX_SID_LIST); // limit 16 IPv6
  f++;

  /* flow sourceIpv6Address, TLV type 27, 16 octets */
  f->e_id_length =
    ipfix_e_id_length (0 /* enterprise */, sourceIPv6Address, 16);
  f++;
  /* flow destinationIPv6Address, TLV type 28, 16 octets */
  f->e_id_length =
    ipfix_e_id_length (0 /* enterprise */, destinationIPv6Address, 16);
  f++;
  /* packetDeltaCount, TLV type 2, u64 */
  f->e_id_length = ipfix_e_id_length (0 /* enterprise */, packetDeltaCount, 8);
  f++;
  f->e_id_length = ipfix_e_id_length (0 /* enterprise */, octetDeltaCount, 8);
  f++;
  return f;
}

/**
 * @brief Create an IPFIX template packet rewrite string
 * @param frm flow_report_main_t *
 * @param fr flow_report_t *
 * @param collector_address ip4_address_t * the IPFIX collector address
 * @param src_address ip4_address_t * the source address we should use
 * @param collector_port u16 the collector port we should use, host byte order
 * @returns u8 * vector containing the indicated IPFIX template packet
 */
static inline u8 *
delayprobe_template_rewrite_inline (ipfix_exporter_t *exp, flow_report_t *fr,
				    u16 collector_port,
				    delayprobe_variant_t which)
{
  ip4_header_t *ip;
  udp_header_t *udp;
  ipfix_message_header_t *h;
  ipfix_set_header_t *s;
  ipfix_template_header_t *t;
  ipfix_field_specifier_t *f;
  ipfix_field_specifier_t *first_field;
  u8 *rewrite = 0;
  ip4_ipfix_template_packet_t *tp;
  u32 field_count = 0;
  flow_report_stream_t *stream;
  delayprobe_main_t *fm = &delayprobe_main;
  delayprobe_record_t flags = fr->opaque.as_uword;

  stream = &exp->streams[fr->stream_index];

  field_count += delayprobe_template_ip6_srh_field_count ();

  /* allocate rewrite space */
  vec_validate_aligned (rewrite,
			sizeof (ip4_ipfix_template_packet_t) +
			  field_count * sizeof (ipfix_field_specifier_t) - 1,
			CLIB_CACHE_LINE_BYTES);

  tp = (ip4_ipfix_template_packet_t *) rewrite;
  ip = (ip4_header_t *) &tp->ip4;
  udp = (udp_header_t *) (ip + 1);
  h = (ipfix_message_header_t *) (udp + 1);
  s = (ipfix_set_header_t *) (h + 1);
  t = (ipfix_template_header_t *) (s + 1);
  first_field = f = (ipfix_field_specifier_t *) (t + 1);

  ip->ip_version_and_header_length = 0x45;
  ip->ttl = 254;
  ip->protocol = IP_PROTOCOL_UDP;
  ip->src_address.as_u32 = exp->src_address.ip.ip4.as_u32;
  ip->dst_address.as_u32 = exp->ipfix_collector.ip.ip4.as_u32;
  udp->src_port = clib_host_to_net_u16 (stream->src_port);
  udp->dst_port = clib_host_to_net_u16 (collector_port);
  udp->length = clib_host_to_net_u16 (vec_len (rewrite) - sizeof (*ip));

  /* FIXUP: message header export_time */
  /* FIXUP: message header sequence_number */
  h->domain_id = clib_host_to_net_u32 (stream->domain_id);

  f = delayprobe_template_ip6_srh_fields (f);

  /* Back to the template packet... */
  ip = (ip4_header_t *) &tp->ip4;
  udp = (udp_header_t *) (ip + 1);

  ASSERT (f - first_field);
  /* Field count in this template */
  t->id_count = ipfix_id_count (fr->template_id, f - first_field);

  fm->template_size[flags] = (u8 *) f - (u8 *) s;

  /* set length in octets */
  s->set_id_length = ipfix_set_id_length (2 /* set_id */, (u8 *) f - (u8 *) s);

  /* message length in octets */
  h->version_length = version_length ((u8 *) f - (u8 *) h);

  ip->length = clib_host_to_net_u16 ((u8 *) f - (u8 *) ip);
  ip->checksum = ip4_header_checksum (ip);

  return rewrite;
}

static u8 *
delayprobe_template_rewrite_srh_ip6 (ipfix_exporter_t *exp, flow_report_t *fr,
				     u16 collector_port,
				     ipfix_report_element_t *elts, u32 n_elts,
				     u32 *stream_index)
{
  return delayprobe_template_rewrite_inline (exp, fr, collector_port,
					     FLOW_VARIANT_SRH_IP6);
}

/**
 * @brief Flush accumulated data
 * @param frm flow_report_main_t *
 * @param fr flow_report_t *
 * @param f vlib_frame_t *
 *
 * <em>Notes:</em>
 * This function must simply return the incoming frame, or no template packets
 * will be sent.
 */

vlib_frame_t *
delayprobe_data_callback_srh_ip6 (flow_report_main_t *frm,
				  ipfix_exporter_t *exp, flow_report_t *fr,
				  vlib_frame_t *f, u32 *to_next,
				  u32 node_index)
{
  delayprobe_flush_callback_srh_ip6 ();
  return f;
}

static int
delayprobe_template_add_del (u32 domain_id, u16 src_port,
			     delayprobe_record_t flags,
			     vnet_flow_data_callback_t *flow_data_callback,
			     vnet_flow_rewrite_callback_t *rewrite_callback,
			     bool is_add, u16 *template_id)
{
  ipfix_exporter_t *exp = &flow_report_main.exporters[0];
  vnet_flow_report_add_del_args_t a = {
    .rewrite_callback = rewrite_callback,
    .flow_data_callback = flow_data_callback,
    .is_add = is_add,
    .domain_id = domain_id,
    .src_port = src_port,
    .opaque.as_uword = flags,
  };
  return vnet_flow_report_add_del (exp, &a, template_id);
}

static void
delayprobe_expired_timer_callback (u32 *expired_timers)
{
  vlib_main_t *vm = vlib_get_main ();
  delayprobe_main_t *fm = &delayprobe_main;
  u32 my_cpu_number = vm->thread_index;
  int i;
  u32 poolindex;

  for (i = 0; i < vec_len (expired_timers); i++)
    {
      poolindex = expired_timers[i] & 0x7FFFFFFF;
      vec_add1 (fm->expired_passive_per_worker[my_cpu_number], poolindex);
    }
}

static clib_error_t *
delayprobe_create_state_tables (u32 active_timer)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vlib_thread_main_t *tm = &vlib_thread_main;
  vlib_main_t *vm = vlib_get_main ();
  clib_error_t *error = 0;
  u32 num_threads;
  int i;

  /* Decide how many worker threads we have */
  num_threads = 1 /* main thread */ + tm->n_threads;

  /* Hash table per worker */
  fm->ht_log2len = delayprobe_LOG2_HASHSIZE;

  /* Init per worker flow state and timer wheels */
  if (active_timer)
    {
      vec_validate (fm->timers_per_worker, num_threads - 1);
      vec_validate (fm->expired_passive_per_worker, num_threads - 1);
      vec_validate (fm->hash_per_worker, num_threads - 1);
      vec_validate (fm->pool_per_worker, num_threads - 1);

      for (i = 0; i < num_threads; i++)
	{
	  int j;
	  pool_alloc (fm->pool_per_worker[i], 1 << fm->ht_log2len);
	  vec_resize (fm->hash_per_worker[i], 1 << fm->ht_log2len);
	  for (j = 0; j < (1 << fm->ht_log2len); j++)
	    fm->hash_per_worker[i][j] = ~0;
	  fm->timers_per_worker[i] =
	    clib_mem_alloc (sizeof (TWT (tw_timer_wheel)));
	  tw_timer_wheel_init_2t_1w_2048sl (fm->timers_per_worker[i],
					    delayprobe_expired_timer_callback,
					    1.0, 1024);
	}
      fm->disabled = true;
    }
  else
    {
      f64 now = vlib_time_now (vm);
      vec_validate (fm->stateless_entry, num_threads - 1);
      for (i = 0; i < num_threads; i++)
	fm->stateless_entry[i].last_exported = now;
      fm->disabled = false;
    }
  fm->initialized = true;
  return error;
}

static int
validate_feature_on_interface (delayprobe_main_t *fm, u32 sw_if_index,
			       u8 which)
{
  vec_validate_init_empty (fm->flow_per_interface, sw_if_index, ~0);
  vec_validate_init_empty (fm->direction_per_interface, sw_if_index, ~0);

  if (fm->flow_per_interface[sw_if_index] == (u8) ~0)
    return -1;
  else if (fm->flow_per_interface[sw_if_index] != which)
    return 0;
  else
    return 1;
}

/**
 * @brief configure / deconfigure the IPFIX flow-per-packet
 * @param fm delayprobe_main_t * fm
 * @param sw_if_index u32 the desired interface
 * @param which u8 the desired datapath
 * @param direction u8 the desired direction
 * @param is_add int 1 to enable the feature, 0 to disable it
 * @returns 0 if successful, non-zero otherwise
 */

static int
delayprobe_interface_add_del_feature (delayprobe_main_t *fm, u32 sw_if_index,
				      u8 which, u8 direction, int is_add)
{
  vlib_main_t *vm = vlib_get_main ();
  int rv = 0;
  u16 template_id = 0;
  delayprobe_record_t flags = fm->record;

  fm->flow_per_interface[sw_if_index] = (is_add) ? which : (u8) ~0;
  fm->direction_per_interface[sw_if_index] = (is_add) ? direction : (u8) ~0;
  fm->template_per_flow[which] += (is_add) ? 1 : -1;
  if (is_add && fm->template_per_flow[which] > 1)
    template_id = fm->template_reports[flags];

  if ((is_add && fm->template_per_flow[which] == 1) ||
      (!is_add && fm->template_per_flow[which] == 0))
    {
      if (which == FLOW_VARIANT_SRH_IP6)
	rv = delayprobe_template_add_del (
	  1, UDP_DST_PORT_ipfix, flags, delayprobe_data_callback_srh_ip6,
	  delayprobe_template_rewrite_srh_ip6, is_add, &template_id);
    }
  if (rv && rv != VNET_API_ERROR_VALUE_EXIST)
    {
      clib_warning ("vnet_flow_report_add_del returned %d", rv);
      return -1;
    }

  if (which != (u8) ~0)
    {
      fm->context[which].flags = fm->record;
      fm->template_reports[flags] = (is_add) ? template_id : 0;
    }
  if (direction == FLOW_DIRECTION_RX || direction == FLOW_DIRECTION_BOTH)
    {
      if (which == FLOW_VARIANT_SRH_IP6)
	{
	  vnet_feature_enable_disable ("ip6-unicast",
				       "delayprobe-input-srh-ip6", sw_if_index,
				       is_add, 0, 0);
	  vnet_feature_enable_disable ("ip6-multicast",
				       "delayprobe-input-srh-ip6", sw_if_index,
				       is_add, 0, 0);
	}
    }

  if (direction == FLOW_DIRECTION_TX || direction == FLOW_DIRECTION_BOTH)
    {
      if (which == FLOW_VARIANT_SRH_IP6)
	{
	  vnet_feature_enable_disable ("ip6-output", "delayprobe-output-ip6",
				       sw_if_index, is_add, 0, 0);
	}
    }

  /* Stateful flow collection */
  if (is_add && !fm->initialized)
    {
      delayprobe_create_state_tables (fm->active_timer);
      if (fm->active_timer)
	vlib_process_signal_event (vm, delayprobe_timer_node.index, 1, 0);
    }

  return 0;
}

/**
 * @brief API message handler
 * @param mp vl_api_delayprobe_tx_interface_add_del_t * mp the api message
 */
void vl_api_delayprobe_tx_interface_add_del_t_handler
  (vl_api_delayprobe_tx_interface_add_del_t * mp)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vl_api_delayprobe_tx_interface_add_del_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = 0;

  VALIDATE_SW_IF_INDEX (mp);

  if (fm->record == 0)
    {
      clib_warning ("Please specify delayprobe params record first...");
      rv = VNET_API_ERROR_CANNOT_ENABLE_DISABLE_FEATURE;
      goto out;
    }

  rv = validate_feature_on_interface (fm, sw_if_index, mp->which);
  if ((rv == 1 && mp->is_add == 1) || rv == 0)
    {
      rv = VNET_API_ERROR_CANNOT_ENABLE_DISABLE_FEATURE;
      goto out;
    }

  rv = delayprobe_interface_add_del_feature (fm, sw_if_index, mp->which,
					    FLOW_DIRECTION_TX, mp->is_add);

out:
  BAD_SW_IF_INDEX_LABEL;

  REPLY_MACRO (VL_API_DELAYPROBE_TX_INTERFACE_ADD_DEL_REPLY);
}

void
vl_api_delayprobe_interface_add_del_t_handler (
  vl_api_delayprobe_interface_add_del_t *mp)
{
  clib_warning("NEVER CALLED");
  delayprobe_main_t *fm = &delayprobe_main;
  vl_api_delayprobe_interface_add_del_reply_t *rmp;
  u32 sw_if_index;
  u8 which;
  u8 direction;
  bool is_add;
  int rv = 0;

  VALIDATE_SW_IF_INDEX (mp);

  sw_if_index = ntohl (mp->sw_if_index);
  is_add = mp->is_add;
  which = FLOW_VARIANT_SRH_IP6;

  if (mp->direction == DELAYPROBE_DIRECTION_RX)
    direction = FLOW_DIRECTION_RX;
  else if (mp->direction == DELAYPROBE_DIRECTION_TX)
    direction = FLOW_DIRECTION_TX;
  else if (mp->direction == DELAYPROBE_DIRECTION_BOTH)
    direction = FLOW_DIRECTION_BOTH;
  else
    {
      clib_warning ("Invalid value of direction");
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto out;
    }

  if (fm->record == 0)
    {
      clib_warning ("Please specify delayprobe params record first");
      rv = VNET_API_ERROR_CANNOT_ENABLE_DISABLE_FEATURE;
      goto out;
    }

  rv = validate_feature_on_interface (fm, sw_if_index, which);
  if (rv == 1)
    {
      if (is_add)
	{
	  clib_warning ("Variant is already enabled for given interface");
	  rv = VNET_API_ERROR_ENTRY_ALREADY_EXISTS;
	  goto out;
	}
    }
  else if (rv == 0)
    {
      clib_warning ("Interface has different variant enabled");
      rv = VNET_API_ERROR_ENTRY_ALREADY_EXISTS;
      goto out;
    }
  else if (rv == -1)
    {
      if (!is_add)
	{
	  clib_warning ("Interface has no variant enabled");
	  rv = VNET_API_ERROR_NO_SUCH_ENTRY;
	  goto out;
	}
    }

  rv = delayprobe_interface_add_del_feature (fm, sw_if_index, which, direction,
					    is_add);

out:
  BAD_SW_IF_INDEX_LABEL;

  REPLY_MACRO (VL_API_DELAYPROBE_INTERFACE_ADD_DEL_REPLY);
}

static void
send_delayprobe_interface_details (u32 sw_if_index, u8 which, u8 direction,
				  vl_api_registration_t *reg, u32 context)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vl_api_delayprobe_interface_details_t *rmp = 0;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  if (!rmp)
    return;
  clib_memset (rmp, 0, sizeof (*rmp));
  rmp->_vl_msg_id =
    ntohs (VL_API_DELAYPROBE_INTERFACE_DETAILS + REPLY_MSG_ID_BASE);
  rmp->context = context;

  rmp->sw_if_index = htonl (sw_if_index);

  rmp->which = DELAYPROBE_WHICH_IP6;

  if (direction == FLOW_DIRECTION_RX)
    rmp->direction = DELAYPROBE_DIRECTION_RX;
  else if (direction == FLOW_DIRECTION_TX)
    rmp->direction = DELAYPROBE_DIRECTION_TX;
  else if (direction == FLOW_DIRECTION_BOTH)
    rmp->direction = DELAYPROBE_DIRECTION_BOTH;
  else
    ASSERT (0);

  vl_api_send_msg (reg, (u8 *) rmp);
}

static void
vl_api_delayprobe_interface_dump_t_handler (
  vl_api_delayprobe_interface_dump_t *mp)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vl_api_registration_t *reg;
  u32 sw_if_index;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  sw_if_index = ntohl (mp->sw_if_index);

  if (sw_if_index == ~0)
    {
      u8 *which;

      vec_foreach (which, fm->flow_per_interface)
	{
	  if (*which == (u8) ~0)
	    continue;

	  sw_if_index = which - fm->flow_per_interface;
	  send_delayprobe_interface_details (
	    sw_if_index, *which, fm->direction_per_interface[sw_if_index], reg,
	    mp->context);
	}
    }
  else if (vec_len (fm->flow_per_interface) > sw_if_index &&
	   fm->flow_per_interface[sw_if_index] != (u8) ~0)
    {
      send_delayprobe_interface_details (
	sw_if_index, fm->flow_per_interface[sw_if_index],
	fm->direction_per_interface[sw_if_index], reg, mp->context);
    }
}

#define vec_neg_search(v, E)                                                  \
  ({                                                                          \
    word _v (i) = 0;                                                          \
    while (_v (i) < vec_len (v) && v[_v (i)] == E)                            \
      {                                                                       \
	_v (i)++;                                                             \
      }                                                                       \
    if (_v (i) == vec_len (v))                                                \
      _v (i) = ~0;                                                            \
    _v (i);                                                                   \
  })

static int
delayprobe_params (delayprobe_main_t *fm, u32 active_timer, u32 passive_timer)
{
  delayprobe_record_t flags = 0;

  if (vec_neg_search (fm->flow_per_interface, (u8) ~0) != ~0)
    return VNET_API_ERROR_UNSUPPORTED;

  flags |= FLOW_RECORD_L3;

  fm->record = flags;

  /*
   * Timers: ~0 is default, 0 is off
   */
  fm->active_timer =
    (active_timer == (u32) ~0 ? delayprobe_TIMER_ACTIVE : active_timer);
  fm->passive_timer =
    (passive_timer == (u32) ~0 ? delayprobe_TIMER_PASSIVE : passive_timer);

  return 0;
}

void
vl_api_delayprobe_params_t_handler (vl_api_delayprobe_params_t *mp)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vl_api_delayprobe_params_reply_t *rmp;
  int rv = 0;

  rv = delayprobe_params (fm,
			  clib_net_to_host_u32 (mp->active_timer),
			  clib_net_to_host_u32 (mp->passive_timer));

  REPLY_MACRO (VL_API_DELAYPROBE_PARAMS_REPLY);
}

void
vl_api_delayprobe_set_params_t_handler (vl_api_delayprobe_set_params_t *mp)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vl_api_delayprobe_set_params_reply_t *rmp;
  bool record_l2, record_l3, record_l4;
  u32 active_timer;
  u32 passive_timer;
  int rv = 0;

  record_l2 = (mp->record_flags & DELAYPROBE_RECORD_FLAG_L2);
  record_l3 = (mp->record_flags & DELAYPROBE_RECORD_FLAG_L3);
  record_l4 = (mp->record_flags & DELAYPROBE_RECORD_FLAG_L4);

  active_timer = clib_net_to_host_u32 (mp->active_timer);
  passive_timer = clib_net_to_host_u32 (mp->passive_timer);

  if (passive_timer > 0 && active_timer > passive_timer)
    {
      clib_warning ("Passive timer must be greater than active timer");
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto out;
    }

  rv = delayprobe_params (fm, active_timer,
			  passive_timer);
  if (rv == VNET_API_ERROR_UNSUPPORTED)
    clib_warning (
      "Cannot change params when feature is enabled on some interfaces");

out:
  REPLY_MACRO (VL_API_DELAYPROBE_SET_PARAMS_REPLY);
}

void
vl_api_delayprobe_get_params_t_handler (vl_api_delayprobe_get_params_t *mp)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vl_api_delayprobe_get_params_reply_t *rmp;
  u8 record_flags = 0;
  int rv = 0;

  // if (fm->record & FLOW_RECORD_L2)
  //   record_flags |= DELAYPROBE_RECORD_FLAG_L2;
  if (fm->record & FLOW_RECORD_L3)
    record_flags |= DELAYPROBE_RECORD_FLAG_L3;
  // if (fm->record & FLOW_RECORD_L4)
  //   record_flags |= DELAYPROBE_RECORD_FLAG_L4;

  // clang-format off
  REPLY_MACRO2 (VL_API_DELAYPROBE_GET_PARAMS_REPLY,
  ({
    rmp->record_flags = record_flags;
    rmp->active_timer = htonl (fm->active_timer);
    rmp->passive_timer = htonl (fm->passive_timer);
  }));
  // clang-format on
}

/* *INDENT-OFF* */
VLIB_PLUGIN_REGISTER () = {
  .version = VPP_BUILD_VER,
  .description = "Flow per Packet",
};
/* *INDENT-ON* */

u8 *
format_delayprobe_direction (u8 *s, va_list *args)
{
  u8 *direction = va_arg (*args, u8 *);
  if (*direction == FLOW_DIRECTION_RX)
    s = format (s, "rx");
  else if (*direction == FLOW_DIRECTION_TX)
    s = format (s, "tx");
  else if (*direction == FLOW_DIRECTION_BOTH)
    s = format (s, "rx tx");

  return s;
}

u8 *
format_delayprobe_entry (u8 *s, va_list *args)
{
  delayprobe_entry_t *e = va_arg (*args, delayprobe_entry_t *);
  s = format (s, " %U", format_delayprobe_direction, &e->key.direction);
  s = format (s, " %d/%d", e->key.rx_sw_if_index, e->key.tx_sw_if_index);

  s = format (s, " %U %U", format_ethernet_address, &e->key.src_mac,
	      format_ethernet_address, &e->key.dst_mac);
  s = format (s, " %U -> %U", format_ip46_address, &e->key.src_address,
	      IP46_TYPE_ANY, format_ip46_address, &e->key.dst_address,
	      IP46_TYPE_ANY);
  s = format (s, " %d", e->key.protocol);
  s = format (s, " %d %d\n", clib_net_to_host_u16 (e->key.src_port),
	      clib_net_to_host_u16 (e->key.dst_port));

  return s;
}

u8 *
format_delayprobe_feature (u8 *s, va_list *args)
{
  u8 *which = va_arg (*args, u8 *);
  if (*which == FLOW_VARIANT_SRH_IP6)
    s = format (s, "srh");

  return s;
}

u8 *
format_delayprobe_params (u8 *s, va_list *args)
{
  delayprobe_record_t flags = va_arg (*args, delayprobe_record_t);
  u32 active_timer = va_arg (*args, u32);
  u32 passive_timer = va_arg (*args, u32);

  if (flags & FLOW_RECORD_L3)
    s = format (s, " l3");

  if (active_timer != (u32) ~0)
    s = format (s, " active: %d", active_timer);

  if (passive_timer != (u32) ~0)
    s = format (s, " passive: %d", passive_timer);

  return s;
}

static clib_error_t *
delayprobe_show_table_fn (vlib_main_t *vm, unformat_input_t *input,
			  vlib_cli_command_t *cm)
{
  delayprobe_main_t *fm = &delayprobe_main;
  int i;
  delayprobe_entry_t *e;

  vlib_cli_output (vm, "Dumping IPFIX table");

  for (i = 0; i < vec_len (fm->pool_per_worker); i++)
    {
      /* *INDENT-OFF* */
      pool_foreach (e, fm->pool_per_worker[i])
	{
	  vlib_cli_output (vm, "%U", format_delayprobe_entry, e);
	}
      /* *INDENT-ON* */
    }
  return 0;
}

static clib_error_t *
delayprobe_show_stats_fn (vlib_main_t *vm, unformat_input_t *input,
			  vlib_cli_command_t *cm)
{
  delayprobe_main_t *fm = &delayprobe_main;
  int i;

  vlib_cli_output (vm, "IPFIX table statistics");
  vlib_cli_output (vm, "Flow entry size: %d\n", sizeof (delayprobe_entry_t));
  vlib_cli_output (vm, "Flow pool size per thread: %d\n",
		   0x1 << delayprobe_LOG2_HASHSIZE);

  for (i = 0; i < vec_len (fm->pool_per_worker); i++)
    vlib_cli_output (vm, "Pool utilisation thread %d is %d%%\n", i,
		     (100 * pool_elts (fm->pool_per_worker[i])) /
		       (0x1 << delayprobe_LOG2_HASHSIZE));
  return 0;
}

static clib_error_t *
delayprobe_interface_add_del_feature_command_fn (vlib_main_t *vm,
						 unformat_input_t *input,
						 vlib_cli_command_t *cmd)
{
  delayprobe_main_t *fm = &delayprobe_main;
  u32 sw_if_index = ~0;
  int is_add = 1;
  u8 which = FLOW_VARIANT_SRH_IP6;
  delayprobe_direction_t direction = FLOW_DIRECTION_TX;
  int rv;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "disable"))
	is_add = 0;
      else if (unformat (input, "%U", unformat_vnet_sw_interface,
			 fm->vnet_main, &sw_if_index))
	;
      else if(unformat (input, "srh"));
      else if (unformat (input, "rx"))
	direction = FLOW_DIRECTION_RX;
      else if (unformat (input, "tx"))
	direction = FLOW_DIRECTION_TX;
      else if (unformat (input, "both"))
	direction = FLOW_DIRECTION_BOTH;
      else
	break;
    }

  if (fm->record == 0)
    return clib_error_return (
      0, "Please specify delayprobe params record first...");

  if (sw_if_index == ~0)
    return clib_error_return (0, "Please specify an interface...");

  rv = validate_feature_on_interface (fm, sw_if_index, which);
  if (rv == 1)
    {
      if (is_add)
	return clib_error_return (
	  0, "Datapath is already enabled for given interface...");
    }
  else if (rv == 0)
    return clib_error_return (0,
			      "Interface has enable different datapath ...");
  else if (rv == -1)
    {
      if (!is_add)
	{
	  return clib_error_return (0, "Interface has no datapath enabled");
	}
    }

  rv = delayprobe_interface_add_del_feature (fm, sw_if_index, which, direction,
					     is_add);
  switch (rv)
    {
    case 0:
      break;

    case VNET_API_ERROR_INVALID_SW_IF_INDEX:
      return clib_error_return (
	0, "Invalid interface, only works on physical ports");
      break;

    case VNET_API_ERROR_UNIMPLEMENTED:
      return clib_error_return (0, "ip6 not supported");
      break;

    default:
      return clib_error_return (0, "delayprobe_enable_disable returned %d",
				rv);
    }
  return 0;
}

static clib_error_t *
delayprobe_show_feature_command_fn (vlib_main_t *vm, unformat_input_t *input,
				    vlib_cli_command_t *cmd)
{
  delayprobe_main_t *fm = &delayprobe_main;
  u8 *which;
  u32 sw_if_index;

  vec_foreach (which, fm->flow_per_interface)
    {
      if (*which == (u8) ~0)
	continue;

      sw_if_index = which - fm->flow_per_interface;
      vlib_cli_output (vm, " %U %U %U", format_vnet_sw_if_index_name,
		       vnet_get_main (), sw_if_index,
		       format_delayprobe_feature, which,
		       format_delayprobe_direction,
		       &fm->direction_per_interface[sw_if_index]);
    }
  return 0;
}

static clib_error_t *
delayprobe_params_command_fn (vlib_main_t *vm, unformat_input_t *input,
			      vlib_cli_command_t *cmd)
{
  delayprobe_main_t *fm = &delayprobe_main;
  // bool record_l3 = true;
  u32 active_timer = ~0;
  u32 passive_timer = ~0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "active %d", &active_timer))
	;
      else if (unformat (input, "passive %d", &passive_timer))
	;
      else if (unformat (input, "record"))
	while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
	  {
	    if (unformat (input, "l3"))
	      break;
	  }
    else
	break;
    }

  if (passive_timer > 0 && active_timer > passive_timer)
    return clib_error_return (
      0, "Passive timer has to be greater than active one...");

  if (delayprobe_params (fm, active_timer, passive_timer))
    return clib_error_return (0, "Couldn't change flowperpacket params when "
				 "feature is enabled on some interface ...");
  return 0;
}

static clib_error_t *
delayprobe_show_params_command_fn (vlib_main_t *vm, unformat_input_t *input,
				   vlib_cli_command_t *cmd)
{
  delayprobe_main_t *fm = &delayprobe_main;
  delayprobe_record_t flags = fm->record;
  u32 active_timer = fm->active_timer;
  u32 passive_timer = fm->passive_timer;

  vlib_cli_output (vm, "%U", format_delayprobe_params, flags, active_timer,
		   passive_timer);
  return 0;
}

/*?
 * '<em>delayprobe feature add-del</em>' commands to enable/disable
 * per-packet IPFIX flow record generation on an interface
 *
 * @cliexpar
 * @parblock
 * To enable per-packet IPFIX flow-record generation on an interface:
 * @cliexcmd{delayprobe feature add-del GigabitEthernet2/0/0}
 *
 * To disable per-packet IPFIX flow-record generation on an interface:
 * @cliexcmd{delayprobe feature add-del GigabitEthernet2/0/0 disable}
 * @cliexend
 * @endparblock
?*/
/* *INDENT-OFF* */
VLIB_CLI_COMMAND (delayprobe_enable_disable_command, static) = {
  .path = "delayprobe feature add-del",
  .short_help = "delayprobe feature add-del <interface-name> srh "
		"[(rx|tx|both)] [disable]",
  .function = delayprobe_interface_add_del_feature_command_fn,
};
VLIB_CLI_COMMAND (delayprobe_params_command, static) = {
  .path = "delayprobe params",
  .short_help = "delayprobe params record l3 [active <timer>] "
		"[passive <timer>]",
  .function = delayprobe_params_command_fn,
};

VLIB_CLI_COMMAND (delayprobe_show_feature_command, static) = {
  .path = "show delayprobe feature",
  .short_help = "show delayprobe feature",
  .function = delayprobe_show_feature_command_fn,
};
VLIB_CLI_COMMAND (delayprobe_show_params_command, static) = {
  .path = "show delayprobe params",
  .short_help = "show delayprobe params",
  .function = delayprobe_show_params_command_fn,
};
VLIB_CLI_COMMAND (delayprobe_show_table_command, static) = {
  .path = "show delayprobe table",
  .short_help = "show delayprobe table",
  .function = delayprobe_show_table_fn,
};
VLIB_CLI_COMMAND (delayprobe_show_stats_command, static) = {
  .path = "show delayprobe statistics",
  .short_help = "show delayprobe statistics",
  .function = delayprobe_show_stats_fn,
};
/* *INDENT-ON* */

/*
 * Main-core process, sending an interrupt to the per worker input
 * process that spins the per worker timer wheel.
 */
static uword
timer_process (vlib_main_t *vm, vlib_node_runtime_t *rt, vlib_frame_t *f)
{
  uword *event_data = 0;
  vlib_main_t **worker_vms = 0, *worker_vm;
  delayprobe_main_t *fm = &delayprobe_main;

  /* Wait for Godot... */
  vlib_process_wait_for_event_or_clock (vm, 1e9);
  uword event_type = vlib_process_get_events (vm, &event_data);
  if (event_type != 1)
    clib_warning ("bogus kickoff event received, %d", event_type);
  vec_reset_length (event_data);

  int i;
  if (vlib_get_n_threads () == 0)
    vec_add1 (worker_vms, vm);
  else
    {
      for (i = 0; i < vlib_get_n_threads (); i++)
	{
	  worker_vm = vlib_get_main_by_index (i);
	  if (worker_vm)
	    vec_add1 (worker_vms, worker_vm);
	}
    }
  f64 sleep_duration = 0.1;

  while (1)
    {
      /* Send an interrupt to each timer input node */
      sleep_duration = 0.1;
      for (i = 0; i < vec_len (worker_vms); i++)
	{
	  worker_vm = worker_vms[i];
	  if (worker_vm)
	    {
	      vlib_node_set_interrupt_pending (worker_vm,
					       delayprobe_walker_node.index);
	      sleep_duration =
		(fm->expired_passive_per_worker[i] > 0) ? 1e-4 : 0.1;
	    }
	}
      vlib_process_suspend (vm, sleep_duration);
    }
  return 0; /* or not */
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (delayprobe_timer_node, static) = {
  .function = timer_process,
  .name = "delayprobe-timer-process",
  .type = VLIB_NODE_TYPE_PROCESS,
};
/* *INDENT-ON* */

#include <delayprobe/delayprobe.api.c>

/**
 * @brief Set up the API message handling tables
 * @param vm vlib_main_t * vlib main data structure pointer
 * @returns 0 to indicate all is well, or a clib_error_t
 */
static clib_error_t *
delayprobe_init (vlib_main_t *vm)
{
  delayprobe_main_t *fm = &delayprobe_main;
  vlib_thread_main_t *tm = &vlib_thread_main;
  clib_error_t *error = 0;
  u32 num_threads;
  int i;

  fm->vnet_main = vnet_get_main ();

  /* Ask for a correctly-sized block of API message decode slots */
  fm->msg_id_base = setup_message_id_table ();

  /* Set up time reference pair */
  fm->vlib_time_0 = vlib_time_now (vm);
  fm->nanosecond_time_0 = unix_time_now_nsec ();

  clib_memset (fm->template_reports, 0, sizeof (fm->template_reports));
  clib_memset (fm->template_size, 0, sizeof (fm->template_size));
  clib_memset (fm->template_per_flow, 0, sizeof (fm->template_per_flow));

  /* Decide how many worker threads we have */
  num_threads = 1 /* main thread */ + tm->n_threads;

  /* Allocate per worker thread vectors per flavour */
  for (i = 0; i < FLOW_N_VARIANTS; i++)
    {
      vec_validate (fm->context[i].buffers_per_worker, num_threads - 1);
      vec_validate (fm->context[i].frames_per_worker, num_threads - 1);
      vec_validate (fm->context[i].next_record_offset_per_worker,
		    num_threads - 1);
    }

  fm->active_timer = delayprobe_TIMER_ACTIVE;
  fm->passive_timer = delayprobe_TIMER_PASSIVE;

  return error;
}

VLIB_INIT_FUNCTION (delayprobe_init);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
