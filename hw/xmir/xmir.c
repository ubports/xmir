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
#include "xf86.h"

#include "xmir.h"

#include <stdio.h>
#include <signal.h>
#include <dlfcn.h>

#include <selection.h>
#include <micmap.h>
#include <misyncshm.h>
#include <compositeext.h>
#include <glx_extinit.h>

#include <mir_toolkit/mir_surface.h>

#include "dri2.h"
#include "glxserver.h"
#include "glamor_priv.h"
#include "dpmsproc.h"

extern __GLXprovider __glXDRI2Provider;

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
    ErrorF("-rootless              run rootless, requires wm support\n");
    ErrorF("-sw                    disable glamor rendering\n");
    ErrorF("-egl                   force use of EGL calls, disables DRI2 pass-through\n");
    ErrorF("-egl_sync              same as -egl, but with synchronous page flips.\n");
    ErrorF("-damage                copy the entire frame on damage, always enabled in egl mode\n");
    ErrorF("-fd <num>              force client connection on only fd\n");
    ErrorF("-shared                open default listening sockets even when -fd is passed\n");
    ErrorF("-mir <appid>           set mir's application id.\n");
    ErrorF("-mirSocket <socket>    use the specified socket for mir\n");
    ErrorF("-2x                    double the fun (2x resolution compared to onscreen)\n");
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
        strcmp(argv[i], "-damage") == 0) {
        return 1;
    }
    else if (strcmp(argv[i], "-mirSocket") == 0 ||
             strcmp(argv[i], "-mir") == 0) {
        return 2;
    } else if (!strcmp(argv[i], "-novtswitch") ||
               !strncmp(argv[i], "vt", 2)) {
        return 1;
    /* Bypass unity8 "security" */
    } else if (!strncmp(argv[i], "--desktop_file_hint=", strlen("--desktop_file_hint="))) {
        return 1;
    } else if (!strcmp(argv[i], "-fd")) {
        if (!seen_shared)
            NoListenAll = 1;

        return 2;
    } else if (!strcmp(argv[i], "-shared")) {
        seen_shared = 1;
        NoListenAll = 0;
        return 1;
    } else if (!strcmp(argv[i], "-listen")) {
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
    return dixSetPrivate(&pixmap->devPrivates, &xmir_pixmap_private_key, xmir_pixmap);
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

    for (int i = 0; i < MIR_MAX_BUFFER_AGE; i++) {
        RegionNull(&xmir_win->past_damage[i]);
    }
    xmir_win->damage_index = 0;
}

static void
xmir_window_disable_damage_tracking(struct xmir_window *xmir_win)
{
    int i;

    for (i = 0; i < MIR_MAX_BUFFER_AGE; i++)
        RegionEmpty(&xmir_win->past_damage[i]);

    if (xmir_win->damage != NULL) {
        DamageUnregister(xmir_win->damage);
        DamageDestroy(xmir_win->damage);
        xmir_win->damage = NULL;
    }
}

static inline int
index_in_damage_buffer(int current_index, int age)
{
    int index = (current_index - age) % MIR_MAX_BUFFER_AGE;

    return index < 0 ? MIR_MAX_BUFFER_AGE + index : index;
}

static RegionPtr
xmir_damage_region_for_current_buffer(struct xmir_window *xmir_win)
{
    MirBufferPackage *package;
    RegionPtr region;
    int age;

    mir_buffer_stream_get_current_buffer(mir_surface_get_buffer_stream(xmir_win->surface), &package);
    age = package->age;

    region = &xmir_win->past_damage[index_in_damage_buffer(xmir_win->damage_index, age)];

    /* As per EGL_EXT_buffer_age, contents are undefined for age == 0 */
    if (age == 0)
        RegionCopy(region, &xmir_win->region);

    return region;
}

static RegionPtr
xmir_window_get_dirty(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win)
{
    RegionPtr damage;
    int i;

    if (xmir_screen->damage_all) {
        RegionCopy(xmir_win->past_damage, &xmir_win->region);
        DamageEmpty(xmir_win->damage);

        return xmir_win->past_damage;
    }

    damage = DamageRegion(xmir_win->damage);
    RegionIntersect(damage, damage, &xmir_win->region);

    for (i = 0; i < MIR_MAX_BUFFER_AGE; i++) {
        RegionUnion(&xmir_win->past_damage[i],
                    &xmir_win->past_damage[i],
                    damage);
    }

    DamageEmpty(xmir_win->damage);

    return xmir_damage_region_for_current_buffer(xmir_win);
}

static void
xmir_submit_rendering_for_window(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, RegionPtr region)
{
    xmir_win->has_free_buffer = FALSE;
    mir_buffer_stream_swap_buffers(mir_surface_get_buffer_stream(xmir_win->surface), xmir_handle_buffer_received, xmir_win);

    RegionEmpty(region);
    xorg_list_del(&xmir_win->link_damage);

    if (xmir_screen->gbm) {
        MirBufferPackage *package;

        mir_buffer_stream_get_current_buffer(mir_surface_get_buffer_stream(xmir_win->surface), &package);

        xmir_output_handle_resize(xmir_win, package->width, package->height);
    } else {
        MirGraphicsRegion reg;

        mir_buffer_stream_get_graphics_region(mir_surface_get_buffer_stream(xmir_win->surface), &reg);
        xmir_output_handle_resize(xmir_win, reg.width, reg.height);
    }
}

static void
xmir_sw_copy(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, RegionPtr dirty)
{
    MirGraphicsRegion region;
    PixmapPtr pix = xmir_screen->screen->GetWindowPixmap(xmir_win->window);
    int y;
    char *off_src = (char *)pix->devPrivate.ptr + pix->devKind * dirty->extents.y1 + dirty->extents.x1 * 4;
    char *off_dst;
    int y2 = dirty->extents.y2;
    int x2 = dirty->extents.x2;

    mir_buffer_stream_get_graphics_region(mir_surface_get_buffer_stream(xmir_win->surface), &region);

    if (x2 > region.width)
        x2 = region.width;

    if (y2 > region.height)
        y2 = region.height;

    if (x2 <= dirty->extents.x1 || y2 <= dirty->extents.y1)
        return;

    off_dst = region.vaddr + dirty->extents.y1 * region.stride + dirty->extents.x1 * 4;

    for (y = dirty->extents.y1; y < y2; ++y, off_src += pix->devKind, off_dst += region.stride)
        memcpy(off_dst, off_src, (x2 - dirty->extents.x1) * 4);
}

static void
xmir_buffer_copy(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, RegionPtr dirty)
{
    if (xmir_screen->glamor) {
        xmir_glamor_copy(xmir_screen, xmir_win, dirty);

        if (!xmir_screen->gbm)
            return;
    } else
        xmir_sw_copy(xmir_screen, xmir_win, dirty);

    xmir_submit_rendering_for_window(xmir_screen, xmir_win, dirty);
}

static void
xmir_handle_buffer_available(void *ctx)
{
    struct xmir_window *xmir_win = *(struct xmir_window **)ctx;
    struct xmir_screen *xmir_screen = xmir_screen_get(xmir_win->window->drawable.pScreen);
    RegionPtr dirty;

    if (!xmir_win->damage || !mir_surface_is_valid(xmir_win->surface)) {
        if (xmir_win->damage)
            ErrorF("Buffer-available recieved for invalid surface?\n");
        return;
    }

    DebugF("Buffer-available on %p\n", xmir_win);
    xmir_win->has_free_buffer = TRUE;
    xmir_win->damage_index = (xmir_win->damage_index + 1) % MIR_MAX_BUFFER_AGE;

    if (xmir_screen->swap_context) {
        int width, height;

        eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface, EGL_HEIGHT, &height);
        eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface, EGL_WIDTH, &width);

        xmir_output_handle_resize(xmir_win, width, height);
    }

    if (xorg_list_is_empty(&xmir_win->link_damage))
        return;

    dirty = xmir_window_get_dirty(xmir_screen, xmir_win);
    xmir_buffer_copy(xmir_screen, xmir_win, dirty);
}

static void
xmir_handle_buffer_received(MirBufferStream *stream, void *ctx)
{
    struct xmir_window *xmir_win = ctx;
    struct xmir_screen *xmir_screen = xmir_screen_get(xmir_win->window->drawable.pScreen);

    xmir_post_to_eventloop(xmir_screen->submit_rendering_handler, &xmir_win);
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

    screen->CreateWindow = xmir_screen->CreateWindow;
    ret = (*screen->CreateWindow) (window);
    xmir_screen->CreateWindow = screen->CreateWindow;
    screen->CreateWindow = xmir_create_window;

    if (ret)
        dixSetPrivate(&window->devPrivates, &xmir_window_private_key, xmir_window);
    else
        free(xmir_window);

    return ret;
}

static Bool
xmir_realize_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_window *xmir_window = xmir_window_get(window);
    Bool ret;
    MirPixelFormat pixel_format = mir_pixel_format_invalid;

    int mir_width = window->drawable.width / (1 + xmir_screen->doubled);
    int mir_height = window->drawable.height / (1 + xmir_screen->doubled);
    MirSurfaceSpec* spec = NULL;

    screen->RealizeWindow = xmir_screen->RealizeWindow;
    ret = (*screen->RealizeWindow) (window);
    xmir_screen->RealizeWindow = screen->RealizeWindow;
    screen->RealizeWindow = xmir_realize_window;

    if (xmir_screen->rootless && !window->parent) {
        RegionNull(&window->clipList);
        RegionNull(&window->borderClip);
        RegionNull(&window->winSize);
    }

    if (xmir_screen->rootless) {
        CompositeRedirectSubwindows(window, /*CompositeRedirectManual*/TRUE); // WTF? Where is this defined
        if (window->redirectDraw != RedirectDrawManual)
            return ret;
    }
    else {
        if (window->parent)
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
    
    spec = mir_connection_create_spec_for_normal_surface(xmir_screen->conn, mir_width, mir_height, pixel_format);

    if (spec == NULL) {
        ErrorF("failed to create a surface spec: %s\n", mir_connection_get_error_message(xmir_screen->conn));
        return FALSE;
    }

    mir_surface_spec_set_buffer_usage(spec,
                                      xmir_screen->glamor
                                      ? mir_buffer_usage_hardware
                                      : mir_buffer_usage_software);

    xmir_window->surface = mir_surface_create_sync(spec);
    xmir_window->has_free_buffer = TRUE;
    if (!mir_surface_is_valid(xmir_window->surface)) {
        ErrorF("failed to create a surface: %s\n", mir_surface_get_error_message(xmir_window->surface));
        return FALSE;
    }
    RegionInit(&xmir_window->region, &(BoxRec){ 0, 0, window->drawable.width, window->drawable.height }, 1);
    mir_surface_set_event_handler(xmir_window->surface, xmir_surface_handle_event, xmir_window);

    xmir_window_enable_damage_tracking(xmir_window);

    if (xmir_screen->glamor)
        xmir_glamor_realize_window(xmir_screen, xmir_window, window);

    return ret;
}

static const char *
xmir_surface_type_str(MirSurfaceType type)
{
    return "unk";
}

static const char *
xmir_surface_state_str(MirSurfaceState state)
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
xmir_surface_focus_str(MirSurfaceFocusState focus)
{
    switch (focus) {
    case mir_surface_unfocused: return "unfocused";
    case mir_surface_focused: return "focused";
    default: return "???";
    }
}

static const char *
xmir_surface_vis_str(MirSurfaceVisibility vis)
{
    switch (vis) {
    case mir_surface_visibility_occluded: return "hidden";
    case mir_surface_visibility_exposed: return "visible";
    default: return "???";
    }
}

void
xmir_handle_surface_event(struct xmir_window *xmir_window, MirSurfaceAttrib attr, int val)
{
    switch (attr) {
    case mir_surface_attrib_type:
        ErrorF("Type: %s\n", xmir_surface_type_str(val));
        break;
    case mir_surface_attrib_state:
        ErrorF("State: %s\n", xmir_surface_state_str(val));
        break;
    case mir_surface_attrib_swapinterval:
        ErrorF("Swap interval: %i\n", val);
        break;
    case mir_surface_attrib_focus:
        ErrorF("Focus: %s\n", xmir_surface_focus_str(val));
        break;
    case mir_surface_attrib_dpi:
        ErrorF("DPI: %i\n", val);
        break;
    case mir_surface_attrib_visibility:
        ErrorF("Visibility: %s\n", xmir_surface_vis_str(val));
        break;
    default:
        ErrorF("Unhandled attribute %i\n", attr);
        break;
    }
}

void
xmir_close_surface(struct xmir_window *xmir_window)
{
    WindowPtr window = xmir_window->window;
    struct xmir_screen *xmir_screen = xmir_screen_get(window->drawable.pScreen);

    if (!xmir_screen->rootless) {
        ErrorF("Root window closed, shutting down Xmir\n");
        GiveUp(0);
    }

    DeleteWindow(window, 1);
}

static void
xmir_unmap_input(struct xmir_screen *xmir_screen, WindowPtr window)
{
    struct xmir_input *xmir_input;

    xorg_list_for_each_entry(xmir_input, &xmir_screen->input_list, link) {
        if (xmir_input->focus_window && xmir_input->focus_window->window == window)
            xmir_input->focus_window = NULL;
    }
}

static void
xmir_unmap_surface(struct xmir_screen *xmir_screen, WindowPtr window, BOOL destroyed)
{
    struct xmir_window *xmir_window =
        dixLookupPrivate(&window->devPrivates, &xmir_window_private_key);

    if (!xmir_window)
        return;

    if (!destroyed)
        xmir_window_disable_damage_tracking(xmir_window);
    else
        xmir_window->damage = NULL;

    xorg_list_del(&xmir_window->link_damage);

    if (xmir_screen->glamor)
        xmir_glamor_unrealize_window(xmir_screen, xmir_window, window);

    if (!xmir_window->surface)
        return;

    mir_surface_release_sync(xmir_window->surface);
    xmir_window->surface = NULL;

    /* drain all events from input and damage to prevent a race condition after mir_surface_release_sync */
    xmir_process_from_eventloop();

    RegionUninit(&xmir_window->region);
}

static Bool
xmir_unrealize_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    Bool ret;

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

    xmir_unmap_input(xmir_screen, window);
    xmir_unmap_surface(xmir_screen, window, TRUE);

    screen->DestroyWindow = xmir_screen->DestroyWindow;
    ret = (*screen->DestroyWindow) (window);
    xmir_screen->DestroyWindow = screen->DestroyWindow;
    screen->DestroyWindow = xmir_destroy_window;

    return ret;
}

static Bool
xmir_close_screen(ScreenPtr screen)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_output *xmir_output, *next_xmir_output;
    Bool ret;

    if (xmir_screen->glamor && xmir_screen->gbm)
        DRI2CloseScreen(screen);

    screen->CloseScreen = xmir_screen->CloseScreen;
    ret = screen->CloseScreen(screen);

    xorg_list_for_each_entry_safe(xmir_output, next_xmir_output,
                                  &xmir_screen->output_list, link)
        xmir_output_destroy(xmir_output);

    xmir_glamor_fini(xmir_screen);
    mir_display_config_destroy(xmir_screen->display);
    mir_connection_release(xmir_screen->conn);

    xmir_fini_thread_to_eventloop();
    free(xmir_screen->driver_name);
    free(xmir_screen->submit_rendering_handler);
    free(xmir_screen->input_handler);
    free(xmir_screen->hotplug_event_handler);
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
    return !xmir_screen->rootless;
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
    } else {
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
xmir_block_handler(ScreenPtr screen, void *ptv, void *read_mask)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_window *xmir_window, *next;

    xorg_list_for_each_entry_safe(xmir_window, next,
                                  &xmir_screen->damage_window_list,
                                  link_damage) {
        if (xmir_window->has_free_buffer) {
            RegionPtr dirty = xmir_window_get_dirty(xmir_screen, xmir_window);

            xmir_buffer_copy(xmir_screen, xmir_window, dirty);
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
        screen->devPrivate = screen->CreatePixmap(screen, screen->width, screen->height, screen->rootDepth, CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
    else
        screen->devPrivate = fbCreatePixmap(screen, 0, 0, screen->rootDepth, 0);

    if (!screen->devPrivate)
        return FALSE;

#ifdef GLAMOR_HAS_GBM
    if (xmir_screen->glamor && !xmir_screen->rootless) {
        glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(screen->devPrivate);

        glBindFramebuffer(GL_FRAMEBUFFER, pixmap_priv->base.fbo->fb);
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

    xmir_screen = calloc(sizeof *xmir_screen, 1);
    if (!xmir_screen)
        return FALSE;

    xmir_screen->conn = NULL;

    xmir_init_thread_to_eventloop();
    dixSetPrivate(&pScreen->devPrivates, &xmir_screen_private_key, xmir_screen);
    xmir_screen->screen = pScreen;
    xmir_screen->submit_rendering_handler = xmir_register_handler(&xmir_handle_buffer_available, sizeof (struct xmir_window *));
    xmir_screen->input_handler = xmir_register_handler(&xmir_handle_input_in_main_thread, sizeof (XMirEventContext));
    xmir_screen->glamor = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-rootless") == 0) {
            xmir_screen->rootless = 1;
        } else if (strcmp(argv[i], "-mir") == 0) {
            appid = argv[++i];
        } else if (strcmp(argv[i], "-mirSocket") == 0) {
            socket = argv[++i];
        } else if (strcmp(argv[i], "-sw") == 0) {
            xmir_screen->glamor = 0;
        } else if (strcmp(argv[i], "-egl") == 0) {
            if (xmir_screen->glamor < 2)
                xmir_screen->glamor = 2;
        } else if (strcmp(argv[i], "-2x") == 0) {
            xmir_screen->doubled = 1;
        } else if (strcmp(argv[i], "-damage") == 0) {
            xmir_screen->damage_all = true;
        } else if (strcmp(argv[i], "-egl_sync") == 0) {
            xmir_screen->glamor = 3;
        } else if (strcmp(argv[i], "-fd") == 0) {
            client_fd = (int)strtol(argv[++i], (char **)NULL, 0);
        }
    }

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
    xmir_screen->depth = 24;

    mir_connection_get_available_surface_formats(xmir_screen->conn,
        format, sizeof(format)/sizeof(format[0]), &formats);
    for (f = 0; f < formats; ++f) {
        switch (format[i]) {
        case mir_pixel_format_argb_8888:
        case mir_pixel_format_abgr_8888:
            xmir_screen->depth32_pixel_format = format[i];
            break;
        case mir_pixel_format_xrgb_8888:
        case mir_pixel_format_xbgr_8888:
        case mir_pixel_format_bgr_888:
     /* case mir_pixel_format_rgb_888:  in Mir 0.15 when landed */
            xmir_screen->depth24_pixel_format = format[i];
            break;
        default:
            ErrorF("Unrecognized Mir pixel format: %d\n", format[i]);
            break;
        }
    }

    xmir_screen->display = mir_connection_create_display_config(conn);
    if (xmir_screen->display == NULL) {
        FatalError("could not create display config\n");
        return FALSE;
    }

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
 /* case mir_pixel_format_rgb_888:  in Mir 0.15 */
        red_mask = 0x0000ff;
        blue_mask = 0xff0000;
        break;
    default:
        ErrorF("No Mir-compatible TrueColor formats (or memory corruption FIXME)\n");
        return FALSE;
    }

    miSetVisualTypesAndMasks(xmir_screen->depth,
                             ((1 << TrueColor) | (1 << DirectColor)),
                             bpc, TrueColor,
                             red_mask, green_mask, blue_mask);

    miSetPixmapDepths();

    ret = fbScreenInit(pScreen, NULL,
                       pScreen->width, pScreen->height,
                       96, 96, 0,
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
        if (xmir_screen->glamor > 1)
            FatalError("EGL requested, but not available\n");
        xmir_screen->glamor = 0;
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

    xmir_screen->UnrealizeWindow = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = xmir_unrealize_window;

    xmir_screen->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = xmir_close_screen;

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
