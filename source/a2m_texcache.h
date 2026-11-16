/* a2m_texcache.h - D/AVE 2D texture cache (RZ/A2M Linux) */
#ifndef A2M_TEXCACHE_H
#define A2M_TEXCACHE_H

#include <stdbool.h>
#include <stdint.h>

/* Get (or create) a D/AVE-reachable copy of a const image.
 * d1_handle: d2_level1interface(d2 handle). Returns false -> draw in software. */
bool a2m_texcache_get(void * d1_handle, const void * image_dsc,
                      uintptr_t * phys, uint32_t * stride, uint32_t * cf);

void a2m_texcache_stats(uint32_t * used_bytes, uint32_t * count);

#endif
