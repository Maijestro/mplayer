# V-MPlayer / MPlayer for AmigaOS 4.1 - VA-API Hardware Acceleration
### By Maijestro | Based on MPlayer SVN trunk r38685

**IMPORTANT: This is MPlayer. All credit goes to the MPlayer development team
and especially to the AmigaOS4 MPlayer developers, in particular smarkusg
whose AmigaOS4 patches and GitHub repository formed the basis for this work.**

- Original MPlayer: https://www.mplayerhq.hu
- AmigaOS4 MPlayer by smarkusg: https://github.com/smarkusg/mplayer
- Base SVN: svn://svn.mplayerhq.hu/mplayer/trunk revision 38685
- License: GPL v2 — full source code and patches are available in this repository

This fork adds VA-API hardware accelerated video output and ARexx control
to MPlayer for AmigaOS 4.1. The GUI frontend "V-MPlayer" is a separate
Hollywood script that uses this MPlayer binary as its playback engine.

Tested on AmigaOne X5000 with RX580 graphics card.

Note for QEMU and Pegasos 2 users: change vo=vaapi to vo=comp or vo=sdl
in the MPlayer configuration file.


## Credits

- MPlayer Team — original MPlayer source code
- smarkusg — AmigaOS4 port, patches and GitHub repository this work is based on
- All AmigaOS4 MPlayer contributors
- Maijestro — VA-API implementation, ARexx support, af_export fix, V-MPlayer GUI


## What was added by Maijestro

### VA-API Hardware Acceleration (vo_vaapi.c, vd_ffmpeg.c)
Full hardware accelerated video decoding and output using va.library on AmigaOS 4.1.
See the Patch and VA-API Architecture sections below for technical details.

### af_export Fix (af_export.c, af.c)
The original af_export audio filter uses mmap() which is not available on AmigaOS 4.1.
Replaced with fopen()/fwrite()/fflush() and fixed the AmigaOS path parser.

### ARexx Support (amigaos_stuff.c)
Added ARexx port MPLAYER.1 for external control. Fixed NP_Path = GetProgramDir()
so MPlayer can be started from Workbench.


## Patch

A complete patch against MPlayer SVN trunk r38685 is available:

    patches/amigaos4_vaapi_all.patch

To apply against a fresh SVN checkout:

    svn co -r 38685 svn://svn.mplayerhq.hu/mplayer/trunk mplayer
    cd mplayer
    patch -p0 < amigaos4_vaapi_all.patch


## VA-API Architecture

The implementation consists of two main components that share a global
VA-API hardware device context:

    vd_ffmpeg.c   -  FFmpeg decoder with VA-API hwaccel
    vo_vaapi.c    -  VA-API video output via vaPutSurface into AmigaOS RastPort


### Shared hardware context

    /* vd_ffmpeg.c - line 102 */
    AVBufferRef *vaapi_hw_device_ctx = NULL;

    /* vo_vaapi.c - line 62 */
    extern AVBufferRef *vaapi_hw_device_ctx;


### Decoder side - vd_ffmpeg.c

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


### Output side - vo_vaapi.c

vaPutSurface() is called directly into the AmigaOS window RastPort
with GPU scaling - no CPU copy is needed:

    vaPutSurface(priv.va_display,
                 surface_to_show,
                 VADT_RastPort, window->RPort,
                 src_x, src_y, src_w, src_h,
                 dst_x, dst_y, dst_w, dst_h,
                 NULL, 0,
                 VA_FRAME_PICTURE);

vaCreateConfig and vaCreateContext are required before vaPutSurface.
The profile is derived from the video fourcc at config time.


### Supported formats

Hardware accelerated decoding via FFmpeg VA-API hwaccel:
H.264, HEVC, MPEG-2, MPEG-4, VC-1, VP8, VP9, AV1


## ARexx commands

Available via port MPLAYER.1:

    PAUSE, QUIT, VOLUME VALUE x ABS, SEEK, LOADFILE, MUTE,
    GET_TIME_POS, GET_TIME_LENGTH

Example:

    C:RX "address MPLAYER.1 PAUSE"
    C:RX "address MPLAYER.1 VOLUME VALUE 80 ABS"


## Build

Cross-compiled with ppc-amigaos-gcc under Linux:

    ppc-amigaos-gcc -O2 -mcrt=newlib -mstrict-align \
        -L/opt/valib-sdk/local/newlib/lib -lva -l:libSDL_patched.a \
        -lssl -lcrypto -lpng -lz -lmad -lvorbis -logg \
        -lmpg123 -lfaad -lopus -lrtmp -lvpx \
        -lfreetype -ldav1d -lm -lunix -lauto


## Tested on

- AmigaOne X5000 with RX580 graphics card
- AmigaOS 4.1 Final Edition


## Support

If you find this useful, donations are welcome — but please also consider
supporting the original MPlayer and smarkusg AmigaOS4 developers:
https://www.paypal.com/paypalme/Maijetsro


## License

GPL v2 — this software is free and open source.
Full source code is available in this repository.
Patch against SVN r38685 is in the patches/ folder.

Original MPlayer SVN: svn://svn.mplayerhq.hu/mplayer/trunk r38685
AmigaOS4 base: https://github.com/smarkusg/mplayer
