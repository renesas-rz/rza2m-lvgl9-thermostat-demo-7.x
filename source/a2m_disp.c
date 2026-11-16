/*
 * a2m_disp.c - LVGL 9 display driver for RZ/A2M Linux (VDC6 fbdev)
 *
 * Why not LVGL's own lv_linux_fbdev driver?
 *   It renders into malloc'd buffers. Those are virtual memory the D/AVE 2D
 *   GPU cannot reach. This driver lets LVGL render straight into the frame
 *   buffer (DIRECT mode), whose physical address the kernel reports
 *   (FBIOGET_FSCREENINFO.smem_start), so D/AVE can draw into it too.
 *
 *   - 3 pages (kernel fb_pages = <3>) and panning works -> triple buffer
 *   - 2 pages (yres_virtual >= 2*yres) and panning works -> double buffer
 *   - otherwise                                          -> single buffer
 *
 * Triple buffer: LVGL renders the next frame while the previous flip waits for
 * vsync; a vsync wait only happens if a frame finished within one refresh.
 *
 * Env:  A2M_FB_SINGLE=1      force single buffer
 *       A2M_FB_PHYS=0x...    override physical address (only if smem_start is 0)
 *       A2M_FB_NOVSYNC=1     don't wait for vsync after a page flip (may tear)
 *       A2M_FB_PAGES=1|2|3   max pages to use (default 3)
 */
#include "a2m_disp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* ---------- virtual -> physical registry (used by the D/AVE draw unit) ---------- */
#define A2M_MAX_REGIONS 4
static struct { uintptr_t virt; uintptr_t phys; size_t len; } regions[A2M_MAX_REGIONS];
static int region_cnt;

void a2m_phys_region_add(const void * virt, uintptr_t phys, size_t len)
{
    if(region_cnt >= A2M_MAX_REGIONS || virt == NULL || phys == 0 || len == 0) return;
    regions[region_cnt].virt = (uintptr_t)virt;
    regions[region_cnt].phys = phys;
    regions[region_cnt].len  = len;
    region_cnt++;
}

uintptr_t a2m_virt_to_phys(const void * p, size_t len)
{
    uintptr_t v = (uintptr_t)p;
    int i;
    for(i = 0; i < region_cnt; i++) {
        if(v >= regions[i].virt && v + len <= regions[i].virt + regions[i].len)
            return regions[i].phys + (v - regions[i].virt);
    }
    return 0;
}

/* ---------- driver ---------- */
static int fb_fd = -1;
static uint8_t * fb_mem;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static lv_draw_buf_t draw_buf1, draw_buf2, draw_buf3;
static bool double_buf;
static int pages = 1;
static uint32_t last_pan_ms;
static bool pan_pending;
#define REFRESH_MS 18   /* > one refresh period (59 Hz = 16.9 ms) */
static bool vsync_ok = true;

static uint32_t tick_cb(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    LV_UNUSED(area);
    /* DIRECT mode: pixels are already in the frame buffer. Only show the page. */
    if(pages == 3 && lv_display_flush_is_last(disp)) {
        /* A2M triple buffer: before showing this page, make sure the previous flip
         * reached the screen, so the page LVGL draws next is no longer visible. */
        if(pan_pending && vsync_ok && tick_cb() - last_pan_ms < REFRESH_MS) {
            int zero = 0;
            if(ioctl(fb_fd, FBIO_WAITFORVSYNC, &zero) < 0) {
                vsync_ok = false;
                printf("a2m_disp: FBIO_WAITFORVSYNC not supported - no vsync wait\n");
            }
        }
        vinfo.yoffset = (px_map == draw_buf3.data) ? 2 * vinfo.yres : (px_map == draw_buf2.data) ? vinfo.yres : 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
        last_pan_ms = tick_cb();
        pan_pending = true;
    }
    else if(double_buf && lv_display_flush_is_last(disp)) {
        vinfo.yoffset = (px_map == draw_buf2.data) ? vinfo.yres : 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
        if(vsync_ok) {
            int zero = 0;
            if(ioctl(fb_fd, FBIO_WAITFORVSYNC, &zero) < 0) {
                vsync_ok = false;
                printf("a2m_disp: FBIO_WAITFORVSYNC not supported - no vsync wait\n");
            }
        }
    }
    lv_display_flush_ready(disp);
}

static bool try_pan(uint32_t yoff)
{
    struct fb_var_screeninfo v = vinfo;
    v.yoffset = yoff;
    return ioctl(fb_fd, FBIOPAN_DISPLAY, &v) == 0;
}

lv_display_t * a2m_disp_create(const char * fbdev)
{
    fb_fd = open(fbdev, O_RDWR);
    if(fb_fd < 0) { perror(fbdev); return NULL; }
    if(ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_*SCREENINFO"); close(fb_fd); return NULL;
    }
    if(vinfo.bits_per_pixel != 16) {
        fprintf(stderr, "a2m_disp: %u bpp not supported (need RGB565)\n", vinfo.bits_per_pixel);
        close(fb_fd); return NULL;
    }

    uint32_t w = vinfo.xres, h = vinfo.yres, stride = finfo.line_length;
    size_t page = (size_t)stride * h;

    /* Use as many pages (max 3) as the frame buffer has and panning allows */
    double_buf = false;
    pages = 1;
    int max_pages = 3;
    const char * single = getenv("A2M_FB_SINGLE");
    const char * pg = getenv("A2M_FB_PAGES");
    if(single && single[0] == '1') max_pages = 1;
    if(pg && pg[0] >= '1' && pg[0] <= '3') max_pages = pg[0] - '0';
    int want;
    for(want = max_pages; want >= 2 && pages == 1; want--) {
        if(finfo.smem_len < (uint32_t)want * page) continue;
        if(vinfo.yres_virtual < (uint32_t)want * h) {
            struct fb_var_screeninfo v = vinfo;
            v.yres_virtual = want * h;
            if(ioctl(fb_fd, FBIOPUT_VSCREENINFO, &v) == 0) ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
        }
        if(vinfo.yres_virtual >= (uint32_t)want * h && try_pan((want - 1) * h) && try_pan(0)) pages = want;
    }
    double_buf = (pages >= 2);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);   /* smem_len/line_length may change */
    stride = finfo.line_length;
    page = (size_t)stride * h;
    vinfo.yoffset = 0;
    const char * nv = getenv("A2M_FB_NOVSYNC");
    if(nv && nv[0] == '1') vsync_ok = false;

    size_t map_len = (size_t)pages * page;
    fb_mem = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if(fb_mem == MAP_FAILED) { perror("mmap fb"); close(fb_fd); return NULL; }

    uintptr_t phys = finfo.smem_start;
    const char * ph = getenv("A2M_FB_PHYS");
    if(phys == 0 && ph) phys = (uintptr_t)strtoul(ph, NULL, 0);
    if(phys) a2m_phys_region_add(fb_mem, phys, map_len);

    lv_tick_set_cb(tick_cb);

    lv_display_t * disp = lv_display_create((int32_t)w, (int32_t)h);
    if(disp == NULL) return NULL;
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    lv_draw_buf_init(&draw_buf1, w, h, LV_COLOR_FORMAT_RGB565, stride, fb_mem, page);
    if(double_buf) {
        lv_draw_buf_init(&draw_buf2, w, h, LV_COLOR_FORMAT_RGB565, stride, fb_mem + page, page);
        lv_display_set_draw_buffers(disp, &draw_buf1, &draw_buf2);
        if(pages == 3) {
            lv_draw_buf_init(&draw_buf3, w, h, LV_COLOR_FORMAT_RGB565, stride, fb_mem + 2 * page, page);
            lv_display_set_3rd_draw_buffer(disp, &draw_buf3);
        }
    }
    else {
        lv_display_set_draw_buffers(disp, &draw_buf1, NULL);
    }
    lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    printf("a2m_disp: %s %ux%u stride %u, %s buffer%s, fb phys 0x%08lx%s\n",
           fbdev, w, h, stride, pages == 3 ? "triple" : double_buf ? "double" : "single",
           (double_buf && vsync_ok) ? " + vsync wait" : "",
           (unsigned long)phys, phys ? "" : " (unknown -> D/AVE off)");
    return disp;
}
