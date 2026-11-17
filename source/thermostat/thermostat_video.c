/**
 * @file thermostat_video.c
 * @brief Full-screen H.264 video player for the thermostat demo (edge264 + LVGL).
 *
 * - Decoder: edge264 (BSD-3, thermostat/edge264/), compiled into the app, no
 *   worker threads. Input is a raw H.264 stream (Annex-B .h264), mapped
 *   read-only from the rootfs and fed one NAL unit at a time. Looped.
 * - Each due frame (YCbCr 4:2:0) is converted to RGB565 (NEON on Cortex-A9).
 * - Output, default: converted straight into the frame-buffer page on screen, at
 *   the centre, so LVGL does not redraw the picture each frame. The
 *   thermostat screen under the black overlay is hidden meanwhile, so LVGL has
 *   nothing to redraw there. THERMO_VIDEO_LVGL=1 or THERMO_VIDEO_FIT=1 use an
 *   lv_image instead (slower; needed for scaling).
 * - Paced by an LVGL timer at a fixed frame rate (raw H.264 has no timestamps).
 *   When the CPU falls behind, late frames are still decoded (they are
 *   references) but only the newest due frame is converted and shown.
 * - Keeps the display "active" while playing so the idle screen does not start.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edge264/edge264.h"

#ifndef TV_HOST_TEST
#include <linux/fb.h>
#include <malloc.h>
#include <sys/ioctl.h>
#include "lvgl.h"
#include "lvgl_private.h"           /* lv_image_cache_drop() */
#include "thermostat_video.h"
#endif

#define TV_DEFAULT_FILE  "/root/video/doorbell.h264"
#define TV_DEFAULT_FPS   (25)
#define TV_TIMER_MS      (10u)
#define TV_MAX_STEP_MS   (100u)     /* longest clock step per tick */
#define TV_MAX_DECODES   (8)        /* most frames decoded in one tick */

/* ------------------------------------------------- colour conversion */

static inline uint8_t tv_clamp(int v) { return (v < 0) ? 0u : ((v > 255) ? 255u : (uint8_t)v); }

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(TV_NO_NEON)
#include <arm_neon.h>
#define TV_HAVE_NEON 1

/* One row, studio range, 16 pixels per step. Same BT.601 formula as the C loop
 * with the coefficients scaled to /64 so the sums fit 16-bit lanes; saturating
 * adds and the narrowing shift do the clamping. Returns the pixels done. */
static int tv_row_neon(const uint8_t * Y, const uint8_t * Cb, const uint8_t * Cr, uint16_t * d, int w)
{
    const int16x8_t k16 = vdupq_n_s16(16), k128 = vdupq_n_s16(128);
    int x = 0;

    for (; x + 16 <= w; x += 16) {
        const uint8x16_t y8 = vld1q_u8(Y + x);
        const int16x8_t cb = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(Cb + (x >> 1)))), k128);
        const int16x8_t cr = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(Cr + (x >> 1)))), k128);
        const int16x8x2_t rc = vzipq_s16(vmulq_n_s16(cr, 102), vmulq_n_s16(cr, 102));
        const int16x8_t gcv = vaddq_s16(vmulq_n_s16(cb, 25), vmulq_n_s16(cr, 52));
        const int16x8x2_t gc = vzipq_s16(gcv, gcv);
        const int16x8x2_t bc = vzipq_s16(vmulq_n_s16(cb, 129), vmulq_n_s16(cb, 129));

        for (int hlf = 0; hlf < 2; hlf++) {
            const uint8x8_t yv = hlf ? vget_high_u8(y8) : vget_low_u8(y8);
            const int16x8_t yy = vmulq_n_s16(vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(yv)), k16), 75);
            const uint8x8_t r = vqrshrun_n_s16(vqaddq_s16(yy, rc.val[hlf]), 6);
            const uint8x8_t g = vqrshrun_n_s16(vqsubq_s16(yy, gc.val[hlf]), 6);
            const uint8x8_t b = vqrshrun_n_s16(vqaddq_s16(yy, bc.val[hlf]), 6);
            uint16x8_t px = vshll_n_u8(r, 8);
            px = vsriq_n_u16(px, vshll_n_u8(g, 8), 5);
            px = vsriq_n_u16(px, vshll_n_u8(b, 8), 11);
            vst1q_u16(d + x + hlf * 8, px);
        }
    }
    return x;
}
#endif

/* BT.601 YCbCr 4:2:0 to RGB565. full = 1: JPEG range (0-255), else studio range (16-235). */
/* dst_stride: pixels from one output row to the next */
static void tv_yuv_to_rgb565(const Edge264Frame * f, uint16_t * dst, int dst_stride, int w, int h, int full)
{
    const int ky = full ? 256 : 298, kr = full ? 359 : 409, kgb = full ? 88 : 100;
    const int kgr = full ? 183 : 208, kb = full ? 454 : 516, y0 = full ? 0 : 16;

    for (int y = 0; y < h; y++) {
        const uint8_t * Y  = f->samples[0] + y * f->stride_Y;
        const uint8_t * Cb = f->samples[1] + (y >> 1) * f->stride_C;
        const uint8_t * Cr = f->samples[2] + (y >> 1) * f->stride_C;
        uint16_t * d = dst + y * dst_stride;
        int x = 0;

#ifdef TV_HAVE_NEON
        if (!full) { x = tv_row_neon(Y, Cb, Cr, d, w); }
#endif
        for (; x < w; x++) {
            const int cb = Cb[x >> 1] - 128;
            const int cr = Cr[x >> 1] - 128;
            const int yy = (Y[x] - y0) * ky;
            const uint8_t r = tv_clamp((yy + cr * kr + 128) >> 8);
            const uint8_t g = tv_clamp((yy - cb * kgb - cr * kgr + 128) >> 8);
            const uint8_t b = tv_clamp((yy + cb * kb + 128) >> 8);
            d[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/* ------------------------------------------------------------ decoder */

typedef struct {
    Edge264Decoder * dec;
    const uint8_t *  buf;       /* whole file, mapped read-only */
    size_t           len;
    const uint8_t *  end;
    const uint8_t *  nal;       /* next NAL unit to feed */
    const uint8_t *  first;     /* first NAL unit, for looping */
} tv_dec_t;

static void tv_dec_close(tv_dec_t * d)
{
    if (NULL != d->dec) { edge264_free(&d->dec); }
    if (NULL != d->buf) { munmap((void *)d->buf, d->len); }
    memset(d, 0, sizeof(*d));
}

static int tv_dec_open(tv_dec_t * d, const char * path)
{
    struct stat st;
    int fd;

    memset(d, 0, sizeof(*d));
    fd = open(path, O_RDONLY);
    if (fd < 0) { return -1; }
    if ((fstat(fd, &st) < 0) || (st.st_size < 8)) { close(fd); return -1; }
    d->len = (size_t)st.st_size;
    d->buf = mmap(NULL, d->len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (MAP_FAILED == d->buf) { d->buf = NULL; return -1; }

    d->end = d->buf + d->len;
    d->first = edge264_find_start_code(d->buf, d->end, 0);   /* skip the [0]001 delimiter */
    if (d->first >= d->end) { tv_dec_close(d); return -1; }
    d->first += 3;
    d->nal = d->first;

    d->dec = edge264_alloc(0, NULL, NULL, 0, NULL, NULL, NULL);   /* no worker threads */
    if (NULL == d->dec) { tv_dec_close(d); return -1; }
    return 0;
}

#define TV_BUSY (2)

/* Next frame, borrowed (give it back with edge264_return_frame):
 * 1 = frame, 0 = end of clip, TV_BUSY = decoder waits for a borrowed frame
 * to come back (try again next tick), < 0 = error. */
static int tv_dec_next(tv_dec_t * d, Edge264Frame * f)
{
    for (;;) {
        const uint8_t * sc;
        int res;

        if (0 == edge264_get_frame(d->dec, f, 1)) { return 1; }
        sc  = edge264_find_start_code(d->nal, d->end, 0);
        res = edge264_decode_NAL(d->dec, d->nal, sc, NULL, NULL);
        if ((ENODATA == res) && (d->nal >= d->end)) {        /* flushed, nothing left */
            return (0 == edge264_get_frame(d->dec, f, 1)) ? 1 : 0;
        }
        if ((EINVAL == res) || (ENOMEM == res)) { return -res; }
        if (ENOBUFS == res) {                       /* take frames, then the same NAL again */
            if (0 == edge264_get_frame(d->dec, f, 1)) { return 1; }
            return (d->nal >= d->end) ? 0 : TV_BUSY;    /* at the end: every frame is out */
        }
        d->nal = (sc < d->end) ? (sc + 3) : d->end;
    }
}

static void tv_dec_rewind(tv_dec_t * d)
{
    edge264_flush(d->dec);
    d->nal = d->first;
}

#ifndef TV_HOST_TEST

/* -------------------------------------------------------------- state */

static tv_dec_t       s_d;
static Edge264Frame   s_cur, s_nxt; /* newest due frame / decoded, not yet due */
static int            s_have_cur, s_have_nxt;
static uint32_t       s_index;      /* frames decoded in this loop */
static double         s_clock_ms;   /* playback clock in this loop */
static double         s_frame_ms;   /* 1000 / fps */
static int            s_full;       /* full-range colour */
static lv_obj_t *     s_layer;
static lv_obj_t *     s_img;
static lv_timer_t *   s_timer;
static lv_image_dsc_t s_dsc;
static uint16_t *     s_px;
static int            s_w, s_h;
static uint32_t       s_last_tick, s_fps_tick, s_decoded, s_shown;
/* per-stage timing for the 5 s report, in microseconds */
static uint64_t       s_us_dec, s_us_cvt, s_us_refr, s_us_refr_t0;
static uint32_t       s_n_refr;
static int            s_refr_drew;
/* direct frame-buffer output */
static int            s_direct;
static uint8_t *      s_fb;
static int            s_fb_fd = -1; /* kept open to read which page is on screen */
static size_t         s_fb_len;
static uint32_t       s_fb_stride, s_fb_pages, s_fb_page_bytes;
static size_t         s_fb_ofs;     /* byte offset of the picture's top-left in a page */
static lv_obj_t *     s_hidden_scr;

static uint64_t tv_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Display refresh time: REFR_START to REFR_READY (render + flush), counted when something was drawn. */
static void tv_refr_cb(lv_event_t * e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (LV_EVENT_REFR_START == c)        { s_us_refr_t0 = tv_us(); s_refr_drew = 0; }
    else if (LV_EVENT_RENDER_START == c) { s_refr_drew = 1; }
    else if ((LV_EVENT_REFR_READY == c) && s_refr_drew) {
        s_us_refr += tv_us() - s_us_refr_t0;
        s_n_refr++;
    }
}

/* Map /dev/fb0 and place a w x h picture at the centre. 0 = ok. */
static int tv_fb_open(int w, int h)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo fx;
    void * m;
    int fd = open("/dev/fb0", O_RDWR);

    if (fd < 0) { return -1; }
    if ((ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0) || (ioctl(fd, FBIOGET_FSCREENINFO, &fx) < 0) ||
        (16 != v.bits_per_pixel) || ((int)v.xres < w) || ((int)v.yres < h)) {
        close(fd);
        return -1;
    }
    m = mmap(NULL, fx.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (MAP_FAILED == m) { close(fd); return -1; }
    s_fb_fd = fd;

    s_fb = m;
    s_fb_len = fx.smem_len;
    s_fb_stride = fx.line_length;
    s_fb_page_bytes = fx.line_length * v.yres;
    s_fb_pages = v.yres_virtual / v.yres;
    if (s_fb_pages < 1u) { s_fb_pages = 1u; }
    if ((size_t)s_fb_pages * s_fb_page_bytes > s_fb_len) { s_fb_pages = (uint32_t)(s_fb_len / s_fb_page_bytes); }
    s_fb_ofs = (size_t)((v.yres - (uint32_t)h) / 2u) * s_fb_stride + (size_t)((v.xres - (uint32_t)w) / 2u) * 2u;
    return 0;
}

/* Picture's top-left pixel in the frame-buffer page now on screen (LVGL pans between pages). */
static uint16_t * tv_fb_dst(void)
{
    struct fb_var_screeninfo v;
    uint32_t page = 0u;
    if ((ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &v) == 0) && (v.yres > 0u)) { page = v.yoffset / v.yres; }
    if (page >= s_fb_pages) { page = 0u; }
    return (uint16_t *)(void *)(s_fb + (size_t)page * s_fb_page_bytes + s_fb_ofs);
}

/* --------------------------------------------------------- callbacks */

static void tv_tick(lv_timer_t * t)
{
    uint32_t ms = lv_tick_elaps(s_last_tick);
    LV_UNUSED(t);

    s_last_tick = lv_tick_get();
    if (ms > TV_MAX_STEP_MS) { ms = TV_MAX_STEP_MS; }
    s_clock_ms += (double)ms;

    for (int n = 0; n < TV_MAX_DECODES; n++) {
        if (!s_have_nxt) {
            uint64_t t0 = tv_us();
            int r = tv_dec_next(&s_d, &s_nxt);
            s_us_dec += tv_us() - t0;
            if (0 == r) {                           /* end of clip: loop */
                if (s_have_cur) { edge264_return_frame(s_d.dec, s_cur.return_arg); s_have_cur = 0; }
                tv_dec_rewind(&s_d);
                s_index = 0u;
                s_clock_ms = 0.0;
                break;
            }
            if (TV_BUSY == r) { break; }            /* frees up once the shown frame is returned */
            if (r < 0) { printf("video: decode error %d\n", r); break; }
            s_have_nxt = 1;
            s_decoded++;
        }
        if ((double)s_index * s_frame_ms > s_clock_ms) { break; }   /* not due yet */
        if (s_have_cur) { edge264_return_frame(s_d.dec, s_cur.return_arg); }  /* skipped: late */
        s_cur = s_nxt;
        s_have_cur = 1;
        s_have_nxt = 0;
        s_index++;
    }

    if (s_have_cur) {
        if ((s_cur.width_Y == s_w) && (s_cur.height_Y == s_h) && (8 == s_cur.bit_depth_Y)) {
            uint64_t t0 = tv_us();
            if (s_direct) {
                uint16_t * dst = tv_fb_dst();
                s_us_refr += tv_us() - t0;          /* "draw" = finding the page on screen */
                s_n_refr++;
                t0 = tv_us();
                tv_yuv_to_rgb565(&s_cur, dst, (int)(s_fb_stride / 2u), s_w, s_h, s_full);
                s_us_cvt += tv_us() - t0;
            } else {
                tv_yuv_to_rgb565(&s_cur, s_px, s_w, s_w, s_h, s_full);
                s_us_cvt += tv_us() - t0;
                lv_image_cache_drop(&s_dsc);        /* same buffer, new pixels */
                lv_obj_invalidate(s_img);
            }
            s_shown++;
        }
        edge264_return_frame(s_d.dec, s_cur.return_arg);
        s_have_cur = 0;
    }
    lv_display_trigger_activity(NULL);              /* keep the idle screen away */

    if (lv_tick_elaps(s_fps_tick) >= 5000u) {
        printf("video: decoded %u fps, shown %u fps | decode %.1f ms/frame, convert %.1f ms/frame, "
               "draw %.1f ms/refresh | CPU share: decode %u%%, convert %u%%, draw %u%%\n",
               (unsigned)((s_decoded + 2u) / 5u), (unsigned)((s_shown + 2u) / 5u),
               s_decoded ? (double)s_us_dec / 1000.0 / s_decoded : 0.0,
               s_shown ? (double)s_us_cvt / 1000.0 / s_shown : 0.0,
               s_n_refr ? (double)s_us_refr / 1000.0 / s_n_refr : 0.0,
               (unsigned)(s_us_dec / 50000u), (unsigned)(s_us_cvt / 50000u), (unsigned)(s_us_refr / 50000u));
        s_decoded = 0u;
        s_shown = 0u;
        s_us_dec = s_us_cvt = s_us_refr = 0u;
        s_n_refr = 0u;
        s_fps_tick = lv_tick_get();
    }
}

static int s_close_pending;

static void tv_close_async(void * p)
{
    LV_UNUSED(p);
    s_close_pending = 0;
    thermostat_video_close();
}

/* Close outside the click event: the teardown then runs from the timer handler,
 * not in the middle of LVGL's input/event processing. */
static void tv_close_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    if (!s_close_pending) {
        s_close_pending = 1;
        lv_async_call(tv_close_async, NULL);
    }
}

/* --------------------------------------------------------------- API */

static lv_obj_t * tv_layer(void)
{
    lv_obj_t * o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, tv_close_cb, LV_EVENT_CLICKED, NULL);
    return o;
}

static void tv_text(lv_obj_t * p, const char * txt, lv_align_t al, int32_t x, int32_t y)
{
    lv_obj_t * l = lv_label_create(p);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, al, x, y);
}

static void tv_fail(const char * line1, const char * line2)
{
    tv_text(s_layer, line1, LV_ALIGN_CENTER, 0, -16);
    if (NULL != line2) { tv_text(s_layer, line2, LV_ALIGN_CENTER, 0, 16); }
    tv_text(s_layer, "Tap to close", LV_ALIGN_BOTTOM_MID, 0, -16);
}

void thermostat_video_open(void)
{
    const char * path = getenv("THERMO_VIDEO");
    const char * fps  = getenv("THERMO_VIDEO_FPS");
    const char * full = getenv("THERMO_VIDEO_FULLRANGE");
    const char * fit  = getenv("THERMO_VIDEO_FIT");
    const char * lvo  = getenv("THERMO_VIDEO_LVGL");
    const int scale = (NULL != fit) && ('1' == fit[0]);
    int rate = TV_DEFAULT_FPS;
    size_t bytes;

    if ((NULL == path) || ('\0' == path[0])) { path = TV_DEFAULT_FILE; }
    if ((NULL != fps) && (atoi(fps) > 0) && (atoi(fps) <= 60)) { rate = atoi(fps); }
    if (NULL != s_layer) { return; }               /* already open */

    s_layer = tv_layer();
    if (tv_dec_open(&s_d, path) < 0) {
        printf("video: cannot open %s\n", path);
        tv_fail("Video not found", path);
        return;
    }

    /* The first frame gives the size; it stays queued as the first one to show. */
    s_have_cur = 0;
    s_have_nxt = (1 == tv_dec_next(&s_d, &s_nxt));
    if (!s_have_nxt || (8 != s_nxt.bit_depth_Y)) {
        printf("video: no 8-bit H.264 frame in %s\n", path);
        if (s_have_nxt) { edge264_return_frame(s_d.dec, s_nxt.return_arg); s_have_nxt = 0; }
        tv_dec_close(&s_d);
        tv_fail("Cannot decode the video", path);
        return;
    }
    s_w = s_nxt.width_Y;
    s_h = s_nxt.height_Y;
    bytes = (size_t)s_w * (size_t)s_h * 2u;
    s_px = malloc(bytes);
    if (NULL == s_px) {
        printf("video: no memory for %dx%d\n", s_w, s_h);
        edge264_return_frame(s_d.dec, s_nxt.return_arg);
        s_have_nxt = 0;
        tv_dec_close(&s_d);
        tv_fail("Not enough memory for the video", NULL);
        return;
    }
    memset(s_px, 0, bytes);

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w      = (uint32_t)s_w;
    s_dsc.header.h      = (uint32_t)s_h;
    s_dsc.header.stride = (uint32_t)s_w * 2u;
    s_dsc.data_size     = (uint32_t)bytes;
    s_dsc.data          = (const uint8_t *)s_px;

    s_direct = !scale && !((NULL != lvo) && ('1' == lvo[0])) && (0 == tv_fb_open(s_w, s_h));
    if (s_direct) {
        /* nothing under the overlay may redraw, or LVGL would paint over the picture */
        s_hidden_scr = lv_screen_active();
        lv_obj_add_flag(s_hidden_scr, LV_OBJ_FLAG_HIDDEN);
        lv_image_cache_drop(&s_dsc);
        free(s_px);                                 /* not needed: frames go straight to the screen */
        s_px = NULL;
    } else {
        s_img = lv_image_create(s_layer);
        lv_image_set_src(s_img, &s_dsc);
        lv_obj_center(s_img);
        lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);   /* taps go to the layer */
    }
    if ((NULL != s_img) && scale) {
        const lv_display_t * disp = lv_display_get_default();
        int32_t sx = (lv_display_get_horizontal_resolution(disp) * 256) / s_w;
        int32_t sy = (lv_display_get_vertical_resolution(disp) * 256) / s_h;
        lv_image_set_scale(s_img, (uint32_t)((sx < sy) ? sx : sy));
    }

    tv_text(s_layer, "Front Door", LV_ALIGN_TOP_LEFT, 16, 12);
    tv_text(s_layer, "Tap to close", LV_ALIGN_BOTTOM_MID, 0, -12);

    s_full      = (NULL != full) && ('1' == full[0]);
    s_frame_ms  = 1000.0 / (double)rate;
    s_index     = 1u;                               /* the queued first frame is index 0, due now */
    s_cur       = s_nxt;
    s_have_cur  = 1;
    s_have_nxt  = 0;
    s_clock_ms  = 0.0;
    s_decoded   = 1u;
    s_shown     = 0u;
    printf("video: %s H.264 %dx%d at %d fps (edge264, %s, %s output)\n", path, s_w, s_h, rate,
#ifdef TV_HAVE_NEON
           s_full ? "C convert" : "NEON convert",
#else
           "C convert",
#endif
           s_direct ? "frame-buffer" : "LVGL image");
    s_us_dec = s_us_cvt = s_us_refr = 0u;
    s_n_refr = 0u;
    lv_display_add_event_cb(lv_display_get_default(), tv_refr_cb, LV_EVENT_ALL, NULL);
    s_last_tick = lv_tick_get();
    s_fps_tick  = s_last_tick;
    s_timer     = lv_timer_create(tv_tick, TV_TIMER_MS, NULL);
}

void thermostat_video_close(void)
{
    if (NULL == s_layer) { return; }
    if (NULL != s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), tv_refr_cb, NULL);
    }
    if (NULL != s_img)   { lv_image_set_src(s_img, NULL); s_img = NULL; }
    if (NULL != s_fb)    { munmap(s_fb, s_fb_len); s_fb = NULL; }
    if (s_fb_fd >= 0)    { close(s_fb_fd); s_fb_fd = -1; }
    if ((NULL != s_hidden_scr) && lv_obj_is_valid(s_hidden_scr)) {
        /* Not lv_obj_remove_flag(): for HIDDEN it marks the parent's layout dirty,
         * and a screen has no parent (NULL dereference). Clear the flag directly. */
        s_hidden_scr->flags &= ~LV_OBJ_FLAG_HIDDEN;
        lv_obj_mark_layout_as_dirty(s_hidden_scr);
        lv_obj_invalidate(s_hidden_scr);
    }
    s_hidden_scr = NULL;
    s_direct = 0;
    if (NULL != s_px)    { lv_image_cache_drop(&s_dsc); free(s_px); s_px = NULL; }
    if (NULL != s_d.dec) {
        if (s_have_cur) { edge264_return_frame(s_d.dec, s_cur.return_arg); s_have_cur = 0; }
        if (s_have_nxt) { edge264_return_frame(s_d.dec, s_nxt.return_arg); s_have_nxt = 0; }
        tv_dec_close(&s_d);
    }
    lv_obj_delete(s_layer);                         /* safe: we are outside its event now */
    s_layer = NULL;
    malloc_trim(0);                                 /* give the decoder's freed memory back to the system */
    printf("video: closed\n");
}

#endif /* TV_HOST_TEST */
