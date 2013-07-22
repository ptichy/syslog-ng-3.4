/*
 * Copyright (c) 2011-2012 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 2011-2012 Gergely Nagy <algernon@balabit.hu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#ifndef AFREDIS_H_INCLUDED
#define AFREDIS_H_INCLUDED

#include "driver.h"

typedef enum
  {
    AFREDIS_RCPT_TYPE_NONE,
    AFREDIS_RCPT_TYPE_TO,
    AFREDIS_RCPT_TYPE_CC,
    AFREDIS_RCPT_TYPE_BCC,
    AFREDIS_RCPT_TYPE_REPLY_TO,
    AFREDIS_RCPT_TYPE_SENDER,
  } afredis_rcpt_type_t;

LogDriver *afredis_dd_new(void);

void afredis_dd_set_host(LogDriver *d, const gchar *host);
void afredis_dd_set_port(LogDriver *d, gint port);

void afredis_dd_set_subject(LogDriver *d, const gchar *subject);
void afredis_dd_set_from(LogDriver *d, const gchar *phrase, const gchar *mbox);
void afredis_dd_add_rcpt(LogDriver *d, afredis_rcpt_type_t type,
                        const gchar *phrase, const gchar *mbox);
void afredis_dd_set_body(LogDriver *d, const gchar *body);
gboolean afredis_dd_add_header(LogDriver *d, const gchar *header,
                              const gchar *value);

#endif
