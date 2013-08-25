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
static int msgcounter = 0;


typedef struct
{
  LogDestDriver super;

  /* Shared between main/writer; only read by the writer, never
     written */
  gchar *host;
  gint port;
  
  redisContext *c;   
    
  gchar *key;
  gchar *value;
  GString *key_str;
  GString *value_str;

  time_t time_reopen;

  StatsCounterItem *dropped_messages;
  StatsCounterItem *stored_messages;
  
  LogTemplate *key_tmpl;
  LogTemplate *value_tmpl;

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
afredis_dd_set_key(LogDriver *d, const gchar *key)
{
  AFREDISDriver *self = (AFREDISDriver *)d;

  g_free(self->key);
  self->key = g_strdup(key);
}

void afredis_dd_set_value(LogDriver *d, const gchar *value)
{
  AFREDISDriver *self = (AFREDISDriver *)d;

  g_free(self->value);
  self->value = g_strdup(value);
}

/*
 * Utilities
 */ 

static gchar *
afredis_dd_format_stats_instance(AFREDISDriver *self)
{
  static gchar persist_name[1024];

  g_snprintf(persist_name, sizeof(persist_name),
             "redis,%s,%u", self->host, self->port);
  return persist_name;
}

static void
afredis_dd_suspend(AFREDISDriver *self)
{
  self->writer_thread_suspended = TRUE;
  g_get_current_time(&self->writer_thread_suspend_target);
  g_time_val_add(&self->writer_thread_suspend_target,
                 self->time_reopen * 1000000);
}

static gboolean
afredis_dd_connect(AFREDISDriver *self, gboolean reconnect)
{
  self->c = redisConnect(self->host, self->port);
  
  if (reconnect && !(self->c->err))
    return TRUE;  
  
  if (self->c->err)
  {            
    msg_error("REDIS server error, suspending",
              evt_tag_str("error", self->c->errstr),
              evt_tag_int("time_reopen", self->time_reopen),
              NULL);
    return FALSE;
  }
  else 
    msg_debug("Connecting to REDIS succeeded",
      evt_tag_str("driver", self->super.super.id), NULL);

  return TRUE;
}

/*
 * Worker thread
 */ 

static gboolean
afredis_worker_insert(AFREDISDriver *self)
{
  gboolean success;
  LogMessage *msg;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  redisReply *reply;
  
  gpointer args[] = { self, NULL, NULL };

  afredis_dd_connect(self, TRUE);
  
  success = log_queue_pop_head(self->queue, &msg, &path_options, FALSE, FALSE);
  if (!success)
    return TRUE;

  msg_set_context(msg);

  g_string_printf(self->str, "%s:%d", self->host, self->port);
  
  /* Defaults */  
  log_template_format(self->key_tmpl, msg, NULL, LTZ_SEND,
		      self->seq_num, NULL, self->key_str); 
    
  log_template_format(self->value_tmpl, msg, NULL, LTZ_SEND,
                             self->seq_num, NULL, self->value_str);
  
  msgcounter++;    
      
  if (self->c->err)
    {          
      success = FALSE;
    }
  else
    { 
      reply = redisCommand(self->c,"SET %s%d %s", self->key_str->str, msgcounter, self->value_str->str);
      
      msg_debug("REDIS result",
                evt_tag_str("key", self->key_str->str),
                evt_tag_str("value", self->value_str->str),
                NULL);
      success = TRUE;
      /*
      reply = redisCommand(self->c,"publish messages %s", self->value_str->str);
      reply = redisCommand(self->c,"publish %s %s", self->key_str->str, self->value_str->str);
            
      if ( reply->integer )
      {
	msg_debug("published to",
		  evt_tag_str("channel", self->key_str->str),
		  evt_tag_str("value", self->value_str->str),
		  NULL);
      }
      else
	msg_debug("no subscribed client on the following channel",
		  evt_tag_str("channel", self->key_str->str),
		  NULL);
	*/
      freeReplyObject(reply);      
      
    }   
  
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
  redisReply *reply;  
  
  msg_debug("Worker thread started",
            evt_tag_str("driver", self->super.super.id),
            NULL);

  self->str = g_string_sized_new(1024);
  self->key_str = g_string_sized_new(1024);
  self->value_str = g_string_sized_new(1024);
  
  afredis_dd_connect(self, FALSE);
  
  if ( !(self->c->err) )
  {
    reply = redisCommand(self->c,"PING");
  
    msg_verbose("PING REDIS",
	      evt_tag_str("PING:", reply->str),              
	      NULL);    
    
    reply = redisCommand(self->c,"SET %s %s", "testkey", "testmessage");
    freeReplyObject(reply);
  } 
  
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
  g_string_free(self->key_str, TRUE);
  g_string_free(self->value_str, TRUE);

  msg_debug("Worker thread finished",
            evt_tag_str("driver", self->super.super.id),
            NULL);

  return NULL;
}

/*
 * Main thread
 */

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

  self->queue = log_dest_driver_acquire_queue(&self->super, afredis_dd_format_stats_instance(self));
 
  if (!self->key) self->key = "$PROGRAM";
  if (!self->key_tmpl)
    {
	self->key_tmpl = log_template_new(cfg, "key");
	log_template_compile(self->key_tmpl, self->key, NULL);
    }
    
  if (!self->value) self->value = "$MSG";
  if (!self->value_tmpl)
    {
      self->value_tmpl = log_template_new(cfg, "value");
      log_template_compile(self->value_tmpl, self->value, NULL);
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

  return TRUE;
}

static gboolean
afredis_dd_deinit(LogPipe *s)
{
  AFREDISDriver *self = (AFREDISDriver *)s;
  redisReply *reply;
  
  reply = redisCommand(self->c, "save");
  
  if ( self->c->err )
    msg_error("Can't save the DB",
              evt_tag_str("error", self->c->errstr),              
              NULL);
  else
    msg_verbose("save DB",
	      evt_tag_str("save", reply->str),
	      NULL);
  
  freeReplyObject(reply);
    
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

  return TRUE;
}

static void
afredis_dd_free(LogPipe *d)
{
  AFREDISDriver *self = (AFREDISDriver *)d;

  g_mutex_free(self->suspend_mutex);
  g_cond_free(self->writer_thread_wakeup_cond);

  if (self->queue)
    log_queue_unref(self->queue);

  g_free(self->host);
  
  log_template_unref(self->key_tmpl);
  log_template_unref(self->value_tmpl);
  if ( !self->key_tmpl ) g_free(self->key);
  if ( !self->value_tmpl ) g_free(self->value);

  log_dest_driver_free(d);
}

static void
afredis_dd_queue(LogPipe *s, LogMessage *msg,
                const LogPathOptions *path_options, gpointer user_data)
{
  AFREDISDriver *self = (AFREDISDriver *)s;  
  
  LogPathOptions local_options;

  if (!path_options->flow_control_requested)
    path_options = log_msg_break_ack(msg, path_options, &local_options);

  log_msg_add_ack(msg, path_options);
  log_queue_push_tail(self->queue, log_msg_ref(msg), path_options);

  log_dest_driver_queue_method(s, msg, path_options, user_data);  
}

/*
 * Plugin glue.
 */

LogDriver *
afredis_dd_new(void)
{
  AFREDISDriver *self = g_new0(AFREDISDriver, 1);

  log_dest_driver_init_instance(&self->super);
  self->super.super.super.init = afredis_dd_init;
  self->super.super.super.deinit = afredis_dd_deinit;
  self->super.super.super.queue = afredis_dd_queue;
  self->super.super.super.free_fn = afredis_dd_free;

  afredis_dd_set_host((LogDriver *)self, "127.0.0.1");
  afredis_dd_set_port((LogDriver *)self, 6379);
  
  init_sequence_number(&self->seq_num);

  self->writer_thread_wakeup_cond = g_cond_new();
  self->suspend_mutex = g_mutex_new();

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
  plugin_register(cfg, &afredis_plugin, 1); 
  
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