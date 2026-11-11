/*
 * IOAM Direct Export (DEX) Option-Type with on-path delay,
 * draft-netana-ippm-ioam-on-path-delay.
 *
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
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ip/ip6.h>
#include <vnet/ip/ip6_hop_by_hop.h>

#include <ioam/lib-trace/trace_util.h>
#include <ioam/lib-trace/trace_config.h>
#include <ioam/encap/ip6_ioam_dex.h>

STATIC_ASSERT (IOAM_DEX_TS_SEC_OFFSET == sizeof (ioam_dex_option_t),
	       "core assumes Timestamp Seconds follows the fixed DEX fields");

#define IOAM_DEX_OPTION_SIZE (sizeof (ioam_dex_option_t) + 2 * sizeof (u32))

int
ip6_ioam_dex_get_sizeof (u32 * result)
{
  *result = IOAM_DEX_OPTION_SIZE;
  return 0;
}

int
ip6_ioam_dex_rewrite (u8 * rewrite_string, u8 * rewrite_size)
{
  trace_profile *profile = trace_profile_find ();
  ioam_dex_option_t *dex = (ioam_dex_option_t *) rewrite_string;

  if (PREDICT_FALSE (!rewrite_string || !profile || !profile->valid))
    return -1;

  dex->hdr.type = HBH_OPTION_TYPE_IOAM_DEX;
  /* Opt Data Len excludes the option type and length octets */
  dex->hdr.length = IOAM_DEX_OPTION_SIZE - 2;
  dex->hdr.reserved = 0;
  dex->hdr.ioam_type = IOAM_TYPE_DEX;
  dex->namespace_id = clib_host_to_net_u16 (profile->namespace_id);
  dex->flags = 0;
  dex->ext_flags = IOAM_DEX_EXT_FLAG_TS_SEC | IOAM_DEX_EXT_FLAG_TS_FRAC;
  dex->trace_type = IOAM_SET_TRACETYPE (profile->trace_type);
  /* Timestamps are stamped per packet by the encap node */
  clib_memset (rewrite_string + sizeof (*dex), 0, 2 * sizeof (u32));
  *rewrite_size = IOAM_DEX_OPTION_SIZE;

  clib_warning ("DEX rewrite: size %u len %u nsid %u ext 0x%x tt 0x%x",
		(u32) IOAM_DEX_OPTION_SIZE, dex->hdr.length,
		profile->namespace_id, dex->ext_flags, profile->trace_type);
  return 0;
}

int
ip6_ioam_dex_profile_setup (void)
{
  ip6_hop_by_hop_ioam_main_t *hm = &ip6_hop_by_hop_ioam_main;
  trace_profile *profile = trace_profile_find ();

  hm->dex_enabled = 1;
  hm->dex_node_type = profile->node_type;
  hm->dex_namespace_id = profile->namespace_id;
  hm->dex_ts_frac_us = (profile->ts_format == IOAM_TSP_MICROSECONDS);

  clib_warning ("DEX setup: node_type %s dex_enabled %u ts_frac_us %u nsid %u",
		ioam_dex_node_type_str (hm->dex_node_type), hm->dex_enabled,
		hm->dex_ts_frac_us, hm->dex_namespace_id);
  return 0;
}

void
ip6_ioam_dex_profile_cleanup (void)
{
  ip6_hop_by_hop_ioam_main_t *hm = &ip6_hop_by_hop_ioam_main;

  hm->dex_enabled = 0;
  hm->dex_node_type = 0;
  hm->dex_ts_frac_us = 0;
  hm->dex_namespace_id = 0;
}

void
ioam_dex_show_profile (vlib_main_t * vm)
{
  ip6_hop_by_hop_ioam_main_t *hm = &ip6_hop_by_hop_ioam_main;
  trace_profile *p = trace_profile_find ();
  u8 *s = 0;

  if (p && p->valid && (p->option_type & IOAM_OPTION_DEX))
    {
      s = format (s, " IOAM DEX PROFILE\n");
      s = format (s, "        Namespace           : %d\n", p->namespace_id);
      s = format (s, "        Node type           : %s\n",
		  ioam_dex_node_type_str (p->node_type));
      s = format (s, "        Trace type          : 0x%x (on-path delay)\n",
		  p->trace_type);
      s = format (s, "        Extension flags     : 0x%x (timestamp sec + fraction)\n",
		  IOAM_DEX_EXT_FLAG_TS_SEC | IOAM_DEX_EXT_FLAG_TS_FRAC);
      s = format (s, "        Timestamp format    : %s\n",
		  p->ts_format == IOAM_TSP_MICROSECONDS ?
		  "POSIX (fraction in us)" : "PTP truncated (fraction in ns)");
      s = format (s, "        HBH option type     : 0x%x, IOAM type %d\n",
		  HBH_OPTION_TYPE_IOAM_DEX, IOAM_TYPE_DEX);
    }
  else
    {
      s = format (s, " IOAM DEX PROFILE not configured\n");
    }

  s = format (s, " Runtime state (ip6_hop_by_hop_ioam_main)\n");
  s = format (s, "        dex_enabled         : %d\n", hm->dex_enabled);
  s = format (s, "        dex_node_type       : %s\n",
	      hm->dex_enabled ? ioam_dex_node_type_str (hm->dex_node_type) : "-");
  s = format (s, "        dex_namespace_id    : %d\n", hm->dex_namespace_id);
  s = format (s, "        dex_ts_frac_us      : %d\n", hm->dex_ts_frac_us);
  s = format (s, "        dex_ts_offset       : %d (0 = no rewrite)\n",
	      hm->dex_ts_offset);

  vlib_cli_output (vm, "%v", s);
  vec_free (s);
}

static clib_error_t *
show_ioam_dex_profile_command_fn (vlib_main_t * vm, unformat_input_t * input,
				  vlib_cli_command_t * cmd)
{
  ioam_dex_show_profile (vm);
  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (show_ioam_dex_profile_command, static) =
{
  .path = "show ioam-dex profile",
  .short_help = "show ioam-dex profile",
  .function = show_ioam_dex_profile_command_fn,
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
