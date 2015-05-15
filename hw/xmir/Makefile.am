bin_PROGRAMS = Xmir

if DRI2
SUBDIRS = dri2
endif

Xmir_CFLAGS =				\
	-I$(top_srcdir)/glamor		\
	-I$(srcdir)/dri2		\
	-I$(top_srcdir)/glx		\
	-I$(top_srcdir)/hw/xfree86/common		\
	-DHAVE_DIX_CONFIG_H		\
	$(XMIRMODULES_CFLAGS)			\
	$(DIX_CFLAGS)			\
	$(GLAMOR_CFLAGS)		\
	$(GBM_CFLAGS)

Xmir_SOURCES =				\
	xmir.c				\
	xmir-cursor.c			\
	xmir-input.c			\
	xmir-output.c			\
	xmir-cvt.c			\
	xmir-thread-proxy.c		\
	xmir.h				\
	$(top_srcdir)/Xi/stubs.c	\
	$(top_srcdir)/mi/miinitext.c

Xmir_LDADD =				\
	$(glamor_lib)			\
	$(aiglx_lib)			\
	$(XMIR_LIBS)			\
	$(XMIR_SYS_LIBS)		\
	$(XSERVER_SYS_LIBS)
Xmir_LDFLAGS = $(LD_EXPORT_SYMBOLS_FLAG)

if GLAMOR_EGL
Xmir_SOURCES += xmir-glamor.c

glamor_lib = $(top_builddir)/glamor/libglamor.la

if DRI2
Xmir_SOURCES += xmir-dri2.c
endif

Xmir_LDADD += $(GLAMOR_LIBS) $(GBM_LIBS) -lEGL -lGL
endif

if DRI2
Xmir_LDADD += dri2/libdri2.la
endif

if AIGLX_DRI_LOADER
aiglx_lib = $(top_builddir)/glx/libglxdri.la
if NO_UNDEFINED
aiglx_lib += $(LIBDRM_LIBS) $(PIXMAN_LIBS)
endif
endif

relink:
	$(AM_V_at)rm -f Xmir$(EXEEXT) && $(MAKE) Xmir$(EXEEXT)
