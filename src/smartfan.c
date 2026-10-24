// SmartFan: drives both fans from the most critical of many SMC temperature
// sensors. Each sensor has its own start/max threshold (sensors.conf); fan speed
// follows the sensor that is closest to its own limit.
//
// Safety: on exit, on lost sensors or SMC errors the fans go back to Apple's
// automatic control; CPU/GPU at 96°C forces full speed immediately.
//
// Usage: smartfan [--dry] [--config PATH]
//   --dry  print decisions every second, never write to the SMC (no root needed)
//
// Mode is read every second from the file "mode" next to the config, written by
// the menu bar app: smart (default) | quiet | max | apple (hand fans to macOS).
// State for the menu bar app goes to status.json.

#include "smc.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SENSORS 128
#define MAX_FANS 4
#define INTERVAL_SEC 1
#define EMA_ALPHA 0.35        // smoothing of jumpy core sensors
#define RAMP_UP_RPM 600.0     // max speed-up per second
#define RAMP_DOWN_RPM 120.0   // max slow-down per second (acts as hysteresis)
#define WRITE_DEADBAND 40.0   // don't touch the SMC for smaller changes
#define REASSERT_SEC 15       // rewrite target periodically (sleep/wake, MFC)
#define EMERGENCY_C 96.0
#define FAILSAFE_RETRY_SEC 5
#define QUIET_SHIFT_C 6.0     // quiet mode: every threshold this much higher
#define STATUS_PATH "/Library/Logs/SmartFan/status.txt"
#define JSON_PATH "/Library/Logs/SmartFan/status.json"

typedef enum { MODE_SMART, MODE_QUIET, MODE_MAX, MODE_APPLE } mode_t_;
static const char *mode_names[] = {"smart", "quiet", "max", "apple"};

typedef struct {
    char key[5];
    double lo, hi;
    char desc[64];
    double ema;
    bool have_ema, valid;
    double raw, score, demand;  // score: unclamped position between lo and hi
} sensor_t;

typedef struct {
    int idx;
    double mn, mx, target, written;
    smc_val_t tg_like;
    time_t last_write;
} fan_t;

static sensor_t sensors[MAX_SENSORS];
static int nsensors;
static fan_t fans[MAX_FANS];
static int nfans;
static bool dry;
static char mode_path[1024];
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool is_core_heat(const char *key) { return key[0] == 'T' && (key[1] == 'C' || key[1] == 'G'); }

static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f) && nsensors < MAX_SENSORS) {
        char key[8], desc[64] = "";
        double lo, hi;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%7s %lf %lf %63[^\n]", key, &lo, &hi, desc) < 3) continue;
        if (strlen(key) != 4 || key[0] != 'T') continue;
        // Config is user-editable: keep thresholds within sane bounds.
        lo = clamp(lo, 20, 95);
        hi = clamp(hi, lo + 3, 98);
        sensor_t *s = &sensors[nsensors++];
        memset(s, 0, sizeof *s);
        memcpy(s->key, key, 5);
        s->lo = lo;
        s->hi = hi;
        snprintf(s->desc, sizeof s->desc, "%s", desc);
    }
    fclose(f);
    return nsensors;
}

static bool read_fan_value(int fan, const char *suffix, smc_val_t *v, double *d) {
    char key[5];
    snprintf(key, sizeof key, "F%d%s", fan, suffix);
    return smc_read(key, v) == KERN_SUCCESS && smc_to_double(v, d);
}

static int init_fans(void) {
    smc_val_t v;
    double n;
    if (smc_read("FNum", &v) != KERN_SUCCESS || !smc_to_double(&v, &n)) return -1;
    nfans = (int)n > MAX_FANS ? MAX_FANS : (int)n;
    for (int i = 0; i < nfans; i++) {
        fan_t *fa = &fans[i];
        double tg;
        fa->idx = i;
        if (!read_fan_value(i, "Mn", &v, &fa->mn) || !read_fan_value(i, "Mx", &v, &fa->mx) ||
            !read_fan_value(i, "Tg", &fa->tg_like, &tg))
            return -1;
        if (fa->mx <= fa->mn) return -1;
        fa->target = fa->mn;
        fa->written = -1;
    }
    return nfans;
}

static void set_mode(int fan, int manual) {
    char key[5];
    uint8_t b[32] = {(uint8_t)manual};
    snprintf(key, sizeof key, "F%dMd", fan);
    if (!dry) smc_write(key, b, 1);
}

static bool write_target(fan_t *fa) {
    char key[5];
    uint8_t b[32];
    snprintf(key, sizeof key, "F%dTg", fa->idx);
    if (!smc_from_double(&fa->tg_like, fa->target, b)) return false;
    return smc_write(key, b, fa->tg_like.size) == KERN_SUCCESS;
}

static void release_fans(const char *why) {
    for (int i = 0; i < nfans; i++) {
        set_mode(i, 0);
        fans[i].written = -1;
    }
    logmsg("fans returned to automatic control: %s", why);
}

static void on_signal(int sig) { (void)sig; stop = 1; }

static mode_t_ read_mode(void) {
    char buf[16] = "";
    FILE *f = fopen(mode_path, "r");
    if (!f) return MODE_SMART;
    if (fscanf(f, "%15s", buf) != 1) buf[0] = 0;
    fclose(f);
    for (int m = 0; m < 4; m++)
        if (!strcmp(buf, mode_names[m])) return (mode_t_)m;
    return MODE_SMART;
}

// Reads all sensors; returns the index of the most critical one, -1 if unsafe.
static int sample(double *demand_out, bool *emergency, double shift) {
    int valid = 0, core_valid = 0, winner = -1;
    double demand = 0;
    *emergency = false;
    for (int i = 0; i < nsensors; i++) {
        sensor_t *s = &sensors[i];
        smc_val_t v;
        double t;
        s->valid = smc_read(s->key, &v) == KERN_SUCCESS && smc_to_double(&v, &t) &&
                   t > 1 && t < 130;  // -127 and 0 mean "no sensor"
        if (!s->valid) continue;
        valid++;
        if (is_core_heat(s->key)) core_valid++;
        s->raw = t;
        s->ema = s->have_ema ? s->ema + EMA_ALPHA * (t - s->ema) : t;
        s->have_ema = true;
        double hi = fmin(s->hi + shift, 98), lo = fmin(s->lo + shift, hi - 3);
        s->score = (s->ema - lo) / (hi - lo);
        s->demand = clamp(s->score, 0, 1);
        if (is_core_heat(s->key) && t >= EMERGENCY_C) *emergency = true;
        if (winner < 0 || s->score > sensors[winner].score) winner = i;
    }
    if (valid < nsensors / 2 || core_valid == 0) return -1;
    demand = *emergency ? 1 : sensors[winner].demand;
    *demand_out = demand;
    return winner;
}

static int cmp_demand(const void *a, const void *b) {
    const sensor_t *x = *(sensor_t *const *)a, *y = *(sensor_t *const *)b;
    double dx = x->valid ? x->score : -1e9, dy = y->valid ? y->score : -1e9;
    return dx < dy ? 1 : dx > dy ? -1 : 0;
}

static void write_status(double demand, int winner, bool emergency) {
    FILE *f = dry ? stdout : fopen(STATUS_PATH ".tmp", "w");
    if (!f) return;
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    sensor_t *w = &sensors[winner];
    fprintf(f, "%s  нагрузка вентиляторов %3.0f%%%s  ведущий датчик: %s %.1f°C (%s, порог %.0f→%.0f)\n", ts,
            demand * 100, emergency ? " [предел]" : "", w->key, w->ema, w->desc, w->lo, w->hi);
    for (int i = 0; i < nfans; i++) {
        smc_val_t v;
        double ac = 0;
        read_fan_value(i, "Ac", &v, &ac);
        fprintf(f, "  вентилятор %d: цель %4.0f, сейчас %4.0f об/мин (диапазон %.0f–%.0f)\n", i, fans[i].target, ac,
                fans[i].mn, fans[i].mx);
    }
    sensor_t *sorted[MAX_SENSORS];
    for (int i = 0; i < nsensors; i++) sorted[i] = &sensors[i];
    qsort(sorted, nsensors, sizeof sorted[0], cmp_demand);
    if (!dry) {
        fprintf(f, "  самые близкие к своему порогу:\n");
        for (int i = 0; i < nsensors && i < 8 && sorted[i]->valid; i++)
            fprintf(f, "    %s %5.1f°C  %3.0f%%  %s\n", sorted[i]->key, sorted[i]->ema, sorted[i]->demand * 100,
                    sorted[i]->desc);
        fclose(f);
        rename(STATUS_PATH ".tmp", STATUS_PATH);
    }
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fputc(' ', f);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void write_json(mode_t_ mode, bool controlling, double demand, int winner, bool emergency) {
    FILE *f = fopen(JSON_PATH ".tmp", "w");
    if (!f) return;
    fprintf(f, "{\"time\":%ld,\"mode\":\"%s\",\"controlling\":%s,\"demand\":%.3f,\"limit\":%s,", (long)time(NULL),
            mode_names[mode], controlling ? "true" : "false", demand, emergency ? "true" : "false");
    fprintf(f, "\"leader\":\"%s\",\"fans\":[", sensors[winner].key);
    for (int i = 0; i < nfans; i++) {
        smc_val_t v;
        double ac = 0;
        read_fan_value(i, "Ac", &v, &ac);
        fprintf(f, "%s{\"target\":%.0f,\"actual\":%.0f,\"min\":%.0f,\"max\":%.0f}", i ? "," : "", fans[i].target, ac,
                fans[i].mn, fans[i].mx);
    }
    fprintf(f, "],\"sensors\":[");
    sensor_t *sorted[MAX_SENSORS];
    for (int i = 0; i < nsensors; i++) sorted[i] = &sensors[i];
    qsort(sorted, nsensors, sizeof sorted[0], cmp_demand);
    for (int i = 0, n = 0; i < nsensors && sorted[i]->valid; i++, n++) {
        fprintf(f, "%s{\"key\":\"%s\",\"temp\":%.1f,\"raw\":%.1f,\"load\":%.3f,\"lo\":%.0f,\"hi\":%.0f,\"desc\":",
                n ? "," : "", sorted[i]->key, sorted[i]->ema, sorted[i]->raw, sorted[i]->demand, sorted[i]->lo, sorted[i]->hi);
        json_str(f, sorted[i]->desc);
        fputc('}', f);
    }
    fprintf(f, "]}\n");
    fclose(f);
    rename(JSON_PATH ".tmp", JSON_PATH);
}

int main(int argc, char **argv) {
    const char *config = "sensors.conf";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry")) dry = true;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) config = argv[++i];
        else { fprintf(stderr, "usage: %s [--dry] [--config PATH]\n", argv[0]); return 2; }
    }
    const char *slash = strrchr(config, '/');
    snprintf(mode_path, sizeof mode_path, "%.*smode", slash ? (int)(slash - config + 1) : 0, config);
    if (load_config(config) <= 0) { fprintf(stderr, "no sensors in %s\n", config); return 1; }
    if (smc_open() != KERN_SUCCESS) { fprintf(stderr, "cannot open AppleSMC\n"); return 1; }
    if (init_fans() <= 0) { fprintf(stderr, "cannot read fan limits\n"); return 1; }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    logmsg("started%s: %d sensors, %d fans", dry ? " (dry run)" : "", nsensors, nfans);

    bool released = true;
    mode_t_ last_mode = MODE_SMART;
    while (!stop) {
        double demand;
        bool emergency;
        mode_t_ mode = read_mode();
        if (mode != last_mode) logmsg("mode: %s", mode_names[mode]);
        last_mode = mode;
        int winner = sample(&demand, &emergency, mode == MODE_QUIET ? QUIET_SHIFT_C : 0);
        if (winner >= 0 && mode == MODE_APPLE) {
            if (!released) release_fans("apple mode selected");
            released = true;
            for (int i = 0; i < nfans; i++) {
                smc_val_t v;
                read_fan_value(i, "Ac", &v, &fans[i].target);
            }
            if (!dry) write_json(mode, false, demand, winner, emergency);
            sleep(INTERVAL_SEC);
            continue;
        }
        if (winner >= 0 && mode == MODE_MAX) demand = 1;
        if (winner < 0) {
            if (!released) release_fans("sensors unavailable");
            released = true;
            sleep(FAILSAFE_RETRY_SEC);
            continue;
        }
        time_t now = time(NULL);
        bool ok = true;
        for (int i = 0; i < nfans; i++) {
            fan_t *fa = &fans[i];
            double want = fa->mn + demand * (fa->mx - fa->mn);
            double step = want - fa->target;
            if (emergency) fa->target = fa->mx;
            else fa->target += clamp(step, -RAMP_DOWN_RPM * INTERVAL_SEC, RAMP_UP_RPM * INTERVAL_SEC);
            fa->target = clamp(fa->target, fa->mn, fa->mx);
            if (dry) continue;

            smc_val_t md;
            double mode = 0;
            bool mode_ok = read_fan_value(i, "Md", &md, &mode);
            bool need = released || !mode_ok || mode != 1 || fabs(fa->target - fa->written) >= WRITE_DEADBAND ||
                        now - fa->last_write >= REASSERT_SEC;
            if (!need) continue;
            if (!mode_ok || mode != 1) set_mode(i, 1);
            if (write_target(fa)) {
                fa->written = fa->target;
                fa->last_write = now;
            } else {
                ok = false;
            }
        }
        if (!ok) {
            release_fans("SMC write failed");
            released = true;
            sleep(FAILSAFE_RETRY_SEC);
            continue;
        }
        if (released && !dry) logmsg("controlling fans");
        released = false;
        write_status(demand, winner, emergency);
        if (!dry) write_json(mode, true, demand, winner, emergency);
        sleep(INTERVAL_SEC);
    }
    if (!dry) release_fans("service stopped");
    smc_close();
    return 0;
}
