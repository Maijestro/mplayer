/*
 * vo_vaapi.c
 * VO module for MPlayer AmigaOS4
 * Using VA-API (va.library) for hardware accelerated video output
 *
 * v4 - correct per autodoc: vaCreateConfig + vaCreateContext required
 *      before vaPutSurface. Profile derived from video fourcc.
 *      Output: vaPutSurface VADT_RastPort direkt ins Fenster (GPU-Scaling)
 */

#define SYSTEM_PRIVATE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "osdep/timer.h"
#include <string.h>
#include <errno.h>

#include "config.h"
#include "mp_msg.h"
#include "video_out.h"
#include "video_out_internal.h"
#include "cgx_common.h"
#include <cybergraphx/cybergraphics.h>
#include <graphics/composite.h>

/* Forward declaration for OSD */
static void draw_alpha(int x0, int y0, int w, int h, unsigned char *src, unsigned char *srca, int stride);

#include "version.h"
#include "sub/osd.h"
#include "sub/sub.h"
#include "aspect.h"
#include "libmpcodecs/img_format.h"

#include "../amigaos/debug.h"
#include "../amigaos/window_icons.h"
#include <intuition/imageclass.h>

extern struct Catalog *catalog;
#define MPlayer_NUMBERS
#define MPlayer_STRINGS
#include "../amigaos/MPlayer_catalog.h"
extern STRPTR myGetCatalogStr(struct Catalog *catalog, LONG num, STRPTR def);
#define CS(id) myGetCatalogStr(catalog,id,id##_STR)

/* __USE_INLINE__ set via -D compiler flag */

#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/exec.h>
#include <proto/layers.h>
#include <exec/types.h>

/* VA-API - va.library */
#include <proto/VA.h>
#include "ffmpeg/libavutil/hwcontext.h"
#include "ffmpeg/libavutil/hwcontext_vaapi.h"

/* Shared hw_device_ctx from vd_ffmpeg.c */
extern AVBufferRef *vaapi_hw_device_ctx;

extern APTR window_mx;
extern float amiga_aspect_ratio;
extern uint32_t is_fullscreen;
extern int resize_sec;
extern int osd_level;
extern struct kIcon iconifyIcon;

static vo_info_t info =
{
    "VA-API hardware accelerated video output",
    "vaapi",
    "AmigaOS4 VA-API",
    ""
};

LIBVO_EXTERN(vaapi)

/* Private state */
static struct {
    /* VA-API */
    VADisplay       va_display;
    VAConfigID      va_config;
    VAContextID     va_context;
    VASurfaceID     va_surface;
    VASurfaceID     hw_surface;  /* Surface from FFmpeg HW decode */
    VAImage         va_image;
    int             va_image_valid;

    /* Window & Bitmaps */
    struct Screen   *screen;
    struct Window   *window;

    /* Video dimensions */
    int             src_width;
    uint32_t        image_format;
    uint32_t       *sw_rgb_buf;   /* SW fallback: ARGB buffer */
    uint32_t       *osd_argb_buf;  /* OSD-only ARGB buffer */
    int             osd_buf_w;     /* OSD buffer width */
    int             osd_buf_h;     /* OSD buffer height */
    int             osd_active;    /* OSD currently visible */
    int             osd_dirty;     /* OSD needs redraw */
    int             osd_x1, osd_y1, osd_x2, osd_y2; /* OSD bounding box */
    int             sw_rgb_size;
    int             orig_dst_width;
    int             pending_refresh;
    int             orig_dst_height;
    int             src_height;
    int             dst_width;
    int             dst_height;

    /* State */
    int             initialized;
    int             va_display_updated;
    int             window_shown;  /* Window shown after first frame */
    int             window_height;
    int             window_width;
    int             win_left;
    int             win_top;
} priv;

/* Map MPlayer fourcc to VA profile */
static char *vaapi_window_title = NULL;
static void uninit(void)
{
    if (priv.va_image_valid) {
        vaDestroyImage(priv.va_display, priv.va_image.image_id);
        priv.va_image_valid = 0;
    }
    if (priv.va_context) {
        vaDestroyContext(priv.va_display, priv.va_context);
        priv.va_context = 0;
    }
    if (priv.va_surface != VA_INVALID_SURFACE) {
        vaDestroySurfaces(priv.va_display, &priv.va_surface, 1);
        priv.va_surface = VA_INVALID_SURFACE;
    }
    priv.hw_surface = VA_INVALID_SURFACE;
    if (priv.sw_rgb_buf) { FreeVec(priv.sw_rgb_buf); priv.sw_rgb_buf = NULL; }
    if (priv.osd_argb_buf) { FreeVec(priv.osd_argb_buf); priv.osd_argb_buf = NULL; }
    if (priv.va_config) {
        vaDestroyConfig(priv.va_display, priv.va_config);
        priv.va_config = 0;
    }
    if (priv.window) {
        dispose_icon(priv.window, &iconifyIcon);
        CloseWindow(priv.window);
        priv.window = NULL;
    }
    if (priv.screen) {
        UnlockPubScreen(NULL, priv.screen);
        priv.screen = NULL;
    }
    if (priv.va_display) {
        vaTerminate(priv.va_display);
        priv.va_display = NULL;
    }
    /* Free window title */
    if (vaapi_window_title) { free(vaapi_window_title); vaapi_window_title = NULL; }
    /* Reset entire priv struct to clean state */
    memset(&priv, 0, sizeof(priv));
    priv.va_surface = VA_INVALID_SURFACE;
    priv.hw_surface = VA_INVALID_SURFACE;
}

extern int fixed_vo;
extern char *filename;


static char *GetVAAPIWindowTitle(void)
{
    if (vaapi_window_title) free(vaapi_window_title);
    if (filename) {
        vaapi_window_title = (char *)malloc(strlen("V-MPlayer - ") + strlen(filename) + 1);
        strcpy(vaapi_window_title, "V-MPlayer - ");
        if (strncmp(filename, "http", 4) == 0 || strncmp(filename, "rtmp", 4) == 0)
            strcat(vaapi_window_title, "Stream");
        else {
            const char *bn = strrchr(filename, '/');
            strcat(vaapi_window_title, bn ? bn + 1 : filename);
        }
    } else {
        vaapi_window_title = strdup("V-MPlayer (VAAPI)");
    }
    return vaapi_window_title;
}

static int preinit(const char *arg)
{
    fixed_vo = 1;  /* Keep window open between files */
    memset(&priv, 0, sizeof(priv));
    priv.va_surface = VA_INVALID_SURFACE;
    priv.hw_surface = VA_INVALID_SURFACE;

    priv.va_display = vaGetDisplay(0);
    if (!priv.va_display) {
        mp_msg(MSGT_VO, MSGL_ERR, "VO: [vaapi] Cannot get VA display\n");
        return -1;
    }

    int major, minor;
    VAStatus status = vaInitialize(priv.va_display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        mp_msg(MSGT_VO, MSGL_ERR, "VO: [vaapi] vaInitialize failed: %s\n",
               vaErrorStr(status));
        vaTerminate(priv.va_display);
        priv.va_display = NULL;
        return -1;
    }

    mp_msg(MSGT_VO, MSGL_INFO, "VO: [vaapi] VA-API %d.%d initialized: %s\n",
           major, minor, vaQueryVendorString(priv.va_display));

    return 0;
}

static int config(uint32_t width, uint32_t height, uint32_t d_width,
                  uint32_t d_height, uint32_t flags, char *title,
                  uint32_t format)
{
    VAStatus status;

    /* If window already open and same resolution, just return */
    if (priv.window && priv.src_width == (int)width && priv.src_height == (int)height) {
        return 0;
    }
    /* Resolution changed - free old VAAPI resources */
    if (priv.window) {
        priv.window_shown = 0;
        if (priv.va_image_valid) {
            vaDestroyImage(priv.va_display, priv.va_image.image_id);
            priv.va_image_valid = 0;
        }
        if (priv.va_context) {
            vaDestroyContext(priv.va_display, priv.va_context);
            priv.va_context = 0;
        }
        if (priv.va_surface != VA_INVALID_SURFACE) {
            vaDestroySurfaces(priv.va_display, &priv.va_surface, 1);
            priv.va_surface = VA_INVALID_SURFACE;
        }
        if (priv.osd_argb_buf) { FreeVec(priv.osd_argb_buf); priv.osd_argb_buf = NULL; }
        if (priv.sw_rgb_buf)  { FreeVec(priv.sw_rgb_buf);  priv.sw_rgb_buf  = NULL; }
        }
    priv.src_width  = width;
    priv.src_height = height;
    priv.image_format = format;
    priv.dst_width  = (d_width  > 0 && d_width  < 4096) ? d_width  : width;
    priv.dst_height = (d_height > 0 && d_height < 4096) ? d_height : height;
    /* Limit window size (only in windowed mode) */
    if (!(flags & VOFLAG_FULLSCREEN)) {
        float ar = (float)priv.dst_width / (float)priv.dst_height;
        if (ar >= 1.0f) {
            /* Landscape: max 1280x720 */
            if (priv.dst_width > 1280 || priv.dst_height > 720) {
                if (ar > (1280.0f / 720.0f)) {
                    priv.dst_width  = 1280;
                    priv.dst_height = (int)(1280.0f / ar);
                } else {
                    priv.dst_height = 720;
                    priv.dst_width  = (int)(720.0f * ar);
                }
            }
        } else {
            /* Portrait: limit height to 720px max (fits on 1080p screen) */
            if (priv.dst_height > 720) {
                priv.dst_height = 720;
                priv.dst_width  = (int)(720.0f * ar);
            }
        }
    }
    /* Ensure minimum width for VAAPI compatibility */
    /* Portrait videos (9:16) can be too narrow for VAAPI */
    if (priv.dst_width < 480) {
        float ar = (float)priv.dst_width / (float)priv.dst_height;
        priv.dst_width  = 480;
        priv.dst_height = (int)(480.0f / ar);
        priv.dst_height = (priv.dst_height + 1) & ~1;
    }
    /* Align width to 32 pixels for VAAPI surface stride compatibility */
    {
        float ar = (float)priv.dst_width / (float)priv.dst_height;
        priv.dst_width  = (priv.dst_width + 31) & ~31;
        priv.dst_height = (int)(priv.dst_width / ar);
        priv.dst_height = (priv.dst_height + 1) & ~1;
    }
    /* Final check: ensure window fits on screen including borders (~26px) */
    if (!(flags & VOFLAG_FULLSCREEN)) {
        int max_h = 1054;
        if (priv.dst_height > max_h) {
            float ar = (float)priv.dst_width / (float)priv.dst_height;
            priv.dst_height = max_h;
            priv.dst_width  = (int)(max_h * ar);
            priv.dst_width  = (priv.dst_width + 31) & ~31;
            priv.dst_height = (priv.dst_height + 1) & ~1;
        }
    }
    /* Only save original size once, not on fullscreen toggle */
    if (priv.orig_dst_width == 0) {
        priv.orig_dst_width  = priv.dst_width;
        priv.orig_dst_height = priv.dst_height;
    }
    amiga_aspect_ratio = (float)priv.dst_width / (float)priv.dst_height;

    /* Cleanup va_config */
    if (priv.va_config) {
        vaDestroyConfig(priv.va_display, priv.va_config);
        priv.va_config = 0;
    }

    /* For HW formats: FFmpeg manages VA context via hw_device_ctx
     * Just open the window, skip vaCreateConfig/Surface/Context */
    int is_hw_fmt = (format == IMGFMT_VAAPI_H264 || format == IMGFMT_VAAPI_MPEG2 ||
                     format == IMGFMT_VAAPI_VC1   || format == IMGFMT_VAAPI_WMV3  ||
                     format == IMGFMT_VAAPI_HEVC);

    if (is_hw_fmt) {
        priv.va_surface = VA_INVALID_SURFACE;
        /* Open window for HW format if not already open */
        if (!priv.window) {
            priv.screen = LockPubScreen(NULL);
            if (!priv.screen) return -1;
            uint32_t win_left, win_top;
            if (flags & VOFLAG_FULLSCREEN) {
                win_left = 0; win_top = -(priv.screen->BarHeight + 1);
                priv.dst_width  = priv.screen->Width;
                priv.dst_height = priv.screen->Height + priv.screen->BarHeight + 1;
            } else {
                /* Restore original window size */
                priv.dst_width  = priv.orig_dst_width;
                priv.dst_height = priv.orig_dst_height;
                /* Re-apply final height check after restore */
                {
                    int max_h = priv.screen->Height - priv.screen->BarHeight - 26;
                    if (priv.dst_height > max_h) {
                        float ar2 = (float)priv.dst_width / (float)priv.dst_height;
                        priv.dst_height = max_h;
                        priv.dst_width  = (int)(max_h * ar2);
                        priv.dst_width  = (priv.dst_width + 31) & ~31;
                        priv.dst_height = (priv.dst_height + 1) & ~1;
                    }
                }
                gfx_center_window(priv.screen, priv.dst_width, priv.dst_height, &win_left, &win_top);
            }
            priv.window = OpenWindowTags(NULL,
                WA_PubScreen, (ULONG)priv.screen,
                WA_Left, win_left, WA_Top, win_top,
                WA_InnerWidth,  (flags & VOFLAG_FULLSCREEN) ? priv.screen->Width  : priv.dst_width,
                WA_InnerHeight, (flags & VOFLAG_FULLSCREEN) ? priv.screen->Height + priv.screen->BarHeight + 1 : priv.dst_height,
                WA_SimpleRefresh, TRUE,
                WA_CloseGadget, (flags & VOFLAG_FULLSCREEN) ? FALSE : TRUE,
                WA_DragBar, (flags & VOFLAG_FULLSCREEN) ? FALSE : TRUE,
                WA_Borderless, (flags & VOFLAG_FULLSCREEN) ? TRUE : FALSE,
                WA_SizeGadget, (flags & VOFLAG_FULLSCREEN) ? FALSE : TRUE,
                WA_DepthGadget, (flags & VOFLAG_FULLSCREEN) ? FALSE : TRUE,
                WA_Title, (flags & VOFLAG_FULLSCREEN) ? (ULONG)NULL : (ULONG)GetVAAPIWindowTitle(),
                WA_ScreenTitle, "V-MPlayer (VAAPI)",
                WA_NewLookMenus, TRUE, WA_Activate, TRUE,
                WA_BackFill, LAYERS_NOBACKFILL,
                WA_IDCMP, IDCMP_COMMON | IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW | IDCMP_GADGETUP,
                WA_Flags, WFLG_REPORTMOUSE | WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM,
                WA_MinWidth, (priv.dst_width/3) > 160 ? priv.dst_width/3 : 160,
                WA_MinHeight, (priv.dst_height/3) > 100 ? priv.dst_height/3 : 100,
                WA_MaxWidth, priv.screen->Width, WA_MaxHeight, priv.screen->Height - priv.screen->BarHeight - 6,
                TAG_DONE);
            if (!priv.window) { UnlockPubScreen(NULL, priv.screen); priv.screen = NULL; return -1; }
            /* Fill window black before first frame using direct RGB */
            {
                int bw = priv.dst_width;
                int bh = priv.dst_height;
                uint32_t *black = AllocVecTags(bw * 4, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
                if (black) {
                    int row;
                    for (row = 0; row < bh; row++)
                        WritePixelArray((uint8 *)black, 0, 0, bw * 4, PIXF_A8R8G8B8,
                            priv.window->RPort,
                            priv.window->BorderLeft, priv.window->BorderTop + row,
                            bw, 1);
                    FreeVec(black);
                }
            }
            WindowToFront(priv.window);
            ScreenToFront(priv.screen);
            ActivateWindow(priv.window);
            /* Iconify Button hinzufuegen */
            if (!(flags & VOFLAG_FULLSCREEN))
                open_icon(priv.window, ICONIFYIMAGE, GID_ICONIFY, &iconifyIcon);
            priv.window_width  = priv.window->Width  - priv.window->BorderLeft - priv.window->BorderRight;
            priv.window_height = priv.window->Height - priv.window->BorderTop  - priv.window->BorderBottom;
            amiga_aspect_ratio = (float)priv.dst_width / (float)priv.dst_height;
            resize_sec = -100;
            /* Free sw_rgb_buf if exists - not needed in HW mode */
            if (priv.sw_rgb_buf) { FreeVec(priv.sw_rgb_buf); priv.sw_rgb_buf = NULL; }
            /* Alloc OSD buffer for HW mode - use dst dimensions */
            priv.osd_buf_w = priv.dst_width;
            priv.osd_buf_h = priv.dst_height;
            if (priv.osd_argb_buf) { FreeVec(priv.osd_argb_buf); priv.osd_argb_buf = NULL; }
            priv.osd_argb_buf = (uint32_t *)AllocVecTags(
                priv.osd_buf_w * priv.osd_buf_h * 4,
                AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
            priv.osd_active = 0;
        }
        priv.initialized = 1;
        return 0;
    }

    /* SW format not supported by VAAPI - let vf_vo try next VO */
    return -1;

    /* Open screen and window - only on first call */
    if (!priv.window) {
    priv.screen = LockPubScreen(NULL);
    if (!priv.screen) {
        mp_msg(MSGT_VO, MSGL_ERR, "VO: [vaapi] Cannot lock public screen\n");
        return -1;
    }

    uint32_t win_left, win_top;
    gfx_center_window(priv.screen, priv.dst_width, priv.dst_height,
                      &win_left, &win_top);

    priv.window = OpenWindowTags(NULL,
        WA_PubScreen,     (ULONG)priv.screen,
        WA_Left,          win_left,
        WA_Top,           win_top,
        WA_InnerWidth,    priv.dst_width,
        WA_InnerHeight,   priv.dst_height,
        WA_SimpleRefresh, TRUE,
        WA_CloseGadget,   TRUE,
        WA_DragBar,       TRUE,
        WA_SizeGadget,    TRUE,
        WA_DepthGadget,   TRUE,
        WA_NewLookMenus,  TRUE,
        WA_Activate,      TRUE,
        WA_BackFill,      LAYERS_NOBACKFILL,
        WA_IDCMP,         IDCMP_COMMON | IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW,
        WA_Flags,         WFLG_REPORTMOUSE | WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM,
        WA_MinWidth,      160,
        WA_MinHeight,     100,
        WA_MaxWidth,      priv.screen->Width,
        WA_MaxHeight,     priv.screen->Height,
        TAG_DONE);

    if (!priv.window) {
        mp_msg(MSGT_VO, MSGL_ERR, "VO: [vaapi] Cannot open window\n");
        return -1;
    }

    priv.window_width  = priv.window->Width  - priv.window->BorderLeft
                                              - priv.window->BorderRight;
    priv.window_height = priv.window->Height - priv.window->BorderTop
                                             - priv.window->BorderBottom;


    } /* end if (!priv.window) */

    /* Disable cgx_common INTUITICKS aspect-ratio auto-resize for vaapi */
    resize_sec = -100;
    priv.initialized = 1;
    return 0;
}


static int draw_slice(uint8_t *image[], int stride[], int w, int h, int x, int y)
{
    /* HW VAAPI mode: surfaces managed by FFmpeg, nothing to do here */
    if (priv.hw_surface != VA_INVALID_SURFACE)
        return 0;

    /* SW mode: write directly to RGB buffer */
    if (priv.hw_surface == VA_INVALID_SURFACE) {
        int total = priv.src_width * priv.src_height;
        if (!priv.sw_rgb_buf || priv.sw_rgb_size < total) {
            if (priv.sw_rgb_buf) FreeVec(priv.sw_rgb_buf);
            priv.sw_rgb_buf = (uint32_t *)AllocVecTags(total * 4, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
            priv.sw_rgb_size = total;
        }
        if (!priv.sw_rgb_buf) return -1;
        /* Clear buffer to black at start of new frame (y==0) */
        if (y == 0) memset(priv.sw_rgb_buf, 0, total * 4);

        uint8_t *y_plane = image[0];
        uint8_t *u_plane = image[1];  /* I420/YUV420p: U=1, V=2 */
        uint8_t *v_plane = image[2];
        int row, col;
        for (row = y; row < y + h && row < priv.src_height; row++) {
            uint8_t *y_row = y_plane + row * stride[0] + x;
            uint8_t *u_row = u_plane + (row/2) * stride[1] + x/2;
            uint8_t *v_row = v_plane + (row/2) * stride[2] + x/2;
            uint32_t *out = priv.sw_rgb_buf + row * priv.src_width + x;
            for (col = 0; col < w; col++) {
                int yy = y_row[col];
                int u  = u_row[col/2] - 128;
                int v  = v_row[col/2] - 128;
                int r = yy + (v * 1436 >> 10);
                int g = yy - (u * 352 >> 10) - (v * 731 >> 10);
                int b = yy + (u * 1814 >> 10);
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
                out[col] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
        priv.va_image_valid = 1;
        return 0;
    }
    VAStatus status;
    void *buf = NULL;
    VAImage derived_image;

    /* Try vaDeriveImage first */
    status = vaDeriveImage(priv.va_display, priv.va_surface, &derived_image);
    if (status == VA_STATUS_SUCCESS) {
        status = vaMapBuffer(priv.va_display, derived_image.buf, &buf);
        if (status != VA_STATUS_SUCCESS) {
            vaDestroyImage(priv.va_display, derived_image.image_id);
            return -1;
        }

        uint8_t *dst = (uint8_t *)buf;
        int i;

        uint8_t *src_y = image[0] + y * stride[0] + x;
        uint8_t *dst_y = dst + derived_image.offsets[0]
                       + y * derived_image.pitches[0] + x;
        for (i = 0; i < h; i++) {
            memcpy(dst_y, src_y, w);
            src_y += stride[0];
            dst_y += derived_image.pitches[0];
        }

        /* YV12: image[1]=U, image[2]=V but surface wants V then U */
        uint8_t *src_u = image[2] + (y/2) * stride[2] + (x/2);
        uint8_t *dst_u = dst + derived_image.offsets[1]
                       + (y/2) * derived_image.pitches[1] + (x/2);
        for (i = 0; i < h/2; i++) {
            memcpy(dst_u, src_u, w/2);
            src_u += stride[2];
            dst_u += derived_image.pitches[1];
        }

        uint8_t *src_v = image[1] + (y/2) * stride[1] + (x/2);
        uint8_t *dst_v = dst + derived_image.offsets[2]
                       + (y/2) * derived_image.pitches[2] + (x/2);
        for (i = 0; i < h/2; i++) {
            memcpy(dst_v, src_v, w/2);
            src_v += stride[1];
            dst_v += derived_image.pitches[2];
        }

        vaUnmapBuffer(priv.va_display, derived_image.buf);
        vaDestroyImage(priv.va_display, derived_image.image_id);
        priv.va_image_valid = 1;
        return 0;
    }

    /* Fallback: vaCreateImage + vaPutImage */
    if (!priv.va_image_valid) {
        VAImageFormat fmt;
        int num_formats = vaMaxNumImageFormats(priv.va_display);
        VAImageFormat *formats = calloc(num_formats, sizeof(VAImageFormat));
        vaQueryImageFormats(priv.va_display, formats, &num_formats);

        int i, found = 0;
        for (i = 0; i < num_formats; i++) {
            if (formats[i].fourcc == VA_FOURCC_YV12 ||
                formats[i].fourcc == VA_FOURCC_NV12) {
                fmt = formats[i];
                found = 1;
                break;
            }
        }
        free(formats);

        if (!found) {
            mp_msg(MSGT_VO, MSGL_ERR, "VO: [vaapi] No YUV image format found\n");
            return -1;
        }

        status = vaCreateImage(priv.va_display, &fmt,
                               priv.src_width, priv.src_height, &priv.va_image);
        if (status != VA_STATUS_SUCCESS) {
            mp_msg(MSGT_VO, MSGL_ERR, "VO: [vaapi] vaCreateImage failed: %s\n",
                   vaErrorStr(status));
            return -1;
        }
        priv.va_image_valid = 1;
    }

    status = vaMapBuffer(priv.va_display, priv.va_image.buf, &buf);
    if (status != VA_STATUS_SUCCESS) return -1;

    uint8_t *dst = (uint8_t *)buf;
    int i, j;

    /* Copy Y plane */
    uint8_t *src_y = image[0] + y * stride[0] + x;
    uint8_t *dst_y = dst + priv.va_image.offsets[0]
                   + y * priv.va_image.pitches[0] + x;
    for (i = 0; i < h; i++) {
        memcpy(dst_y, src_y, w);
        src_y += stride[0];
        dst_y += priv.va_image.pitches[0];
    }

    if (priv.va_image.format.fourcc == VA_FOURCC_NV12) {
        /* NV12: interleaved UV in plane 1 */
        uint8_t *src_u = image[0] + stride[0] * priv.src_height + (y/2) * stride[1] + (x/2);
        uint8_t *src_v = image[0] + stride[0] * priv.src_height + stride[1] * (priv.src_height/2) + (y/2) * stride[2] + (x/2);
        /* Use image[1] and image[2] directly */
        src_u = image[1] + (y/2) * stride[1] + (x/2);
        src_v = image[2] + (y/2) * stride[2] + (x/2);
        uint8_t *dst_uv = dst + priv.va_image.offsets[1]
                        + (y/2) * priv.va_image.pitches[1] + x;
        for (i = 0; i < h/2; i++) {
            for (j = 0; j < w/2; j++) {
                dst_uv[j*2]   = src_u[j];
                dst_uv[j*2+1] = src_v[j];
            }
            src_u  += stride[1];
            src_v  += stride[2];
            dst_uv += priv.va_image.pitches[1];
        }
    } else {
        /* YV12: separate U and V planes */
        uint8_t *src_u = image[1] + (y/2) * stride[1] + (x/2);
        uint8_t *dst_u = dst + priv.va_image.offsets[1]
                       + (y/2) * priv.va_image.pitches[1] + (x/2);
        for (i = 0; i < h/2; i++) {
            memcpy(dst_u, src_u, w/2);
            src_u += stride[1];
            dst_u += priv.va_image.pitches[1];
        }

        uint8_t *src_v = image[2] + (y/2) * stride[2] + (x/2);
        uint8_t *dst_v = dst + priv.va_image.offsets[2]
                       + (y/2) * priv.va_image.pitches[2] + (x/2);
        for (i = 0; i < h/2; i++) {
            memcpy(dst_v, src_v, w/2);
            src_v += stride[2];
            dst_v += priv.va_image.pitches[2];
        }
    }

    vaUnmapBuffer(priv.va_display, priv.va_image.buf);
    return 0;
}

static int draw_frame(uint8_t *src[])
{
    /* In HW mode: src[3] contains the VASurfaceID from FFmpeg */
    if (src && src[3]) {
        priv.hw_surface = (VASurfaceID)(uintptr_t)src[3];
        mp_msg(MSGT_VO, MSGL_DBG2, "VO: [vaapi] draw_frame hw_surface=0x%x\n",
               (unsigned)priv.hw_surface);
    }
    return 0;
}

static void flip_page(void)
{
    if (!priv.initialized || !priv.window) return;
    /* Update va_display from FFmpeg hw_device_ctx - only once */
    if (!priv.va_display_updated && vaapi_hw_device_ctx) {
        AVHWDeviceContext *dev_ctx = (AVHWDeviceContext *)vaapi_hw_device_ctx->data;
        AVVAAPIDeviceContext *va_ctx = (AVVAAPIDeviceContext *)dev_ctx->hwctx;
        if (va_ctx && va_ctx->display) {
            priv.va_display = va_ctx->display;
            priv.va_display_updated = 1;
        }
    }

    /* Determine which surface to display */
    VASurfaceID surface_to_show = VA_INVALID_SURFACE;
    if (priv.hw_surface != VA_INVALID_SURFACE) {
        surface_to_show = priv.hw_surface;  /* HW decoded surface */
    } else if (priv.va_surface != VA_INVALID_SURFACE) {
        surface_to_show = priv.va_surface;  /* SW upload surface */
    }
    if (surface_to_show == VA_INVALID_SURFACE) return;

    /* After zip-resize: draw frame normally then refresh borders */
    int do_border_refresh = (priv.pending_refresh > 0);
    if (do_border_refresh) priv.pending_refresh--;

    /* Always use actual window size directly - avoids stale cached values after Zip */
    int cur_w = priv.window->Width  - priv.window->BorderLeft - priv.window->BorderRight;
    int cur_h = priv.window->Height - priv.window->BorderTop  - priv.window->BorderBottom;
    if (cur_w <= 0) cur_w = priv.dst_width;
    if (cur_h <= 0) cur_h = priv.dst_height;
    /* Detect Zip-Button resize that check_events missed */
    if (cur_w != priv.window_width || cur_h != priv.window_height) {
        if (priv.osd_argb_buf) { FreeVec(priv.osd_argb_buf); priv.osd_argb_buf = NULL; }
        priv.window_width  = cur_w;
        priv.window_height = cur_h;
        priv.dst_width  = cur_w;
        priv.dst_height = cur_h;
        priv.osd_buf_w  = cur_w;
        priv.osd_buf_h  = cur_h;
        /* Mark that next frame needs border refresh after draw */
        priv.pending_refresh = 1;
    }

    /* SW mode: use sw_rgb_buf filled by draw_slice */
    if (priv.hw_surface == VA_INVALID_SURFACE && priv.va_image_valid && priv.sw_rgb_buf) {
        int out_w = cur_w < priv.src_width ? cur_w : priv.src_width;
        int out_h = cur_h < priv.src_height ? cur_h : priv.src_height;
        struct RastPort *rp = priv.window->RPort;
        WritePixelArray((uint8 *)priv.sw_rgb_buf, 0, 0,
                       priv.src_width * 4, PIXF_A8R8G8B8,
                       rp,
                       priv.window->BorderLeft,
                       priv.window->BorderTop,
                       out_w, out_h);
        return;
    }

        /* HW mode: use vaPutSurface */
    /* vaSyncSurface nicht noetig - vaPutSurface wartet intern */

    /* Step 1: vaPutSurface direkt in Fenster RastPort mit GPU-Scaling */
    LockLayer(0, priv.window->WLayer);
    VAStatus st = vaPutSurface(priv.va_display, surface_to_show,
                 VADT_RastPort, priv.window->RPort,
                 0, 0, priv.src_width, priv.src_height,
                 priv.window->BorderLeft, priv.window->BorderTop,
                 cur_w, cur_h,
                 VA_FRAME_PICTURE | VA_SRC_BT709 | VA_FILTER_SCALING_HQ);
    /* Step 2: OSD und Unlock */
    if (st == VA_STATUS_SUCCESS) {
        /* OSD Buffer aktualisieren - nur wenn OSD aktiv oder dirty */
        /* draw_osd nur wenn sich OSD geaendert hat - nicht jeden Frame */
        if (vo_osd_changed_flag || (!priv.osd_active && priv.osd_dirty))
            draw_osd();
        /* OSD zeichnen */
        int fs = is_fullscreen;
        if (priv.osd_argb_buf && priv.osd_active && priv.osd_dirty && !fs) {
            struct RastPort *win_rp = priv.window->RPort;
            int ox1 = priv.osd_x1 < 0 ? 0 : priv.osd_x1;
            int oy1 = priv.osd_y1 < 0 ? 0 : priv.osd_y1;
            int ox2 = priv.osd_x2 > cur_w ? cur_w : priv.osd_x2;
            int oy2 = priv.osd_y2 > cur_h ? cur_h : priv.osd_y2;
            int ow = ox2;
            int i, j;
            for (j = oy1; j < oy2; j++) {
                int run_start = -1;
                for (i = 0; i <= ow; i++) {
                    uint32_t px = (i < ow) ? priv.osd_argb_buf[j * priv.osd_buf_w + i] : 0;
                    if (i < ow && (px >> 24)) {
                        if (run_start < 0) run_start = i;
                    } else {
                        if (run_start >= 0) {
                            WritePixelArray(
                                (uint8 *)(priv.osd_argb_buf + j * priv.osd_buf_w + run_start),
                                0, 0, (i - run_start) * 4, RECTFMT_ARGB,
                                win_rp,
                                priv.window->BorderLeft + run_start,
                                priv.window->BorderTop + j,
                                i - run_start, 1);
                            run_start = -1;
                        }
                    }
                }
            }
            priv.osd_dirty = 0;
        }
        UnlockLayer(priv.window->WLayer);
        /* Refresh borders after draw if zip-resize was detected */
        if (do_border_refresh) RefreshWindowFrame(priv.window);
    } else if (st != VA_STATUS_SUCCESS) {
        mp_msg(MSGT_VO, MSGL_ERR, "VO: [vaapi] vaPutSurface failed: %s\n",
               vaErrorStr(st));
        return;
    }
}

static void draw_alpha(int x0, int y0, int w, int h,
                        unsigned char *src, unsigned char *srca, int stride)
{
    /* Lazy alloc OSD buffer on first use */
    if (!priv.osd_argb_buf) {
        if (priv.osd_buf_w <= 0 || priv.osd_buf_h <= 0) return;
        priv.osd_argb_buf = (uint32_t *)AllocVecTags(
            priv.osd_buf_w * priv.osd_buf_h * 4,
            AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
        if (!priv.osd_argb_buf) return;
    }
    priv.osd_active = 1;
    /* Bounding Box aktualisieren */
    if (x0 < priv.osd_x1) priv.osd_x1 = x0;
    if (y0 < priv.osd_y1) priv.osd_y1 = y0;
    if (x0 + w > priv.osd_x2) priv.osd_x2 = x0 + w;
    if (y0 + h > priv.osd_y2) priv.osd_y2 = y0 + h;
    int i, j;
    int dw = priv.osd_buf_w;
    for (j = 0; j < h; j++) {
        if ((y0 + j) >= priv.osd_buf_h) break;
        uint32_t *dst = priv.osd_argb_buf + (y0 + j) * dw + x0;
        for (i = 0; i < w; i++) {
            if (x0 + i >= dw) break;
            unsigned char a = srca[j * stride + i];
            if (a) {
                unsigned char v = src[j * stride + i];
                if (v > 64)
                    dst[i] = 0xFFFFFFFF;
            }
        }
    }
}

static void draw_osd(void)
{
    if (priv.osd_buf_w <= 0 || priv.osd_buf_h <= 0) return;
    if (osd_level == 0) { priv.osd_active = 0; priv.osd_dirty = 0; return; }
    int was_active = priv.osd_active;
    /* Bounding Box zuruecksetzen */
    priv.osd_x1 = priv.osd_buf_w; priv.osd_y1 = priv.osd_buf_h;
    priv.osd_x2 = 0; priv.osd_y2 = 0;
    /* Buffer leeren und neu zeichnen */
    if (priv.osd_argb_buf)
        memset(priv.osd_argb_buf, 0, priv.osd_buf_w * priv.osd_buf_h * 4);
    priv.osd_active = 0;
    vo_draw_text(priv.osd_buf_w, priv.osd_buf_h, draw_alpha);
    /* dirty setzen wenn OSD aktiv oder gerade verschwunden */
    if (priv.osd_active || was_active)
        priv.osd_dirty = 1;
}

static void check_events(void)
{
    if (!priv.window) return;
    int old_w = priv.window_width;
    int old_h = priv.window_height;

    gfx_CheckEvents(priv.screen, priv.window,
                    &priv.window_height, &priv.window_width,
                    &priv.win_left, &priv.win_top);
    resize_sec = -100; /* Prevent cgx_common auto aspect-ratio resize */

    /* Resize: reallocate back buffer and OSD buffers if window size changed */
    if (priv.window_width != old_w || priv.window_height != old_h) {
        if (priv.osd_argb_buf) { FreeVec(priv.osd_argb_buf); priv.osd_argb_buf = NULL; }
        priv.dst_width  = priv.window_width;
        priv.dst_height = priv.window_height;
        priv.osd_buf_w  = priv.window_width;
        priv.osd_buf_h  = priv.window_height;
        /* Lazy alloc - allocated on first OSD draw */
        /* Refresh window frame to ensure borders are visible */
        RefreshWindowFrame(priv.window);
        priv.pending_refresh = 1;
    }
}

static int control(uint32_t request, void *data)
{
    switch (request) {
    case VOCTRL_QUERY_FORMAT:
        switch (*(uint32_t *)data) {
        case IMGFMT_YV12:
        case IMGFMT_I420:
        case IMGFMT_IYUV:
            return VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW |
                   VFCAP_ACCEPT_STRIDE;
        case IMGFMT_VAAPI_H264:
        case IMGFMT_VAAPI_MPEG2:
        case IMGFMT_VAAPI_VC1:
        case IMGFMT_VAAPI_WMV3:
        case IMGFMT_VAAPI_HEVC:
            return VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW | VOCAP_NOSLICES;
        }
        return 0;
    case VOCTRL_GET_IMAGE: {
        mp_image_t *mpi = (mp_image_t *)data;
        if (!mpi) return VO_FALSE;
        /* For HW formats: just return the mpi as-is, FFmpeg fills planes[3] */
        if (IMGFMT_IS_VAAPI(mpi->imgfmt)) {
            mpi->flags |= MP_IMGFLAG_DIRECT;
            mpi->priv = NULL;
            return VO_TRUE;
        }
        return VO_FALSE;
    }
    case VOCTRL_DRAW_IMAGE: {
        mp_image_t *mpi = (mp_image_t *)data;
        if (mpi) {
            if (mpi->planes[3]) {
                priv.hw_surface = (VASurfaceID)(uintptr_t)mpi->planes[3];
            }
        }
        /* Return VO_FALSE so MPlayer calls flip_page() normally */
        return VO_FALSE;
    }
    case VOCTRL_FULLSCREEN:
        vo_fs ^= 1;
        is_fullscreen ^= VOFLAG_FULLSCREEN;
        /* Free VAAPI resources before closing window */
        if (priv.va_image_valid) {
            vaDestroyImage(priv.va_display, priv.va_image.image_id);
            priv.va_image_valid = 0;
        }
        if (priv.va_context) {
            vaDestroyContext(priv.va_display, priv.va_context);
            priv.va_context = 0;
        }
        if (priv.va_surface != VA_INVALID_SURFACE) {
            vaDestroySurfaces(priv.va_display, &priv.va_surface, 1);
            priv.va_surface = VA_INVALID_SURFACE;
        }
        if (priv.osd_argb_buf) { FreeVec(priv.osd_argb_buf); priv.osd_argb_buf = NULL; }
        if (priv.window) { CloseWindow(priv.window); priv.window = NULL; }
        if (config(priv.src_width, priv.src_height,
                   priv.dst_width, priv.dst_height,
                   is_fullscreen, NULL, priv.image_format) < 0)
            return VO_FALSE;
        return VO_TRUE;
        case VOCTRL_UPDATE_SCREENINFO:
        vo_screenwidth  = priv.screen ? priv.screen->Width  : 1024;
        vo_screenheight = priv.screen ? priv.screen->Height : 768;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}
