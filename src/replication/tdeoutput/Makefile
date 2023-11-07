#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/replication/tdeoutput
#
# IDENTIFICATION
#    src/replication/tdeoutput
#
#-------------------------------------------------------------------------

subdir = src/replication/tdeoutput
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	tdeoutput.o
PGFILEDESC = "tdeoutput - standard logical replication output plugin"
NAME = tdeoutput

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
