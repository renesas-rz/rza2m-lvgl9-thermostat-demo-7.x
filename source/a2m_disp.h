/* a2m_disp.h - RZ/A2M Linux display driver + virtual->physical lookup for D/AVE 2D */
#ifndef A2M_DISP_H
#define A2M_DISP_H

#include <stddef.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

/* Frame buffer display in DIRECT mode (double buffered when the driver can pan). */
lv_display_t * a2m_disp_create(const char * fbdev);

/* Register memory that D/AVE can reach, and look up physical addresses.
 * a2m_virt_to_phys() returns 0 when [p, p+len) is not inside a registered region. */
void      a2m_phys_region_add(const void * virt, uintptr_t phys, size_t len);
uintptr_t a2m_virt_to_phys(const void * p, size_t len);

#endif
