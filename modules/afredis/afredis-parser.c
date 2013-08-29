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

#include "afredis.h"
#include "cfg-parser.h"
#include "afredis-grammar.h"

extern int afredis_debug;
int afredis_parse(CfgLexer *lexer, LogDriver **instance, gpointer arg);

static CfgLexerKeyword afredis_keywords[] = {
  { "redis",			KW_REDIS },
  { "host",			KW_HOST },
  { "port",			KW_PORT },
  { "key",			KW_KEY },
  { "value",			KW_VALUE },
  { "command",			KW_COMMAND },  
  { NULL }
};

CfgParser afredis_parser =
{
#if ENABLE_DEBUG
  .debug_flag = &afredis_debug,
#endif
  .name = "afredis",
  .keywords = afredis_keywords,
  .parse = (int (*)(CfgLexer *lexer, gpointer *instance, gpointer)) afredis_parse,
  .cleanup = (void (*)(gpointer)) log_pipe_unref,
};

CFG_PARSER_IMPLEMENT_LEXER_BINDING(afredis_, LogDriver **)
