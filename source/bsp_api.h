/*
 * bsp_api.h - header included by LVGL's D/AVE 2D draw unit, for RZ/A2M Linux.
 * On Linux it only needs the virtual->physical lookup provided by a2m_disp.c.
 */
#ifndef A2M_BSP_API_H
#define A2M_BSP_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

uintptr_t a2m_virt_to_phys(const void * p, size_t len);

/* D/AVE status for the app (lv_draw_dave2d.c, A2M additions) */
bool     lv_draw_dave2d_a2m_available(void);
uint32_t lv_draw_dave2d_a2m_task_count(void);   /* tasks drawn by D/AVE since start */

/* D/AVE texture cache (a2m_texcache.c) */
#include "a2m_texcache.h"

#endif
