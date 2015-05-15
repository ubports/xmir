noinst_LTLIBRARIES = libdri2.la

AM_CFLAGS = \
	-DHAVE_DIX_CONFIG_H	\
	-I$(top_srcdir)/hw/xmir	\
	$(DIX_CFLAGS)		\
	$(XMIRMODULES_CFLAGS)

libdri2_la_SOURCES = \
	dri2.c \
	dri2.h \
	dri2ext.c \
	dri2int.h

