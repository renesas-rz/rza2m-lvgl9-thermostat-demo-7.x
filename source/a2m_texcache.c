/*
 * a2m_texcache.c - D/AVE 2D texture cache for RZ/A2M Linux
 *
 * D/AVE can only read textures at a physical address it can reach. The app's
 * images are const C arrays (executed in place from QSPI flash), so the first
 * time an image is drawn by D/AVE it is copied once into the D/AVE video
 * memory pool (on-chip RAM, 'vidmem' in the device tree):
 *
 *   RGB565, ARGB8888 -> copied as is
 *   RGB565A8         -> converted to ARGB8888 (D/AVE has no RGB565A8 format)
 *
 * Entries are never evicted, because batched D/AVE display lists may still
 * reference them. When the budget is used up, further images are simply left
 * to software rendering.
 *
 * Env:  A2M_TEXCACHE_KB=<n>   total budget        (default 512)
 *       A2M_TEX_MAX_KB=<n>    max size per image  (default 160)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "dave_driver.h"
#include "dave_base.h"
#include "a2m_texcache.h"

#define MAX_ENTRIES 64

typedef struct {
    const lv_image_dsc_t * src;
    uintptr_t phys;
    uint32_t stride;
    lv_color_format_t cf;
    bool failed;                 /* don't retry images that did not fit */
} entry_t;

static entry_t entries[MAX_ENTRIES];
static int entry_cnt;
static uint32_t used_bytes, budget_bytes, max_img_bytes;

static uint32_t env_kb(const char * name, uint32_t def)
{
    const char * v = getenv(name);
    return (v && v[0]) ? (uint32_t)strtoul(v, NULL, 0) : def;
}

static void convert_rgb565a8(const lv_image_dsc_t * img, uint32_t * dst)
{
    uint32_t w = img->header.w, h = img->header.h;
    uint32_t stride = img->header.stride ? img->header.stride : w * 2;
    const uint8_t * rgb = img->data;
    const uint8_t * a8 = img->data + stride * h;   /* alpha plane follows the colour plane */
    uint32_t a_stride = stride / 2;
    uint32_t x, y;
    for(y = 0; y < h; y++) {
        const uint16_t * c = (const uint16_t *)(rgb + y * stride);
        const uint8_t * a = a8 + y * a_stride;
        uint32_t * d = dst + y * w;
        for(x = 0; x < w; x++) {
            uint16_t p = c[x];
            uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            d[x] = ((uint32_t)a[x] << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

bool a2m_texcache_get(void * d1_handle, const void * image_dsc,
                      uintptr_t * phys, uint32_t * stride, uint32_t * cf)
{
    const lv_image_dsc_t * img = image_dsc;
    int i;
    if(d1_handle == NULL || img == NULL || img->data == NULL) return false;
    if(img->header.flags & LV_IMAGE_FLAGS_COMPRESSED) return false;

    for(i = 0; i < entry_cnt; i++) {
        if(entries[i].src == img) {
            if(entries[i].failed) return false;
            *phys = entries[i].phys; *stride = entries[i].stride; *cf = entries[i].cf;
            return true;
        }
    }
    if(entry_cnt >= MAX_ENTRIES) return false;

    if(budget_bytes == 0) {
        budget_bytes  = env_kb("A2M_TEXCACHE_KB", 512) * 1024;
        max_img_bytes = env_kb("A2M_TEX_MAX_KB", 160) * 1024;
    }

    entry_t * e = &entries[entry_cnt++];
    memset(e, 0, sizeof(*e));
    e->src = img;
    e->failed = true;

    uint32_t w = img->header.w, h = img->header.h;
    uint32_t bytes;
    lv_color_format_t in_cf = (lv_color_format_t)img->header.cf;
    if(in_cf == LV_COLOR_FORMAT_RGB565) { e->cf = in_cf; e->stride = w * 2; }
    else if(in_cf == LV_COLOR_FORMAT_ARGB8888 || in_cf == LV_COLOR_FORMAT_RGB565A8) {
        e->cf = LV_COLOR_FORMAT_ARGB8888; e->stride = w * 4;
    }
    else return false;
    bytes = e->stride * h;

    uint32_t in_stride = img->header.stride ? img->header.stride : w * lv_color_format_get_size(in_cf);
    if(in_cf != LV_COLOR_FORMAT_RGB565A8 && in_stride != e->stride) return false;   /* padded rows: skip */
    if(bytes > max_img_bytes || used_bytes + bytes > budget_bytes) return false;

    void * p = d1_allocvidmem((d1_device *)d1_handle, d1_mem_texture, bytes);
    if(p == NULL) return false;
    void * v = d1_mapfromvidmem((d1_device *)d1_handle, p);

    if(in_cf == LV_COLOR_FORMAT_RGB565A8) {
        /* convert in normal RAM, then copy in one go (vidmem is uncached) */
        uint32_t * tmp = malloc(bytes);
        if(tmp == NULL) { d1_freevidmem((d1_device *)d1_handle, d1_mem_texture, p); return false; }
        convert_rgb565a8(img, tmp);
        memcpy(v, tmp, bytes);
        free(tmp);
    }
    else {
        memcpy(v, img->data, bytes);
    }

    e->phys = (uintptr_t)p;
    e->failed = false;
    used_bytes += bytes;
    *phys = e->phys; *stride = e->stride; *cf = e->cf;
    return true;
}

void a2m_texcache_stats(uint32_t * used, uint32_t * count)
{
    int i, n = 0;
    for(i = 0; i < entry_cnt; i++) if(!entries[i].failed) n++;
    *used = used_bytes;
    *count = (uint32_t)n;
}
