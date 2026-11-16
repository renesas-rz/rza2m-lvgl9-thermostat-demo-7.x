/**
 * @file thermostat_app.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thermostat_app.h"
#include "thermostat_service.h"
#include "thermostat_ui.h"
#include "thermostat_ui_elite.h"

void thermostat_app_start(void)
{
    int style = THERMOSTAT_UI_STYLE;
    const char * env = getenv("THERMO_UI");      /* THERMO_UI=elite or classic overrides the default */

    if (NULL != env) {
        if (0 == strcmp(env, "elite"))   { style = 1; }
        if (0 == strcmp(env, "classic")) { style = 0; }
    }
    if (!thermostat_service_start()) {
        printf("thermostat: service start failed - UI will show static data\n");
    }
    if (1 == style) {
        thermostat_ui_elite_create();
    } else {
        thermostat_ui_create();
    }
    printf("thermostat: UI style %d created (%s)\n", style, thermostat_service_rtos_version());
}
