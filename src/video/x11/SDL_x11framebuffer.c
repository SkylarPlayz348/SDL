/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_X11

#include "SDL_hints.h"
#include "SDL_x11video.h"
#include "SDL_x11framebuffer.h"

/* --- Kindle e-ink (mxc_epdc) fast-refresh support ---------------------------
 *
 * The Kindle X server normally picks a slow, full GC16-style waveform. For
 * animation we can instead drive the panel directly via the mxcfb framebuffer
 * ioctls, selecting the A2 (or DU) 2-level waveform. A2 is 1-bit only, so the
 * grayscale surface is ordered-dithered to pure black/white before presenting,
 * and a periodic GC16 refresh clears accumulated ghosting.
 *
 * Enabled per-window via the SDL_X11_EINK_WAVEFORM hint ("a2" / "du" / "off").
 *
 * NOTE: the struct layout, ioctl numbers and waveform constants below are the
 * common i.MX/Kindle values but ARE device/firmware specific. Verify them
 * against the kernel mxcfb.h for the target firmware (and/or strace FBInk on
 * device) before trusting the result. They are intentionally kept local so the
 * build does not depend on Kindle kernel headers being present.
 */
#if defined(__linux__)
#define SDL_X11_HAVE_EINK 1
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

#ifndef WAVEFORM_MODE_DU
#define WAVEFORM_MODE_DU   1
#endif
#ifndef WAVEFORM_MODE_GC16
#define WAVEFORM_MODE_GC16 2
#endif
#ifndef WAVEFORM_MODE_A2
#define WAVEFORM_MODE_A2   4   /* TODO: confirm per device (often 4 or 6) */
#endif

#define EINK_UPDATE_MODE_PARTIAL 0x0
#define EINK_UPDATE_MODE_FULL    0x1
#define EINK_TEMP_USE_AMBIENT    0x1000

/* Issue a GC16 full refresh every N frames to clear A2 ghosting. Tunable. */
#define EINK_DEGHOST_EVERY 32

struct sdl_mxcfb_rect
{
    Uint32 top;
    Uint32 left;
    Uint32 width;
    Uint32 height;
};

struct sdl_mxcfb_alt_buffer_data
{
    Uint32 phys_addr;
    Uint32 width;
    Uint32 height;
    struct sdl_mxcfb_rect alt_update_region;
};

struct sdl_mxcfb_update_data
{
    struct sdl_mxcfb_rect update_region;
    Uint32 waveform_mode;
    Uint32 update_mode;
    Uint32 update_marker;
    int temp;
    unsigned int flags;
    struct sdl_mxcfb_alt_buffer_data alt_buffer_data;
};

/* TODO: confirm the ioctl number for the target firmware. */
#define SDL_MXCFB_SEND_UPDATE _IOW('F', 0x2E, struct sdl_mxcfb_update_data)
#endif /* __linux__ */

#ifndef NO_SHARED_MEMORY

/* Shared memory error handler routine */
static int shm_error;
static int (*X_handler)(Display *, XErrorEvent *) = NULL;
static int shm_errhandler(Display *d, XErrorEvent *e)
{
    if (e->error_code == BadAccess) {
        shm_error = True;
        return 0;
    }
    return X_handler(d, e);
}

static SDL_bool have_mitshm(Display *dpy)
{
    /* Only use shared memory on local X servers */
    return X11_XShmQueryExtension(dpy) ? SDL_X11_HAVE_SHM : SDL_FALSE;
}

#endif /* !NO_SHARED_MEMORY */

/* Textbook 4x4 Bayer ordered-dither threshold matrix (values 0..15). Ordered
   dithering is deterministic per (x,y), so unchanged regions stay byte-identical
   between frames -- exactly what A2 partial updates want. */
static const Uint8 eink_bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

/* Convert one clipped rect of the ARGB8888 app surface to the Y8 grayscale
   XImage buffer. When dither is true, ordered-dither down to 1-bit (0x00/0xFF)
   for 2-level e-ink waveforms (A2/DU); otherwise keep the full 8bpp luma. */
static void X11_grayscale_convert_rect(SDL_WindowData *data, int window_w,
                                       int x, int y, int w, int h, SDL_bool dither)
{
    const Uint32 *src = (const Uint32 *)data->argb_buf;
    unsigned char *dst = data->grayscale_buf;
    int r, c;

    for (r = y; r < y + h; ++r) {
        const Uint32 *row = src + (size_t)r * window_w;
        unsigned char *grow = dst + (size_t)r * window_w;
        for (c = x; c < x + w; ++c) {
            Uint32 p = row[c];
            unsigned int R = (p >> 16) & 0xFF;
            unsigned int G = (p >>  8) & 0xFF;
            unsigned int B = (p      ) & 0xFF;
            /* BT.601 luma: Y = 0.299R + 0.587G + 0.114B (integer approx) */
            unsigned int gray = (77 * R + 150 * G + 29 * B) >> 8;
            if (dither) {
                unsigned int t = (unsigned int)eink_bayer4[r & 3][c & 3] * 16; /* 0..240 */
                grow[c] = (gray > t) ? 0xFF : 0x00;
            } else {
                grow[c] = (unsigned char)gray;
            }
        }
    }
}

#ifdef SDL_X11_HAVE_EINK
/* Map the SDL_X11_EINK_WAVEFORM hint to a 2-level waveform id, or 0 if disabled. */
static int X11_eink_parse_hint(void)
{
    const char *hint = SDL_GetHint("SDL_X11_EINK_WAVEFORM");
    if (!hint || !*hint || SDL_strcasecmp(hint, "off") == 0) {
        return 0;
    }
    if (SDL_strcasecmp(hint, "a2") == 0) {
        return WAVEFORM_MODE_A2;
    }
    if (SDL_strcasecmp(hint, "du") == 0) {
        return WAVEFORM_MODE_DU;
    }
    SDL_Log("SDL_X11_EINK_WAVEFORM: unknown value '%s' (expected a2/du/off)", hint);
    return 0;
}

/* Drive the e-ink panel for one presented rect: the configured 2-level waveform
   most of the time, with a periodic GC16 full refresh to clear ghosting. */
static void X11_eink_update(SDL_WindowData *data, int x, int y, int w, int h)
{
    struct sdl_mxcfb_update_data upd;
    SDL_bool full_refresh;

    if (data->eink_fd < 0 || data->eink_waveform == 0) {
        return;
    }

    data->eink_frame_count++;
    full_refresh = (data->eink_frame_count >= EINK_DEGHOST_EVERY) ? SDL_TRUE : SDL_FALSE;

    SDL_zero(upd);
    upd.update_region.left   = (Uint32)x;
    upd.update_region.top    = (Uint32)y;
    upd.update_region.width  = (Uint32)w;
    upd.update_region.height = (Uint32)h;
    upd.update_marker = ++data->eink_marker;
    upd.temp = EINK_TEMP_USE_AMBIENT;

    if (full_refresh) {
        upd.waveform_mode = WAVEFORM_MODE_GC16;
        upd.update_mode   = EINK_UPDATE_MODE_FULL;
        data->eink_frame_count = 0;
    } else {
        upd.waveform_mode = (Uint32)data->eink_waveform;
        upd.update_mode   = EINK_UPDATE_MODE_PARTIAL;
    }

    if (ioctl(data->eink_fd, SDL_MXCFB_SEND_UPDATE, &upd) == -1) {
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
                     "mxcfb update ioctl failed: %s", strerror(errno));
    }
}
#endif /* SDL_X11_HAVE_EINK */

int X11_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format,
                                void **pixels, int *pitch)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    Display *display = data->videodata->display;
    XGCValues gcv;
    XVisualInfo vinfo;
    int w, h;
    int eink_wave = 0;

    SDL_GetWindowSizeInPixels(window, &w, &h);

    /* Free the old framebuffer surface */
    X11_DestroyWindowFramebuffer(_this, window);

    /* Create the graphics context for drawing */
    gcv.graphics_exposures = False;
    data->gc = X11_XCreateGC(display, data->xwindow, GCGraphicsExposures, &gcv);
    if (!data->gc) {
        return SDL_SetError("Couldn't create graphics context");
    }

    /* Find out the pixel format and depth */
    if (X11_GetVisualInfoFromVisual(display, data->visual, &vinfo) < 0) {
        return SDL_SetError("Couldn't get window visual information");
    }

    *format = X11_GetPixelFormatFromVisualInfo(display, &vinfo);

    if (*format == SDL_PIXELFORMAT_UNKNOWN) {
        SDL_Log("X11 framebuffer: visual class=%d depth=%d red=0x%lx green=0x%lx blue=0x%lx",
            vinfo.class, vinfo.depth,
            vinfo.visual->red_mask, vinfo.visual->green_mask, vinfo.visual->blue_mask);
        return SDL_SetError("Unknown window pixel format");
    }

#ifdef SDL_X11_HAVE_EINK
    eink_wave = X11_eink_parse_hint();
#endif

    /* Private 8bpp XImage buffer + ARGB8888 app surface. Engaged when either:
         - the visual is a Kindle StaticGray/GrayScale INDEX8 ramp (ARGB -> Y8), or
         - any 8bpp visual with an A2/DU e-ink waveform requested. In that case the
           per-rect conversion ordered-dithers to pure 0x00/0xFF, which is a valid
           black/white pixel in any 8bpp visual (incl. the Kindle's DirectColor
           RGB332), so it works without caring about the channel layout. */
    if ((*format == SDL_PIXELFORMAT_INDEX8 &&
         (vinfo.class == StaticGray || vinfo.class == GrayScale)) ||
        (vinfo.depth == 8 && eink_wave != 0)) {
        data->grayscale_buf = (unsigned char *)SDL_malloc((size_t)w * h);
        if (!data->grayscale_buf) {
            return SDL_OutOfMemory();
        }
        SDL_memset(data->grayscale_buf, 0, (size_t)w * h);
        data->ximage = X11_XCreateImage(display, data->visual,
                                        vinfo.depth, ZPixmap, 0,
                                        (char *)data->grayscale_buf,
                                        w, h, 8, w);
        if (!data->ximage) {
            SDL_free(data->grayscale_buf);
            data->grayscale_buf = NULL;
            return SDL_SetError("Couldn't create grayscale XImage");
        }
        data->ximage->byte_order = (SDL_BYTEORDER == SDL_BIG_ENDIAN) ? MSBFirst : LSBFirst;
        *format = SDL_PIXELFORMAT_ARGB8888;
        *pitch  = w * 4;
        *pixels = SDL_malloc((size_t)h * (*pitch));
        if (!*pixels) {
            XDestroyImage(data->ximage); /* frees grayscale_buf via ximage->data */
            data->ximage = NULL;
            data->grayscale_buf = NULL;
            return SDL_OutOfMemory();
        }
        SDL_memset(*pixels, 0, (size_t)h * (*pitch));
        data->argb_buf = *pixels;

        /* Optional A2/DU fast-refresh: open the e-ink fb and pick the waveform. */
        data->eink_fd = -1;
        data->eink_waveform = 0;
        data->eink_frame_count = 0;
        data->eink_marker = 0;
#ifdef SDL_X11_HAVE_EINK
        data->eink_waveform = eink_wave;
        if (data->eink_waveform != 0) {
            data->eink_fd = open("/dev/fb0", O_RDWR);
            if (data->eink_fd < 0) {
                SDL_Log("SDL_X11_EINK_WAVEFORM set but couldn't open /dev/fb0 (%s); "
                        "falling back to 8bpp grayscale", strerror(errno));
                data->eink_waveform = 0;
            }
        }
#endif
        return 0;
    }

    /* Calculate pitch */
    *pitch = (((w * SDL_BYTESPERPIXEL(*format)) + 3) & ~3);

    /* Create the actual image */
#ifndef NO_SHARED_MEMORY
    if (have_mitshm(display)) {
        XShmSegmentInfo *shminfo = &data->shminfo;

        shminfo->shmid = shmget(IPC_PRIVATE, (size_t)h * (*pitch), IPC_CREAT | 0777);
        if (shminfo->shmid >= 0) {
            shminfo->shmaddr = (char *)shmat(shminfo->shmid, 0, 0);
            shminfo->readOnly = False;
            if (shminfo->shmaddr != (char *)-1) {
                shm_error = False;
                X_handler = X11_XSetErrorHandler(shm_errhandler);
                X11_XShmAttach(display, shminfo);
                X11_XSync(display, False);
                X11_XSetErrorHandler(X_handler);
                if (shm_error) {
                    shmdt(shminfo->shmaddr);
                }
            } else {
                shm_error = True;
            }
            shmctl(shminfo->shmid, IPC_RMID, NULL);
        } else {
            shm_error = True;
        }
        if (!shm_error) {
            data->ximage = X11_XShmCreateImage(display, data->visual,
                                               vinfo.depth, ZPixmap,
                                               shminfo->shmaddr, shminfo,
                                               w, h);
            if (!data->ximage) {
                X11_XShmDetach(display, shminfo);
                X11_XSync(display, False);
                shmdt(shminfo->shmaddr);
            } else {
                /* Done! */
                data->ximage->byte_order = (SDL_BYTEORDER == SDL_BIG_ENDIAN) ? MSBFirst : LSBFirst;
                data->use_mitshm = SDL_TRUE;
                *pixels = shminfo->shmaddr;
                return 0;
            }
        }
    }
#endif /* not NO_SHARED_MEMORY */

    *pixels = SDL_malloc((size_t)h * (*pitch));
    if (!*pixels) {
        return SDL_OutOfMemory();
    }

    data->ximage = X11_XCreateImage(display, data->visual,
                                    vinfo.depth, ZPixmap, 0, (char *)(*pixels),
                                    w, h, 32, 0);
    if (!data->ximage) {
        SDL_free(*pixels);
        return SDL_SetError("Couldn't create XImage");
    }
    data->ximage->byte_order = (SDL_BYTEORDER == SDL_BIG_ENDIAN) ? MSBFirst : LSBFirst;
    return 0;
}

int X11_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects,
                                int numrects)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    Display *display = data->videodata->display;
    int i;
    int x, y, w, h;
    int window_w, window_h;

    SDL_GetWindowSizeInPixels(window, &window_w, &window_h);

    for (i = 0; i < numrects; ++i) {
        x = rects[i].x;
        y = rects[i].y;
        w = rects[i].w;
        h = rects[i].h;

        if (w <= 0 || h <= 0 || (x + w) <= 0 || (y + h) <= 0) {
            /* Clipped? */
            continue;
        }
        if (x < 0) {
            x += w;
            w += rects[i].x;
        }
        if (y < 0) {
            y += h;
            h += rects[i].y;
        }
        if (x + w > window_w) {
            w = window_w - x;
        }
        if (y + h > window_h) {
            h = window_h - y;
        }

        /* StaticGray path: convert just this dirty rect from ARGB8888 to Y8
           grayscale (ordered-dithered to 1-bit when a 2-level e-ink waveform
           is active). */
        if (data->grayscale_buf) {
            X11_grayscale_convert_rect(data, window_w, x, y, w, h,
                                       (data->eink_waveform != 0) ? SDL_TRUE : SDL_FALSE);
        }

#ifndef NO_SHARED_MEMORY
        if (data->use_mitshm) {
            X11_XShmPutImage(display, data->xwindow, data->gc, data->ximage,
                             x, y, x, y, w, h, False);
        } else
#endif /* !NO_SHARED_MEMORY */
        {
            X11_XPutImage(display, data->xwindow, data->gc, data->ximage,
                          x, y, x, y, w, h);
        }

#ifdef SDL_X11_HAVE_EINK
        /* Drive the panel refresh for this rect using the chosen waveform. */
        X11_eink_update(data, x, y, w, h);
#endif
    }

    X11_XSync(display, False);

    return 0;
}

void X11_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    Display *display;

    if (!data) {
        /* The window wasn't fully initialized */
        return;
    }

    display = data->videodata->display;

    if (data->ximage) {
        XDestroyImage(data->ximage); /* frees ximage->data (grayscale_buf or SHM addr) */

#ifndef NO_SHARED_MEMORY
        if (data->use_mitshm) {
            X11_XShmDetach(display, &data->shminfo);
            X11_XSync(display, False);
            shmdt(data->shminfo.shmaddr);
            data->use_mitshm = SDL_FALSE;
        }
#endif /* !NO_SHARED_MEMORY */

        data->ximage = NULL;
        data->grayscale_buf = NULL; /* freed by XDestroyImage above */
    }
    if (data->argb_buf) {
        SDL_free(data->argb_buf);
        data->argb_buf = NULL;
    }
#ifdef SDL_X11_HAVE_EINK
    if (data->eink_fd >= 0) {
        close(data->eink_fd);
        data->eink_fd = -1;
        data->eink_waveform = 0;
    }
#endif
    if (data->gc) {
        X11_XFreeGC(display, data->gc);
        data->gc = NULL;
    }
}

#endif /* SDL_VIDEO_DRIVER_X11 */

/* vi: set ts=4 sw=4 expandtab: */
