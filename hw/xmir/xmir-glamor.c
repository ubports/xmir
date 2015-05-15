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

/* include glamor_priv instead of the public headers to get the prototype for glamor_copy_n_to_n */
#include "glamor_priv.h"
#include "xmir.h"

#include <mir_toolkit/mir_surface.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#define MESA_EGL_NO_X11_HEADERS
#include <gbm.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <pthread.h>
#include <mir_toolkit/mir_connection.h>
#include <mir_toolkit/mir_platform_message.h>
#include <mir_toolkit/mesa/platform_operation.h>

static void
xmir_glamor_egl_make_current(struct glamor_context *glamor_ctx)
{
    if (!glamor_ctx->drawable)
        eglMakeCurrent(glamor_ctx->display, EGL_NO_SURFACE,
                       EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (!eglMakeCurrent(glamor_ctx->display,
                        glamor_ctx->drawable, glamor_ctx->drawable,
                        glamor_ctx->ctx))
        FatalError("Failed to make EGL context current\n");
}

void
glamor_egl_screen_init(ScreenPtr screen, struct glamor_context *glamor_ctx)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);

    glamor_ctx->ctx = xmir_screen->egl_context;
    glamor_ctx->display = xmir_screen->egl_display;

    glamor_ctx->make_current = xmir_glamor_egl_make_current;
    glamor_ctx->drawable = xmir_screen->egl_surface;

    xmir_screen->glamor_ctx = glamor_ctx;
}

static PixmapPtr
xmir_glamor_win_reuse_pixmap(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, DrawablePtr draw)
{
    struct xmir_pixmap *xmir_pixmap;
    PixmapPtr ret;

    if (!xmir_win->reuse_pixmap)
        return NULL;

    ret = xmir_win->reuse_pixmap;
    xmir_win->reuse_pixmap = NULL;

    xmir_pixmap = xmir_pixmap_get(ret);
    eglDestroyImageKHR(xmir_screen->egl_display, xmir_pixmap->image);
    gbm_bo_destroy(xmir_pixmap->bo);
    memset(xmir_pixmap, 0, sizeof(*xmir_pixmap));
    return ret;
}

PixmapPtr
xmir_glamor_win_get_back(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, DrawablePtr draw)
{
    ScreenPtr screen = xmir_screen->screen;
    struct xmir_pixmap *xmir_pixmap;
    PixmapPtr ret = NULL;
    struct gbm_bo *bo;
    unsigned int tex;

    if (xmir_win) {
        if (xmir_win->back_pixmap) {
            ErrorF("Uh oh!\n");
            return xmir_win->back_pixmap;
        }

        ret = xmir_glamor_win_reuse_pixmap(xmir_screen, xmir_win, draw);
    }

    if (!ret)
        ret = screen->CreatePixmap(screen,
                                   draw->width, draw->height, draw->depth,
                                   XMIR_CREATE_PIXMAP_USAGE_FLIP);

    xmir_pixmap = xmir_pixmap_get(ret);

    if (xmir_win && xmir_win->surface) {
        MirNativeBuffer *buffer;
        struct gbm_import_fd_data gbm_data;

        mir_buffer_stream_get_current_buffer(mir_surface_get_buffer_stream(xmir_win->surface), &buffer);

        gbm_data.fd = buffer->fd[0];
        gbm_data.width = buffer->width;
        gbm_data.height = buffer->height;
        gbm_data.stride = buffer->stride;
        gbm_data.format = GBM_FORMAT_ARGB8888; /* TODO: detect this properly */

        bo = gbm_bo_import(xmir_screen->gbm, GBM_BO_IMPORT_FD, &gbm_data, GBM_BO_USE_RENDERING);
        xmir_pixmap->fake_back = false;
    } else {
        bo = gbm_bo_create(xmir_screen->gbm, draw->width, draw->height,
                           GBM_FORMAT_ARGB8888,
                           GBM_BO_USE_RENDERING);
        xmir_pixmap->fake_back = true;
    }
    if (!bo)
        FatalError("Failed to allocate bo\n");

    ret->devKind = gbm_bo_get_stride(bo);
    xmir_pixmap->bo = bo;
    xmir_pixmap->image = eglCreateImageKHR(xmir_screen->egl_display, xmir_screen->egl_context, EGL_NATIVE_PIXMAP_KHR, bo, NULL);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, xmir_pixmap->image);
    glBindTexture(GL_TEXTURE_2D, 0);
    glamor_set_pixmap_texture(ret, tex);

    return ret;
}

static void
complete_flips(struct xmir_window *xmir_win)
{
    if (xmir_win->flip.client) {
        DebugF("Flipping on %p\n", xmir_win->window);

        DRI2SwapComplete(xmir_win->flip.client, xmir_win->flip.draw, 0, 0, 0, xmir_win->flip.type, xmir_win->flip.func, xmir_win->flip.data);
        DRI2SwapLimit(xmir_win->flip.draw, 2);
        xmir_win->flip.client = NULL;
    }

    if (!xmir_win->surface) {
        xorg_list_del(&xmir_win->flip.entry);
        return;
    }

    while (!xorg_list_is_empty(&xmir_win->flip.entry)) {
        struct xmir_window *xwin = xorg_list_first_entry(&xmir_win->flip.entry, struct xmir_window, flip.entry);
        struct xmir_flip *flip = &xwin->flip;

        DebugF("Flipping child %p\n", xwin->window);

        DRI2SwapComplete(flip->client, flip->draw, 0, 0, 0, flip->type, flip->func, flip->data);
        DRI2SwapLimit(flip->draw, 2);
        flip->client = NULL;
        xorg_list_del(&flip->entry);
    }
}

static void
xmir_glamor_copy_egl_tex(int fbo, DrawablePtr src, PixmapPtr src_pixmap, glamor_pixmap_private *src_pixmap_priv, BoxPtr box, EGLint width, EGLint height, int dstx, int dsty, int orientation)
{
    glamor_screen_private *glamor_priv = glamor_get_screen_private(src->pScreen);
    struct xmir_screen *xmir_screen = xmir_screen_get(src->pScreen);
    float vertices[8], texcoords[8];
    GLfloat src_xscale, src_yscale, dst_xscale = 1.0 / width, dst_yscale = 1.0 / height;
    int dx, dy;

    bool reflect_x = false;
    bool reflect_y = false;
    bool swap_xy = false;
    BoxRec dbox;

    /* reflection test parameters */
    bool magic_x_invert = false, magic_y_invert = false;

    if (xmir_screen->doubled) {
        dst_xscale /= (1. + xmir_screen->doubled);
        dst_yscale /= (1. + xmir_screen->doubled);
    }

    if (magic_x_invert)
        reflect_x = !reflect_x;

    if (magic_y_invert)
        reflect_y = !reflect_y;

    switch (orientation) {
    case 90:
        reflect_y = !reflect_y; reflect_x = !reflect_x; swap_xy = true; break;
    case 180:
        reflect_x = !reflect_x; reflect_y = !reflect_y; break;
    case 270:
        swap_xy = true; break;
    }

    glamor_get_drawable_deltas(src, src_pixmap, &dx, &dy);

    pixmap_priv_get_scale(src_pixmap_priv, &src_xscale, &src_yscale);

    if (src_pixmap_priv->base.gl_fbo == GLAMOR_FBO_UNATTACHED)
        FatalError("aeiou\n");

    glViewport(dx, dy, width + dx, height + dy);

    glVertexAttribPointer(GLAMOR_VERTEX_POS, 2, GL_FLOAT,
                          GL_FALSE, 2 * sizeof(float), vertices);
    glEnableVertexAttribArray(GLAMOR_VERTEX_POS);

    if (!fbo) {
       glActiveTexture(GL_TEXTURE0);
       glBindTexture(GL_TEXTURE_2D, src_pixmap_priv->base.fbo->tex);

        if (glamor_priv->gl_flavor == GLAMOR_GL_DESKTOP) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glVertexAttribPointer(GLAMOR_VERTEX_SOURCE, 2, GL_FLOAT, GL_FALSE,
                          2 * sizeof(float), texcoords);
    glEnableVertexAttribArray(GLAMOR_VERTEX_SOURCE);
    if (!fbo) {
        glUseProgram(glamor_priv->finish_access_prog[0]);
        glUniform1i(glamor_priv->finish_access_revert[0], REVERT_NONE);
        glUniform1i(glamor_priv->finish_access_swap_rb[0], SWAP_NONE_UPLOADING);
    }

    if (!swap_xy) {
        float _tx1, _tx2, _ty1, _ty2;

        if (reflect_x) {
            dbox.x1 = box->x2 + dstx;
            dbox.x2 = box->x1 + dstx;
        } else {
            dbox.x1 = box->x1 + dstx;
            dbox.x2 = box->x2 + dstx;
        }

        if (reflect_y) {
            dbox.y1 = box->y2 + dsty;
            dbox.y2 = box->y1 + dsty;
        } else {
            dbox.y1 = box->y1 + dsty;
            dbox.y2 = box->y2 + dsty;
        }

        _tx1 = v_from_x_coord_x(dst_xscale, dbox.x1);
        _tx2 = v_from_x_coord_x(dst_xscale, dbox.x2);

        if (xmir_screen->gbm) {
            _ty1 = v_from_x_coord_y_inverted(dst_yscale, dbox.y1);
            _ty2 = v_from_x_coord_y_inverted(dst_yscale, dbox.y2);
        } else {
            _ty1 = v_from_x_coord_y(dst_yscale, dbox.y1);
            _ty2 = v_from_x_coord_y(dst_yscale, dbox.y2);
        }

        /* upper left */
        vertices[0] = _tx1;
        vertices[1] = _ty1;

        /* upper right */
        vertices[2] = _tx2;
        vertices[3] = _ty1;

        /* bottom right */
        vertices[4] = _tx2;
        vertices[5] = _ty2;

        /* bottom left */
        vertices[6] = _tx1;
        vertices[7] = _ty2;
    } else {
        float _tx1, _tx2, _ty1, _ty2;

        if (reflect_x) {
            dbox.y1 = box->x2 + dstx;
            dbox.y2 = box->x1 + dstx;
        } else {
            dbox.y1 = box->x1 + dstx;
            dbox.y2 = box->x2 + dstx;
        }

        if (reflect_y) {
            dbox.x1 = box->y2 + dsty;
            dbox.x2 = box->y1 + dsty;
        } else {
            dbox.x1 = box->y1 + dsty;
            dbox.x2 = box->y2 + dsty;
        }

        _tx1 = v_from_x_coord_x(dst_xscale, dbox.x1);
        _tx2 = v_from_x_coord_x(dst_xscale, dbox.x2);

        if (xmir_screen->gbm) {
            _ty1 = v_from_x_coord_y_inverted(dst_yscale, dbox.y1);
            _ty2 = v_from_x_coord_y_inverted(dst_yscale, dbox.y2);
        } else {
            _ty1 = v_from_x_coord_y(dst_yscale, dbox.y1);
            _ty2 = v_from_x_coord_y(dst_yscale, dbox.y2);
        }

        /* upper right */
        vertices[0] = _tx2;
        vertices[1] = _ty1;

        /* bottom right */
        vertices[2] = _tx2;
        vertices[3] = _ty2;

        /* bottom left */
        vertices[4] = _tx1;
        vertices[5] = _ty2;

        /* upper left */
        vertices[6] = _tx1;
        vertices[7] = _ty1;
    }

    if (orientation)
        DebugF("(%u,%u)(%u,%u) -> (%u,%u)(%u,%u) with %u orientation\n",
               box->x1 + dx, box->y1 + dy, box->x2 + dx, box->y2 + dy,
               dbox.x1, dbox.y1, dbox.x2, dbox.y2, orientation);

    glamor_set_normalize_tcoords_ext(src_pixmap_priv,
                                 src_xscale, src_yscale,
                                 box->x1 + dx, box->y1 + dy,
                                 box->x2 + dx, box->y2 + dy,
                                 texcoords, 2);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glDisableVertexAttribArray(GLAMOR_VERTEX_POS);
    glDisableVertexAttribArray(GLAMOR_VERTEX_SOURCE);
}

void
xmir_glamor_copy_egl_common(DrawablePtr src, PixmapPtr src_pixmap,
                            glamor_pixmap_private *src_pixmap_priv,
                            BoxPtr ext, int width, int height, int dx, int dy,
                            int orientation)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(src->pScreen);
    DebugF("Box: (%i,%i)->(%i,%i)\n", ext->x1, ext->y1, ext->x2, ext->y2);

    if (epoxy_has_gl_extension("GL_EXT_framebuffer_blit") && !xmir_screen->doubled && !orientation) {
        glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, src_pixmap_priv->base.fbo->fb);

        glBlitFramebuffer(ext->x1, ext->y2, ext->x2, ext->y1,
                          ext->x1 + dx, ext->y2 + dy, ext->x2 + dx, ext->y1 + dy,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else
        xmir_glamor_copy_egl_tex(0, src, src_pixmap, src_pixmap_priv, ext, width, height, dx, dy, orientation);
}

static void
xmir_glamor_copy_gbm(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, RegionPtr dirty)
{
    ScreenPtr screen = xmir_screen->screen;
    WindowPtr window = xmir_win->window;

    if (lastGLContext != xmir_screen->egl_context) {
        lastGLContext = xmir_screen->egl_context;
        xmir_glamor_egl_make_current(xmir_screen->glamor_ctx);
    }

    complete_flips(xmir_win);

    if (xmir_win->front_pixmap) {
        if (xmir_win->reuse_pixmap) {
            ErrorF("Got too many buffers!\n");
            screen->DestroyPixmap(xmir_win->reuse_pixmap);
        }

        xmir_win->reuse_pixmap = xmir_win->front_pixmap;
    }

    if (!xmir_win->back_pixmap) {
        PixmapPtr back = xmir_glamor_win_get_back(xmir_screen, xmir_win, &window->drawable);
        PixmapPtr from = screen->GetWindowPixmap(window);
        glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(back);

        glBindFramebuffer(GL_FRAMEBUFFER, pixmap_priv->base.fbo->fb);
        xmir_glamor_copy_egl_common(&window->drawable, from, glamor_get_pixmap_private(from),
                                    RegionExtents(dirty),
                                    back->drawable.width, back->drawable.height, 0, 0, xmir_win->orientation);

        xmir_win->front_pixmap = back;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        xmir_win->front_pixmap = xmir_win->back_pixmap;
        xmir_win->back_pixmap = NULL;
    }
}

static GLint xmir_glamor_passthrough_prog(ScreenPtr screen)
{
    const char *vs_source =
        "attribute vec4 v_position;\n"
        "attribute vec4 v_texcoord0;\n"
        "varying vec2 source_texture;\n"
        "void main()\n"
        "{\n"
        "	gl_Position = v_position;\n"
        "	source_texture = v_texcoord0.xy;\n"
        "}\n";

    const char *fs_source =
        GLAMOR_DEFAULT_PRECISION
        "varying vec2 source_texture;\n"
        "uniform sampler2D sampler;\n"
        "void main()\n"
        "{\n"
        "    gl_FragColor = texture2D(sampler, source_texture);\n"
        "}\n";


    GLint fs_prog, vs_prog;
    GLint sampler_uniform_location;
    GLint passthrough_prog = glCreateProgram();

    vs_prog = glamor_compile_glsl_prog(GL_VERTEX_SHADER, vs_source);

    fs_prog = glamor_compile_glsl_prog(GL_FRAGMENT_SHADER, fs_source);

    glAttachShader(passthrough_prog, vs_prog);
    glAttachShader(passthrough_prog, fs_prog);

    glBindAttribLocation(passthrough_prog,
                         GLAMOR_VERTEX_POS, "v_position");
    glBindAttribLocation(passthrough_prog,
                         GLAMOR_VERTEX_SOURCE, "v_texcoord0");
    glamor_link_glsl_prog(screen, passthrough_prog,
                          "finish swap through blit");

    sampler_uniform_location =
        glGetUniformLocation(passthrough_prog, "sampler");
    glUseProgram(passthrough_prog);
    glUniform1i(sampler_uniform_location, 0);

    return passthrough_prog;
}

static void *
xmir_glamor_flip(void *data)
{
    struct xmir_screen *xmir_screen = data;
    struct glamor_screen_private *glamor_priv =
        glamor_get_screen_private(xmir_screen->screen);
    int passthrough_prog;
    GLuint tex;

    pthread_mutex_lock(&xmir_screen->mutex);
    if (xmir_screen->alive < 0)
        pthread_exit(NULL);

    if (glamor_priv->gl_flavor == GLAMOR_GL_DESKTOP)
        eglBindAPI(EGL_OPENGL_API);

    xmir_screen->alive = 1;
    if (!eglMakeCurrent(xmir_screen->egl_display, xmir_screen->swap_surface, xmir_screen->swap_surface, xmir_screen->swap_context))
        ErrorF("eglMakeCurrent failed: %x\n", eglGetError());
    passthrough_prog = xmir_glamor_passthrough_prog(xmir_screen->screen);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (glamor_priv->gl_flavor == GLAMOR_GL_DESKTOP) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    }

    while (xmir_screen->alive >= 0) {
        if (xorg_list_is_empty(&xmir_screen->swap_list))
            pthread_cond_wait(&xmir_screen->cond, &xmir_screen->mutex);

        while (!xorg_list_is_empty(&xmir_screen->swap_list)) {
            struct xmir_window *xmir_win;
            Bool ret;
            EGLint val, width, height;
            PixmapPtr src_pixmap;

            xmir_win = xorg_list_first_entry(&xmir_screen->swap_list, struct xmir_window, flip.entry);

            DebugF("Handling %p\n", xmir_win);
            if (xmir_win->flip.data) {
                val = eglClientWaitSync(xmir_screen->egl_display, xmir_win->flip.data, 0, 1000000000);
                if (val != EGL_CONDITION_SATISFIED_KHR)
                    ErrorF("eglClientWaitSync failed: %x/%x\n", val, eglGetError());
                eglDestroySync(xmir_screen->egl_display, xmir_win->flip.data);
            }

            ret = eglMakeCurrent(xmir_screen->egl_display, xmir_win->egl_surface, xmir_win->egl_surface, xmir_screen->swap_context);
            if (!ret)
                ErrorF("eglMakeCurrent failed: %x\n", eglGetError());

            glClearColor(0., 1., 0., 1.);
            glClear(GL_COLOR_BUFFER_BIT);

            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                                         (GLeglImageOES)xmir_win->image);

            eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface, EGL_HEIGHT, &height);
            eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface, EGL_WIDTH, &width);
            src_pixmap = xmir_screen->screen->GetWindowPixmap(xmir_win->window);
            xmir_glamor_copy_egl_tex(1, &xmir_win->window->drawable, src_pixmap, glamor_get_pixmap_private(src_pixmap), RegionExtents(&xmir_win->region), width, height, 0, 0, xmir_win->orientation);

            ret = eglSwapBuffers(xmir_screen->egl_display, xmir_win->egl_surface);
            if (!ret)
                ErrorF("eglSwapBuffers failed: %x\n", eglGetError());
            ret = eglMakeCurrent(xmir_screen->egl_display, xmir_screen->swap_surface, xmir_screen->swap_surface, xmir_screen->swap_context);
            if (!ret)
                ErrorF("eglMakeCurrent failed: %x\n", eglGetError());

            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            xorg_list_del(&xmir_win->flip.entry);
            xmir_post_to_eventloop(xmir_handle_buffer_available, xmir_screen,
                                   xmir_win, 0);
        }
    }
    glDeleteTextures(1, &tex);
    glDeleteProgram(passthrough_prog);
    if (!eglMakeCurrent(xmir_screen->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
        ErrorF("eglMakeCurrent failed: %x\n", eglGetError());
    pthread_mutex_unlock(&xmir_screen->mutex);

    return NULL;
}

static void
xmir_glamor_copy_egl_direct(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, RegionPtr dirty)
{
    ScreenPtr screen = xmir_screen->screen;
    WindowPtr window = xmir_win->window;
    PixmapPtr src_pixmap = screen->GetWindowPixmap(window);
    glamor_pixmap_private *src_pixmap_priv = glamor_get_pixmap_private(src_pixmap);

    BoxPtr ext = RegionExtents(dirty);
    EGLint width, height;

    lastGLContext = xmir_screen->egl_context;

    if (!eglMakeCurrent(xmir_screen->egl_display, xmir_win->egl_surface, xmir_win->egl_surface, xmir_screen->egl_context))
        ErrorF("Failed to make current!\n");

    if (epoxy_is_desktop_gl())
        glDrawBuffer(GL_BACK);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface, EGL_HEIGHT, &height);
    eglQuerySurface(xmir_screen->egl_display, xmir_win->egl_surface, EGL_WIDTH, &width);
    xmir_glamor_copy_egl_common(&window->drawable, src_pixmap, src_pixmap_priv, ext, width, height, 0, 0, xmir_win->orientation);
    eglSwapBuffers(xmir_screen->egl_display, xmir_win->egl_surface);
}

static void
xmir_glamor_copy_egl_queue(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, RegionPtr dirty)
{
    void *sync_fd = NULL;
    ScreenPtr screen = xmir_screen->screen;
    WindowPtr window = xmir_win->window;
    PixmapPtr src_pixmap = screen->GetWindowPixmap(window);
    glamor_pixmap_private *src_pixmap_priv = glamor_get_pixmap_private(src_pixmap);

    if (lastGLContext != xmir_screen->egl_context) {
        lastGLContext = xmir_screen->egl_context;
        xmir_glamor_egl_make_current(xmir_screen->glamor_ctx);
    }

    if (!xmir_win->image) {
        EGLint attribs[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
            EGL_GL_TEXTURE_LEVEL_KHR, 0,
            EGL_NONE
        };

        /* Keep the image around until resizing is done, and mark image as
         * external so it won't re-enter the FBO cache. This texture has to
         * be deleted to allow followup eglCreateImageKHR's to succeed after
         * rotating back and forth.
         */
        glamor_set_pixmap_type(src_pixmap, GLAMOR_TEXTURE_DRM);
        src_pixmap_priv->base.fbo->external = TRUE;

        xmir_win->image = eglCreateImageKHR(xmir_screen->egl_display, xmir_screen->egl_context, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(intptr_t)src_pixmap_priv->base.fbo->tex, attribs);
        if (!xmir_win->image) {
            GLint error;
            ErrorF("eglCreateImageKHR failed with %x\n", eglGetError());

            while ((error = eglGetError()) != EGL_SUCCESS)
                ErrorF("Error stack: %x\n", error);

            xmir_glamor_copy_egl_direct(xmir_screen, xmir_win, dirty);
            return;
        }
    }

    if (epoxy_has_gl_extension("GL_OES_EGL_sync"))
        sync_fd = eglCreateSyncKHR(xmir_screen->egl_display, EGL_SYNC_FENCE_KHR, NULL);

    /* Flush work, and the sync_fd if created */
    glFlush();

    DebugF("Queueing on %p with %p\n", xmir_win, xmir_win->image);

    pthread_mutex_lock(&xmir_screen->mutex);
    xmir_win->flip.data = sync_fd;
    xorg_list_add(&xmir_win->flip.entry, &xmir_screen->swap_list);
    pthread_mutex_unlock(&xmir_screen->mutex);

    pthread_cond_signal(&xmir_screen->cond);

    xmir_win->has_free_buffer = FALSE;
}

void
xmir_glamor_copy(struct xmir_screen *xmir_screen, struct xmir_window *xmir_win, RegionPtr dirty)
{
    if (xmir_screen->gbm)
        xmir_glamor_copy_gbm(xmir_screen, xmir_win, dirty);
    else {
        xorg_list_del(&xmir_win->link_damage);

        if (!xmir_screen->swap_context)
            xmir_glamor_copy_egl_direct(xmir_screen, xmir_win, dirty);
        else
            xmir_glamor_copy_egl_queue(xmir_screen, xmir_win, dirty);

        RegionEmpty(dirty);
    }
}

static EGLConfig
xmir_glamor_get_egl_config(struct xmir_screen *xmir_screen)
{
    EGLConfig eglconfig;
    EGLint neglconfigs;

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
        EGL_BUFFER_SIZE, 32,
        EGL_NONE
    };

    if (!epoxy_has_egl_extension(xmir_screen->egl_display, "EGL_KHR_surfaceless_context"))
        attribs[1] |= EGL_PBUFFER_BIT;

    if (!eglChooseConfig(xmir_screen->egl_display, attribs, &eglconfig, 1, &neglconfigs) ||
        !neglconfigs)
        FatalError("Could not create a compatible config!\n");

    return eglconfig;
}

void
xmir_glamor_realize_window(struct xmir_screen *xmir_screen, struct xmir_window *xmir_window, WindowPtr window)
{
    EGLConfig eglconfig = xmir_glamor_get_egl_config(xmir_screen);
    MirEGLNativeWindowType egl_win;

    if (xmir_screen->gbm)
        return;

    egl_win = mir_buffer_stream_get_egl_native_window(mir_surface_get_buffer_stream(xmir_window->surface));

    xmir_window->egl_surface = eglCreateWindowSurface(xmir_screen->egl_display, eglconfig, (EGLNativeWindowType)egl_win, NULL);
}

void
xmir_glamor_unrealize_window(struct xmir_screen *xmir_screen, struct xmir_window *xmir_window, WindowPtr window)
{
    ScreenPtr screen = xmir_screen->screen;

    if (xmir_window->egl_surface) {
        lastGLContext = NULL;
        if (!eglMakeCurrent(xmir_screen->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
            ErrorF("eglMakeCurrent failed: %x\n", eglGetError());

        if (!xmir_window->has_free_buffer) {
            Bool flush = TRUE;

            pthread_mutex_lock(&xmir_screen->mutex);
            if (!xorg_list_is_empty(&xmir_window->flip.entry)) {
                if (xmir_window->flip.data)
                    eglDestroySync(xmir_screen->egl_display, xmir_window->flip.data);
                xorg_list_del(&xmir_window->flip.entry);
                flush = FALSE;
            }
            pthread_mutex_unlock(&xmir_screen->mutex);

            if (flush)
                xmir_process_from_eventloop();
        }

        if (xmir_window->image)
            eglDestroyImageKHR(xmir_screen->egl_display, xmir_window->image);

        eglDestroySurface(xmir_screen->egl_display, xmir_window->egl_surface);
    }

    complete_flips(xmir_window);

    if (xmir_window->reuse_pixmap) {
        screen->DestroyPixmap(xmir_window->reuse_pixmap);
        xmir_window->reuse_pixmap = NULL;
    }

    if (xmir_window->front_pixmap) {
        screen->DestroyPixmap(xmir_window->front_pixmap);
        xmir_window->front_pixmap = NULL;
    }

    if (xmir_window->back_pixmap) {
        screen->DestroyPixmap(xmir_window->back_pixmap);
        xmir_window->back_pixmap = NULL;
    }
}

static void
xmir_drm_set_gbm_device_response(MirConnection *con, MirPlatformMessage* reply, void* context)
{
    mir_platform_message_release(reply);
}

static Bool
xmir_drm_init_egl(struct xmir_screen *xmir_screen)
{
    EGLint major, minor;
    const char *version;
    EGLConfig egl_config;

    EGLint gles2_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_CONTEXT_FLAGS_KHR, 0,
        EGL_NONE
    };

    EGLint pbuffer_attribs[] = {
        EGL_HEIGHT, 1,
        EGL_WIDTH, 1,
        EGL_NONE
    };


    if (xmir_screen->drm_fd > 0) {
        struct MirMesaSetGBMDeviceRequest req = {
            .device = gbm_create_device(xmir_screen->drm_fd)
        };
        MirPlatformMessage* msg = mir_platform_message_create(set_gbm_device);

        xmir_screen->gbm = req.device;
        if (xmir_screen->gbm == NULL) {
            ErrorF("couldn't get display device\n");
            mir_platform_message_release(msg);
            return FALSE;
        }

        mir_platform_message_set_data(msg, &req, sizeof req);

        mir_wait_for(mir_connection_platform_operation(
                xmir_screen->conn,
                msg,
                &xmir_drm_set_gbm_device_response,
                NULL));
        mir_platform_message_release(msg);
        /* In GBM mode no mir functions are used in any way.
         * This means using the GBM device directly is safe.. */
        xmir_screen->egl_display = eglGetDisplay(xmir_screen->gbm);
    } else
        xmir_screen->egl_display = eglGetDisplay(mir_connection_get_egl_native_display(xmir_screen->conn));

    if (xmir_screen->egl_display == EGL_NO_DISPLAY) {
        ErrorF("eglGetDisplay() failed\n");
        return FALSE;
    }

    eglBindAPI(!xmir_screen->gbm ? EGL_OPENGL_ES_API : EGL_OPENGL_API);

    if (!eglInitialize(xmir_screen->egl_display, &major, &minor)) {
        ErrorF("eglInitialize() failed\n");
        return FALSE;
    }

    version = eglQueryString(xmir_screen->egl_display, EGL_VERSION);
    ErrorF("glamor EGL version: %s\n", version);
    ErrorF("glamor EGL extensions: %s\n", eglQueryString(xmir_screen->egl_display, EGL_EXTENSIONS));

    egl_config = xmir_glamor_get_egl_config(xmir_screen);

    xmir_screen->egl_context = eglCreateContext(xmir_screen->egl_display,
                                                egl_config, EGL_NO_CONTEXT,
                                                !xmir_screen->gbm ? gles2_attribs : NULL);
    if (xmir_screen->egl_context == EGL_NO_CONTEXT) {
        ErrorF("Failed to create EGL context: %i/%x\n", eglGetError(), eglGetError());
        return FALSE;
    }

    if (!epoxy_has_egl_extension(xmir_screen->egl_display, "EGL_KHR_surfaceless_context")) {
        xmir_screen->egl_surface = eglCreatePbufferSurface(xmir_screen->egl_display, egl_config, pbuffer_attribs);
    } else
        xmir_screen->egl_surface = EGL_NO_SURFACE;

    if (!eglMakeCurrent(xmir_screen->egl_display,
                        xmir_screen->egl_surface, xmir_screen->egl_surface,
                        xmir_screen->egl_context)) {
        ErrorF("Failed to make EGL context current: %i/%x\n", eglGetError(), eglGetError());
        return FALSE;
    }
    lastGLContext = xmir_screen->egl_context;

    ErrorF("glamor GL version: %s\n", glGetString(GL_VERSION));
    ErrorF("glamor GL extensions: %s\n", glGetString(GL_EXTENSIONS));
    ErrorF("glamor GLSL version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
        ErrorF("GL_OES_EGL_image not available\n");
        return FALSE;
    }

    if (!xmir_screen->gbm && xmir_screen->glamor != glamor_egl_sync) {
        xmir_screen->swap_context = eglCreateContext(xmir_screen->egl_display, egl_config, EGL_NO_CONTEXT, gles2_attribs);
        if (!xmir_screen->swap_context) {
            ErrorF("Failed to create EGL context: %i/%x\n", eglGetError(), eglGetError());
            return FALSE;
        }

        if (xmir_screen->egl_surface)
            xmir_screen->swap_surface = eglCreatePbufferSurface(xmir_screen->egl_display, egl_config, pbuffer_attribs);

        xorg_list_init(&xmir_screen->swap_list);
    }

    return TRUE;
}

static Bool
xmir_screen_init_glamor_drm(struct xmir_screen *xmir_screen)
{
    xmir_screen->drm_fd = xmir_screen->platform.fd[0];

    xmir_screen->device_name = drmGetDeviceNameFromFd(xmir_screen->drm_fd);
    if (!xmir_screen->device_name)
        return FALSE;

    return TRUE;
}

Bool
xmir_screen_init_glamor(struct xmir_screen *xmir_screen)
{
    if (xmir_screen->platform.fd_items >= 1 &&
        xmir_screen->doubled)
        ErrorF("Disabling DRI2 support because of -2x\n");

    if (xmir_screen->platform.fd_items >= 1 &&
        !xmir_screen->doubled &&
        xmir_screen->glamor == glamor_dri &&
        !xmir_screen_init_glamor_drm(xmir_screen))
        return FALSE;

    return xmir_drm_init_egl(xmir_screen);
}

void
xmir_glamor_fini(struct xmir_screen *xmir_screen)
{
    if (xmir_screen->thread) {
        xmir_screen->alive = -1;

        pthread_cond_signal(&xmir_screen->cond);
        pthread_join(xmir_screen->thread, NULL);
        pthread_cond_destroy(&xmir_screen->cond);
        pthread_mutex_destroy(&xmir_screen->mutex);
    }

    if (xmir_screen->swap_context)
        eglDestroyContext(xmir_screen->egl_display, xmir_screen->swap_context);

    if (xmir_screen->swap_surface)
        eglDestroySurface(xmir_screen->egl_display, xmir_screen->swap_surface);

    lastGLContext = NULL;
    if (!eglMakeCurrent(xmir_screen->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
        ErrorF("eglMakeCurrent failed: %x\n", eglGetError());
    if (xmir_screen->egl_surface)
        eglDestroySurface(xmir_screen->egl_display, xmir_screen->egl_surface);
    eglDestroyContext(xmir_screen->egl_display, xmir_screen->egl_context);
    eglTerminate(xmir_screen->egl_display);

    if (xmir_screen->gbm)
        gbm_device_destroy(xmir_screen->gbm);
    free(xmir_screen->device_name);
}

void
glamor_egl_destroy_textured_pixmap(PixmapPtr pixmap)
{
    glamor_destroy_textured_pixmap(pixmap);
}

static void
xmir_glamor_get_name_from_bo(int drm_fd, struct gbm_bo *bo, int *name)
{
    struct drm_gem_flink flink;
    unsigned handle = gbm_bo_get_handle(bo).u32;

    flink.handle = handle;
    if (ioctl(drm_fd, DRM_IOCTL_GEM_FLINK, &flink) < 0)
        *name = -1;
    else
        *name = flink.name;
}

static int
xmir_glamor_get_fd_from_bo(int gbm_fd, struct gbm_bo *bo, int *fd)
{
    union gbm_bo_handle handle;
    struct drm_prime_handle args;

    handle = gbm_bo_get_handle(bo);
    args.handle = handle.u32;
    args.flags = DRM_CLOEXEC;
    if (ioctl(gbm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args))
        return FALSE;
    *fd = args.fd;
    return TRUE;
}

int
glamor_egl_dri3_fd_name_from_tex(ScreenPtr screen,
                                 PixmapPtr pixmap,
                                 unsigned int tex,
                                 Bool want_name, CARD16 *stride, CARD32 *size)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    struct xmir_pixmap *xmir_pixmap = xmir_pixmap_get(pixmap);
    struct glamor_screen_private *glamor_priv =
        glamor_get_screen_private(screen);
    struct gbm_bo *bo;
    int fd = -1;

    EGLint attribs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_GL_TEXTURE_LEVEL_KHR, 0,
        EGL_NONE
    };

    glamor_make_current(glamor_priv);

    if (!xmir_pixmap) {
        void *image;

        xmir_pixmap = calloc(sizeof(*xmir_pixmap), 1);
        xmir_pixmap_set(pixmap, xmir_pixmap);

        xmir_pixmap->fake_back = true;

        image = eglCreateImageKHR(xmir_screen->egl_display,
                                  xmir_screen->egl_context,
                                  EGL_GL_TEXTURE_2D_KHR,
                                  (EGLClientBuffer) (uintptr_t)
                                  tex, attribs);
        if (image == EGL_NO_IMAGE_KHR)
            goto failure;
        xmir_pixmap->image = image;

        glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
        xmir_pixmap->bo = gbm_bo_import(xmir_screen->gbm, GBM_BO_IMPORT_EGL_IMAGE, image, 0);
    }
    bo = xmir_pixmap->bo;

    if (!bo)
        goto failure;

    pixmap->devKind = gbm_bo_get_stride(bo);

    if (want_name)
        xmir_glamor_get_name_from_bo(xmir_screen->drm_fd, bo, &fd);
    else
        xmir_glamor_get_fd_from_bo(xmir_screen->drm_fd, bo, &fd);

    *stride = pixmap->devKind;
    *size = pixmap->devKind * gbm_bo_get_height(bo);

 failure:
    return fd;
}

unsigned int
glamor_egl_create_argb8888_based_texture(ScreenPtr screen, int w, int h)
{
    return 0;
}

static PixmapPtr
xmir_glamor_create_pixmap(ScreenPtr screen,
                          int width, int height, int depth, unsigned int hint)
{
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);

    if (width > 0 && height > 0 && depth >= 24 &&
        (hint == 0 ||
         hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP ||
         hint == CREATE_PIXMAP_USAGE_SHARED ||
         hint == XMIR_CREATE_PIXMAP_USAGE_FLIP)) {
        struct xmir_pixmap *xmir_pixmap = malloc(sizeof(*xmir_pixmap));
        PixmapPtr pixmap = NULL;
        struct gbm_bo *bo = NULL;
        void *image = NULL;
        unsigned int tex = 0;

        if (!xmir_pixmap)
            goto free;

        pixmap = glamor_create_pixmap(screen, width, height, depth, GLAMOR_CREATE_PIXMAP_NO_TEXTURE);
        if (!pixmap)
            goto free;

        glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);

        if (hint != XMIR_CREATE_PIXMAP_USAGE_FLIP) {
            bo = gbm_bo_create(xmir_screen->gbm, width, height,
                               GBM_FORMAT_ARGB8888,
                               GBM_BO_USE_RENDERING);

            if (!bo)
                goto free;

            image = eglCreateImageKHR(xmir_screen->egl_display, xmir_screen->egl_context, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
            if (!image)
                goto free;

            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
            glBindTexture(GL_TEXTURE_2D, 0);
            glamor_set_pixmap_texture(pixmap, tex);
        }

        xmir_pixmap->image = image;
        xmir_pixmap->bo = bo;
        xmir_pixmap_set(pixmap, xmir_pixmap);
        xmir_pixmap->fake_back = true;
        if (bo)
            pixmap->devKind = gbm_bo_get_stride(bo);

        if (!glGetError())
            return pixmap;

        ErrorF("Failed to allocate pixmap - a opengl error occured!\n");

free:
        if (tex)
            glDeleteTextures(1, &tex);

        if (image)
            eglDestroyImageKHR(xmir_screen->egl_display, image);

        if (pixmap)
            glamor_destroy_pixmap(pixmap);

        if (bo)
            gbm_bo_destroy(bo);

        free(xmir_pixmap);
        return NULL;
    }

    return glamor_create_pixmap(screen, width, height, depth, hint);
}

void
glamor_egl_destroy_pixmap_image(PixmapPtr pixmap)
{
    /* XXX: Unused */
}

static Bool
xmir_glamor_destroy_pixmap(PixmapPtr pixmap)
{
    struct xmir_pixmap *xmir_pixmap;

    if (pixmap->refcnt == 1 && (xmir_pixmap = xmir_pixmap_get(pixmap))) {
        ScreenPtr screen = pixmap->drawable.pScreen;
        struct xmir_screen *xmir_screen = xmir_screen_get(screen);
        Bool ret;

        ret = glamor_destroy_pixmap(pixmap);
        if (!ret)
            return ret;
        glamor_block_handler(screen);

        if (xmir_pixmap->image)
            eglDestroyImageKHR(xmir_screen->egl_display, xmir_pixmap->image);
        if (xmir_pixmap->bo)
            gbm_bo_destroy(xmir_pixmap->bo);
        free(xmir_pixmap);
        return ret;
    }

    return glamor_destroy_pixmap(pixmap);
}

Bool
xmir_glamor_init(struct xmir_screen *xmir_screen)
{
    ScreenPtr screen = xmir_screen->screen;

    if (xmir_screen->egl_context == EGL_NO_CONTEXT) {
        ErrorF("Disabling glamor and dri2, EGL setup failed\n");
        return FALSE;
    }

    if (!glamor_init(screen,
                     GLAMOR_INVERTED_Y_AXIS |
                     GLAMOR_USE_EGL_SCREEN |
                     GLAMOR_USE_SCREEN |
                     GLAMOR_USE_PICTURE_SCREEN |
                     GLAMOR_NO_DRI3)) {
        ErrorF("Failed to initialize glamor\n");
        return FALSE;
    }

    if (xmir_screen->swap_context) {
        pthread_mutex_init(&xmir_screen->mutex, NULL);
        pthread_cond_init(&xmir_screen->cond, NULL);
        pthread_create(&xmir_screen->thread, NULL, xmir_glamor_flip, xmir_screen);
    }

    if (xmir_screen->gbm) {
        screen->CreatePixmap = xmir_glamor_create_pixmap;
        screen->DestroyPixmap = xmir_glamor_destroy_pixmap;

        /* Tell the core that we have the interfaces for import/export
         * of pixmaps.
         */
        glamor_enable_dri3(screen);
    }

    return TRUE;
}
