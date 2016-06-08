/*
 * Copyright © 2012-2015 Canonical Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Soft-
 * ware"), to deal in the Software without restriction, including without
 * limitation the rights to use, copy, modify, merge, publish, distribute,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, provided that the above copyright
 * notice(s) and this permission notice appear in all copies of the Soft-
 * ware and that both the above copyright notice(s) and this permission
 * notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABIL-
 * ITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY
 * RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN
 * THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSE-
 * QUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFOR-
 * MANCE OF THIS SOFTWARE.
 *
 * Except as contained in this notice, the name of a copyright holder shall
 * not be used in advertising or otherwise to promote the sale, use or
 * other dealings in this Software without prior written authorization of
 * the copyright holder.
 *
 * Authors:
 *   Christopher James Halse Rogers (christopher.halse.rogers@canonical.com)
 * Later rewritten, simplified and optimized by:
 *   Daniel van Vugt <daniel.van.vugt@canonical.com>
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "xmir.h"

struct message {
    xmir_event_callback *callback;
    struct xmir_screen *xmir_screen;
    struct xmir_window *xmir_window;
    void *arg;
};

static int pipefds[2];

static void
xmir_wakeup_handler(void* data, int err, void* read_mask)
{
    if (err >= 0 && FD_ISSET(pipefds[0], (fd_set *)read_mask))
        xmir_process_from_eventloop();
}

void
xmir_init_thread_to_eventloop(void)
{
	int err = pipe(pipefds);
	if (err == -1)
		FatalError("[XMIR] Failed to create thread-proxy pipes: %s\n", strerror(errno));

	/* Set the read end to not block; we'll pull from this in the event loop
	 * We don't need to care about the write end, as that'll be written to
	 * from its own thread
	 */
	fcntl(pipefds[0], F_SETFL, O_NONBLOCK);

	AddGeneralSocket(pipefds[0]);
	RegisterBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
				       xmir_wakeup_handler,
				       NULL);
}

void
xmir_fini_thread_to_eventloop(void)
{
	RemoveBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
				     xmir_wakeup_handler, NULL);
	RemoveGeneralSocket(pipefds[0]);
	close(pipefds[1]);
	close(pipefds[0]);
}

void
xmir_post_to_eventloop(xmir_event_callback *cb,
                       struct xmir_screen *s, struct xmir_window *w, void *a)
{
    struct message msg = {cb, s, w, a};
    ssize_t written = write(pipefds[1], &msg, sizeof msg);
    if (written != sizeof(msg))
        ErrorF("[XMIR] Failed to proxy message to mainloop\n");
}

void
xmir_process_from_eventloop_except(const struct xmir_window *w)
{
    for (;;) {
        struct message msg;
        ssize_t got = read(pipefds[0], &msg, sizeof msg);
        if (got < 0)
            return;
        if (got == sizeof(msg) && w != msg.xmir_window)
            msg.callback(msg.xmir_screen, msg.xmir_window, msg.arg);
    }
}

void
xmir_process_from_eventloop(void)
{
    xmir_process_from_eventloop_except(NULL);
}
