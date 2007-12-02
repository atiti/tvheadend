/*
 *  tvheadend, MPEG transport stream demuxer
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE
#include <stdlib.h>

#include <pthread.h>

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#include <libhts/htscfg.h>

#include <ffmpeg/avcodec.h>

#include "tvhead.h"
#include "dispatch.h"
#include "teletext.h"
#include "transports.h"
#include "subscriptions.h"
#include "pes.h"
#include "psi.h"
#include "buffer.h"
#include "tsdemux.h"
#include "plugin.h"

static int 
ts_reassembly_pes(th_transport_t *t, th_stream_t *st, uint8_t *data, int len)
{
  int l2;

  if(len < 9) 
    return -1;

  if(data[0] != 0 || data[1] != 0 || data[2] != 1)
    return -1;

  l2 = (data[4] << 8) | data[5];
  
  len  -= 6;
  data += 6;
 
  return pes_packet_input(t, st, data, len);
}

/*
 * TS packet reassembly
 */
static void
ts_reassembly(th_transport_t *t, th_stream_t *st, uint8_t *data, int len, 
	      int pusi, int err)
{
  if(pusi) {
    if(ts_reassembly_pes(t, st, st->st_buffer, st->st_buffer_ptr) == 0)
      st->st_buffer = NULL; /* Memory was consumed by pes_packet_input() */

    st->st_buffer_ptr = 0;
    st->st_buffer_errors = 0;

    if(st->st_buffer == NULL) {

      if(st->st_buffer_size < 1000)
	st->st_buffer_size = 4000;

      st->st_buffer = malloc(st->st_buffer_size);
    }
  }

  if(st->st_buffer == NULL)
    return;

  st->st_buffer_errors += err;

  if(st->st_buffer_ptr + len >= st->st_buffer_size) {
    st->st_buffer_size += len * 4;
    st->st_buffer = realloc(st->st_buffer, st->st_buffer_size);
  }
  
  memcpy(st->st_buffer + st->st_buffer_ptr, data, len);
  st->st_buffer_ptr += len;
}

/**
 * Code for dealing with a complete section
 */

static void
got_section(th_transport_t *t, th_stream_t *st)
{
  th_plugin_t *p;
  LIST_FOREACH(p, &th_plugins, thp_link)
    if(p->thp_psi_table != NULL)
      p->thp_psi_table(p, &t->tht_plugin_aux, t, st, 
		       st->st_section->ps_data, st->st_section->ps_offset);

  if(st->st_got_section != NULL)
    st->st_got_section(t, st, st->st_section->ps_data,
		       st->st_section->ps_offset);
}



/*
 * Process transport stream packets
 */
void
ts_recv_packet(th_transport_t *t, int pid, uint8_t *tsb, int doplugin)
{
  th_stream_t *st = NULL;
  th_subscription_t *s;
  int cc, err = 0, afc, afl = 0;
  int len, pusi;
  pluginaux_t *pa;
  th_plugin_t *p;

  LIST_FOREACH(st, &t->tht_streams, st_link) 
    if(st->st_pid == pid)
      break;

  if(st == NULL)
    return;

  if(doplugin) {
    LIST_FOREACH(pa, &t->tht_plugin_aux, pa_link) {
      p = pa->pa_plugin;
      if(p->thp_tsb_process != NULL)
	if(p->thp_tsb_process(pa, t, st, tsb))
	  return;
    }
  }
    
  avgstat_add(&t->tht_rate, 188, dispatch_clock);
  if((tsb[3] >> 6) & 3)
    return; /* channel is encrypted */


  LIST_FOREACH(s, &t->tht_subscriptions, ths_transport_link)
    if(s->ths_raw_input != NULL)
      s->ths_raw_input(s, tsb, 188, st, s->ths_opaque);

  afc = (tsb[3] >> 4) & 3;

  if(afc & 1) {
    cc = tsb[3] & 0xf;
    if(st->st_cc_valid && cc != st->st_cc) {
      /* Incorrect CC */
      avgstat_add(&t->tht_cc_errors, 1, dispatch_clock);
      avgstat_add(&st->st_cc_errors, 1, dispatch_clock);
      err = 1;
    }
    st->st_cc_valid = 1;
    st->st_cc = (cc + 1) & 0xf;
  }


  if(afc & 2)
    afl = tsb[4] + 1;
  
  pusi = tsb[1] & 0x40;

  afl += 4;

  switch(st->st_type) {

  case HTSTV_TABLE:
    if(st->st_section == NULL)
      st->st_section = calloc(1, sizeof(struct psi_section));

    if(err || afl >= 188) {
      st->st_section->ps_offset = -1; /* hold parser until next pusi */
      break;
    }

    if(pusi) {
      len = tsb[afl++];
      if(len > 0) {
	if(len > 188 - afl)
	  break;
	if(!psi_section_reassemble(st->st_section, tsb + afl, len, 0,
				   st->st_section_docrc))
	  got_section(t, st);
	afl += len;
      }
    }
    
    if(!psi_section_reassemble(st->st_section, tsb + afl, 188 - afl, pusi,
			       st->st_section_docrc))
      got_section(t, st);
    break;

  case HTSTV_TELETEXT:
    teletext_input(t, tsb);
    break;

  default:
    if(afl > 188)
      break;

    ts_reassembly(t, st, tsb + afl, 188 - afl, pusi, err);
    break;
  }
}