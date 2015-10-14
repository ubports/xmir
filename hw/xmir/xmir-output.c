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

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "xmir.h"
#include <randrstr.h>
#include "glamor_priv.h"
#include "mipointer.h"

static const char*
xmir_get_output_type_str(MirDisplayOutput *mir_output)
{
    const char *str = "Invalid";

    switch(mir_output->type)
    {
    case mir_display_output_type_vga: str = "VGA"; break;
    case mir_display_output_type_dvii: str = "DVI"; break;
    case mir_display_output_type_dvid: str = "DVI"; break;
    case mir_display_output_type_dvia: str = "DVI"; break;
    case mir_display_output_type_composite: str = "Composite"; break;
    case mir_display_output_type_svideo: str = "TV"; break;
    case mir_display_output_type_lvds: str = "LVDS"; break;
    case mir_display_output_type_component: str = "CTV"; break;
    case mir_display_output_type_ninepindin: str = "DIN"; break;
    case mir_display_output_type_displayport: str = "DP"; break;
    case mir_display_output_type_hdmia: str = "HDMI"; break;
    case mir_display_output_type_hdmib: str = "HDMI"; break;
    case mir_display_output_type_tv: str = "TV"; break;
    case mir_display_output_type_edp: str = "eDP"; break;
    case mir_display_output_type_unknown: str = "None"; break;
    default: break;
    }

    return str;
}

static Rotation
to_rr_rotation(MirOrientation orient)
{
    switch (orient) {
    default: return RR_Rotate_0;
    case mir_orientation_left: return RR_Rotate_90;
    case mir_orientation_inverted: return RR_Rotate_180;
    case mir_orientation_right: return RR_Rotate_270;
    }
}

Bool
xmir_output_dpms(struct xmir_screen *xmir_screen, int mode)
{
    MirDisplayConfiguration *display_config = xmir_screen->display;
    MirPowerMode mir_mode = mir_power_mode_on;
    Bool unchanged = TRUE;

    if (xmir_screen->rootless)
        return FALSE;

    switch (mode) {
    case DPMSModeOn:
        mir_mode = mir_power_mode_on;
        xmir_screen->dpms_on = TRUE;
        break;
    case DPMSModeStandby:
        mir_mode = mir_power_mode_standby;
        xmir_screen->dpms_on = FALSE;
        break;
    case DPMSModeSuspend:
        mir_mode = mir_power_mode_suspend;
        xmir_screen->dpms_on = FALSE;
        break;
    case DPMSModeOff:
        mir_mode = mir_power_mode_off;
        xmir_screen->dpms_on = FALSE;
        break;
    }

    DebugF("Setting DPMS mode to %d\n", mode);

    for (int i = 0; i < display_config->num_outputs; i++) {
        if (display_config->outputs[i].power_mode != mir_mode) {
            display_config->outputs[i].power_mode = mir_mode;
            unchanged = FALSE;
        }
    }

    if (!unchanged)
        mir_wait_for(mir_connection_apply_display_config(xmir_screen->conn, xmir_screen->display));

    return TRUE;
}

static void
xmir_output_update(struct xmir_output *xmir_output, MirDisplayOutput *mir_output)
{
    RROutputSetConnection(xmir_output->randr_output, mir_output->connected ? RR_Connected : RR_Disconnected);
    RROutputSetSubpixelOrder(xmir_output->randr_output, SubPixelUnknown);

    if (mir_output->connected && mir_output->used) {
        MirDisplayMode *mode = &mir_output->modes[mir_output->current_mode];
        RRModePtr randr_mode;

        xmir_output->width = mode->horizontal_resolution;
        xmir_output->height = mode->vertical_resolution;
        xmir_output->x = mir_output->position_x;
        xmir_output->y = mir_output->position_y;

        randr_mode = xmir_cvt(xmir_output->width, xmir_output->height, mode->refresh_rate, 0, 0);
        /* Odd resolutions like 1366x768 don't show correctly otherwise */
        randr_mode->mode.width = mode->horizontal_resolution;
        randr_mode->mode.height = mode->vertical_resolution;
        sprintf(randr_mode->name, "%dx%d@%.1fHz",
                randr_mode->mode.width, randr_mode->mode.height, mode->refresh_rate);

        RROutputSetPhysicalSize(xmir_output->randr_output, mir_output->physical_width_mm, mir_output->physical_height_mm);
        RROutputSetModes(xmir_output->randr_output, &randr_mode, 1, 1);

        // TODO: Hook up subpixel order when available
        RRCrtcNotify(xmir_output->randr_crtc, randr_mode,
                     xmir_output->x, xmir_output->y,
                     to_rr_rotation(mir_output->orientation), NULL, 1, &xmir_output->randr_output);
    } else {
        xmir_output->width = xmir_output->height = xmir_output->x = xmir_output->y = 0;

        RROutputSetPhysicalSize(xmir_output->randr_output, 0, 0);
        RROutputSetModes(xmir_output->randr_output, NULL, 0, 0);

        RRCrtcNotify(xmir_output->randr_crtc, NULL,
                     0, 0, RR_Rotate_0, NULL, 1, &xmir_output->randr_output);
    }
}

static void
xmir_output_screen_resized(struct xmir_screen *xmir_screen)
{
    ScreenPtr screen = xmir_screen->screen;
    struct xmir_output *xmir_output;
    int width, height;

    width = 0;
    height = 0;
    xorg_list_for_each_entry(xmir_output, &xmir_screen->output_list, link) {
        if (width < xmir_output->x + xmir_output->width)
            width = xmir_output->x + xmir_output->width;
        if (height < xmir_output->y + xmir_output->height)
            height = xmir_output->y + xmir_output->height;
    }

    screen->width = width;
    screen->height = height;
    if (ConnectionInfo)
        RRScreenSizeNotify(xmir_screen->screen);
    update_desktop_dimensions();
}

static void
xmir_output_create(struct xmir_screen *xmir_screen, MirDisplayOutput *mir_output, const char *name)
{
    struct xmir_output *xmir_output;

    xmir_output = calloc(sizeof *xmir_output, 1);
    if (xmir_output == NULL) {
        FatalError("No memory for creating output\n");
        return;
    }

    xmir_output->xmir_screen = xmir_screen;
    xmir_output->randr_crtc = RRCrtcCreate(xmir_screen->screen, xmir_output);
    xmir_output->randr_output = RROutputCreate(xmir_screen->screen, name, strlen(name), xmir_output);

    RRCrtcGammaSetSize(xmir_output->randr_crtc, 256);
    RROutputSetCrtcs(xmir_output->randr_output, &xmir_output->randr_crtc, 1);
    xorg_list_append(&xmir_output->link, &xmir_screen->output_list);
    if (mir_output)
        xmir_output_update(xmir_output, mir_output);
}

void
xmir_output_destroy(struct xmir_output *xmir_output)
{
    xorg_list_del(&xmir_output->link);
    free(xmir_output);
}

static Bool
xmir_randr_get_info(ScreenPtr pScreen, Rotation * rotations)
{
    *rotations = 0;

    return TRUE;
}

static Bool
xmir_randr_set_config(ScreenPtr pScreen,
                     Rotation rotation, int rate, RRScreenSizePtr pSize)
{
    return FALSE;
}

static void
xmir_update_config(struct xmir_screen *xmir_screen)
{
    MirDisplayConfiguration *new_config;
    MirDisplayOutput **mir_output;
    struct xmir_output *xmir_output;

    if (xmir_screen->windowed)
        return;

    new_config = mir_connection_create_display_config(xmir_screen->conn);
    if (new_config->num_outputs != xmir_screen->display->num_outputs)
        FatalError("Number of outputs changed on update.\n");

    mir_display_config_destroy(xmir_screen->display);
    xmir_screen->display = new_config;

    mir_output = &new_config->outputs;
    xorg_list_for_each_entry(xmir_output, &xmir_screen->output_list, link) {
        xmir_output_update(xmir_output, *mir_output);
        mir_output++;
    }
    xmir_output_screen_resized(xmir_screen);
}

void
xmir_output_handle_orientation(struct xmir_window *xmir_window, MirOrientation dir)
{
    ErrorF("Orientation: %i\n", dir);

    xmir_output_handle_resize(xmir_window, -1, -1);
}

void
xmir_output_handle_resize(struct xmir_window *xmir_window, int width, int height)
{
    WindowPtr window = xmir_window->window;
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    PixmapPtr pixmap;
    BoxRec box;
    int window_width, window_height;
    DeviceIntPtr pDev;

    MirOrientation old = xmir_window->orientation;
    xmir_window->orientation = mir_surface_get_orientation(xmir_window->surface);

    if (width < 0 && height < 0) {
        if (old % 180 == xmir_window->orientation % 180) {
            window_width = window->drawable.width;
            window_height = window->drawable.height;
        } else {
            window_width = window->drawable.height;
            window_height = window->drawable.width;
        }
    } else if (xmir_window->orientation == 0 || xmir_window->orientation == 180) {
        window_width = width * (1 + xmir_screen->doubled);
        window_height = height * (1 + xmir_screen->doubled);
    } else {
        window_width = height * (1 + xmir_screen->doubled);
        window_height = width * (1 + xmir_screen->doubled);
    }

    if (window_width == window->drawable.width &&
        window_height == window->drawable.height) {
        /* Damage window if rotated */
        if (old != xmir_window->orientation)
            DamageDamageRegion(&window->drawable, &xmir_window->region);
        return;
    }

    /* In case of async EGL, destroy the image after swap has finished */
    if (xmir_window->image) {
        if (!xmir_window->has_free_buffer) {
            while (1) {
                xmir_process_from_eventloop();
                if (xmir_window->has_free_buffer)
                    break;
                usleep(1000);
            }
        }

        eglDestroyImageKHR(xmir_screen->egl_display, xmir_window->image);
        xmir_window->image = NULL;
    }

    if (xmir_screen->rootless)
        return;

    if (!xmir_screen->windowed) {
        xmir_screen->windowed = 1;

        ErrorF("Root resized, removing all outputs and inserting fake output\n");

        while (!xorg_list_is_empty(&xmir_screen->output_list)) {
            struct xmir_output *xmir_output = xorg_list_first_entry(&xmir_screen->output_list, typeof(*xmir_output), link);

            RRCrtcDestroy(xmir_output->randr_crtc);
            RROutputDestroy(xmir_output->randr_output);
            xmir_output_destroy(xmir_output);
        }
    }

    ErrorF("Output resized %ix%i with rotation %i\n",
           width, height, xmir_window->orientation);

    screen->width = window_width;
    screen->height = window_height;
    screen->mmWidth = screen->mmHeight = 0;
    if (ConnectionInfo)
        RRScreenSizeNotify(xmir_screen->screen);
    update_desktop_dimensions();

    pixmap = screen->CreatePixmap(screen, window_width, window_height, screen->rootDepth, CREATE_PIXMAP_USAGE_BACKING_PIXMAP);

    box.x1 = box.y1 = 0;
    box.x2 = window_width;
    box.y2 = window_height;

    if (xmir_screen->glamor) {
        glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(pixmap);
        DrawablePtr oldroot = &screen->root->drawable;
        BoxRec copy_box;

        copy_box.x1 = copy_box.y1 = 0;
        copy_box.x2 = min(window_width, oldroot->width);
        copy_box.y2 = min(window_height, oldroot->height);

        glBindFramebuffer(GL_FRAMEBUFFER, pixmap_priv->base.fbo->fb);
        glClearColor(0., 0., 0., 1.);
        glClear(GL_COLOR_BUFFER_BIT);

        glamor_copy_n_to_n_nf(&screen->root->drawable, &pixmap->drawable,
                              NULL, &copy_box, 1, 0, 0, FALSE, FALSE, 0, NULL);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    screen->SetScreenPixmap(pixmap);

    window->drawable.width = box.x2;
    window->drawable.height = box.y2;

    RegionReset(&xmir_window->region, &box);

    RegionReset(&window->winSize, &box);
    RegionReset(&window->clipList, &box);
    RegionReset(&window->borderSize, &box);
    RegionReset(&window->borderClip, &box);
    DamageDamageRegion(&window->drawable, &xmir_window->region);

    /* Update cursor info too */
    for (pDev = inputInfo.devices; pDev; pDev = pDev->next) {
        int x, y;

        if (!IsPointerDevice(pDev))
            continue;

        miPointerGetPosition(pDev, &x, &y);
        UpdateSpriteForScreen(pDev, screen);
        miPointerSetScreen(pDev, 0, x, y);
    }
}

static void
xmir_handle_hotplug(void *ctx)
{
    struct xmir_screen *xmir_screen = *(struct xmir_screen **)ctx;

    xmir_update_config(xmir_screen);

    /* Trigger RANDR refresh */
    RRGetInfo(screenInfo.screens[0], TRUE);
}

static void
xmir_display_config_callback(MirConnection *conn, void *ctx)
{
    struct xmir_screen *xmir_screen = ctx;

    xmir_post_to_eventloop(xmir_screen->hotplug_event_handler, &xmir_screen);
}

Bool
xmir_screen_init_output(struct xmir_screen *xmir_screen)
{
    rrScrPrivPtr rp;
    int i;
    MirDisplayConfiguration *display_config = xmir_screen->display;
    int output_type_count[mir_display_output_type_edp + 1] = {};

    if (!RRScreenInit(xmir_screen->screen))
        return FALSE;

    /* Hook up hotplug notification */
    xmir_screen->hotplug_event_handler = xmir_register_handler(&xmir_handle_hotplug, sizeof (*xmir_screen));

    mir_connection_set_display_config_change_callback(xmir_screen->conn, &xmir_display_config_callback, xmir_screen);

    for (i = 0; i < display_config->num_outputs; i++) {
        char name[32];
        MirDisplayOutput *mir_output = &display_config->outputs[i];
        const char* output_type_str = xmir_get_output_type_str(mir_output);
        int type_count = i;

        if (mir_output->type >= 0 && mir_output->type <= mir_display_output_type_edp)
            type_count = output_type_count[mir_output->type]++;

        snprintf(name, sizeof name, "%s-%d", output_type_str, type_count);
        xmir_output_create(xmir_screen, mir_output, name);
    }

    RRScreenSetSizeRange(xmir_screen->screen, 320, 200, INT16_MAX, INT16_MAX);

    xmir_output_screen_resized(xmir_screen);

    rp = rrGetScrPriv(xmir_screen->screen);
    rp->rrGetInfo = xmir_randr_get_info;
    rp->rrSetConfig = xmir_randr_set_config;
    // TODO: rp->rrCrtcSet = xmir_randr_set_crtc;

    return TRUE;
}
