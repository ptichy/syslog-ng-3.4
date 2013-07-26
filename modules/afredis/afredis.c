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

#include <signal.h>

#include "afredis.h"
#include "afredis-parser.h"
#include "plugin.h"
#include "messages.h"
#include "misc.h"
#include "stats.h"
#include "logqueue.h"

#include "hiredis/hiredis.h"
#include "driver.h"

typedef struct
{
  gchar *name;
  gchar *template;
  LogTemplate *value;
} AFREDISHeader;

typedef struct
{
  gchar *phrase;
  gchar *address;
  afredis_rcpt_type_t type;
} AFREDISRecipient;

typedef struct
{
  LogDestDriver super;

  /* Shared between main/writer; only read by the writer, never
     written */
  gchar *host;
  gint port;

  gchar *subject;
  AFREDISRecipient *mail_from;
  GList *rcpt_tos;
  GList *headers;
  gchar *body;

  time_t time_reopen;

  StatsCounterItem *dropped_messages;
  StatsCounterItem *stored_messages;

  LogTemplate *subject_tmpl;
  LogTemplate *body_tmpl;

  /* Thread related stuff; shared */
  GThread *writer_thread;
  GMutex *suspend_mutex;
  GCond *writer_thread_wakeup_cond;

  gboolean writer_thread_terminate;
  gboolean writer_thread_suspended;
  GTimeVal writer_thread_suspend_target;

  LogQueue *queue;

  /* Writer-only stuff */
  gint32 seq_num;
  GString *str;
} AFREDISDriver;

static gchar *
afredis_wash_string (gchar *str)
{
  gint i;

  for (i = 0; i < strlen (str); i++)
    if (str[i] == '\n' ||
        str[i] == '\r')
      str[i] = ' ';

  return str;
}

/*
 * Configuration
 */

void
afredis_dd_set_host(LogDriver *d, const gchar *host)
{   
  AFREDISDriver *self = (AFREDISDriver *)d;
    
  g_free(self->host);
  self->host = g_strdup (host);
}

void
afredis_dd_set_port(LogDriver *d, gint port)
{
  AFREDISDriver *self = (AFREDISDriver *)d;

  self->port = (int)port;
}

void
afredis_dd_set_subject(LogDriver *d, const gchar *subject)
{
  AFREDISDriver *self = (AFREDISDriver *)d;

  g_free(self->subject);
  self->subject = g_strdup(subject);
}

void
afredis_dd_set_from(LogDriver *d, const gchar *phrase, const gchar *mbox)
{
  AFREDISDriver *self = (AFREDISDriver *)d;

  g_free(self->mail_from->phrase);
  g_free(self->mail_from->address);
  self->mail_from->phrase = afredis_wash_string(g_strdup(phrase));
  self->mail_from->address = afredis_wash_string(g_strdup(mbox));
}

void
afredis_dd_add_rcpt(LogDriver *d, afredis_rcpt_type_t type, const gchar *phrase,
                   const gchar *mbox)
{
  AFREDISDriver *self = (AFREDISDriver *)d;
  AFREDISRecipient *rcpt;

  rcpt = g_new0(AFREDISRecipient, 1);
  rcpt->phrase = afredis_wash_string(g_strdup(phrase));
  rcpt->address = afredis_wash_string(g_strdup(mbox));
  rcpt->type = type;

  self->rcpt_tos = g_list_append(self->rcpt_tos, rcpt);
}

void
afredis_dd_set_body(LogDriver *d, const gchar *body)
{
  AFREDISDriver *self = (AFREDISDriver *)d;

  g_free(self->body);
  self->body = g_strdup(body);
}

gboolean
afredis_dd_add_header(LogDriver *d, const gchar *header, const gchar *value)
{
  AFREDISDriver *self = (AFREDISDriver *)d;
  AFREDISHeader *h;

  if (!g_ascii_strcasecmp(header, "to") ||
      !g_ascii_strcasecmp(header, "cc") ||
      !g_ascii_strcasecmp(header, "bcc") ||
      !g_ascii_strcasecmp(header, "from") ||
      !g_ascii_strcasecmp(header, "sender") ||
      !g_ascii_strcasecmp(header, "reply-to") ||
      !g_ascii_strcasecmp(header, "date"))
    return FALSE;

  h = g_new0(AFREDISHeader, 1);
  h->name = g_strdup(header);
  h->template = g_strdup(value);

  self->headers = g_list_append(self->headers, h);

  return TRUE;
}

/*
 * Utilities
 */ /*
void
ignore_sigpipe (void)
{
  struct sigaction sa;

  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGPIPE, &sa, NULL);
}
*/
static gchar *
afredis_dd_format_stats_instance(AFREDISDriver *self)
{
  static gchar persist_name[1024];

  g_snprintf(persist_name, sizeof(persist_name),
             "redis,%s,%u", self->host, self->port);
  return persist_name;
}
/*
static void
afredis_dd_suspend(AFREDISDriver *self)
{
  self->writer_thread_suspended = TRUE;
  g_get_current_time(&self->writer_thread_suspend_target);
  g_time_val_add(&self->writer_thread_suspend_target,
                 self->time_reopen * 1000000);
}
*/
/*
 * Worker thread
 */ /*
static void
afredis_dd_msg_add_recipient(AFREDISRecipient *rcpt, redis_message_t message)
{
  gchar *hdr;

  redis_add_recipient(message, rcpt->address);

  switch (rcpt->type)
    {
    case AFREDIS_RCPT_TYPE_TO:
      hdr = "To";
      break;
    case AFREDIS_RCPT_TYPE_CC:
      hdr = "Cc";
      break;
    case AFREDIS_RCPT_TYPE_REPLY_TO:
      hdr = "Reply-To";
      break;
    default:
      return;
    }
  redis_set_header(message, hdr, rcpt->phrase, rcpt->address);
  redis_set_header_option(message, hdr, Hdr_OVERRIDE, 1);
}

static void
afredis_dd_msg_add_header(AFREDISHeader *hdr, gpointer user_data)
{
  AFREDISDriver *self = ((gpointer *)user_data)[0];
  LogMessage *msg = ((gpointer *)user_data)[1];
  redis_message_t message = ((gpointer *)user_data)[2];

  log_template_format(hdr->value, msg, NULL, LTZ_SEND, self->seq_num, NULL, self->str);

  redis_set_header(message, hdr->name, afredis_wash_string (self->str->str), NULL);
  redis_set_header_option(message, hdr->name, Hdr_OVERRIDE, 1);
}*/
/*
static void
afredis_dd_log_rcpt_status(redis_recipient_t rcpt, const char *mailbox,
                          gpointer user_data)
{
  const redis_status_t *status;

  status = redis_recipient_status(rcpt);
  msg_debug("REDIS recipient result",
            evt_tag_str("recipient", mailbox),
            evt_tag_int("code", status->code),
            evt_tag_str("text", status->text),
            NULL);
}

static void
afredis_dd_cb_event(redis_session_t session, int event, AFREDISDriver *self)
{
  switch (event)
    {
    case REDIS_EV_CONNECT:
      msg_verbose("Connected to REDIS server",
                  evt_tag_str("host", self->host),
                  evt_tag_int("port", self->port),
                  NULL);
      break;
    case REDIS_EV_MAILSTATUS:
    case REDIS_EV_RCPTSTATUS:
    case REDIS_EV_MESSAGEDATA:
    case REDIS_EV_MESSAGESENT:
      /* Ignore */ 
   //   break; 
   /* case REDIS_EV_DISCONNECT:
      msg_verbose("Disconnected from REDIS server",
                  evt_tag_str("host", self->host),
                  evt_tag_int("port", self->port),
                  NULL);
      break;
    default:
      msg_verbose("Unknown REDIS event",
                  evt_tag_int("event_id", event),
                  NULL);
      break;
    }
}

static void
afredis_dd_cb_monitor(const gchar *buf, gint buflen, gint writing,
                     AFREDISDriver *self)
{
  gchar fmt[32];

  g_snprintf(fmt, sizeof(fmt), "%%.%us", buflen);

  switch (writing)
    {
    case REDIS_CB_READING:
      msg_debug ("REDIS Session: SERVER",
                 evt_tag_printf("message", fmt, buf),
                 NULL);
      break;
    case REDIS_CB_WRITING:
      msg_debug("REDIS Session: CLIENT",
                evt_tag_printf("message", fmt, buf),
                NULL);
      break;
    case REDIS_CB_HEADERS:
      msg_debug("REDIS Session: HEADERS",
                evt_tag_printf("data", fmt, buf),
                NULL);
      break;
    }
}

static gboolean
afredis_worker_insert(AFREDISDriver *self)
{
  gboolean success;
  LogMessage *msg;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  redis_session_t session;
  redis_message_t message;
  gpointer args[] = { self, NULL, NULL };

  success = log_queue_pop_head(self->queue, &msg, &path_options, FALSE, FALSE);
  if (!success)
    return TRUE;

  msg_set_context(msg);

  session = redis_create_session();
  message = redis_add_message(session);

  g_string_printf(self->str, "%s:%d", self->host, self->port);
  redis_set_server(session, self->str->str);

  redis_set_eventcb(session, (redis_eventcb_t)afredis_dd_cb_event, (void *)self);
  redis_set_monitorcb(session, (redis_monitorcb_t)afredis_dd_cb_monitor,
                     (void *)self, 1);

  redis_set_reverse_path(message, self->mail_from->address);

  /* Defaults */  
 /* redis_set_header(message, "To", NULL, NULL);
  redis_set_header(message, "From", NULL, NULL);

  log_template_format(self->subject_tmpl, msg, NULL, LTZ_SEND,
                      self->seq_num, NULL, self->str);
  redis_set_header(message, "Subject", afredis_wash_string(self->str->str));
  redis_set_header_option(message, "Subject", Hdr_OVERRIDE, 1);

  /* Add recipients */  
/*  g_list_foreach(self->rcpt_tos, (GFunc)afredis_dd_msg_add_recipient, message);

  /* Add custom header (overrides anything set before, or in the
     body). */ 
 /* args[1] = msg;
  args[2] = message;
  g_list_foreach(self->headers, (GFunc)afredis_dd_msg_add_header, args);

  /* Set the body.
   *
   * We add a header to the body, otherwise libesmtp will not
   * recognise headers, and will append them to the end of the body.
   */ 
/*  g_string_assign(self->str, "X-Mailer: syslog-ng " VERSION "\r\n\r\n");
  log_template_append_format(self->body_tmpl, msg, NULL, LTZ_SEND,
                             self->seq_num, NULL, self->str);
  redis_set_message_str(message, self->str->str);

  if (!redis_start_session(session))
    {
      gchar error[1024];
      redis_strerror(redis_errno(), error, sizeof (error) - 1);

      msg_error("REDIS server error, suspending",
                evt_tag_str("error", error),
                evt_tag_int("time_reopen", self->time_reopen),
                NULL);
      success = FALSE;
    }
  else
    {
      const redis_status_t *status = redis_message_transfer_status(message);
      msg_debug("REDIS result",
                evt_tag_int("code", status->code),
                evt_tag_str("text", status->text),
                NULL);
      redis_enumerate_recipients(message, afredis_dd_log_rcpt_status, NULL);
    }
  redis_destroy_session(session);

  msg_set_context(NULL);

  if (success)
    {
      stats_counter_inc(self->stored_messages);
      step_sequence_number(&self->seq_num);
      log_msg_ack(msg, &path_options);
      log_msg_unref(msg);
    }
  else
    {
      log_queue_push_head(self->queue, msg, &path_options);
    }

  return success;
}

static void
afredis_dd_message_became_available_in_the_queue(gpointer user_data)
{
  AFREDISDriver *self = (AFREDISDriver *) user_data;

  g_mutex_lock(self->suspend_mutex);
  g_cond_signal(self->writer_thread_wakeup_cond);
  g_mutex_unlock(self->suspend_mutex);
}

static gpointer
afredis_worker_thread(gpointer arg)
{
  AFREDISDriver *self = (AFREDISDriver *)arg;

  msg_debug("Worker thread started",
            evt_tag_str("driver", self->super.super.id),
            NULL);

  self->str = g_string_sized_new(1024);

  ignore_sigpipe();

  while (!self->writer_thread_terminate)
    {
      g_mutex_lock(self->suspend_mutex);
      if (self->writer_thread_suspended)
        {
          g_cond_timed_wait(self->writer_thread_wakeup_cond,
                            self->suspend_mutex,
                            &self->writer_thread_suspend_target);
          self->writer_thread_suspended = FALSE;
          g_mutex_unlock(self->suspend_mutex);
        }
      else if (!log_queue_check_items(self->queue, NULL, afredis_dd_message_became_available_in_the_queue, self, NULL))
        {
          g_cond_wait(self->writer_thread_wakeup_cond, self->suspend_mutex);
          g_mutex_unlock(self->suspend_mutex);
        }
      else
        {
          g_mutex_unlock(self->suspend_mutex);
        }

      if (self->writer_thread_terminate)
        break;

      if (!afredis_worker_insert (self))
        {
          afredis_dd_suspend(self);
        }
    }

  g_string_free(self->str, TRUE);

  msg_debug("Worker thread finished",
            evt_tag_str("driver", self->super.super.id),
            NULL);

  return NULL;
}
*/
/*
 * Main thread
 */
/*
static void
afredis_dd_start_thread(AFREDISDriver *self)
{
  self->writer_thread = create_worker_thread(afredis_worker_thread, self, TRUE, NULL);
}

static void
afredis_dd_stop_thread(AFREDISDriver *self)
{
  self->writer_thread_terminate = TRUE;
  g_mutex_lock(self->suspend_mutex);
  g_cond_signal(self->writer_thread_wakeup_cond);
  g_mutex_unlock(self->suspend_mutex);
  g_thread_join(self->writer_thread);
}

static void
afredis_dd_init_header(AFREDISHeader *hdr, GlobalConfig *cfg)
{
  if (!hdr->value)
    {
      hdr->value = log_template_new(cfg, hdr->name);
      log_template_compile(hdr->value, hdr->template, NULL);
    }
}
*/
static gboolean
afredis_dd_init(LogPipe *s)
{
  AFREDISDriver *self = (AFREDISDriver *)s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (cfg)
    self->time_reopen = cfg->time_reopen;

  msg_verbose("Initializing REDIS destination",
              evt_tag_str("host", self->host),
              evt_tag_int("port", self->port),
              NULL);
/*
  self->queue = log_dest_driver_acquire_queue(&self->super, afredis_dd_format_stats_instance(self));

  g_list_foreach(self->headers, (GFunc)afredis_dd_init_header, cfg);
  if (!self->subject_tmpl)
    {
      self->subject_tmpl = log_template_new(cfg, "subject");
      log_template_compile(self->subject_tmpl, self->subject, NULL);
    }
  if (!self->body_tmpl)
    {
      self->body_tmpl = log_template_new(cfg, "body");
      log_template_compile(self->body_tmpl, self->body, NULL);
    }

  stats_lock();
  stats_register_counter(0, SCS_REDIS | SCS_DESTINATION, self->super.super.id,
                         afredis_dd_format_stats_instance(self),
                         SC_TYPE_STORED, &self->stored_messages);
  stats_register_counter(0, SCS_REDIS | SCS_DESTINATION, self->super.super.id,
                         afredis_dd_format_stats_instance(self),
                         SC_TYPE_DROPPED, &self->dropped_messages);
  stats_unlock();

  afredis_dd_start_thread(self);
*/
  return TRUE;
}

static gboolean
afredis_dd_deinit(LogPipe *s)
{
  AFREDISDriver *self = (AFREDISDriver *)s;
/*
  afredis_dd_stop_thread(self);
  log_queue_reset_parallel_push(self->queue);

  stats_lock();
  stats_unregister_counter(SCS_REDIS | SCS_DESTINATION, self->super.super.id,
                           afredis_dd_format_stats_instance(self),
                           SC_TYPE_STORED, &self->stored_messages);
  stats_unregister_counter(SCS_REDIS | SCS_DESTINATION, self->super.super.id,
                           afredis_dd_format_stats_instance(self),
                           SC_TYPE_DROPPED, &self->dropped_messages);
  stats_unlock();
*/
  return TRUE;
}

static void
afredis_dd_free(LogPipe *d)
{
  AFREDISDriver *self = (AFREDISDriver *)d;
  GList *l;
/*
  g_mutex_free(self->suspend_mutex);
  g_cond_free(self->writer_thread_wakeup_cond);

  if (self->queue)
    log_queue_unref(self->queue);
*/
  g_free(self->host);
  g_free(self->mail_from->phrase);
  g_free(self->mail_from->address);
  g_free(self->mail_from);
  log_template_unref(self->subject_tmpl);
  log_template_unref(self->body_tmpl);
  g_free(self->body);
  g_free(self->subject);
  g_string_free(self->str, TRUE);

  l = self->rcpt_tos;
  while (l)
    {
      AFREDISRecipient *rcpt = (AFREDISRecipient *)l->data;
      g_free(rcpt->address);
      g_free(rcpt->phrase);
      g_free(rcpt);
      l = g_list_delete_link(l, l);
    }

  l = self->headers;
  while (l)
    {
      AFREDISHeader *hdr = (AFREDISHeader *)l->data;
      g_free(hdr->name);
      g_free(hdr->template);
      log_template_unref(hdr->value);
      g_free(hdr);
      l = g_list_delete_link(l, l);
    }

  log_dest_driver_free(d);
}

static void
afredis_dd_queue(LogPipe *s, LogMessage *msg,
                const LogPathOptions *path_options, gpointer user_data)
{
  AFREDISDriver *self = (AFREDISDriver *)s;
  redisReply *reply;
  redisContext *c;
  
  LogPathOptions local_options;
/*
  if (!path_options->flow_control_requested)
    path_options = log_msg_break_ack(msg, path_options, &local_options);

  log_msg_add_ack(msg, path_options);
  log_queue_push_tail(self->queue, log_msg_ref(msg), path_options);

  log_dest_driver_queue_method(s, msg, path_options, user_data);
  */

  
  
  c = redisConnect("127.0.0.1", 6379);
    if (c->err)
    {
        printf("Connection error: %s\n", c->errstr);
        exit(1);
    }
    reply = redisCommand(c,"PING");
    printf("PING: %s\n", reply->str);
   reply = redisCommand(c,"SET %s %s", "message", msg);
    freeReplyObject(reply);
}

/*
 * Plugin glue.
 */

LogDriver *
afredis_dd_new(void)
{
  AFREDISDriver *self = g_new0(AFREDISDriver, 1);

  log_dest_driver_init_instance(&self->super);
  //self->super.super.super.init = afredis_dd_init;
  //self->super.super.super.deinit = afredis_dd_deinit;
  //self->super.super.super.queue = afredis_dd_queue;
  //self->super.super.super.free_fn = afredis_dd_free;

  afredis_dd_set_host((LogDriver *)self, "127.0.0.1");
  afredis_dd_set_port((LogDriver *)self, 6379);

  self->mail_from = g_new0(AFREDISRecipient, 1);

  //init_sequence_number(&self->seq_num);

  //self->writer_thread_wakeup_cond = g_cond_new();
  //self->suspend_mutex = g_mutex_new();

  return (LogDriver *)self;
}
 
extern CfgParser afredis_dd_parser;

static Plugin afredis_plugin =
{
  .type = LL_CONTEXT_DESTINATION,
  .name = "redis",
  .parser = &afredis_parser,
};

gboolean
afredis_module_init(GlobalConfig *cfg, CfgArgs *args)
{
  redisReply *reply;
  redisContext *c;
  plugin_register(cfg, &afredis_plugin, 1); 
  c = redisConnect("127.0.0.1", 6379);
    if (c->err)
    {
        printf("Connection error: %s\n", c->errstr);
        exit(1);
    }
    reply = redisCommand(c,"PING");
    printf("PING: %s\n", reply->str);
    
   reply = redisCommand(c,"SET %s %s", "testkey", "testmessage");
    freeReplyObject(reply);
  return TRUE;
}

const ModuleInfo module_info =
{
  .canonical_name = "afredis",
  .version = VERSION,
  .description = "The afredis module provides REDIS destination support for syslog-ng.",
  .core_revision = SOURCE_REVISION,
  .plugins = &afredis_plugin,
  .plugins_len = 1,
};
