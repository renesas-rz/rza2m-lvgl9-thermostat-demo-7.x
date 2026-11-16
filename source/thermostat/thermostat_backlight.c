/**
 * @file thermostat_backlight.c
 * @brief Linux implementation of thermostat_hal_set_backlight() (replaces the weak no-op).
 *
 * 1. If the kernel exposes a backlight (/sys/class/backlight/<dev>/), the panel
 *    backlight is set there, scaled to its max_brightness.
 * 2. Otherwise the screen is dimmed in software: a black overlay on LVGL's system
 *    layer whose opacity follows the slider. It does not take touch input.
 *
 * Called from the LVGL thread only (thermostat_hal.h).
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "thermostat_hal.h"

static int        s_bl_max = -2;          /* -2 not checked yet, -1 none, >0 max_brightness */
static char       s_bl_path[160];
static lv_obj_t * s_dim;

static void backlight_find(void)
{
    DIR * d = opendir("/sys/class/backlight");
    struct dirent * e;

    s_bl_max = -1;
    if (NULL != d) {
        while ((s_bl_max < 0) && (NULL != (e = readdir(d)))) {
            char p[160];
            FILE * f;
            int m = 0;

            if ('.' == e->d_name[0]) { continue; }
            snprintf(p, sizeof(p), "/sys/class/backlight/%.100s/max_brightness", e->d_name);
            f = fopen(p, "r");
            if (NULL != f) {
                if (1 != fscanf(f, "%d", &m)) { m = 0; }
                fclose(f);
            }
            if (m > 0) {
                snprintf(s_bl_path, sizeof(s_bl_path), "/sys/class/backlight/%.100s/brightness", e->d_name);
                s_bl_max = m;
            }
        }
        closedir(d);
    }
    if (s_bl_max > 0) {
        printf("backlight: %s (max %d)\n", s_bl_path, s_bl_max);
    } else {
        printf("backlight: no /sys/class/backlight device, dimming in software\n");
    }
}

void thermostat_hal_set_backlight(uint8_t percent)
{
    if (percent < 10u)  { percent = 10u; }
    if (percent > 100u) { percent = 100u; }
    if (-2 == s_bl_max) { backlight_find(); }

    if (s_bl_max > 0) {
        FILE * f = fopen(s_bl_path, "w");
        if (NULL != f) {
            fprintf(f, "%d\n", (s_bl_max * (int)percent + 50) / 100);
            fclose(f);
        }
        return;
    }

    /* software dimming: 100 % = no overlay, 10 % = about 78 % black */
    if (NULL == s_dim) {
        s_dim = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(s_dim);
        lv_obj_set_size(s_dim, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_dim, lv_color_black(), 0);
        lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE);
    }
    {
        lv_opa_t opa = (lv_opa_t)(((100u - percent) * 200u) / 90u);
        lv_obj_set_style_bg_opa(s_dim, opa, 0);
        if (0u == opa) { lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN); }
        else           { lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_HIDDEN); }
    }
}
