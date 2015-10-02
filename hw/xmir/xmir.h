/*
 * Copyright © 2015 Canonical Ltd
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of the
 * copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifndef XMIR_H
#define XMIR_H

#include <dix-config.h>
#include <xorg-server.h>

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <mir_toolkit/mir_client_library.h>

#include <X11/X.h>

#include <fb.h>
#include <input.h>
#include <dix.h>
#include <randrstr.h>
#include <exevents.h>
#include <dri2.h>

#define MESA_EGL_NO_X11_HEADERS
#include <epoxy/egl.h>
#include <epoxy/gl.h>

#define MIR_MAX_BUFFER_AGE 3

struct xmir_window;

struct xmir_screen {
    ScreenPtr screen;

    int depth, rootless, windowed, doubled;
    enum {glamor_off=0, glamor_dri, glamor_egl, glamor_egl_sync} glamor;

    CreateScreenResourcesProcPtr CreateScreenResources;
    CloseScreenProcPtr CloseScreen;
    CreateWindowProcPtr CreateWindow;
    DestroyWindowProcPtr DestroyWindow;
    RealizeWindowProcPtr RealizeWindow;
    UnrealizeWindowProcPtr UnrealizeWindow;

    struct xorg_list output_list;
    struct xorg_list input_list;
    struct xorg_list damage_window_list;

    MirConnection *conn;
    MirDisplayConfiguration *display;
    MirPlatformPackage platform;

    struct xmir_marshall_handler *input_handler;
    struct xmir_marshall_handler *hotplug_event_handler;
    struct xmir_marshall_handler *submit_rendering_handler;

    /* Bookkeeping for eglSwapBuffers */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    int alive, damage_all;
    struct xorg_list swap_list;
    void *swap_surface;

    char *device_name, *driver_name;
    int drm_fd, dpms_on;
    void *egl_display, *egl_context, *swap_context;
    struct gbm_device *gbm;
    struct glamor_context *glamor_ctx;
    void *egl_surface;

    MirPixelFormat depth24_pixel_format, depth32_pixel_format;
    Bool do_own_wm;
    Bool flatten;
    Bool neverclose;
    MirSurface *neverclosed;
    struct xorg_list flattened_list;
    struct xmir_window *flatten_top;
    WindowPtr last_focus;

    DRI2InfoRec dri2;
};

struct xmir_pixmap {
    unsigned int fake_back;
    struct gbm_bo *bo;
    void *image;
};

struct xmir_window {
    struct xmir_screen *xmir_screen;
    MirSurface *surface;
    WindowPtr window;
    DamagePtr damage;
    RegionRec region;

    struct xorg_list link_damage;
    RegionRec past_damage[MIR_MAX_BUFFER_AGE];
    int damage_index, orientation;
    unsigned int has_free_buffer:1;

    struct xorg_list link_flattened;

    void *egl_surface, *image;
    PixmapPtr back_pixmap, front_pixmap, reuse_pixmap;

    struct xmir_flip {
        ClientPtr client;
        int type;
        DrawablePtr draw;
        DRI2SwapEventPtr func;
        void *data;
        struct xorg_list entry;
    } flip;
};

struct xmir_input {
    DeviceIntPtr pointer;
    DeviceIntPtr keyboard;
    DeviceIntPtr touch;
    struct xmir_screen *xmir_screen;
    struct xmir_window *focus_window;
    uint32_t id;
    int touch_id;
    struct xorg_list link;
    CursorPtr x_cursor;
};

struct xmir_output {
    struct xorg_list link;
    struct wl_output *output;
    struct xmir_screen *xmir_screen;
    RROutputPtr randr_output;
    RRCrtcPtr randr_crtc;
    int32_t x, y, width, height;
    Rotation rotation;
};

struct xmir_window *xmir_window_get(WindowPtr window);
struct xmir_screen *xmir_screen_get(ScreenPtr screen);
struct xmir_pixmap *xmir_pixmap_get(PixmapPtr pixmap);
void xmir_window_resize(struct xmir_window *, unsigned w, unsigned h);
void xmir_pixmap_set(PixmapPtr pixmap, struct xmir_pixmap *xmir_pixmap);

void xmir_handle_surface_event(struct xmir_window *, MirSurfaceAttrib, int);
void xmir_close_surface(struct xmir_window *);

/* xmir-input.c */
Bool xmir_screen_init_cursor(struct xmir_screen *xmir_screen);
void xmir_input_set_cursor(struct xmir_input *xmir_input);

/* xmir-output.c */
Bool xmir_screen_init_output(struct xmir_screen *xmir_screen);
void xmir_output_destroy(struct xmir_output *xmir_output);
Bool xmir_output_dpms(struct xmir_screen *xmir_screen, int dpms);
void xmir_output_handle_resize(struct xmir_window *, int, int);
void xmir_output_handle_orientation(struct xmir_window *, MirOrientation);

/* xmir-cvt.c */
RRModePtr xmir_cvt(int HDisplay, int VDisplay, float VRefresh, Bool Reduced, Bool Interlaced);

/* xmir-dri2.c */
Bool xmir_dri2_screen_init(struct xmir_screen *xmir_screen);

/* xmir-glamor.c */
Bool xmir_glamor_init(struct xmir_screen *xmir_screen);
Bool xmir_screen_init_glamor(struct xmir_screen *xmir_screen);
void xmir_glamor_fini(struct xmir_screen *xmir_screen);

PixmapPtr xmir_glamor_win_get_back(struct xmir_screen *, struct xmir_window *, DrawablePtr);
void xmir_glamor_copy(struct xmir_screen *, struct xmir_window *, RegionPtr);
void xmir_glamor_realize_window(struct xmir_screen *, struct xmir_window *, WindowPtr);
void xmir_glamor_unrealize_window(struct xmir_screen *, struct xmir_window *, WindowPtr);

struct glamor_pixmap_private;
void xmir_glamor_copy_egl_common(DrawablePtr, PixmapPtr src, struct glamor_pixmap_private *,
                                 BoxPtr, int width, int height, int dx, int dy, int orientation);

/* xmir-thread-proxy.c */
void xmir_init_thread_to_eventloop(void);
void xmir_fini_thread_to_eventloop(void);

struct xmir_marshall_handler *xmir_register_handler(void (*)(void *), size_t);

void xmir_post_to_eventloop(struct xmir_marshall_handler *handler, void *msg);
void xmir_process_from_eventloop(void);

/* xmir-input.c */
typedef struct XMirEventContext {
    struct xmir_screen *xmir_screen;
    struct xmir_window *xmir_window;
    const MirEvent *ev;
} XMirEventContext;

void xmir_handle_input_in_main_thread(void *vctx);
void xmir_surface_handle_event(MirSurface *surface, MirEvent const* ev, void *context);

#define XMIR_CREATE_PIXMAP_USAGE_FLIP 0x10000000

#define XORG_VERSION_NUMERIC(major,minor,patch,snap,dummy) \
	(((major) * 10000000) + ((minor) * 100000) + ((patch) * 1000) + snap)

#endif
