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

#include "xmir.h"

#include <linux/input.h>

#include <sys/mman.h>
#include <xkbsrv.h>
#include <xserver-properties.h>
#include <inpututils.h>

static void
xmir_pointer_control(DeviceIntPtr device, PtrCtrl *ctrl)
{
    /* Nothing to do, dix handles all settings */
}

static int
xmir_pointer_proc(DeviceIntPtr device, int what)
{
#define NBUTTONS 10
#define NAXES 2
    BYTE map[NBUTTONS + 1];
    int i = 0;
    Atom btn_labels[NBUTTONS] = { 0 };
    Atom axes_labels[NAXES] = { 0 };

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;

        for (i = 1; i <= NBUTTONS; i++)
            map[i] = i;

        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
        btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
        btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
        btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
        /* Don't know about the rest */

        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y);

        if (!InitValuatorClassDeviceStruct(device, 2, btn_labels,
                                           GetMotionHistorySize(), Absolute))
            return BadValue;

        /* Valuators */
        InitValuatorAxisStruct(device, 0, axes_labels[0],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);
        InitValuatorAxisStruct(device, 1, axes_labels[1],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);

        if (!InitPtrFeedbackClassDeviceStruct(device, xmir_pointer_control))
            return BadValue;

        if (!InitButtonClassDeviceStruct(device, 3, btn_labels, map))
            return BadValue;

        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    }

    return BadMatch;

#undef NBUTTONS
#undef NAXES
}

static void
xmir_keyboard_control(DeviceIntPtr device, KeybdCtrl *ctrl)
{
}

static int
xmir_keyboard_proc(DeviceIntPtr device, int what)
{
    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;
        if (!InitKeyboardDeviceStructFromString(device,
                                                NULL /*xmir_input->keymap*/, 0,
                                                NULL, xmir_keyboard_control))
            return BadValue;

        return Success;
    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    }

    return BadMatch;
}

static void
pointer_convert_xy(struct xmir_input *xmir_input,
                   struct xmir_window *xmir_window,
                   int *x, int *y)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(xmir_window->window->drawable.pScreen);
    bool reflect_x = false;
    bool reflect_y = false;
    bool swap_xy = false;
    int dx = xmir_window->window->drawable.x;
    int dy = xmir_window->window->drawable.y;
    int sx = *x, sy = *y;
    int scale = 1 + xmir_screen->doubled;
    int w = xmir_window->window->drawable.width;
    int h = xmir_window->window->drawable.height;

    /* reflection test parameters */
    bool magic_x_invert = false, magic_y_invert = false;

    DebugF("Raw input %i,%i in window (%i,%i)->(%i,%i) orientation %i and scale %i\n", *x, *y, dx, dy, dx + w, dy + h, xmir_window->orientation, scale);

    if (magic_x_invert)
        reflect_x = !reflect_x;

    if (magic_y_invert)
        reflect_y = !reflect_y;

    switch (xmir_window->orientation) {
    case 90:
        reflect_x = !reflect_x; swap_xy = true; break;
    case 180:
        reflect_x = !reflect_x; reflect_y = !reflect_y; break;
    case 270:
        reflect_y = !reflect_y; swap_xy = true; break;
    }

    if (!swap_xy) {
        sx = *x;
        sy = *y;
    } else {
        sx = *y;
        sy = *x;
    }

    if (!reflect_x)
        *x = (sx * scale) + dx;
    else
        *x = w + dx - (sx * scale);

    if (!reflect_y)
        *y = (sy * scale) + dy;
    else
        *y = h + dy - (sy * scale);

    DebugF("Converted to %i, %i\n", *x, *y);
}

static Bool
pointer_ensure_focus(struct xmir_input *xmir_input,
                     struct xmir_window *xmir_window,
                     DeviceIntPtr dev, int sx, int sy)
{
    ScreenPtr screen = xmir_window->window->drawable.pScreen;

    if (xmir_input->focus_window == xmir_window)
        return FALSE;

    if (xmir_input->focus_window) {
        xmir_input->focus_window = NULL;
        CheckMotion(NULL, GetMaster(dev, MASTER_POINTER));
    }

    xmir_input->focus_window = xmir_window;

    pointer_convert_xy(xmir_input, xmir_window, &sx, &sy);

    (screen->SetCursorPosition) (dev, screen, sx, sy, TRUE);
    CheckMotion(NULL, GetMaster(dev, MASTER_POINTER));

    return TRUE;
}

static void
pointer_handle_motion(struct xmir_input *xmir_input,
                      struct xmir_window *xmir_window,
                      MirPointerEvent const *pev)
{
    int sx = mir_pointer_event_axis_value(pev, mir_pointer_axis_x);
    int sy = mir_pointer_event_axis_value(pev, mir_pointer_axis_y);
    int vscroll = 0;
    ValuatorMask mask;

    pointer_ensure_focus(xmir_input, xmir_window, xmir_input->pointer, sx, sy);

    pointer_convert_xy(xmir_input, xmir_window, &sx, &sy);

    valuator_mask_zero(&mask);
    valuator_mask_set(&mask, 0, sx);
    valuator_mask_set(&mask, 1, sy);

    QueuePointerEvents(xmir_input->pointer, MotionNotify, 0,
                       POINTER_ABSOLUTE | POINTER_SCREEN, &mask);

    /* Mouse wheel: Moving the wheel is a press+release of button 4/5 */
    vscroll = mir_pointer_event_axis_value(pev, mir_pointer_axis_vscroll);
    if (vscroll) {
        int button = vscroll < 0 ? 5 : 4;
        valuator_mask_zero(&mask);
        QueuePointerEvents(xmir_input->pointer, ButtonPress, button, 0, &mask);
        QueuePointerEvents(xmir_input->pointer, ButtonRelease, button, 0, &mask);
    }
}

static void
pointer_handle_button(struct xmir_input *xmir_input,
                      struct xmir_window *xmir_window,
                      MirPointerEvent const *pev)
{
    DeviceIntPtr dev = xmir_input->pointer;
    struct {MirPointerButton mir_button; int x_button;} map[3] =
    {
        {mir_pointer_button_primary, 1},   /* Usually left button */
        {mir_pointer_button_secondary, 3}, /* Middle button */
        {mir_pointer_button_tertiary, 2},  /* Right button */
    };
    int i;
    ValuatorMask mask;
    valuator_mask_zero(&mask);

    for (i = 0; i < 3; ++i) {
        MirPointerButton mir_button = map[i].mir_button;
        int x_button = map[i].x_button;
        int oldstate = BitIsOn(dev->button->down, x_button) ?
                       ButtonPress : ButtonRelease;
        int newstate = mir_pointer_event_button_state(pev, mir_button) ?
                       ButtonPress : ButtonRelease;

        if (oldstate != newstate)
            QueuePointerEvents(dev, newstate, x_button, 0, &mask);
    }

    /* XXX: Map rest of input buttons too! */
}

static DeviceIntPtr
add_device(struct xmir_input *xmir_input,
           const char *driver, DeviceProc device_proc)
{
    DeviceIntPtr dev = NULL;
    static Atom type_atom;
    char name[32];

    dev = AddInputDevice(serverClient, device_proc, TRUE);
    if (dev == NULL)
        return NULL;

    if (type_atom == None)
        type_atom = MakeAtom(driver, strlen(driver), TRUE);
    snprintf(name, sizeof name, "%s:%d", driver, xmir_input->id);
    AssignTypeAndName(dev, type_atom, name);
    dev->public.devicePrivate = xmir_input;
    dev->type = SLAVE;
    dev->spriteInfo->spriteOwner = FALSE;

    return dev;
}

static void
xmir_input_destroy(struct xmir_input *xmir_input)
{
    RemoveDevice(xmir_input->pointer, FALSE);
    RemoveDevice(xmir_input->keyboard, FALSE);
    free(xmir_input);
}

Bool
LegalModifier(unsigned int key, DeviceIntPtr pDev)
{
    return TRUE;
}

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

void
DDXRingBell(int volume, int pitch, int duration)
{
}

static WindowPtr
xmir_xy_to_window(ScreenPtr screen, SpritePtr sprite, int x, int y)
{
    struct xmir_input *xmir_input = NULL;
    DeviceIntPtr device;

    for (device = inputInfo.devices; device; device = device->next) {
        if (device->deviceProc == xmir_pointer_proc &&
            device->spriteInfo->sprite == sprite) {
            xmir_input = device->public.devicePrivate;
            break;
        }
    }

    if (xmir_input == NULL) {
        /* XTEST device */
        sprite->spriteTraceGood = 1;
        return sprite->spriteTrace[0];
    }

    if (xmir_input->focus_window) {
        sprite->spriteTraceGood = 2;
        sprite->spriteTrace[1] = xmir_input->focus_window->window;
        return miSpriteTrace(sprite, x, y);
    }
    else {
        sprite->spriteTraceGood = 1;
        return sprite->spriteTrace[0];
    }
}

static void
fake_touch_move(struct xmir_input *xmir_input, struct xmir_window *xmir_window, int sx, int sy)
{
    ValuatorMask mask;

    pointer_convert_xy(xmir_input, xmir_window, &sx, &sy);

    valuator_mask_zero(&mask);
    valuator_mask_set(&mask, 0, sx);
    valuator_mask_set(&mask, 1, sy);

    QueuePointerEvents(xmir_input->touch, MotionNotify, 0,
                       POINTER_ABSOLUTE | POINTER_SCREEN, &mask);
}

static void
xmir_window_handle_input_event(struct xmir_input *xmir_input,
                               struct xmir_window *xmir_window,
                               MirInputEvent const* ev)
{
    switch (mir_input_event_get_type(ev)) {
    case mir_input_event_type_key: {
        MirKeyboardEvent const *kev;
        MirKeyboardAction action;

        kev = mir_input_event_get_keyboard_event(ev);
        action = mir_keyboard_event_action(kev);

        QueueKeyboardEvents(xmir_input->keyboard,
                            action == mir_keyboard_action_up ? KeyRelease : KeyPress,
                            mir_keyboard_event_scan_code(kev) + 8);
        break;
    }
    case mir_input_event_type_touch: {
        MirTouchEvent const *tev;
        int i = 0, count, sx, sy;
        ValuatorMask mask;

        tev = mir_input_event_get_touch_event(ev);
        count = mir_touch_event_point_count(tev);

        /* Do we really need this multifinger tracking at all?... */
        if (count < 1) {
            xmir_input->touch_id = -1;
            break;
        }

        if (xmir_input->touch_id != -1) {
            for (i = 0; i < count; ++i)
                if (mir_touch_event_id(tev, i) == xmir_input->touch_id)
                    break;
        }
        if (i >= count) {
            for (i = 0; i < count; ++i)
                if (mir_touch_event_action(tev, i) == mir_touch_action_down)
                    break;
        }

        if (i >= count)
            break;

        sx = mir_touch_event_axis_value(tev, i, mir_touch_axis_x);
        sy = mir_touch_event_axis_value(tev, i, mir_touch_axis_y);
        valuator_mask_zero(&mask);

        switch (mir_touch_event_action(tev, i)) {
        case mir_touch_action_up:
            fake_touch_move(xmir_input, xmir_window, sx, sy);
            QueuePointerEvents(xmir_input->touch, ButtonRelease, 1, 0, &mask);
            xmir_input->touch_id = -1;
            break;
        case mir_touch_action_down:
            xmir_input->touch_id = mir_touch_event_id(tev, i);
            if (!pointer_ensure_focus(xmir_input, xmir_window, xmir_input->touch, sx, sy))
                fake_touch_move(xmir_input, xmir_window, sx, sy);
            QueuePointerEvents(xmir_input->touch, ButtonPress, 1, 0, &mask);
            break;
        case mir_touch_action_change:
            fake_touch_move(xmir_input, xmir_window, sx, sy);
            break;
        }
        break;


    }
    case mir_input_event_type_pointer: {
        MirPointerEvent const *pev;

        pev = mir_input_event_get_pointer_event(ev);
        switch (mir_pointer_event_action(pev)) {
        case mir_pointer_action_button_up:
        case mir_pointer_action_button_down:
            pointer_handle_motion(xmir_input, xmir_window, pev);
            pointer_handle_button(xmir_input, xmir_window, pev);
            break;
        case mir_pointer_action_motion:
            pointer_handle_motion(xmir_input, xmir_window, pev);
            break;
        default:
            ErrorF("Unknown action: %u\n", mir_pointer_event_action(pev));
        case mir_pointer_action_enter:
        case mir_pointer_action_leave:
            break;
        }
        break;
    }
    default: ErrorF("Unknown input type: %u\n", mir_input_event_get_type(ev));
    }
}

static void
xmir_handle_keymap_event(struct xmir_input *xmir_input,
                                MirKeymapEvent const* ev)
{
    char * buffer = NULL;
    size_t length = 0;
    DeviceIntPtr master;
    XkbDescPtr xkb;
    XkbChangesRec changes = { 0 };

    mir_keymap_event_get_keymap_buffer(ev, (char const **)&buffer, &length);

    buffer[length] = '\0';

    xkb = XkbCompileKeymapFromString(xmir_input->keyboard, buffer, length);

    XkbUpdateDescActions(xkb, xkb->min_key_code, XkbNumKeys(xkb), &changes);

    XkbDeviceApplyKeymap(xmir_input->keyboard, xkb);

    master = GetMaster(xmir_input->keyboard, MASTER_KEYBOARD);
    if (master && master->lastSlave == xmir_input->keyboard)
        XkbDeviceApplyKeymap(master, xkb);

    XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
}

static void
xmir_handle_surface_event_in_main_thread(struct xmir_screen *xmir_screen,
                                         struct xmir_window *xmir_window,
                                         void *arg)
{
    const MirEvent *ev = arg;
    struct xmir_input *xmir_input = xorg_list_first_entry(&xmir_screen->input_list, struct xmir_input, link);

    switch (mir_event_get_type(ev))
    {
    case mir_event_type_input:
        xmir_window_handle_input_event(xmir_input, xmir_window, mir_event_get_input_event(ev));
        break;
    case mir_event_type_surface:
        xmir_handle_surface_event(xmir_window, mir_surface_event_get_attribute(mir_event_get_surface_event(ev)), mir_surface_event_get_attribute_value(mir_event_get_surface_event(ev)));
        break;
    case mir_event_type_resize: {
        WindowPtr window = xmir_window->window;
        const MirResizeEvent *resize = mir_event_get_resize_event(ev);
        unsigned future_width = mir_resize_event_get_width(resize);
        unsigned future_height = mir_resize_event_get_height(resize);
        XMIR_DEBUG(("Mir surface for win %p resized to %ux%u (buffers arriving soon)\n",
                    window, future_width, future_height));
        xmir_window->surface_width = future_width;
        xmir_window->surface_height = future_height;
        if (xmir_window->damage)
            DamageDamageRegion(&window->drawable, &xmir_window->region);
        }
        break;
    case mir_event_type_prompt_session_state_change:
        ErrorF("No idea about prompt_session_state_change\n");
        break;
    case mir_event_type_orientation:
        xmir_output_handle_orientation(xmir_window, mir_orientation_event_get_direction(mir_event_get_orientation_event(ev)));
        break;
    case mir_event_type_close_surface:
        xmir_close_surface(xmir_window);
        break;
    case mir_event_type_surface_output:
        break;
    case mir_event_type_keymap:
        xmir_handle_keymap_event(xmir_input, mir_event_get_keymap_event(ev));
        break;
    default:
        ErrorF("Received an unknown %u event\n", mir_event_get_type(ev));
        break;
    }
    mir_event_unref(ev);
}

void
xmir_surface_handle_event(MirSurface *surface, MirEvent const* ev,
                          void *context)
{
    struct xmir_window *xmir_window = context;
    struct xmir_screen *xmir_screen = xmir_window->xmir_screen;

    /* We are in a Mir event thread, so unsafe to do X things. Post the event
     * to the X event loop thread...
     */
    xmir_post_to_eventloop(&xmir_handle_surface_event_in_main_thread,
        xmir_screen, xmir_window, (void*)mir_event_ref(ev));
}

void
InitInput(int argc, char *argv[])
{
    ScreenPtr pScreen = screenInfo.screens[0];
    struct xmir_screen *xmir_screen = xmir_screen_get(pScreen);
    struct xmir_input *xmir_input;

    if (xmir_screen->rootless)
        pScreen->XYToWindow = xmir_xy_to_window;

    mieqInit();

    xmir_input = calloc(1, sizeof(*xmir_input));
    if (!xmir_input)
        FatalError("Failed to allocate input\n");

    xmir_input->xmir_screen = xmir_screen;
    xorg_list_add(&xmir_input->link, &xmir_screen->input_list);
    xmir_input->touch_id = -1;
    xmir_input->pointer = add_device(xmir_input, "xmir-pointer", xmir_pointer_proc);
    xmir_input->touch = add_device(xmir_input, "xmir-fake-touch-pointer", xmir_pointer_proc);
    xmir_input->keyboard = add_device(xmir_input, "xmir-keyboard", xmir_keyboard_proc);
}

void
CloseInput(void)
{
    ScreenPtr pScreen = screenInfo.screens[0];
    struct xmir_screen *xmir_screen = xmir_screen_get(pScreen);
    struct xmir_input *xmir_input, *next_xmir_input;

    xorg_list_for_each_entry_safe(xmir_input, next_xmir_input,
                                  &xmir_screen->input_list, link)
        xmir_input_destroy(xmir_input);

    mieqFini();
}
