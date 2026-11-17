/**
 * @file thermostat_video.h
 * @brief Full-screen H.264 video player (edge264) for the thermostat demo.
 *
 * thermostat_video_open() shows the clip on LVGL's top layer over the current
 * page; a tap closes it. Call from the LVGL thread only.
 *
 * Environment:
 *   THERMO_VIDEO=<file>     raw H.264 clip to play (default /root/video/doorbell.h264)
 *   THERMO_VIDEO_FPS=<n>    playback rate, 1-60 (default 25)
 *   THERMO_VIDEO_FULLRANGE=1  colours are full range (0-255)
 *   THERMO_VIDEO_FIT=0      1:1 at the centre instead of full screen (default: full screen, D/AVE)
 *   THERMO_VIDEO_LVGL=1     draw through an LVGL image instead of the frame buffer
 */
#ifndef THERMOSTAT_VIDEO_H
#define THERMOSTAT_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

void thermostat_video_open(void);
void thermostat_video_close(void);

#ifdef __cplusplus
}
#endif

#endif /* THERMOSTAT_VIDEO_H */
