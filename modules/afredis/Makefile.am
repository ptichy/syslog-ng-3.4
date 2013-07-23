moduledir = @moduledir@
AM_CFLAGS = -I$(top_srcdir)/lib -I../../lib

export top_srcdir

if ENABLE_REDIS
module_LTLIBRARIES = libafredis.la

libafredis_la_CFLAGS = $(LIBHIREDIS_CFLAGS)
libafredis_la_SOURCES = afredis-grammar.y afredis.c afredis.h afredis-parser.c afredis-parser.h
libafredis_la_LIBADD = $(LIBHIREDIS_LIBS) $(MODULE_DEPS_LIBS)
libafredis_la_LDFLAGS = $(MODULE_LDFLAGS)

endif

BUILT_SOURCES = afredis-grammar.y afredis-grammar.c afredis-grammar.h
EXTRA_DIST = $(BUILT_SOURCES) afredis-grammar.ym

include $(top_srcdir)/build/lex-rules.am
