/*
 * Copyright © 2015-2017 Canonical Ltd
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
#include "xf86.h"

#include "xmir.h"

#include <stdio.h>
#include <signal.h>
#include <dlfcn.h>

#include <selection.h>
#include <micmap.h>
#include <misyncshm.h>
#include <glx_extinit.h>
#include <X11/Xatom.h>

#include <mir_toolkit/version.h>
#include <mir_toolkit/mir_surface.h>

#include "compint.h"
#include "dri2.h"
#include "glxserver.h"
#include "glamor_priv.h"
#include "dpmsproc.h"

static struct {
    Atom UTF8_STRING;
    Atom _NET_WM_NAME;
    Atom WM_PROTOCOLS;
    Atom WM_DELETE_WINDOW;
    Atom _NET_WM_WINDOW_TYPE;
    Atom _NET_WM_WINDOW_TYPE_DESKTOP;
    Atom _NET_WM_WINDOW_TYPE_DOCK;
    Atom _NET_WM_WINDOW_TYPE_TOOLBAR;
    Atom _NET_WM_WINDOW_TYPE_MENU;
    Atom _NET_WM_WINDOW_TYPE_UTILITY;
    Atom _NET_WM_WINDOW_TYPE_SPLASH;
    Atom _NET_WM_WINDOW_TYPE_DIALOG;
    Atom _NET_WM_WINDOW_TYPE_DROPDOWN_MENU;
    Atom _NET_WM_WINDOW_TYPE_POPUP_MENU;
    Atom _NET_WM_WINDOW_TYPE_TOOLTIP;
    Atom _NET_WM_WINDOW_TYPE_NOTIFICATION;
    Atom _NET_WM_WINDOW_TYPE_COMBO;
    Atom _NET_WM_WINDOW_TYPE_DND;
    Atom _NET_WM_WINDOW_TYPE_NORMAL;
    Atom _MIR_WM_PERSISTENT_ID;
} known_atom;

static Atom get_atom(const char *name, Atom *cache, Bool create)
{
    if (!*cache) {
        *cache = MakeAtom(name, strlen(name), create);
        if (*cache)
            XMIR_DEBUG(("Atom %s = %lu\n", name, (unsigned long)*cache));
    }
    return *cache;
}

#define GET_ATOM(_a)  get_atom(#_a, &known_atom._a, FALSE)
#define MAKE_ATOM(_a) get_atom(#_a, &known_atom._a, TRUE)

extern __GLXprovider __glXDRI2Provider;

Bool xmir_debug_logging = FALSE;

static const char get_title_from_top_window[] = "@";

struct xmir_swap {
    int server_generation;
    struct xmir_screen *xmir_screen;
    struct xmir_window *xmir_window;
};

static void xmir_handle_buffer_received(MirBufferStream *stream, void *ctx);

/* Required by GLX module */
ScreenPtr xf86ScrnToScreen(ScrnInfoPtr pScrn)
{
    return NULL;
}

/* Required by GLX module */
ScrnInfoPtr xf86ScreenToScrn(ScreenPtr pScreen)
{
    static ScrnInfoRec rec;
    return &rec;
}

/* Required by GLX module */
void
xf86ProcessOptions(int scrnIndex, XF86OptionPtr options, OptionInfoPtr optinfo)
{
}

/* Required by GLX module */
const char *
xf86GetOptValString(const OptionInfoRec *table, int token)
{
    /* This may bite us in the bum since it sends up hardcoding "mesa" as the GL
     * vendor, but... */
    return NULL;
}

void
ddxGiveUp(enum ExitCode error)
{
}

void
AbortDDX(enum ExitCode error)
{
    ddxGiveUp(error);
}

void
OsVendorInit(void)
{
}

void
OsVendorFatalError(const char *f, va_list args)
{
}

#if defined(DDXBEFORERESET)
void
ddxBeforeReset(void)
{
    return;
}
#endif

void
ddxUseMsg(void)
{
    ErrorF("-rootless              Run rootless\n");
    ErrorF("  -flatten none|all|overrideredirects   Flatten rootless X windows into their parent surface\n");
    ErrorF("-title <name>          Set window title (@ = automatic)\n");
    ErrorF("-sw                    disable glamor rendering\n");
    ErrorF("-egl                   force use of EGL calls, disables DRI2 pass-through\n");
    ErrorF("-egl_sync              same as -egl, but with synchronous page flips.\n");
    ErrorF("-damage                copy the entire frame on damage, always enabled in egl mode\n");
    ErrorF("-fd <num>              force client connection on only fd\n");
    ErrorF("-shared                open default listening sockets even when -fd is passed\n");
    ErrorF("-mir <appid>           set mir's application id.\n");
    ErrorF("-mirSocket <socket>    use the specified socket for mir\n");
    ErrorF("-2x                    double the fun (2x resolution compared to onscreen)\n");
    ErrorF("-debug                 Log everything Xmir is doing\n");
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    static int seen_shared;

    if (strcmp(argv[i], "-rootless") == 0 ||
        strcmp(argv[i], "-sw") == 0 ||
        strcmp(argv[i], "-egl") == 0 ||
        strcmp(argv[i], "-egl_sync") == 0 ||
        strcmp(argv[i], "-2x") == 0 ||
        strcmp(argv[i], "-debug") == 0 ||
        strcmp(argv[i], "-damage") == 0) {
        return 1;
    }
    else if (strcmp(argv[i], "-mirSocket") == 0 ||
             strcmp(argv[i], "-flatten") == 0 ||
             strcmp(argv[i], "-title") == 0 ||
             strcmp(argv[i], "-mir") == 0) {
        return 2;
    }
    else if (!strcmp(argv[i], "-novtswitch") ||
               !strncmp(argv[i], "vt", 2)) {
        return 1;
    }
    else if (!strcmp(argv[i], "-fd")) {
        if (!seen_shared)
            NoListenAll = 1;

        return 2;
    }
    else if (!strcmp(argv[i], "-shared")) {
        seen_shared = 1;
        NoListenAll = 0;
        return 1;
    }
    else if (!strcmp(argv[i], "-listen")) {
        seen_shared = 1;
        NoListenAll = 0;
        return 0;
    }

    return 0;
}

static DevPrivateKeyRec xmir_window_private_key;
static DevPrivateKeyRec xmir_screen_private_key;
static DevPrivateKeyRec xmir_pixmap_private_key;

struct xmir_screen *
xmir_screen_get(ScreenPtr screen)
{
    return dixLookupPrivate(&screen->devPrivates, &xmir_screen_private_key);
}

struct xmir_pixmap *
xmir_pixmap_get(PixmapPtr pixmap)
{
    return dixLookupPrivate(&pixmap->devPrivates, &xmir_pixmap_private_key);
}

struct xmir_window *
xmir_window_get(WindowPtr window)
{
    return dixLookupPrivate(&window->devPrivates, &xmir_window_private_key);
}

void
xmir_pixmap_set(PixmapPtr pixmap, struct xmir_pixmap *xmir_pixmap)
{
    return dixSetPrivate(&pixmap->devPrivates,
                         &xmir_pixmap_private_key,
                         xmir_pixmap);
}

static Bool
xmir_get_window_prop_string8(WindowPtr window, ATOM atom,
                             char *buf, size_t bufsize)
{
    if (window->optional) {
        PropertyPtr p = window->optional->userProps;
        while (p) {
            if (p->propertyName == atom) {
                if ((  p->type == XA_STRING
                    || p->type == GET_ATOM(UTF8_STRING)
                    ) &&
                    p->format == 8 && p->data) {
                    size_t len = p->size >= bufsize ? bufsize - 1 : p->size;
                    memcpy(buf, p->data, len);
                    buf[len] = '\0';
                    return TRUE;
                }
                else {
                    ErrorF("xmir_get_window_prop_string8: Atom %d is not "
                           "an 8-bit string as expected\n", atom);
                    break;
                }
            }
            p = p->next;
        }
    }

    if (bufsize)
        buf[0] = '\0';
    return FALSE;
}

static Bool
xmir_get_window_name(WindowPtr window, char *buf, size_t bufsize)
{
    return xmir_get_window_prop_string8(window, GET_ATOM(_NET_WM_NAME),
                                        buf, bufsize)
        || xmir_get_window_prop_string8(window, XA_WM_NAME, buf, bufsize);
}

static WindowPtr
xmir_get_window_prop_window(WindowPtr window, ATOM atom)
{
    if (window->optional) {
        PropertyPtr p = window->optional->userProps;
        while (p) {
            if (p->propertyName == atom) {
                if (p->type == XA_WINDOW) {
                    WindowPtr ptr;
                    XID id = *(XID*)p->data;
                    if (dixLookupWindow(&ptr, id, serverClient,
                                        DixReadAccess) != Success)
                        ptr = NULL;
                    return ptr;
                }
                else {
                    ErrorF("xmir_get_window_prop_window: Atom %d is not "
                           "a Window as expected\n", atom);
                    return NULL;
                }
            }
            p = p->next;
        }
    }
    return NULL;
}

static const Atom*
xmir_get_window_prop_atoms(WindowPtr window, Atom name, int *count)
{
    if (window->optional) {
        PropertyPtr p = window->optional->userProps;
        while (p) {
            if (p->propertyName == name) {
                if (p->type == XA_ATOM) {
                    if (count)
                        *count = p->size;
                    return (Atom*)p->data;
                }
                else {
                    ErrorF("xmir_get_window_prop_atoms: Atom %d is not "
                           "an Atom as expected\n", name);
                    return NULL;
                }
            }
            p = p->next;
        }
    }
    return NULL;
}

static Atom
xmir_get_window_prop_atom(WindowPtr window, ATOM name)
{
    Atom *first = xmir_get_window_prop_atoms(window, name, NULL);
    return first ? *first : 0;
}

enum XWMHints_flag {
    InputHint = 1
    /* There are more but not yet required */
};

typedef struct {
    long flags;     /* marks which fields in this structure are defined */
    Bool input;     /* does this application rely on the window manager to
                       get keyboard input? */
    int initial_state;      /* see below */
    Pixmap icon_pixmap;     /* pixmap to be used as icon */
    Window icon_window;     /* window to be used as icon */
    int icon_x, icon_y;     /* initial position of icon */
    Pixmap icon_mask;       /* icon mask bitmap */
    XID window_group;       /* id of related window group */
    /* this structure may be extended in the future */
} XWMHints;

static XWMHints*
xmir_get_window_prop_hints(WindowPtr window)
{
    if (window->optional) {
        PropertyPtr p = window->optional->userProps;
        while (p) {
            if (p->propertyName == XA_WM_HINTS)
                return (XWMHints*)p->data;
            p = p->next;
        }
    }
    return NULL;
}

static void
damage_report(DamagePtr pDamage, RegionPtr pRegion, void *data)
{
    struct xmir_window *xmir_window = data;
    struct xmir_screen *xmir_screen = xmir_window->xmir_screen;

    xorg_list_add(&xmir_window->link_damage, &xmir_screen->damage_window_list);
}

static void
damage_destroy(DamagePtr pDamage, void *data)
{
}

static void
xmir_window_enable_damage_tracking(struct xmir_window *xmir_win)
{
    WindowPtr win = xmir_win->window;

    if (xmir_win->damage != NULL)
        return;

    xmir_win->damage = DamageCreate(damage_report, damage_destroy,
                                    DamageReportNonEmpty, FALSE,
                                    win->drawable.pScreen, xmir_win);
    DamageRegister(&win->drawable, xmir_win->damage);
    DamageSetReportAfterOp(xmir_win->damage, TRUE);
}

static void
xmir_window_disable_damage_tracking(struct xmir_window *xmir_win)
{
    if (xmir_win->damage != NULL) {
        DamageUnregister(xmir_win->damage);
        DamageDestroy(xmir_win->damage);
        xmir_win->damage = NULL;
    }
}

static void
xmir_sw_copy(struct xmir_screen *xmir_screen,
             struct xmir_window *xmir_win,
             RegionPtr dirty)
{
    PixmapPtr pix = xmir_screen->screen->GetWindowPixmap(xmir_win->window);
    int x1 = dirty->extents.x1, y1 = dirty->extents.y1;
    int x2 = dirty->extents.x2, y2 = dirty->extents.y2;
    int y, line_len, src_stride = pix->devKind;
    int bpp = pix->drawable.bitsPerPixel >> 3;
    char *src, *dst;
    MirGraphicsRegion region;

    mir_buffer_stream_get_graphics_region(
        mir_window_get_buffer_stream(xmir_win->surface), &region);

    /*
     * Our window region (and hence damage region) might be a little ahead of
     * the current buffer in terms of size, during a resize. So we must accept
     * that their dimensions might not match and take the safe intersection...
     */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > region.width) x2 = region.width;
    if (y2 > region.height) y2 = region.height;
    if (x2 > pix->drawable.width) x2 = pix->drawable.width;
    if (y2 > pix->drawable.height) y2 = pix->drawable.height;
    if (x2 <= x1 || y2 <= y1) return;

    src = (char*)pix->devPrivate.ptr + src_stride*y1 + x1*bpp;
    dst = region.vaddr + y1*region.stride + x1*bpp;

    line_len = (x2 - x1) * bpp;
    for (y = y1; y < y2; ++y) {
        memcpy(dst, src, line_len);
        if (x2 < region.width)
            memset(dst+x2*bpp, 0, (region.width - x2)*bpp);
        src += src_stride;
        dst += region.stride;
    }

    if (y2 < region.height)
        memset(dst, 0, (region.height - y2)*region.stride);
}

static void
xmir_get_current_buffer_dimensions(
    struct xmir_screen *xmir_screen, struct xmir_window *xmir_win,
    int *width, int *height)
{
    MirBufferPackage *package;
    MirGraphicsRegion reg;
    MirBufferStream *stream = mir_window_get_buffer_stream(xmir_win->surface);

    switch (xmir_screen->glamor) {
    case glamor_off:
        mir_buffer_stream_get_graphics_region(stream, &reg);
        *width = reg.width;
        *height = reg.height;
        break;
    case glamor_dri:
        mir_buffer_stream_get_current_buffer(stream, &package);
        *width = package->width;
        *height = package->height;
        break;
    case glamor_egl:
    case glamor_egl_sync:
        eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface,
                        EGL_WIDTH, width);
        eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface,
                        EGL_HEIGHT, height);
        break;
    default:
        break;
    }
}

static void
xmir_swap(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win)
{
    MirBufferStream *stream = mir_window_get_buffer_stream(xmir_win->surface);
    struct xmir_swap *swap = calloc(sizeof(struct xmir_swap), 1);
    swap->server_generation = serverGeneration;
    swap->xmir_screen = xmir_screen;
    swap->xmir_window = xmir_win;
    mir_buffer_stream_swap_buffers(stream, xmir_handle_buffer_received, swap);
}

void xmir_repaint(struct xmir_window *xmir_win)
{
    struct xmir_screen *xmir_screen;
    RegionPtr dirty = &xmir_win->region;
    char wm_name[256];
    WindowPtr named = NULL;

    if (!xmir_win->has_free_buffer)
        ErrorF("ERROR: xmir_repaint requested without a buffer to paint to\n");

    /*
     * TODO: Move this into the new xmir_property_changed callback.
     *       Although some of it is likely to get deleted instead, since we
     *       may never need or want title propagation again (which was for
     *       Ubuntu Touch).
     */
    xmir_screen = xmir_screen_get(xmir_win->window->drawable.pScreen);
    if (strcmp(xmir_screen->title, get_title_from_top_window)) {
        /* Fixed title mode. Never change it. */
        named = NULL;
    }
    else if (xmir_screen->rootless) {
        named = xmir_win->window;
    }
    else { /* Try and guess from the most relevant app window */
        WindowPtr top = xmir_screen->screen->root->firstChild;
        WindowPtr top_named = NULL;
        WindowPtr top_normal = NULL;

        while (top) {
            Atom wm_type;
            WindowPtr app_window;
            if (!top->viewable) {
                top = top->nextSib;
                continue;
            }
            app_window = xmir_get_window_prop_window(top, XA_WM_TRANSIENT_FOR);
            if (app_window) {
                named = app_window;
                break;
            }
            wm_type = xmir_get_window_prop_atom(top,
                                               GET_ATOM(_NET_WM_WINDOW_TYPE));
            if (wm_type && wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_NORMAL))
                top_normal = top;
            if (xmir_get_window_name(top, wm_name, sizeof wm_name))
                top_named = top;

            top = top->firstChild;
        }
        if (!named)
            named = top_normal ? top_normal : top_named;
    }

    if (named &&
        xmir_get_window_name(named, wm_name, sizeof wm_name) &&
        strcmp(wm_name, xmir_win->wm_name)) {
        MirWindowSpec *rename =
            mir_create_window_spec(xmir_screen->conn);
        mir_window_spec_set_name(rename, wm_name);
        mir_window_apply_spec(xmir_win->surface, rename);
        mir_window_spec_release(rename);
        strncpy(xmir_win->wm_name, wm_name, sizeof(xmir_win->wm_name));
    }

    switch (xmir_screen->glamor) {
    case glamor_off:
        xmir_sw_copy(xmir_screen, xmir_win, dirty);
        xmir_win->has_free_buffer = FALSE;
        xmir_swap(xmir_screen, xmir_win);
        break;
    case glamor_dri:
        xmir_glamor_copy(xmir_screen, xmir_win, dirty);
        xmir_win->has_free_buffer = FALSE;
        xmir_swap(xmir_screen, xmir_win);
        break;
    case glamor_egl:
    case glamor_egl_sync:
        xmir_glamor_copy(xmir_screen, xmir_win, dirty);
        xmir_win->has_free_buffer = TRUE;
        /* Will eglSwapBuffers (?) */
        break;
    default:
        break;
    }

    DamageEmpty(xmir_win->damage);
    xorg_list_del(&xmir_win->link_damage);
}

void
xmir_handle_buffer_available(struct xmir_screen *xmir_screen,
                             struct xmir_window *xmir_win,
                             void *unused)
{
    int buf_width, buf_height;
    Bool xserver_lagging, xclient_lagging;

    if (!xmir_win->damage || !mir_window_is_valid(xmir_win->surface)) {
        if (xmir_win->damage)
            ErrorF("Buffer-available recieved for invalid surface?\n");
        return;
    }

    DebugF("Buffer-available on %p\n", xmir_win);
    xmir_get_current_buffer_dimensions(xmir_screen, xmir_win,
                                       &buf_width, &buf_height);

    xmir_win->has_free_buffer = TRUE;
    xmir_win->buf_width = buf_width;
    xmir_win->buf_height = buf_height;

    xserver_lagging = buf_width != xmir_win->surface_width ||
                      buf_height != xmir_win->surface_height;

    xclient_lagging = buf_width != xmir_win->window->drawable.width ||
                      buf_height != xmir_win->window->drawable.height;

    if (xserver_lagging || !xorg_list_is_empty(&xmir_win->link_damage))
        xmir_repaint(xmir_win);

    if (xclient_lagging) {
        if (xmir_screen->rootless) {
            XID vlist[2] = {buf_width, buf_height};
            ConfigureWindow(xmir_win->window, CWWidth|CWHeight, vlist,
                            serverClient);
        }
        else {
            /* Output resizing takes time, so start it going and let it
             * finish next frame or so...
             */
            xmir_output_handle_resize(xmir_win, buf_width, buf_height);
        }
    }

    if (xserver_lagging)
        DamageDamageRegion(&xmir_win->window->drawable, &xmir_win->region);
}

static void
xmir_handle_buffer_received(MirBufferStream *stream, void *ctx)
{
    struct xmir_swap *swap = ctx;
    struct xmir_screen *xmir_screen = swap->xmir_screen;

    if (swap->server_generation == serverGeneration && !xmir_screen->closing) {
        xmir_post_to_eventloop(xmir_handle_buffer_available, xmir_screen,
                               swap->xmir_window, 0);
    }

    free(swap);
}

static Bool
xmir_create_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_window *xmir_window = calloc(sizeof(*xmir_window), 1);
    Bool ret;

    if (!xmir_window)
        return FALSE;

    xmir_window->xmir_screen = xmir_screen;
    xmir_window->window = window;
    xorg_list_init(&xmir_window->link_damage);
    xorg_list_init(&xmir_window->flip.entry);
    xorg_list_init(&xmir_window->link_flattened);

    screen->CreateWindow = xmir_screen->CreateWindow;
    ret = (*screen->CreateWindow) (window);
    xmir_screen->CreateWindow = screen->CreateWindow;
    screen->CreateWindow = xmir_create_window;

    if (ret)
        dixSetPrivate(&window->devPrivates,
                      &xmir_window_private_key,
                      xmir_window);
    else
        free(xmir_window);

    return ret;
}

static void
xmir_window_update_region(struct xmir_window *xmir_window)
{
    WindowPtr window = xmir_window->window;
    BoxRec box = {0, 0, window->drawable.width, window->drawable.height};
    RegionReset(&xmir_window->region, &box);
}

static Bool
xmir_realize_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_window *xmir_window = xmir_window_get(window);
    Bool ret;
    MirPixelFormat pixel_format = mir_pixel_format_invalid;
    Atom wm_type = 0;
    int mir_width = window->drawable.width / (1 + xmir_screen->doubled);
    int mir_height = window->drawable.height / (1 + xmir_screen->doubled);
    MirWindowSpec* spec = NULL;
    WindowPtr wm_transient_for = NULL, positioning_parent = NULL;
    MirWindowId *persistent_id = NULL;
    XWMHints *wm_hints = NULL;
    char wm_name[1024];

    screen->RealizeWindow = xmir_screen->RealizeWindow;
    ret = (*screen->RealizeWindow) (window);
    xmir_screen->RealizeWindow = screen->RealizeWindow;
    screen->RealizeWindow = xmir_realize_window;

    if (xmir_screen->rootless && !window->parent) {
        RegionNull(&window->clipList);
        RegionNull(&window->borderClip);
        RegionNull(&window->winSize);
    }
    xmir_window_update_region(xmir_window);

    xmir_get_window_name(window, wm_name, sizeof wm_name);
    wm_type = xmir_get_window_prop_atom(window, GET_ATOM(_NET_WM_WINDOW_TYPE));
    wm_transient_for = xmir_get_window_prop_window(window, XA_WM_TRANSIENT_FOR);

    XMIR_DEBUG(("Realize %swindow %p id=0x%x \"%s\": %dx%d %+d%+d parent=%p\n"
           "\tdepth=%d redir=%u type=%hu class=%u visibility=%u viewable=%u\n"
           "\toverride=%d _NET_WM_WINDOW_TYPE=%lu(%s)\n"
           "\tWM_TRANSIENT_FOR=%p\n",
           window == screen->root ? "ROOT " : "",
           window, (int)window->drawable.id, wm_name, mir_width, mir_height,
           window->drawable.x, window->drawable.y,
           window->parent,
           window->drawable.depth,
           window->redirectDraw, window->drawable.type,
           window->drawable.class, window->visibility, window->viewable,
           window->overrideRedirect,
           (unsigned long)wm_type, NameForAtom(wm_type)?:"",
           wm_transient_for));

    wm_hints = xmir_get_window_prop_hints(window);
    if (wm_hints) {
        XMIR_DEBUG(("\tWM_HINTS={flags=0x%lx,input=%s}\n",
                    wm_hints->flags, wm_hints->input?"True":"False"));
    }
    else {
        XMIR_DEBUG(("\tWM_HINTS=<none>\n"));
    }

    if (!window->viewable) {
        return ret;
    }
    else if (xmir_screen->rootless) {
        if (!window->parent || window->parent == screen->root) {
            compRedirectWindow(serverClient, window,
                               CompositeRedirectManual);
            compRedirectSubwindows(serverClient, window,
                                   CompositeRedirectAutomatic);
        }
        if (window->redirectDraw != RedirectDrawManual)
            return ret;
    }
    else if (window->parent) {
        return ret;
    }

    if (window->drawable.depth == 32)
        pixel_format = xmir_screen->depth32_pixel_format;
    else if (window->drawable.depth == 24)
        pixel_format = xmir_screen->depth24_pixel_format;
    else {
        ErrorF("No pixel format available for depth %d\n",
               (int)window->drawable.depth);
        return FALSE;
    }

    /* TODO: Replace pixel_format with the actual right answer from the
     *       graphics driver when using EGL:
     *         mir_connection_get_egl_pixel_format()
     */

    if (!wm_type)   /* Avoid spurious matches with undetected types */
        wm_type = -1;

    positioning_parent = wm_transient_for;
    if (!positioning_parent) {
        /* The toolkit has not provided a definite positioning parent so the
         * next best option is to guess. But we can only reasonably guess for
         * window types that are typically subordinate to normal windows...
         */
        Bool is_subordinate = wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_DROPDOWN_MENU)
                           || wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_POPUP_MENU)
                           || wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_MENU)
                           || wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_COMBO)
                           || wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_TOOLBAR)
                           || wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_UTILITY)
                           || wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_TOOLTIP)
                           || (wm_type == -1 && window->overrideRedirect);

        if (is_subordinate)
            positioning_parent = xmir_screen->last_focus;
    }

    if (xmir_screen->flatten_top &&
        (xmir_screen->flatten == flatten_all ||
         (xmir_screen->flatten == flatten_overrideredirects &&
          window->overrideRedirect))) {
        WindowPtr top = xmir_screen->flatten_top->window;
        int dx = window->drawable.x - top->drawable.x;
        int dy = window->drawable.y - top->drawable.y;
        xorg_list_append(&xmir_window->link_flattened,
                         &xmir_screen->flattened_list);
        ReparentWindow(window, top, dx, dy, serverClient);
        XMIR_DEBUG(("Flattened window %p (reparented under %p %+d%+d)\n",
                    window, top, dx, dy));
        /* And thanks to the X Composite extension, window will now be
         * automatically composited into the existing flatten_top surface
         * so we retain only a single Mir surface, as Unity8 likes to see.
         */
        return ret;
    }

    if (positioning_parent) {
        struct xmir_window *rel = xmir_window_get(positioning_parent);
        if (rel && rel->surface) {
            short dx = window->drawable.x - rel->window->drawable.x;
            short dy = window->drawable.y - rel->window->drawable.y;
            MirRectangle placement = {dx, dy, 0, 0};

            if (wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_TOOLTIP)) {
                spec = mir_create_tip_window_spec(
                    xmir_screen->conn, mir_width, mir_height,
                    rel->surface, &placement, mir_edge_attachment_any);
            }
            else if (wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_DIALOG)) {
                spec = mir_create_modal_dialog_window_spec(
                    xmir_screen->conn, mir_width, mir_height,
                    rel->surface);
            }
            else {  /* Probably a menu. If not, still close enough... */
                MirEdgeAttachment edge = mir_edge_attachment_any;
                if (wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_DROPDOWN_MENU))
                    edge = mir_edge_attachment_vertical;
                spec = mir_create_menu_window_spec(
                    xmir_screen->conn,
                    mir_width, mir_height, rel->surface,
                    &placement, edge);
            }
        }
    }

    if (!spec) {
        if (wm_type == GET_ATOM(_NET_WM_WINDOW_TYPE_DIALOG)) {
            spec = mir_create_dialog_window_spec(
                xmir_screen->conn, mir_width, mir_height);
        }
        else {
            spec = mir_create_normal_window_spec(
                xmir_screen->conn, mir_width, mir_height);
        }
    }

    if (strcmp(xmir_screen->title, get_title_from_top_window))
        mir_window_spec_set_name(spec, xmir_screen->title);
    else if (xmir_screen->rootless)
        mir_window_spec_set_name(spec, wm_name);

    xmir_window->surface_width = mir_width;
    xmir_window->surface_height = mir_height;
    xmir_window->buf_width = mir_width;
    xmir_window->buf_height = mir_height;

    mir_window_spec_set_pixel_format(spec, pixel_format);
    mir_window_spec_set_buffer_usage(spec, xmir_screen->glamor ?
                                           mir_buffer_usage_hardware :
                                           mir_buffer_usage_software);
    xmir_window->surface = mir_create_window_sync(spec);
    mir_window_spec_release(spec);

    persistent_id =
        mir_window_request_window_id_sync(xmir_window->surface);
    if (mir_window_id_is_valid(persistent_id)) {
        const char *str = mir_window_id_as_string(persistent_id);
        dixChangeWindowProperty(serverClient, window,
                                MAKE_ATOM(_MIR_WM_PERSISTENT_ID),
                                XA_STRING, 8, PropModeReplace,
                                strlen(str), (void*)str, FALSE);
    }
    mir_window_id_release(persistent_id);

    xmir_window->has_free_buffer = TRUE;
    if (!mir_window_is_valid(xmir_window->surface)) {
        ErrorF("failed to create a surface: %s\n",
               mir_window_get_error_message(xmir_window->surface));
        return FALSE;
    }
    if (!xmir_screen->flatten_top)
        xmir_screen->flatten_top = xmir_window;
    mir_window_set_event_handler(xmir_window->surface,
                                  xmir_surface_handle_event,
                                  xmir_window);

    xmir_window_enable_damage_tracking(xmir_window);

    if (xmir_screen->glamor)
        xmir_glamor_realize_window(xmir_screen, xmir_window, window);

    return ret;
}

static const char *
xmir_surface_type_str(MirWindowType type)
{
    return "unk";
}

static const char *
xmir_surface_state_str(MirWindowState state)
{
    switch (state) {
    case mir_surface_state_unknown: return "unknown";
    case mir_surface_state_restored: return "restored";
    case mir_surface_state_minimized: return "minimized";
    case mir_surface_state_maximized: return "maximized";
    case mir_surface_state_vertmaximized: return "vert maximized";
    case mir_surface_state_fullscreen: return "fullscreen";
    default: return "???";
    }
}

static const char *
xmir_surface_focus_str(MirWindowFocusState focus)
{
    switch (focus) {
    case mir_surface_unfocused: return "unfocused";
    case mir_window_focus_state_focused: return "focused";
    default: return "???";
    }
}

static const char *
xmir_surface_vis_str(MirWindowVisibility vis)
{
    switch (vis) {
    case mir_surface_visibility_occluded: return "hidden";
    case mir_surface_visibility_exposed: return "visible";
    default: return "???";
    }
}

static Window
xmir_get_current_input_focus(DeviceIntPtr kbd)
{
    Window id = None;
    FocusClassPtr focus = kbd->focus;
    if (focus->win == NoneWin)
        id = None;
    else if (focus->win == PointerRootWin)
        id = PointerRoot;
    else
        id = focus->win->drawable.id;
    return id;
}

static void
xmir_handle_focus_event(struct xmir_window *xmir_window,
                        MirWindowFocusState state)
{
    struct xmir_screen *xmir_screen = xmir_window->xmir_screen;
    DeviceIntPtr keyboard = inputInfo.keyboard; /*PickKeyboard(serverClient);*/

    if (xmir_screen->destroying_root)
        return;

    if (xmir_window->surface) {  /* It's a real Mir window */
        xmir_screen->last_focus = (state == mir_window_focus_state_focused) ?
                                  xmir_window->window : NULL;
    }

    if (xmir_screen->rootless) {
        WindowPtr window = xmir_window->window;
        const XWMHints *hints = xmir_get_window_prop_hints(window);
        Bool refuse_focus = window->overrideRedirect ||
            (hints && (hints->flags & InputHint) && !hints->input);
        if (!refuse_focus) {
            Window id = (state == mir_window_focus_state_focused) ?
                        window->drawable.id : None;
            SetInputFocus(serverClient, keyboard, id, RevertToParent,
                          CurrentTime, FALSE);
        }
    }
    else if (!strcmp(xmir_screen->title, get_title_from_top_window)) {
        /*
         * So as to not break default behaviour, we only hack focus within
         * the root window when in Unity8 invasive mode (-title @).
         */
        Window id = None;
        if (state == mir_window_focus_state_focused) {
            id = xmir_screen->saved_focus;
            if (id == None)
                id = PointerRoot;
        }
        else {
            xmir_screen->saved_focus = xmir_get_current_input_focus(keyboard);
            id = None;
        }
        SetInputFocus(serverClient, keyboard, id, RevertToNone, CurrentTime,
                      FALSE);
    }
    /* else normal root window mode -- Xmir does not interfere in focus */
}

void
xmir_handle_surface_event(struct xmir_window *xmir_window,
                          MirWindowAttrib attr,
                          int val)
{
    switch (attr) {
    case mir_surface_attrib_type:
        XMIR_DEBUG(("Type: %s\n", xmir_surface_type_str(val)));
        break;
    case mir_surface_attrib_state:
        XMIR_DEBUG(("State: %s\n", xmir_surface_state_str(val)));
        break;
    case mir_surface_attrib_swapinterval:
        XMIR_DEBUG(("Swap interval: %i\n", val));
        break;
    case mir_surface_attrib_focus:
        XMIR_DEBUG(("Focus: %s\n", xmir_surface_focus_str(val)));
        xmir_handle_focus_event(xmir_window, (MirWindowFocusState)val);
        break;
    case mir_surface_attrib_dpi:
        XMIR_DEBUG(("DPI: %i\n", val));
        break;
    case mir_surface_attrib_visibility:
        XMIR_DEBUG(("Visibility: %s\n", xmir_surface_vis_str(val)));
        break;
    default:
        XMIR_DEBUG(("Unhandled attribute %i\n", attr));
        break;
    }
}

void
xmir_close_surface(struct xmir_window *xmir_window)
{
    WindowPtr window = xmir_window->window;
    struct xmir_screen *xmir_screen = xmir_screen_get(window->drawable.pScreen);

    if (xmir_screen->rootless) {
        xEvent event;
        event.u.u.type = ClientMessage;
        event.u.u.detail = 32;
        event.u.clientMessage.window = window->drawable.id;
        event.u.clientMessage.u.l.type = GET_ATOM(WM_PROTOCOLS);
        event.u.clientMessage.u.l.longs0 = GET_ATOM(WM_DELETE_WINDOW);
        event.u.clientMessage.u.l.longs1 = CurrentTime;
        DeliverEvents(window, &event, 1, NullWindow);
    }
    else {
        ErrorF("Root window closed, shutting down Xmir\n");
        GiveUp(0);
        /*DeleteWindow(window, 1); ? */
    }
}

static void
xmir_unmap_input(struct xmir_screen *xmir_screen, WindowPtr window)
{
    struct xmir_input *xmir_input;

    xorg_list_for_each_entry(xmir_input, &xmir_screen->input_list, link) {
        if (xmir_input->focus_window &&
            xmir_input->focus_window->window == window)
            xmir_input->focus_window = NULL;
    }
}

static void
xmir_bequeath_surface(struct xmir_window *dying, struct xmir_window *benef)
{
    struct xmir_screen *xmir_screen = benef->xmir_screen;
    struct xmir_window *other;

    XMIR_DEBUG(("flatten bequeath: %p --> %p\n",
                dying->window, benef->window));

    assert(!benef->surface);
    benef->surface = dying->surface;
    dying->surface = NULL;

    ReparentWindow(benef->window, xmir_screen->screen->root,
                   0, 0, serverClient);
    compRedirectWindow(serverClient, benef->window, CompositeRedirectManual);
    compRedirectSubwindows(serverClient,
                           benef->window,
                           CompositeRedirectAutomatic);

    xorg_list_for_each_entry(other, &xmir_screen->flattened_list,
                             link_flattened) {
        ReparentWindow(other->window, benef->window, 0, 0, serverClient);
    }

    mir_window_set_event_handler(benef->surface, xmir_surface_handle_event,
                                  benef);

    xmir_window_enable_damage_tracking(benef);

    if (xmir_screen->glamor)
        xmir_glamor_realize_window(xmir_screen, benef, benef->window);
}

static void
xmir_unmap_surface(struct xmir_screen *xmir_screen,
                   WindowPtr window,
                   BOOL destroyed)
{
    struct xmir_window *xmir_window =
        dixLookupPrivate(&window->devPrivates, &xmir_window_private_key);

    if (!xmir_window)
        return;

    XMIR_DEBUG(("Unmap/unrealize window %p\n", window));

    if (!destroyed)
        xmir_window_disable_damage_tracking(xmir_window);
    else
        xmir_window->damage = NULL;

    xorg_list_del(&xmir_window->link_damage);

    if (xmir_screen->glamor)
        xmir_glamor_unrealize_window(xmir_screen, xmir_window, window);

    xorg_list_del(&xmir_window->link_flattened);

    if (!xmir_window->surface)
        return;

    mir_window_set_event_handler(xmir_window->surface, NULL, NULL);

    if (xmir_screen->flatten && xmir_screen->flatten_top == xmir_window) {
        xmir_screen->flatten_top = NULL;
        if (!xorg_list_is_empty(&xmir_screen->flattened_list)) {
            xmir_screen->flatten_top =
                xorg_list_first_entry(&xmir_screen->flattened_list,
                                      struct xmir_window,
                                      link_flattened);
            xorg_list_del(&xmir_screen->flatten_top->link_flattened);
            xmir_bequeath_surface(xmir_window, xmir_screen->flatten_top);
        }
    }

    if (xmir_window->surface) {
        mir_window_release_sync(xmir_window->surface);
        xmir_window->surface = NULL;
    }

    xmir_process_from_eventloop_except(xmir_window);

    RegionUninit(&xmir_window->region);
}

static Bool
xmir_unrealize_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    Bool ret;

    if (window == xmir_screen->last_focus)
        xmir_screen->last_focus = NULL;

    xmir_unmap_input(xmir_screen, window);

    screen->UnrealizeWindow = xmir_screen->UnrealizeWindow;
    ret = (*screen->UnrealizeWindow) (window);
    xmir_screen->UnrealizeWindow = screen->UnrealizeWindow;
    screen->UnrealizeWindow = xmir_unrealize_window;

    xmir_unmap_surface(xmir_screen, window, FALSE);

    return ret;
}

static Bool
xmir_destroy_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    Bool ret;

    if (!window->parent)
        xmir_screen->destroying_root = TRUE;

    xmir_unmap_input(xmir_screen, window);
    xmir_unmap_surface(xmir_screen, window, TRUE);

    screen->DestroyWindow = xmir_screen->DestroyWindow;
    ret = (*screen->DestroyWindow) (window);
    xmir_screen->DestroyWindow = screen->DestroyWindow;
    screen->DestroyWindow = xmir_destroy_window;

    return ret;
}

static void
xmir_property_changed(WindowPtr window, int state, Atom atom)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_window *xmir_window = xmir_window_get(window);

    screen->PropertyChanged = xmir_screen->PropertyChanged;
    if (screen->PropertyChanged)
        (*screen->PropertyChanged)(window, state, atom);
    xmir_screen->PropertyChanged = screen->PropertyChanged;
    screen->PropertyChanged = xmir_property_changed;

    XMIR_DEBUG(("X window %p property %s %s\n",
                window,
                NameForAtom(atom)?:"?",
                state == PropertyDelete ? "deleted" :
                state == PropertyNewValue ? "changed" :
                                            "confused"));
}

static void
xmir_resize_window(WindowPtr window, int x, int y,
                   unsigned int w, unsigned int h, WindowPtr sib)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_window *xmir_window = xmir_window_get(window);

    screen->ResizeWindow = xmir_screen->ResizeWindow;
    (*screen->ResizeWindow) (window, x, y, w, h, sib);
    xmir_screen->ResizeWindow = screen->ResizeWindow;
    screen->ResizeWindow = xmir_resize_window;

    if (xmir_window->surface) {
        /* This is correct in theory but most Mir shells don't do it yet */
        MirWindowSpec *changes =
            mir_create_window_spec(xmir_screen->conn);
        mir_window_spec_set_width(changes, w);
        mir_window_spec_set_height(changes, h);
        mir_window_apply_spec(xmir_window->surface, changes);
        mir_window_spec_release(changes);

        XMIR_DEBUG(("X window %p resized to %ux%u %+d%+d with sibling %p\n",
                    window, w, h, x, y, sib));
    }

    xmir_window_update_region(xmir_window);
}

static Bool
xmir_close_screen(ScreenPtr screen)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_output *xmir_output, *next_xmir_output;
    Bool ret;

    xmir_screen->closing = TRUE;

    if (xmir_screen->glamor && xmir_screen->gbm)
        DRI2CloseScreen(screen);

    screen->CloseScreen = xmir_screen->CloseScreen;
    ret = screen->CloseScreen(screen);

    xorg_list_for_each_entry_safe(xmir_output, next_xmir_output,
                                  &xmir_screen->output_list, link)
        xmir_output_destroy(xmir_output);

    if (xmir_screen->glamor)
        xmir_glamor_fini(xmir_screen);
    mir_display_config_release(xmir_screen->display);
    mir_connection_release(xmir_screen->conn);

    xmir_fini_thread_to_eventloop();
    free(xmir_screen->driver_name);
    free(xmir_screen);

    return ret;
}

static Bool
xmir_is_unblank(int mode)
{
    switch (mode) {
    case SCREEN_SAVER_OFF:
    case SCREEN_SAVER_FORCER:
        return TRUE;
    case SCREEN_SAVER_ON:
    case SCREEN_SAVER_CYCLE:
        return FALSE;
    default:
        ErrorF("Unexpected save screen mode: %d\n", mode);
        return TRUE;
    }
}

Bool
DPMSSupported(void)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screenInfo.screens[0]);
    return !xmir_screen->rootless && !xmir_screen->windowed;
}

int
DPMSSet(ClientPtr client, int level)
{
    int rc = Success;
    struct xmir_screen *xmir_screen = xmir_screen_get(screenInfo.screens[0]);

    DPMSPowerLevel = level;

    if (level != DPMSModeOn) {
        if (xmir_is_unblank(screenIsSaved))
            rc = dixSaveScreens(client, SCREEN_SAVER_FORCER, ScreenSaverActive);
    }
    else {
        if (!xmir_is_unblank(screenIsSaved))
            rc = dixSaveScreens(client, SCREEN_SAVER_OFF, ScreenSaverReset);
    }

    if (rc != Success)
        return rc;

    xmir_output_dpms(xmir_screen, level);

    return Success;
}

static Bool
xmir_save_screen(ScreenPtr screen, int mode)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);

    if (xmir_is_unblank(mode))
        return xmir_output_dpms(xmir_screen, DPMSModeOn);
    else
        return xmir_output_dpms(xmir_screen, DPMSModeOff);
}

static void
xmir_block_handler(ScreenPtr screen, void *ptv)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_window *xmir_window, *next;

    xorg_list_for_each_entry_safe(xmir_window, next,
                                  &xmir_screen->damage_window_list,
                                  link_damage) {
        if (xmir_window->has_free_buffer) {
            xmir_repaint(xmir_window);
        }
    }
}

static Bool
xmir_create_screen_resources(ScreenPtr screen)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    int ret;

    screen->CreateScreenResources = xmir_screen->CreateScreenResources;
    ret = (*screen->CreateScreenResources) (screen);
    xmir_screen->CreateScreenResources = screen->CreateScreenResources;
    screen->CreateScreenResources = xmir_create_screen_resources;

    if (!ret)
        return ret;

    if (!xmir_screen->rootless)
        screen->devPrivate = screen->CreatePixmap(screen,
                                                  screen->width,
                                                  screen->height,
                                                  screen->rootDepth,
                                                  CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
    else
        screen->devPrivate = fbCreatePixmap(screen, 0, 0, screen->rootDepth, 0);

    if (!screen->devPrivate)
        return FALSE;

#ifdef GLAMOR_HAS_GBM
    if (xmir_screen->glamor && !xmir_screen->rootless) {
        glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(screen->devPrivate);

        glBindFramebuffer(GL_FRAMEBUFFER, pixmap_priv->fbo->fb);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glamor_set_screen_pixmap(screen->devPrivate, NULL);
    }
#endif

    return TRUE;
}

struct xmir_visit_set_pixmap_window {
    PixmapPtr old, new;
};

static int
xmir_visit_set_window_pixmap(WindowPtr window, void *data)
{
    struct xmir_visit_set_pixmap_window *visit = data;

    if (fbGetWindowPixmap(window) == visit->old) {
        window->drawable.pScreen->SetWindowPixmap(window, visit->new);
        return WT_WALKCHILDREN;
    }

    return WT_DONTWALKCHILDREN;
}

static void
xmir_set_screen_pixmap(PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    PixmapPtr old_front = screen->devPrivate;
    WindowPtr root;

    root = screen->root;
    if (root) {
        struct xmir_visit_set_pixmap_window visit = { old_front, pixmap };
        assert(fbGetWindowPixmap(root) == old_front);
        TraverseTree(root, xmir_visit_set_window_pixmap, &visit);
        assert(fbGetWindowPixmap(root) == pixmap);
    }

    screen->devPrivate = pixmap;

    if (old_front)
        screen->DestroyPixmap(old_front);
}

void
xmir_disable_screensaver(struct xmir_screen *xmir_screen)
{
    ScreenSaverTime = 0;
}

static Bool
xmir_screen_init(ScreenPtr pScreen, int argc, char **argv)
{
    struct xmir_screen *xmir_screen;
    MirConnection *conn;
    Pixel red_mask, blue_mask, green_mask;
    int ret, bpc, i;
    int client_fd = -1;
    char *socket = NULL;
    const char *appid = "XMIR";
    unsigned int formats, f;
    MirPixelFormat format[1024];

    if (!dixRegisterPrivateKey(&xmir_screen_private_key, PRIVATE_SCREEN, 0) ||
        !dixRegisterPrivateKey(&xmir_window_private_key, PRIVATE_WINDOW, 0) ||
        !dixRegisterPrivateKey(&xmir_pixmap_private_key, PRIVATE_PIXMAP, 0))
        return FALSE;

    memset(&known_atom, 0, sizeof known_atom);

    xmir_screen = calloc(sizeof *xmir_screen, 1);
    if (!xmir_screen)
        return FALSE;

    xmir_screen->conn = NULL;

    xmir_init_thread_to_eventloop();
    dixSetPrivate(&pScreen->devPrivates, &xmir_screen_private_key, xmir_screen);
    xmir_screen->screen = pScreen;
    xmir_screen->glamor = glamor_dri;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-rootless") == 0) {
            xmir_screen->rootless = 1;
            xmir_disable_screensaver(xmir_screen);
        }
        else if (strcmp(argv[i], "-flatten") == 0) {
            const char *what = argv[++i];
            if (!strcmp(what, "none"))
                xmir_screen->flatten = flatten_none;
            else if (!strcmp(what, "all"))
                xmir_screen->flatten = flatten_all;
            else if (!strcmp(what, "overrideredirects"))
                xmir_screen->flatten = flatten_overrideredirects;
            else {
                FatalError("Invalid -flatten mode `%s'\n", what);
                return FALSE;
            }
        }
        else if (strcmp(argv[i], "-title") == 0) {
            xmir_screen->title = argv[++i];
        }
        else if (strcmp(argv[i], "-mir") == 0) {
            appid = argv[++i];
        }
        else if (strcmp(argv[i], "-mirSocket") == 0) {
            socket = argv[++i];
        }
        else if (strcmp(argv[i], "-sw") == 0) {
            xmir_screen->glamor = glamor_off;
        }
        else if (strcmp(argv[i], "-egl") == 0) {
            if (xmir_screen->glamor != glamor_egl_sync)
                xmir_screen->glamor = glamor_egl;
        }
        else if (strcmp(argv[i], "-2x") == 0) {
            xmir_screen->doubled = 1;
        }
        else if (strcmp(argv[i], "-debug") == 0) {
            xmir_debug_logging = TRUE;
        }
        else if (strcmp(argv[i], "-damage") == 0) {
            /* Ignored. Damage-all is now the default and only option. */
        }
        else if (strcmp(argv[i], "-egl_sync") == 0) {
            xmir_screen->glamor = glamor_egl_sync;
        }
        else if (strcmp(argv[i], "-fd") == 0) {
            client_fd = (int)strtol(argv[++i], (char **)NULL, 0);
        }
    }

    if (xmir_screen->flatten && !xmir_screen->rootless) {
        FatalError("-flatten is not valid without -rootless\n");
        return FALSE;
    }

    if (!xmir_screen->title)
        xmir_screen->title = xmir_screen->rootless ? get_title_from_top_window
                                                   : "Xmir root window";

#if defined(__arm__) || defined(__aarch64__)
    if (xmir_screen->glamor == glamor_dri) {
        XMIR_DEBUG(("ARM architecture: Defaulting to software mode because "
                    "glamor is not stable\n"));
        /* Hide the ARM glamor bugs for now so we can have working phones */
        xmir_screen->glamor = glamor_off;
    }
#endif

    if (client_fd != -1) {
        if (!AddClientOnOpenFD(client_fd)) {
            FatalError("failed to connect to client fd %d\n", client_fd);
            return FALSE;
        }
    }

    conn = mir_connect_sync(socket, appid);
    if (!mir_connection_is_valid(conn)) {
        FatalError("Failed to connect to Mir: %s\n",
                   mir_connection_get_error_message(conn));
        return FALSE;
    }
    xmir_screen->conn = conn;
    mir_connection_get_platform(xmir_screen->conn, &xmir_screen->platform);

    xorg_list_init(&xmir_screen->output_list);
    xorg_list_init(&xmir_screen->input_list);
    xorg_list_init(&xmir_screen->damage_window_list);
    xorg_list_init(&xmir_screen->flattened_list);
    xmir_screen->depth = 24;

    mir_connection_get_available_surface_formats(xmir_screen->conn,
        format, sizeof(format)/sizeof(format[0]), &formats);

#if 0  /* Emulate the Mir Android graphics platform for LP: #1573470 */
    format[0] = mir_pixel_format_abgr_8888;
    format[1] = mir_pixel_format_xbgr_8888;
    formats = 2;
#endif

    for (f = 0; f < formats; ++f) {
        switch (format[f]) {
        case mir_pixel_format_argb_8888:
        case mir_pixel_format_abgr_8888:
            xmir_screen->depth32_pixel_format = format[f];
            break;
        case mir_pixel_format_xrgb_8888:
        case mir_pixel_format_xbgr_8888:
        case mir_pixel_format_bgr_888:
            xmir_screen->depth24_pixel_format = format[f];
            break;
        default:
            /* Other/new pixel formats don't need mentioning. We only
               care about Xorg-compatible formats */
            break;
        }
    }

    xmir_screen->display = mir_connection_create_display_configuration(conn);
    if (xmir_screen->display == NULL) {
        FatalError("could not create display config\n");
        return FALSE;
    }

    /*
     * Core DPI cannot report correct values (it's one value and we might have
     * multiple displays). Use the value from the -dpi command line if set, or
     * 96 otherwise.
     *
     * This matches the behaviour of all the desktop Xorg drivers. Clients
     * which care can use the XRandR extension to get correct per-output DPI
     * information.
     */
    xmir_screen->dpi = monitorResolution > 0 ? monitorResolution : 96;

    if (!xmir_screen_init_output(xmir_screen))
        return FALSE;

    if (xmir_screen->glamor)
        xmir_screen_init_glamor(xmir_screen);

    bpc = 8;
    green_mask = 0x00ff00;
    switch (xmir_screen->depth24_pixel_format)
    {
    case mir_pixel_format_xrgb_8888:
    case mir_pixel_format_bgr_888:  /* Little endian: Note the reversal */
        red_mask = 0xff0000;
        blue_mask = 0x0000ff;
        break;
    case mir_pixel_format_xbgr_8888:
        red_mask = 0x0000ff;
        blue_mask = 0xff0000;
        break;
    default:
        ErrorF("No Mir-compatible TrueColor formats\n");
        return FALSE;
    }

    miSetVisualTypesAndMasks(xmir_screen->depth,
                             ((1 << TrueColor) | (1 << DirectColor)),
                             bpc, TrueColor,
                             red_mask, green_mask, blue_mask);

    miSetPixmapDepths();

    ret = fbScreenInit(pScreen, NULL,
                       pScreen->width, pScreen->height,
                       xmir_screen->dpi, xmir_screen->dpi, 0,
                       BitsPerPixel(xmir_screen->depth));
    if (!ret)
        return FALSE;

    fbPictureInit(pScreen, 0, 0);

    pScreen->blackPixel = 0;
    pScreen->whitePixel = 1;

    ret = fbCreateDefColormap(pScreen);

    if (!xmir_screen_init_cursor(xmir_screen))
        return FALSE;

    pScreen->SaveScreen = xmir_save_screen;
    pScreen->BlockHandler = xmir_block_handler;
    pScreen->SetScreenPixmap = xmir_set_screen_pixmap;

    xmir_screen->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = xmir_create_screen_resources;

#ifdef GLAMOR_HAS_GBM
    if (xmir_screen->glamor && !xmir_glamor_init(xmir_screen)) {
        if (xmir_screen->glamor >= glamor_egl)
            FatalError("EGL requested, but not available\n");
        xmir_screen->glamor = glamor_off;
    }

    if (xmir_screen->glamor && xmir_screen->gbm && !xmir_dri2_screen_init(xmir_screen))
        ErrorF("Failed to initialize DRI2.\n");
#endif

    if (!xmir_screen->glamor && xmir_screen->doubled)
        FatalError("-2x requires EGL support\n");

    xmir_screen->CreateWindow = pScreen->CreateWindow;
    pScreen->CreateWindow = xmir_create_window;

    xmir_screen->RealizeWindow = pScreen->RealizeWindow;
    pScreen->RealizeWindow = xmir_realize_window;

    xmir_screen->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = xmir_destroy_window;

    xmir_screen->ResizeWindow = pScreen->ResizeWindow;
    pScreen->ResizeWindow = xmir_resize_window;

    xmir_screen->UnrealizeWindow = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = xmir_unrealize_window;

    xmir_screen->PropertyChanged = pScreen->PropertyChanged;
    pScreen->PropertyChanged = xmir_property_changed;

    xmir_screen->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = xmir_close_screen;

    {
        int v;
        XMIR_DEBUG(("XMir initialized with %hd visuals:\n",
                    pScreen->numVisuals));
        for (v = 0; v < pScreen->numVisuals; ++v) {
            VisualPtr visual = pScreen->visuals + v;
            XMIR_DEBUG(("\tVisual id 0x%x: %lx %lx %lx, %hd planes\n",
                        (int)visual->vid,
                        (long)visual->redMask,
                        (long)visual->greenMask,
                        (long)visual->blueMask,
                        visual->nplanes));
        }
    }

    return ret;
}

static const ExtensionModule xmir_extensions[] = {
#ifdef DRI2
    { DRI2ExtensionInit, "DRI2", &noDRI2Extension },
#endif
#ifdef GLXEXT
    { GlxExtensionInit, "GLX", &noGlxExtension },
#endif
};

void
InitOutput(ScreenInfo *screen_info, int argc, char **argv)
{
    int depths[] = { 1, 4, 8, 15, 16, 24, 32 };
    int bpp[] =    { 1, 8, 8, 16, 16, 32, 32 };
    int i;

    for (i = 0; i < ARRAY_SIZE(depths); i++) {
        screen_info->formats[i].depth = depths[i];
        screen_info->formats[i].bitsPerPixel = bpp[i];
        screen_info->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    }

    screen_info->imageByteOrder = IMAGE_BYTE_ORDER;
    screen_info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screen_info->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screen_info->bitmapBitOrder = BITMAP_BIT_ORDER;
    screen_info->numPixmapFormats = ARRAY_SIZE(depths);

    if (serverGeneration == 1) {
#ifdef GLXEXT
        GlxPushProvider(&__glXDRI2Provider);
#endif
        LoadExtensionList(xmir_extensions,
                          ARRAY_SIZE(xmir_extensions), TRUE);
    }

    if (AddScreen(xmir_screen_init, argc, argv) == -1) {
        FatalError("Couldn't add screen\n");
    }
}
