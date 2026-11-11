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

#ifndef PLUGINS_IOAM_PLUGIN_IOAM_ENCAP_IP6_IOAM_DEX_H_
#define PLUGINS_IOAM_PLUGIN_IOAM_ENCAP_IP6_IOAM_DEX_H_

#include <vnet/ip/ip6_hop_by_hop_packet.h>
#include <ioam/lib-trace/trace_util.h>

/* RFC 9326 Extension-Flags; the timestamp bits are placeholders for TBD1/TBD2 */
#define IOAM_DEX_EXT_FLAG_FLOW_ID  0x80
#define IOAM_DEX_EXT_FLAG_SEQ_NUM  0x40
#define IOAM_DEX_EXT_FLAG_TS_SEC   0x20
#define IOAM_DEX_EXT_FLAG_TS_FRAC  0x10

/* DEX Option-Type as built by the encap node: Timestamp Sec + Fraction follow opt_fields[0] */
typedef CLIB_PACKED (struct {
  ip6_hop_by_hop_option_t hdr;
  u16 namespace_id;
  u8 flags;
  u8 ext_flags;
  u32 trace_type;		/* 24-bit trace type, last octet reserved */
  u32 opt_fields[0];
}) ioam_dex_option_t;

static inline const char *
ioam_dex_node_type_str (u8 node_type)
{
  if (node_type == IOAM_NODE_ENCAP)
    return "Encap";
  if (node_type == IOAM_NODE_TRANSIT)
    return "Transit";
  if (node_type == IOAM_NODE_DECAP)
    return "Decap";
  return "Unknown";
}

/* Copy the active DEX profile into ip6_hop_by_hop_ioam_main */
int ip6_ioam_dex_profile_setup (void);

/* Clear profile-derived DEX state (dex_ts_offset follows the rewrite) */
void ip6_ioam_dex_profile_cleanup (void);

/* Size in bytes of the DEX option written into the HBH rewrite */
int ip6_ioam_dex_get_sizeof (u32 * result);

/* Write the DEX option (timestamps zeroed) at rewrite_string */
int ip6_ioam_dex_rewrite (u8 * rewrite_string, u8 * rewrite_size);

/* Print the DEX profile and the runtime state used by the data path */
void ioam_dex_show_profile (vlib_main_t * vm);

#endif /* PLUGINS_IOAM_PLUGIN_IOAM_ENCAP_IP6_IOAM_DEX_H_ */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
