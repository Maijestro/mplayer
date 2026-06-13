# MPlayer for AmigaOS 4.1 - VA-API Hardware Acceleration
### Fork by Maijestro | Based on [smarkusg/mplayer](https://github.com/smarkusg/mplayer)

Full VA-API hardware accelerated video decoding and output for AmigaOS 4.1,
using va.library and FFmpeg hwaccel integration.

Tested on AmigaOne X5000 with RX580 graphics card.

Note for QEMU and Pegasos 2 users: change vo=vaapi to vo=comp or vo=sdl
in the MPlayer configuration file.


## VA-API Architecture

The implementation consists of two main components that share a global
VA-API hardware device context:

    vd_ffmpeg.c   -  FFmpeg decoder with VA-API hwaccel
    vo_vaapi.c    -  VA-API video output via vaPutSurface into AmigaOS RastPort


### Shared hardware context

A global AVBufferRef is declared in vd_ffmpeg.c and used by vo_vaapi.c:

    /* vd_ffmpeg.c - line 102 */
    AVBufferRef *vaapi_hw_device_ctx = NULL;

    /* vo_vaapi.c - line 62 */
    extern AVBufferRef *vaapi_hw_device_ctx;

This allows the decoder and the output driver to share the same VADisplay
without opening it twice.


### Decoder side - vd_ffmpeg.c

FFmpeg provides a get_format() callback that is called by the codec
to negotiate the pixel format. We hook into this to activate VA-API:

    avctx->get_format = get_format;

Inside get_format(), when AV_PIX_FMT_VAAPI is offered by the codec:

    case AV_PIX_FMT_VAAPI:
        av_hwdevice_ctx_create(&hw_device_ctx,
                               AV_HWDEVICE_TYPE_VAAPI,
                               "/dev/dri/renderD128", NULL, 0);

        avctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        vaapi_hw_device_ctx  = av_buffer_ref(hw_device_ctx);

For FFmpeg 5 and later, hw_frames_ctx must also be set inside get_format():

        AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(avctx->hw_device_ctx);
        AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
        frames_ctx->format    = AV_PIX_FMT_VAAPI;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width     = avctx->coded_width;
        frames_ctx->height    = avctx->coded_height;
        av_hwframe_ctx_init(hw_frames_ref);
        avctx->hw_frames_ctx = hw_frames_ref;

The decoded frames are then VA-API surfaces (VASurfaceID) that stay
in GPU memory and never need to be copied to the CPU.


### Output side - vo_vaapi.c

The output driver receives the decoded frame as an AVFrame with
data[3] containing the VASurfaceID. The VA display is retrieved
from the shared hw_device_ctx:

    AVHWDeviceContext *dev_ctx =
        (AVHWDeviceContext *)vaapi_hw_device_ctx->data;

vaPutSurface() is called directly into the AmigaOS window RastPort
with GPU scaling - no CPU copy is needed:

    vaPutSurface(priv.va_display,
                 surface_to_show,
                 VADT_RastPort, window->RPort,
                 src_x, src_y, src_w, src_h,
                 dst_x, dst_y, dst_w, dst_h,
                 NULL, 0,
                 VA_FRAME_PICTURE);

The vaCreateConfig and vaCreateContext are required before vaPutSurface.
The profile is derived from the video fourcc at config time.


### Supported formats

Hardware accelerated decoding via FFmpeg VA-API hwaccel:
H.264, HEVC, MPEG-2, MPEG-4, VC-1, VP8, VP9, AV1


## af_export Fix for AmigaOS 4.1

The original af_export audio filter uses mmap() which is not available
on AmigaOS 4.1.

Changes in af_export.c:
- Replaced mmap() with fopen() / fwrite() / fflush()
- Fixed path parser: AmigaOS uses volume:path notation (e.g. RAM:file)
  so the first colon must be skipped when parsing the filename argument
- sys/mman.h excluded on AmigaOS4 via #ifndef __amigaos4__

Changes in af.c:
- Removed #if HAVE_SYS_MMAN_H so af_export is always compiled in

Usage:

    mplayer -af export=RAM:mplayer_vis.raw:512 file.mp3

Output format (RAM:mplayer_vis.raw, 2064 bytes total):

    Bytes 0-3:   nch (channels, big-endian)
    Bytes 4-7:   size (samples x bps x nch, big-endian)
    Bytes 8-15:  counter (incremented each update, big-endian)
    Bytes 16+:   512 x int16_t PCM per channel (s16le, non-interleaved)


## ARexx Support

ARexx port MPLAYER.1 for external control.

Critical fix for Workbench startup in amigaos_stuff.c:

    NP_Path = GetProgramDir()

Without this, OpenLibrary("arexx.class") fails when MPlayer is
started from Workbench. StartArexx() must be called after all
open_lib() calls.

Available commands:

    PAUSE, QUIT, VOLUME VALUE x ABS, SEEK, LOADFILE, MUTE,
    GET_TIME_POS, GET_TIME_LENGTH


## Build

Cross-compiled with ppc-amigaos-gcc under Linux:

    ppc-amigaos-gcc -O2 -mcrt=newlib -mstrict-align \
        -L/opt/valib-sdk/local/newlib/lib -lva -l:libSDL_patched.a \
        -lssl -lcrypto -lpng -lz -lmad -lvorbis -logg \
        -lmpg123 -lfaad -lopus -lrtmp -lvpx \
        -lfreetype -ldav1d -lm -lunix -lauto


## Support

If you find this useful, donations are welcome:
https://www.paypal.me/Maijestro


## License

GPL v2 - see LICENSE file for details.
Original: https://github.com/smarkusg/mplayer
