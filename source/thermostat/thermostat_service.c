/**
 * @file thermostat_service.c
 * @brief Control thread that owns the thermostat model (Linux, POSIX threads).
 *
 * Loop:
 *   wait on the command queue until the next control deadline
 *     - command received  -> apply, publish snapshot immediately (snappy UI)
 *     - deadline reached  -> read HAL sensor, step model, write HAL outputs, publish
 */
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/utsname.h>

#include "thermostat_service.h"
#include "thermostat_hal.h"

static tstat_model_t    s_model;           /* owned by the control thread only */
static tstat_snapshot_t s_snapshot;        /* guarded by s_snap_lock           */
static pthread_mutex_t  s_snap_lock = PTHREAD_MUTEX_INITIALIZER;

static tstat_cmd_t      s_queue[THERMOSTAT_CMD_QUEUE_LEN];
static unsigned         s_q_head, s_q_count;
static pthread_mutex_t  s_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   s_q_cond;

static pthread_t        s_thread;
static bool             s_started;
static uint16_t         s_sim_speed = (uint16_t)THERMOSTAT_DEFAULT_SIM_SPEED;
static uint32_t         s_seq;

static void publish(void)
{
    pthread_mutex_lock(&s_snap_lock);
    s_seq++;
    memcpy(&s_snapshot.model, &s_model, sizeof(s_model));
    s_snapshot.sim_speed = s_sim_speed;
    s_snapshot.seq       = s_seq;
    pthread_mutex_unlock(&s_snap_lock);
}

static void apply_command(const tstat_cmd_t * c)
{
    switch (c->id) {
        case TSTAT_CMD_SET_MODE:
            tstat_model_set_mode(&s_model, (tstat_mode_t)c->a);
            break;
        case TSTAT_CMD_SET_FAN:
            tstat_model_set_fan(&s_model, (tstat_fan_t)c->a);
            break;
        case TSTAT_CMD_SET_SETPOINT:
            tstat_model_set_hold_setpoint(&s_model, c->a, (tdeci_t)c->v1);
            break;
        case TSTAT_CMD_SHIFT_SETPOINTS:
            tstat_model_shift_hold(&s_model, (tdeci_t)c->v1);
            break;
        case TSTAT_CMD_RESUME_SCHEDULE:
            tstat_model_resume_schedule(&s_model);
            break;
        case TSTAT_CMD_SET_COMFORT:
            tstat_model_set_comfort(&s_model, (tstat_comfort_t)c->a, (tdeci_t)c->v1, (tdeci_t)c->v2);
            break;
        case TSTAT_CMD_SET_SENSOR_PART:
            tstat_model_set_sensor_participation(&s_model, c->a, (0 != c->v1));
            break;
        case TSTAT_CMD_SET_SIM_SPEED:
            if ((c->v1 >= 1) && (c->v1 <= 600)) { s_sim_speed = (uint16_t)c->v1; }
            break;
        default:
            break;
    }
}

static void control_tick(void)
{
    tdeci_t t;
    uint8_t rh;

    if (thermostat_hal_read_sensor(&t, &rh)) {
        tstat_model_inject_measurement(&s_model, t, rh);
    }
    tstat_model_step(&s_model, s_sim_speed);
    thermostat_hal_write_outputs(s_model.out_w1, s_model.out_y1, s_model.out_g);
}

/* Take one command, waiting until the absolute deadline. Caller holds s_q_lock. */
static bool queue_pop_until(tstat_cmd_t * out, const struct timespec * deadline)
{
    while (0u == s_q_count) {
        if (NULL == deadline) { return false; }
        if (ETIMEDOUT == pthread_cond_timedwait(&s_q_cond, &s_q_lock, deadline)) {
            if (0u == s_q_count) { return false; }
        }
    }
    *out = s_queue[s_q_head];
    s_q_head = (s_q_head + 1u) % THERMOSTAT_CMD_QUEUE_LEN;
    s_q_count--;
    return true;
}

static void ts_add_ms(struct timespec * t, uint32_t ms)
{
    t->tv_sec  += (time_t)(ms / 1000u);
    t->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}

static bool ts_reached(const struct timespec * now, const struct timespec * t)
{
    return (now->tv_sec > t->tv_sec) || ((now->tv_sec == t->tv_sec) && (now->tv_nsec >= t->tv_nsec));
}

static void * thermostat_ctrl_thread(void * arg)
{
    struct timespec next, now;
    tstat_cmd_t cmd;
    (void)arg;

    thermostat_hal_init();
    publish();

    clock_gettime(CLOCK_MONOTONIC, &next);
    ts_add_ms(&next, THERMOSTAT_CTRL_PERIOD_MS);

    for (;;) {
        bool got;

        pthread_mutex_lock(&s_q_lock);
        got = queue_pop_until(&cmd, &next);
        if (got) {
            apply_command(&cmd);
            /* drain any burst (e.g. fast +/- taps) before publishing */
            while (queue_pop_until(&cmd, NULL)) { apply_command(&cmd); }
        }
        pthread_mutex_unlock(&s_q_lock);

        if (got) {
            publish();
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_reached(&now, &next)) {
            control_tick();
            publish();
            ts_add_ms(&next, THERMOSTAT_CTRL_PERIOD_MS);
            if (ts_reached(&now, &next)) {            /* resync after a stall */
                next = now;
                ts_add_ms(&next, THERMOSTAT_CTRL_PERIOD_MS);
            }
        }
    }
    return NULL;
}

bool thermostat_service_start(void)
{
    pthread_condattr_t ca;

    if (s_started) { return true; }

    tstat_model_init(&s_model);

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s_q_cond, &ca);
    pthread_condattr_destroy(&ca);

    /* publish an initial snapshot so the UI has data before the thread runs */
    publish();

    /* Small fixed stack: the default (8 MiB) does not fit the board's RAM */
    pthread_attr_t ta;
    pthread_attr_init(&ta);
    pthread_attr_setstacksize(&ta, 64u * 1024u);
    int err = pthread_create(&s_thread, &ta, thermostat_ctrl_thread, NULL);
    pthread_attr_destroy(&ta);
    if (0 != err) {
        printf("thermostat: control thread creation failed (%d)\n", err);
        return false;
    }
    pthread_detach(s_thread);
    s_started = true;
    return true;
}

bool thermostat_service_send(const tstat_cmd_t * cmd)
{
    bool ok = false;

    if (!s_started) { return false; }
    pthread_mutex_lock(&s_q_lock);
    if (s_q_count < THERMOSTAT_CMD_QUEUE_LEN) {
        s_queue[(s_q_head + s_q_count) % THERMOSTAT_CMD_QUEUE_LEN] = *cmd;
        s_q_count++;
        ok = true;
        pthread_cond_signal(&s_q_cond);
    }
    pthread_mutex_unlock(&s_q_lock);
    return ok;
}

bool thermostat_service_get_snapshot(tstat_snapshot_t * out, uint32_t * last_seq)
{
    bool updated = false;

    pthread_mutex_lock(&s_snap_lock);
    if (s_snapshot.seq != *last_seq) {
        memcpy(out, &s_snapshot, sizeof(*out));
        *last_seq = s_snapshot.seq;
        updated = true;
    }
    pthread_mutex_unlock(&s_snap_lock);
    return updated;
}

const char * thermostat_service_rtos_version(void)
{
    static char ver[48];   /* "Linux " + up to 32 chars */
    struct utsname u;

    if ('\0' == ver[0]) {
        if (0 == uname(&u)) {
            char * dash = strchr(u.release, '-');
            if (NULL != dash) { *dash = '\0'; }      /* "7.3.0-00011-g..." -> "7.3.0" */
            snprintf(ver, sizeof(ver), "Linux %.32s", u.release);
        } else {
            snprintf(ver, sizeof(ver), "Linux");
        }
    }
    return ver;
}
