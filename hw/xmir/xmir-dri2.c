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

#include "glamor_priv.h"
#include "glamor_transform.h"
#include "xmir.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#define MESA_EGL_NO_X11_HEADERS
#include <gbm.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <mir_toolkit/mir_connection.h>
#include <mir_toolkit/mesa/platform_operation.h>

/* XMir dri2 support:
 *
 * Pixmaps:
 *  DRI2BufferFrontLeft: glamor pixmap
 *  DRI2BufferFakeFrontLeft: last page flipped bo
 *  DRI2BufferBackLeft: MirNativeBuffer
 *
 * Swap support:
 *   Page will get flipped from BackLeft to FrontLeft,
 *   but what happens is that we call mir_surface_swap_buffers
 *
 * There is no guarantee X and DRI2 is serialized, unless the
 * glXWaitGL and glXWaitX calls are used. These calls are implemented
 * by copying FakeFront to Front for glXWaitGL, and
 * Front to FakeFront for glXwaitX
 *
 * TODO:
 *  - Make xmir_dri2_copy_region do something.
 */

static char
is_fd_render_node(int fd)
{
    struct stat render;

    if (fstat(fd, &render))
        return 0;
    if (!S_ISCHR(render.st_mode))
        return 0;
    if (render.st_rdev & 0x80)
        return 1;

    return 0;
}

static Bool
xmir_dri2_flink(int drm_fd, unsigned int handle, unsigned int *name)
{
    struct drm_gem_flink flink;

    flink.handle = handle;
    if (ioctl(drm_fd, DRM_IOCTL_GEM_FLINK, &flink) < 0)
        return 0;
    *name = flink.name;
    return 1;
}

static struct xmir_window *
xmir_window_swappable_parent(WindowPtr win)
{
    ScreenPtr screen = win->drawable.pScreen;
    PixmapPtr root, pixmap;

    root = screen->GetScreenPixmap(screen);
    pixmap = screen->GetWindowPixmap(win);

    if (root == pixmap &&
        win->drawable.depth == pixmap->drawable.depth &&
        win->drawable.height == pixmap->drawable.height &&
        win->drawable.width == pixmap->drawable.width)
        return xmir_window_get(screen->root);

    return NULL;
}

static void
xmir_dri2_reusebuffer_notify(DrawablePtr draw, DRI2BufferPtr buf)
{
    struct xmir_window *xmir_window;
    struct xmir_screen *xmir_screen;
    struct xmir_pixmap *xmir_pixmap;
    struct gbm_bo *bo;

    if (buf->attachment != DRI2BufferBackLeft || draw->type != DRAWABLE_WINDOW)
        return;

    xmir_window = xmir_window_get((WindowPtr)draw);
    if (!xmir_window)
        return;

    xmir_screen = xmir_screen_get(draw->pScreen);
    if (xmir_window->back_pixmap)
        FatalError("Returned before swapping?\n");

    if (xmir_window->surface) {
        buf->driverPrivate = xmir_glamor_win_get_back(xmir_screen, xmir_window, &xmir_window->window->drawable);
        xmir_pixmap = xmir_pixmap_get(buf->driverPrivate);
    } else {
        struct xmir_window *xmir_window_parent = xmir_window_swappable_parent((WindowPtr)draw);

        if (xmir_window_parent && xmir_window_parent->back_pixmap)
            FatalError("Returned before swapping?!\n");

        if (buf->driverPrivate) {
            xmir_pixmap = xmir_pixmap_get(buf->driverPrivate);

            if (xmir_pixmap->fake_back && !xmir_window_parent)
                return;

            draw->pScreen->DestroyPixmap(buf->driverPrivate);
        }

        buf->driverPrivate = xmir_glamor_win_get_back(xmir_screen, xmir_window_parent ?: xmir_window, &xmir_window->window->drawable);
        xmir_pixmap = xmir_pixmap_get(buf->driverPrivate);
    }
    bo = xmir_pixmap->bo;
    if (!bo)
        FatalError("Uh oh!\n");

    buf->pitch = gbm_bo_get_stride(bo);
    xmir_dri2_flink(xmir_screen->drm_fd, gbm_bo_get_handle(bo).u32, &buf->name);
}

static void
xmir_dri2_auth_magic_reply(MirConnection* con, MirPlatformMessage* reply, Bool* ret)
{
    struct MirMesaAuthMagicResponse const* response;
    unsigned int opcode = mir_platform_message_get_opcode(reply);
    MirPlatformMessageData data = mir_platform_message_get_data(reply);

    *ret = 0;
    response = data.data;

    if (auth_magic != opcode ||
        data.size != sizeof response ||
        response == NULL)
    {
        mir_platform_message_release(reply);
        return;
    }

    /* status == 0 indciates success */
    if (response->status == 0)
        *ret = 1;
    mir_platform_message_release(reply);
}

static Bool
xmir_dri2_auth_magic(ScreenPtr screen, uint32_t magic)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    Bool ret = 0;
    MirPlatformMessage *msg = NULL;

    if (!is_fd_render_node(xmir_screen->drm_fd)) {
        struct MirMesaAuthMagicRequest req = {
            .magic = magic
        };
        msg = mir_platform_message_create(auth_magic);

        if (msg == NULL)
            return ret;

        mir_platform_message_set_data(msg, &req, sizeof req);
        mir_wait_for(mir_connection_platform_operation(
                xmir_screen->conn,
                msg,
                (mir_platform_operation_callback)&xmir_dri2_auth_magic_reply,
                &ret));
        mir_platform_message_release(msg);
    }

    return ret;
}

static DRI2BufferPtr
xmir_dri2_create_buffer(ScreenPtr screen, DrawablePtr pDraw,
                        unsigned int attachment, unsigned int format)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_pixmap *xmir_pixmap;
    struct xmir_window *xmir_window = NULL;
    PixmapPtr pixmap;
    DRI2BufferPtr ret = malloc(sizeof(*ret));
    struct gbm_bo *bo = NULL;
    unsigned int bpp = max(format, pDraw->bitsPerPixel);

    if (format && format < pDraw->bitsPerPixel && format < 24) {
        ErrorF("Format %u must match bpp %u for window\n", format, pDraw->bitsPerPixel);
        return NULL;
    }

    if (pDraw->type == DRAWABLE_WINDOW) {
        struct xmir_window *xmir_window_parent;

        xmir_window = xmir_window_get((WindowPtr)pDraw);
        xmir_window_parent = xmir_window_swappable_parent((WindowPtr)pDraw);

        if (xmir_window_parent)
            xmir_window = xmir_window_parent;

        pixmap = screen->GetWindowPixmap((WindowPtr)pDraw);
    } else
        pixmap = (PixmapPtr)pDraw;

    ret->attachment = attachment;
    ret->format = format;
    ret->driverPrivate = NULL;
    ret->flags = 0;
    ret->cpp = bpp / 8;

    ret->name = 0;
    ret->pitch = 0;

    switch (attachment) {
    case DRI2BufferFakeFrontLeft:
        if (xmir_window && xmir_window->front_pixmap) {
            xmir_pixmap = xmir_pixmap_get(xmir_window->front_pixmap);
            bo = xmir_pixmap->bo;
            break;
        }
        /* Fall-through */
    case DRI2BufferFrontLeft:
        xmir_pixmap = xmir_pixmap_get(pixmap);

        if (!xmir_pixmap) {
            CARD16 pitch;
            CARD32 size;

            ret->name = glamor_name_from_pixmap(pixmap, &pitch, &size);
            ret->pitch = pitch;
            return ret;
        }
        bo = xmir_pixmap->bo;

        if (!bo) {
            ErrorF("Window doesn't have a mir backing?\n");
            break;
        }
        break;
    case DRI2BufferBackLeft: {
        ret->driverPrivate = pixmap = xmir_glamor_win_get_back(xmir_screen, xmir_window, pDraw);
        bo = xmir_pixmap_get(pixmap)->bo;
        break;
    }
    default:
        ErrorF("Unsupported attachment %i\n", attachment);
        return NULL;
    }

    if (!bo) {
        ErrorF("Cannot create a %u attachment for a %u\n", attachment, pDraw->type);
        free(ret);
        return NULL;
    }
    DebugF("Allocated a %u attachment for %p/%u\n", attachment, pDraw, pDraw->type);
    ret->pitch = gbm_bo_get_stride(bo);
    xmir_dri2_flink(xmir_screen->drm_fd, gbm_bo_get_handle(bo).u32, &ret->name);
    return ret;
}

static void
xmir_dri2_destroy_buffer(ScreenPtr screen, DrawablePtr pDraw, DRI2BufferPtr buf)
{
    DebugF("DestroyBuffer %p/%u\n", buf, buf->attachment);

    if (buf->driverPrivate)
        screen->DestroyPixmap(buf->driverPrivate);
    free(buf);
}

static void
xmir_dri2_copy_region(ScreenPtr pScreen, DrawablePtr draw, RegionPtr region,
                      DRI2BufferPtr dest, DRI2BufferPtr src)
{
    struct xmir_window *xmir_window = NULL;
    ScreenPtr screen = draw->pScreen;

    if (draw->type != DRAWABLE_WINDOW)
        FatalError("Can't copy :-(\n");

    if (src->attachment == DRI2BufferFakeFrontLeft && dest->attachment == DRI2BufferFrontLeft)
        ErrorF("glXWaitGL\n");
    else if (src->attachment == DRI2BufferFrontLeft && dest->attachment == DRI2BufferFakeFrontLeft)
        ErrorF("glXWaitX\n");
    else {
        /* No swap interval, copy to front */
        int dx, dy;
        PixmapPtr dsrc = src->driverPrivate;
        PixmapPtr pixmap;

        if (draw->type == DRAWABLE_WINDOW) {
            pixmap = screen->GetWindowPixmap((WindowPtr)draw);
            xmir_window = xmir_window_get((WindowPtr)draw);
        } else
            pixmap = (PixmapPtr)draw;

        dx = draw->x - pixmap->screen_x;
        dy = draw->y - pixmap->screen_y;

        DebugF("Copying region! from %p/%u to %p/%u on %p/%u\n",
               src, src->attachment, dest, dest->attachment, draw, draw->type);

        glamor_set_destination_pixmap(pixmap);
        xmir_glamor_copy_egl_common(&dsrc->drawable, dsrc, glamor_get_pixmap_private(dsrc),
                                    RegionExtents(region), dsrc->drawable.width, dsrc->drawable.height,
                                    dx, dy, xmir_window ? xmir_window->orientation : 0);

        RegionTranslate(region, draw->x, draw->y);
        DamageDamageRegion(draw, region);
    }
}

static int
xmir_dri2_schedule_swap(ClientPtr client, DrawablePtr draw, DRI2BufferPtr dest, DRI2BufferPtr src,
                        CARD64 *target_msc, CARD64 divisor, CARD64 remainder,
                        DRI2SwapEventPtr func, void *data)
{
    ScreenPtr screen = draw->pScreen;
    struct xmir_pixmap *xmir_pixmap;
    struct xmir_window *xmir_window = NULL;
    PixmapPtr pixmap;
    int type;
    RegionRec region;
    int ret = 1;

    /* Noop on a glxpixmap */
    if (draw->type == DRAWABLE_WINDOW) {
        xmir_window = xmir_window_get((WindowPtr)draw);
        pixmap = screen->GetWindowPixmap((WindowPtr)draw);

        /* Make sure DRI2GetBuffers blocks, there is no updated buffer until the next flip */
        DRI2SwapLimit(draw, 1);
    } else {
        pixmap = (PixmapPtr)draw;
        memset(target_msc, 0, sizeof(*target_msc));
    }

    xmir_pixmap = xmir_pixmap_get(pixmap);
    if ((!xmir_pixmap || xmir_pixmap->fake_back) && (!xmir_window || !xmir_window->surface)) {
        PixmapRegionInit(&region, src->driverPrivate);

        if (draw->width == pixmap->drawable.width && draw->height == pixmap->drawable.height) {
            glamor_pixmap_fbo *glamor_front, *glamor_back;
            struct xmir_pixmap swap_pix;
            struct xmir_screen *xmir_screen = xmir_screen_get(screen);

            /* Exchange pixmap data with the front glamor pixmap, and update src name/pitch */
            DebugF("%s: Exchanging glamor pixmap from %ux%u to %ux%u\n",
                   GetClientCmdName(client), draw->width, draw->height,
                   pixmap->drawable.width, pixmap->drawable.height);
            type = DRI2_EXCHANGE_COMPLETE;

            src->pitch = gbm_bo_get_stride(xmir_pixmap->bo);
            xmir_dri2_flink(xmir_screen->drm_fd, gbm_bo_get_handle(xmir_pixmap->bo).u32, &src->name);

            glamor_front = glamor_pixmap_detach_fbo(glamor_get_pixmap_private(pixmap));
            glamor_back = glamor_pixmap_detach_fbo(glamor_get_pixmap_private(src->driverPrivate));

            glamor_pixmap_attach_fbo(pixmap, glamor_back);
            glamor_pixmap_attach_fbo(src->driverPrivate, glamor_front);

            swap_pix = *xmir_pixmap;
            *xmir_pixmap = *xmir_pixmap_get(src->driverPrivate);
            *xmir_pixmap_get(src->driverPrivate) = swap_pix;
        } else {
            PixmapPtr dsrc = src->driverPrivate;
            int dx = draw->x - pixmap->screen_x, dy = draw->y - pixmap->screen_y;

            type = DRI2_BLIT_COMPLETE;
            glamor_set_destination_pixmap(pixmap);

            DebugF("%s: Blitting into glamor pixmap from src %u,%u %ux%u@%u draw %u,%u %ux%u@%u to %u,%u %ux%u@%u\n",
                   GetClientCmdName(client), dsrc->drawable.x, dsrc->drawable.y,
                   dsrc->drawable.width, dsrc->drawable.height, dsrc->drawable.depth,
                   draw->x, draw->y, draw->width, draw->height, draw->depth,
                   pixmap->drawable.x, pixmap->drawable.y,
                   pixmap->drawable.width, pixmap->drawable.height, pixmap->drawable.depth);

            xmir_glamor_copy_egl_common(&dsrc->drawable, dsrc, glamor_get_pixmap_private(dsrc),
                                        RegionExtents(&region), dsrc->drawable.width, dsrc->drawable.height,
                                        dx, dy, xmir_window ? xmir_window->orientation : 0);
        }
        RegionTranslate(&region, draw->x, draw->y);
    } else {
        if (!xmir_window->surface)
            xmir_window = xmir_window_get(screen->root);

        if (xmir_window->back_pixmap)
            FatalError("Swapping twice?\n");

        /* Fastest case, no pixels need to be copied! */
        type = DRI2_FLIP_COMPLETE;

        xmir_window->back_pixmap = src->driverPrivate;
        src->driverPrivate = NULL;

        DebugF("%s: Queuing flip on %p\n", GetClientCmdName(client), draw);
    }

    if (!xmir_window) {
        DebugF("%s: No window, completing immediately\n", GetClientCmdName(client));

        DRI2SwapComplete(client, draw, 0, 0, 0, type, func, data);
        goto err;
    }

    xmir_window->flip = (typeof(xmir_window->flip)){ client, type, draw, func, data, xmir_window->flip.entry };

    if (!xmir_window->surface) {
        struct xorg_list *entry = &xmir_window->flip.entry;

        if (!xorg_list_is_empty(&xmir_window->flip.entry))
            FatalError("%s: Flipping child window repeatedly!\n", GetClientCmdName(client));

        while (!xmir_window->surface) {
           WindowPtr window = xmir_window->window->parent;
           if (!window) {
               ErrorF("%s: Could not find mir surface for swapping!\n", GetClientCmdName(client));
               ret = 0;
               goto err;
           }

           xmir_window = xmir_window_get(window);
        }
        xorg_list_add(entry, &xmir_window->flip.entry);
    }

    /* Must report damage after adding flip entry, in case flip completes immediately */
    if (type != DRI2_FLIP_COMPLETE) {
        DamageDamageRegion(draw, &region);
        RegionUninit(&region);
    } else
        DamageReportDamage(xmir_window->damage, &xmir_window->region);

    return 1;

err:
    RegionUninit(&region);
    DRI2SwapLimit(draw, 2);
    return ret;
}

static Bool
xmir_dri2_swap_limit_validate(DrawablePtr draw, int swap_limit)
{
	if ((swap_limit < 1) || (swap_limit > 2))
		return FALSE;

	return TRUE;
}

Bool
xmir_dri2_screen_init(struct xmir_screen *xmir_screen)
{
    const char *driverNames[2];
    Bool ret;
    drmVersion *vers = drmGetVersion(xmir_screen->drm_fd);
    const char *driver;

    if (!vers)
        return FALSE;

    xmir_screen->dri2.version = 9;

    /* Abuse the megablob ability to load all needed drivers */
    if (!strcmp(vers->name, "radeon"))
        driver = "r600";
    else
        driver = vers->name;

    driverNames[0] = driverNames[1] = xmir_screen->driver_name = strdup(driver);
    drmFreeVersion(vers);

    /* As far as I can tell, only legacy AuthMagic has a use for the fd.. oh well */
    xmir_screen->dri2.fd = xmir_screen->drm_fd;
    xmir_screen->dri2.driverName = driverNames[0];
    xmir_screen->dri2.deviceName = xmir_screen->device_name;

    xmir_screen->dri2.numDrivers = 2;
    xmir_screen->dri2.driverNames = driverNames;

    /* 6 */
    xmir_screen->dri2.ReuseBufferNotify = xmir_dri2_reusebuffer_notify;
    xmir_screen->dri2.SwapLimitValidate = xmir_dri2_swap_limit_validate;
    xmir_screen->dri2.ScheduleSwap = xmir_dri2_schedule_swap;

    /* 8 */
    xmir_screen->dri2.AuthMagic2 = xmir_dri2_auth_magic;

    /* 9 */
    xmir_screen->dri2.CreateBuffer2 = xmir_dri2_create_buffer;
    xmir_screen->dri2.DestroyBuffer2 = xmir_dri2_destroy_buffer;
    xmir_screen->dri2.CopyRegion2 = xmir_dri2_copy_region;

    ret = DRI2ScreenInit(xmir_screen->screen, &xmir_screen->dri2);
    return ret;
}
