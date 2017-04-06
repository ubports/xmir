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

#include "xmir.h"

#include <mipointer.h>

static DevPrivateKeyRec xmir_cursor_private_key;

static void
expand_source_and_mask(CursorPtr cursor, void *data)
{
    CARD32 *p, d, fg, bg;
    CursorBitsPtr bits = cursor->bits;
    int x, y, stride, i, bit;

    p = data;
    fg = ((cursor->foreRed & 0xff00) << 8) |
        (cursor->foreGreen & 0xff00) | (cursor->foreGreen >> 8);
    bg = ((cursor->backRed & 0xff00) << 8) |
        (cursor->backGreen & 0xff00) | (cursor->backGreen >> 8);
    stride = (bits->width / 8 + 3) & ~3;
    for (y = 0; y < bits->height; y++)
        for (x = 0; x < bits->width; x++) {
            i = y * stride + x / 8;
            bit = 1 << (x & 7);
            if (bits->source[i] & bit)
                d = fg;
            else
                d = bg;
            if (bits->mask[i] & bit)
                d |= 0xff000000;
            else
                d = 0x00000000;

            *p++ = d;
        }
}

static Bool
xmir_realize_cursor(DeviceIntPtr device, ScreenPtr screen, CursorPtr cursor)
{
    return TRUE;
}

static void xmir_input_set_cursor(struct xmir_input *xmir_input,
                                  CursorPtr cursor);

static Bool
xmir_unrealize_cursor(DeviceIntPtr device, ScreenPtr screen, CursorPtr cursor)
{
    struct xmir_input *xmir_input = device ? device->public.devicePrivate : NULL;
    MirBufferStream *stream;

    stream = dixGetPrivate(&cursor->devPrivates, &xmir_cursor_private_key);
    dixSetPrivate(&cursor->devPrivates, &xmir_cursor_private_key, NULL);

    if (xmir_input)
        xmir_input_set_cursor(xmir_input, rootCursor);

    if (stream)
        mir_buffer_stream_release_sync(stream);

    return TRUE;
}

static void
xmir_input_set_cursor(struct xmir_input *xmir_input, CursorPtr cursor)
{
    MirGraphicsRegion region;
    MirCursorConfiguration *config;
    MirBufferStream *stream;

    if (!cursor) {
        config = mir_cursor_configuration_from_name(mir_disabled_cursor_name);
        goto apply;
    }
    else if (cursor == rootCursor) {
        /* Avoid using the old style X default black cross cursor */
        config = mir_cursor_configuration_from_name(mir_arrow_cursor_name);
        goto apply;
    }

    stream = dixGetPrivate(&cursor->devPrivates, &xmir_cursor_private_key);
    if (stream) {
        mir_buffer_stream_get_graphics_region(stream, &region);
        if (region.width != cursor->bits->width ||
            region.height != cursor->bits->height) {
            mir_buffer_stream_release_sync(stream);
            stream = NULL;
        }
    }

    if (!stream) {
        stream = mir_connection_create_buffer_stream_sync(xmir_input->xmir_screen->conn,
                                                          cursor->bits->width,
                                                          cursor->bits->height,
                                                          mir_pixel_format_argb_8888,
                                                          mir_buffer_usage_software);
        if (!stream) {
            ErrorF("xmir_input_set_cursor: "
                   "mir_connection_create_buffer_stream_sync failed\n");
            return;
        }

        mir_buffer_stream_set_swapinterval(stream, 0);
        dixSetPrivate(&cursor->devPrivates, &xmir_cursor_private_key, stream);
        mir_buffer_stream_get_graphics_region(stream, &region);
    }

    if (cursor->bits->argb) {
        int y, stride;

        stride = cursor->bits->width * 4;
        for (y = 0; y < cursor->bits->height; y++)
           memcpy(region.vaddr + y * region.stride,
                  (char*)cursor->bits->argb + y * stride, stride);
    }
    else
        expand_source_and_mask(cursor, region.vaddr);

    mir_buffer_stream_swap_buffers(stream, NULL, NULL);
    config = mir_cursor_configuration_from_buffer_stream(stream,
                                                         cursor->bits->xhot,
                                                         cursor->bits->yhot);

apply:
    if (!xmir_input->xmir_screen->rootless) {
        struct xmir_window *w = xmir_window_get(xmir_input->xmir_screen->screen->root);
        mir_window_configure_cursor(w->surface, config);
    }
    else if (xmir_input->focus_window)
        mir_window_configure_cursor(xmir_input->focus_window->surface, config);
    mir_cursor_configuration_destroy(config);
}

static void
xmir_set_cursor(DeviceIntPtr device,
                ScreenPtr screen, CursorPtr cursor, int x, int y)
{
    struct xmir_input *xmir_input;

    xmir_input = device->public.devicePrivate;
    if (xmir_input == NULL)
        return;

    xmir_input_set_cursor(xmir_input, cursor);
}

static void
xmir_move_cursor(DeviceIntPtr device, ScreenPtr screen, int x, int y)
{
}

static Bool
xmir_device_cursor_initialize(DeviceIntPtr device, ScreenPtr screen)
{
    return TRUE;
}

static void
xmir_device_cursor_cleanup(DeviceIntPtr device, ScreenPtr screen)
{
}

static miPointerSpriteFuncRec xmir_pointer_sprite_funcs = {
    xmir_realize_cursor,
    xmir_unrealize_cursor,
    xmir_set_cursor,
    xmir_move_cursor,
    xmir_device_cursor_initialize,
    xmir_device_cursor_cleanup
};

static Bool
xmir_cursor_off_screen(ScreenPtr *ppScreen, int *x, int *y)
{
    return FALSE;
}

static void
xmir_cross_screen(ScreenPtr pScreen, Bool entering)
{
}

static void
xmir_pointer_warp_cursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
}

static miPointerScreenFuncRec xmir_pointer_screen_funcs = {
    xmir_cursor_off_screen,
    xmir_cross_screen,
    xmir_pointer_warp_cursor
};

Bool
xmir_screen_init_cursor(struct xmir_screen *xmir_screen)
{
    if (!dixRegisterPrivateKey(&xmir_cursor_private_key,
                               PRIVATE_CURSOR_BITS, 0))
        return FALSE;

    return miPointerInitialize(xmir_screen->screen,
                               &xmir_pointer_sprite_funcs,
                               &xmir_pointer_screen_funcs, TRUE);
}
