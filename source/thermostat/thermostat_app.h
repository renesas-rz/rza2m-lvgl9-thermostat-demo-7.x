/**
 * @file thermostat_app.h
 * @brief Entry point for the RZ/A2M LVGL smart-thermostat demo.
 */
#ifndef THERMOSTAT_APP_H
#define THERMOSTAT_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the thermostat demo. Call from main() after lv_init() and the display
 * and touch setup, before entering the lv_timer_handler() loop.
 *  - starts the control thread (model + control + HAL)
 *  - builds the LVGL UI on the active screen
 */
/**
 * UI skin (same model, control thread and HAL for both):
 *   0 = classic UI   (thermostat_ui.c)
 *   1 = elite UI     (thermostat_ui_elite.c), default
 * The THERMO_UI environment variable (elite or classic) overrides it at run time.
 */
#ifndef THERMOSTAT_UI_STYLE
#define THERMOSTAT_UI_STYLE    (1)
#endif

void thermostat_app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* THERMOSTAT_APP_H */
