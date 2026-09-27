/* Telemetry — an American Rocketry Challenge launch-day logbook for the
 * FREE-WILi 2.
 *
 * Flight: OK starts the stopwatch at first motion and stops it at
 * touchdown. Tap a field (or move to it with the D-pad and press CENTER) to
 * type the altimeter's altitude, a second timer's reading, the motor, the
 * liftoff mass, the wind and a note; eggs and practice / qualifying toggle.
 * The ARC score is worked out as you go:
 *     |target - altitude|  +  4 x seconds outside the duration window
 * with the official time the average of the two timers, rounded to 0.01 s
 * (.005 up). A cracked egg disqualifies the flight.
 * Log: every flight, newest first, and the best two qualifying flights.
 * Trim: for one motor, altitude and duration against mass, fitted over your
 * flights, and the mass that should reach the target.
 * Setup: targets (the Finals target changes on the day), team, clock.
 *
 * The five colour buttons under the screen pick the page (GREY Flight,
 * YELLOW Log, GREEN Trim, BLUE Setup); RED is the page's own action. Hold
 * HOME for 5 s to leave, PAGE for 5 s for the About screen.
 *
 * Flights are saved to /appdata/telemetry/flights.csv on the SD card in the
 * MAIN processor's slot, a file any spreadsheet opens. Built as a PSRAM app
 * (fw2_psram_app): the board-clock calls are OneWili text commands, which
 * need about 10 KB of stack, and a PSRAM app has the SRAM for its stack. */
#include "fw2.h"
#include "platform/diag.h"
#include "platform/psram.h"
#include "display/font5x7.h"
#include "pico/stdlib.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ layout */
#define W ST7796_W                  /* 480 */
#define H ST7796_H                  /* 320 */
#define BAR_H 22
#define SOFT_Y 290                  /* soft-key labels above the colour buttons */
#define SOFT_W 96

#define WATCH_Y 28                  /* stopwatch digits */
#define DIG_W 40
#define DIG_H 60
#define DIG_T 8
#define HINT_Y 92
#define GRID_Y 112                  /* flight fields: 5 rows */
#define ROW_H 26
#define SCORE_Y 244

#define KB_Y 144                    /* entry keyboard: four rows */
#define KEY_H 44

#define APP_DIR    "/appdata"
#define DATA_DIR   APP_DIR "/telemetry"
#define CSV_PATH   DATA_DIR "/flights.csv"
#define CSV_NEW    DATA_DIR "/flights.new"
#define SET_PATH   DATA_DIR "/settings.txt"
#define MAX_FLIGHTS 400
#define CSV_MAX    (96 * 1024)
#define SAVE_DELAY_US 800000
#define MASS_LIMIT_G 650            /* ARC 2027: liftoff mass, motor included */

/* Colours as ordinary RGB565; converted to the LCD's wire order when drawn. */
#define C_BG       0x0000
#define C_TEXT     0xFFFF
#define C_BAR      0x18E3
#define C_DIM      0x8410
#define C_FAINT    0x2104
#define C_PANEL    0x10A2
#define C_FOCUS    0x2B4F
#define C_KEY      0x39E7
#define C_KEY_FN   0x2124
#define C_KEY_DOWN 0x2D7F
#define C_RUN      0xFFE0
#define C_OK       0x07E0
#define C_WARN     0xFD20
#define C_BAD      0xF800
static const uint16_t k_soft_col[5] = { 0xD69A, 0xFF06, 0x1200, 0x00F8, 0x8007 };  /* grey yellow green blue red */
static const uint16_t k_soft_txt[5] = { 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF };

static inline uint16_t be(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

/* ------------------------------------------------------------- framebuffer */
static uint16_t __uninitialized_psram("fb") s_fb[W * H];
static int s_dirty_y0 = H, s_dirty_y1 = 0;       /* rows to send, [y0, y1) */

static void mark(int y0, int y1) {
    if (y0 < s_dirty_y0) s_dirty_y0 = y0;
    if (y1 > s_dirty_y1) s_dirty_y1 = y1;
}

static void fb_rect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t v = be(c);
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = s_fb + yy * W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = v;
    }
}

/* Text in the 5x7 font; n < 0 means the whole string. Returns the x after it. */
static int fb_text(int x, int y, int scale, uint16_t fg, uint16_t bg, const char *s, int n) {
    uint16_t f = be(fg), b = be(bg);
    int cw = 6 * scale, ch = 8 * scale;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++, x += cw) {
        if (x < 0 || x + cw > W || y < 0 || y + ch > H) break;
        char c = s[i];
        const uint8_t *cols = (c >= FONT5X7_FIRST && c <= FONT5X7_LAST) ? font5x7[c - FONT5X7_FIRST] : font5x7[0];
        for (int gy = 0; gy < ch; gy++) {
            int r = gy / scale;
            uint16_t *row = s_fb + (y + gy) * W + x;
            for (int gx = 0; gx < cw; gx++) {
                int col = gx / scale;
                row[gx] = (col < 5 && r < 7 && ((cols[col] >> r) & 1)) ? f : b;
            }
        }
    }
    return x;
}

static void fb_text_centre(int x, int w, int y, int scale, uint16_t fg, uint16_t bg, const char *s) {
    int tw = (int)strlen(s) * 6 * scale;
    fb_text(x + (w - tw) / 2, y, scale, fg, bg, s, -1);
}

/* Send the changed rows to the LCD. Rows are whole screen width, so a band
 * of the framebuffer is one contiguous run the DMA can read directly. */
static void flush(void) {
    if (s_dirty_y0 >= s_dirty_y1 || st7796_flush_busy()) return;
    int y0 = s_dirty_y0, y1 = s_dirty_y1;
    s_dirty_y0 = H; s_dirty_y1 = 0;
    st7796_flush_async(0, (uint16_t)y0, W - 1, (uint16_t)(y1 - 1), s_fb + y0 * W, NULL);
}

/* Seven-segment digits for the stopwatch: readable at arm's length in sun.
 * Unlit segments are drawn faintly, like an LCD. */
static void seg_digit(int x, int y, int d, uint16_t on) {
    static const uint8_t k_seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };  /* gfedcba */
    const int w = DIG_W, h = DIG_H, t = DIG_T, h2 = h / 2;
    const int r[7][4] = {
        { x + t, y, w - 2 * t, t },                    /* a */
        { x + w - t, y + t, t, h2 - t - t / 2 },       /* b */
        { x + w - t, y + h2 + t / 2, t, h2 - t - t / 2 }, /* c */
        { x + t, y + h - t, w - 2 * t, t },            /* d */
        { x, y + h2 + t / 2, t, h2 - t - t / 2 },      /* e */
        { x, y + t, t, h2 - t - t / 2 },               /* f */
        { x + t, y + h2 - t / 2, w - 2 * t, t },       /* g */
    };
    uint8_t m = d >= 0 && d <= 9 ? k_seg[d] : 0;
    for (int i = 0; i < 7; i++) fb_rect(r[i][0], r[i][1], r[i][2], r[i][3], (m >> i) & 1 ? on : C_FAINT);
}

/* ------------------------------------------------------------------ flights */
#define UNKNOWN (-1)
#define NO_TEMP (-999)

typedef struct {
    int num;
    char date[11];                  /* "2027-05-16", or "" when the clock isn't set */
    char tod[6];                    /* "09:41" */
    uint8_t qual;                   /* 0 practice, 1 qualifying */
    uint8_t cracked;                /* an egg cracked: disqualified */
    char motor[12];
    int mass_g, wind_mph, temp_f, rh_pct;
    int t1_cs, t2_cs;               /* the two timers, in hundredths of a second */
    int alt_ft;
    int target_ft, win_lo_cs, win_hi_cs;
    char note[41];
} flight_t;

static flight_t s_fl[MAX_FLIGHTS + 1];          /* [s_n] is the next flight, not yet in the log */
static int s_n;                                  /* flights in the log */
static int s_cur;                                /* the flight on the Flight page */

typedef struct { int target_ft, win_lo_cs, win_hi_cs; char team[25]; } settings_t;
static settings_t s_set = { 800, 3700, 4000, "" };

static bool has_data(const flight_t *f) {
    return f->t1_cs >= 0 || f->t2_cs >= 0 || f->alt_ft >= 0 || f->wind_mph >= 0 || f->cracked || f->note[0];
}

/* Official duration: the average of the two timers to the nearest 0.01 s,
 * .005 rounded up; one timer alone if only one ran. */
static int duration_cs(const flight_t *f) {
    if (f->t1_cs >= 0 && f->t2_cs >= 0) return (f->t1_cs + f->t2_cs + 1) / 2;
    return f->t1_cs >= 0 ? f->t1_cs : f->t2_cs;
}

typedef struct { bool complete; int alt, time_cs, total_cs; } score_t;

static score_t score_of(const flight_t *f) {
    score_t s = { false, 0, 0, 0 };
    int d = duration_cs(f);
    if (f->alt_ft >= 0) s.alt = abs(f->target_ft - f->alt_ft);
    if (d >= 0) {
        if (d < f->win_lo_cs) s.time_cs = 4 * (f->win_lo_cs - d);
        else if (d > f->win_hi_cs) s.time_cs = 4 * (d - f->win_hi_cs);
    }
    s.complete = f->alt_ft >= 0 && d >= 0;
    s.total_cs = s.alt * 100 + s.time_cs;
    return s;
}

#define CS_LEN 16                                        /* room for any fmt_cs() result */
static char *fmt_cs(char *out, int cs) {                /* 3847 -> "38.47" */
    snprintf(out, CS_LEN, "%d.%02d", (cs / 100) % 1000000, cs % 100);
    return out;
}

static int next_number(void) {
    int n = 0;
    for (int i = 0; i < s_n; i++) if (s_fl[i].num > n) n = s_fl[i].num;
    return n + 1;
}

/* The next flight, carrying over what usually stays the same. */
static void new_draft(void) {
    flight_t *d = &s_fl[s_n];
    const flight_t *prev = s_n ? &s_fl[s_cur < s_n ? s_cur : s_n - 1] : NULL;
    memset(d, 0, sizeof *d);
    d->num = next_number();
    d->mass_g = d->wind_mph = d->rh_pct = d->t1_cs = d->t2_cs = d->alt_ft = UNKNOWN;
    d->temp_f = NO_TEMP;
    d->target_ft = s_set.target_ft;
    d->win_lo_cs = s_set.win_lo_cs;
    d->win_hi_cs = s_set.win_hi_cs;
    if (prev) {
        memcpy(d->motor, prev->motor, sizeof d->motor);
        d->mass_g = prev->mass_g;
        d->qual = prev->qual;
    }
    s_cur = s_n;
}

static bool is_draft(void) { return s_cur == s_n; }

/* ------------------------------------------------------------------ state */
typedef enum { SCR_FLIGHT, SCR_LOG, SCR_TRIM, SCR_SETUP } screen_t;
static screen_t s_scr = SCR_FLIGHT;
typedef enum { SD_LOADING, SD_SAVED, SD_EDITED, SD_NONE, SD_ERROR, SD_FULL } sd_state_t;
static sd_state_t s_sd = SD_LOADING;
static bool s_link_ok, s_sd_ok, s_read_only;
static ow_device s_dev;                          /* ~37 KB of link buffers: static, not on the stack */
static uint64_t s_save_due;                      /* 0: nothing to save */

static bool s_run;                               /* stopwatch running */
static uint64_t s_run_t0;
static char s_msg[48];                           /* a passing message on the hint line */
static uint64_t s_msg_until;
static int s_focus = -1;                         /* Flight page field under the D-pad */
static int s_log_sel;                            /* Log page: selected row (0 = newest) */
static uint64_t s_delete_armed_until;
static int s_trim_motor;                         /* Trim page: which motor */
static int s_setup_focus = -1;

static bool s_bar_changed = true, s_page_changed = true, s_watch_changed = true;

static void say(const char *m) {
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = time_us_64() + 3000000;
    s_page_changed = s_bar_changed = true;
}

static void stamp(flight_t *f);

/* A flight changed: it joins the log (if it was the next flight) and the
 * card is written a little later, never while the stopwatch runs. */
static void changed(void) {
    if (is_draft()) {
        if (s_n >= MAX_FLIGHTS) { say("the log is full (400 flights)"); return; }
        s_n++;
        stamp(&s_fl[s_cur]);                       /* when and in what weather it flew */
    }
    s_save_due = time_us_64() + SAVE_DELAY_US;
    if (s_sd == SD_SAVED) s_sd = SD_EDITED;
    s_bar_changed = s_page_changed = true;
    const flight_t *f = &s_fl[s_cur];
    score_t sc = score_of(f);
    if (sc.complete) {
        char a[CS_LEN], b[CS_LEN];
        DIAG("telemetry: flight %d score %s (altitude %d, duration %s)%s\n", f->num, fmt_cs(a, sc.total_cs),
             sc.alt, fmt_cs(b, sc.time_cs), f->cracked ? " DQ" : "");
    }
}

/* ------------------------------------------------------------ board clock */
static bool s_clock_ok;                          /* MAIN answered */
static bool s_clock_set;                         /* and the date is plausible */
static char s_date[11], s_hm[6];
static uint64_t s_clock_next;

static void clock_read(void) {
    int32_t y, mo, d, wd, h, mi, se;
    s_clock_next = time_us_64() + 20000000;
    if (!s_link_ok) return;
    s_clock_ok = ow_hardware_get_time(&s_dev, &y, &mo, &d, &wd, &h, &mi, &se) == OW_OK;
    fw2_app_recovery_task();
    s_clock_set = s_clock_ok && y >= 2026 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
    if (s_clock_set) {
        snprintf(s_date, sizeof s_date, "%04d-%02d-%02d", (int)y, (int)mo, (int)d);
        snprintf(s_hm, sizeof s_hm, "%02d:%02d", (int)h, (int)mi);
    } else {
        s_date[0] = s_hm[0] = 0;
    }
    s_bar_changed = true;
}

/* -------------------------------------------------------------- sensors */
static bool s_have_sht;

static void weather_now(flight_t *f) {
    sht40_reading_t r;
    if (s_have_sht && sht40_read(&r) && r.valid) {
        f->temp_f = (int)(r.temp_c * 9.0f / 5.0f + 32.5f);
        f->rh_pct = (int)(r.rh_pct + 0.5f);
    }
}

/* A flight's date, time and weather, taken when it joins the log: at
 * first motion, or when its first value is typed. */
static void stamp(flight_t *f) {
    clock_read();
    if (s_clock_set) {
        snprintf(f->date, sizeof f->date, "%s", s_date);
        snprintf(f->tod, sizeof f->tod, "%s", s_hm);
    }
    weather_now(f);
}

/* ------------------------------------------------------------- CSV file */
static char __uninitialized_psram("csv") s_csv[CSV_MAX];

static const char k_csv_header[] =
    "flight,date,time,type,motor,mass_g,wind_mph,temp_f,humidity_pct,timer1_s,timer2_s,duration_s,"
    "altitude_ft,eggs,target_ft,window_s,altitude_score,duration_score,score,note\n";

static int put_int(char *p, int v) { return v < 0 ? 0 : sprintf(p, "%d", v); }
static int put_cs(char *p, int v) { return v < 0 ? 0 : sprintf(p, "%d.%02d", v / 100, v % 100); }

static int put_quoted(char *p, const char *s) {           /* spreadsheet-style: "a ""b"", c" */
    if (!strpbrk(s, ",\"\n")) return sprintf(p, "%s", s);
    char *o = p;
    *o++ = '"';
    for (; *s; s++) { if (*s == '"') *o++ = '"'; *o++ = *s; }
    *o++ = '"';
    return (int)(o - p);
}

static int csv_build(void) {
    int len = (int)strlen(k_csv_header);
    memcpy(s_csv, k_csv_header, (size_t)len);
    for (int i = 0; i < s_n; i++) {
        if (len > CSV_MAX - 400) return -1;
        const flight_t *f = &s_fl[i];
        score_t sc = score_of(f);
        char *p = s_csv + len;
        p += sprintf(p, "%d,%s,%s,%s,", f->num, f->date, f->tod, f->qual ? "qualifying" : "practice");
        p += put_quoted(p, f->motor);
        *p++ = ',';
        p += put_int(p, f->mass_g); *p++ = ',';
        p += put_int(p, f->wind_mph); *p++ = ',';
        if (f->temp_f != NO_TEMP) p += sprintf(p, "%d", f->temp_f);
        *p++ = ',';
        p += put_int(p, f->rh_pct); *p++ = ',';
        p += put_cs(p, f->t1_cs); *p++ = ',';
        p += put_cs(p, f->t2_cs); *p++ = ',';
        p += put_cs(p, duration_cs(f)); *p++ = ',';
        p += put_int(p, f->alt_ft); *p++ = ',';
        p += sprintf(p, "%s,%d,", f->cracked ? "cracked" : "ok", f->target_ft);
        p += put_cs(p, f->win_lo_cs); *p++ = '-';
        p += put_cs(p, f->win_hi_cs); *p++ = ',';
        if (f->alt_ft >= 0) p += sprintf(p, "%d", sc.alt);
        *p++ = ',';
        if (duration_cs(f) >= 0) p += put_cs(p, sc.time_cs);
        *p++ = ',';
        if (sc.complete) p += f->cracked ? sprintf(p, "DQ") : put_cs(p, sc.total_cs);
        *p++ = ',';
        p += put_quoted(p, f->note);
        *p++ = '\n';
        len = (int)(p - s_csv);
    }
    return len;
}

/* Split one CSV line into fields in place. Returns the field count. */
static int csv_split(char *line, char **field, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        if (*p == '"') {
            char *o = ++p;
            field[n++] = o;
            while (*p) {
                if (*p == '"' && p[1] == '"') { *o++ = '"'; p += 2; }
                else if (*p == '"') { p++; break; }
                else *o++ = *p++;
            }
            while (*p && *p != ',') p++;
            char c = *p;
            *o = 0;
            if (!c) break;
            p++;
        } else {
            field[n++] = p;
            while (*p && *p != ',') p++;
            if (!*p) break;
            *p++ = 0;
        }
    }
    return n;
}

static int parse_int(const char *s) { return *s ? atoi(s) : UNKNOWN; }

static int parse_cs(const char *s) {                        /* "38.5" -> 3850; "" -> UNKNOWN */
    if (!*s) return UNKNOWN;
    int whole = 0, frac = 0, digits = 0;
    while (isdigit((unsigned char)*s)) whole = whole * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s) && digits < 2) { frac = frac * 10 + (*s++ - '0'); digits++; }
        if (digits == 1) frac *= 10;
        if (isdigit((unsigned char)*s) && *s >= '5') frac++;   /* a third decimal rounds */
    }
    return whole * 100 + frac;
}

static void csv_load(char *text, size_t len) {
    text[len] = 0;
    char *line = text;
    while (line && *line && s_n < MAX_FLIGHTS) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = 0; if (nl > line && nl[-1] == '\r') nl[-1] = 0; }
        char *f[20];
        int n = csv_split(line, f, 20);
        if (n >= 16 && isdigit((unsigned char)f[0][0])) {
            flight_t *d = &s_fl[s_n];
            memset(d, 0, sizeof *d);
            d->num = atoi(f[0]);
            snprintf(d->date, sizeof d->date, "%s", f[1]);
            snprintf(d->tod, sizeof d->tod, "%s", f[2]);
            d->qual = !strncmp(f[3], "qual", 4);
            snprintf(d->motor, sizeof d->motor, "%s", f[4]);
            d->mass_g = parse_int(f[5]);
            d->wind_mph = parse_int(f[6]);
            d->temp_f = *f[7] ? atoi(f[7]) : NO_TEMP;
            d->rh_pct = parse_int(f[8]);
            d->t1_cs = parse_cs(f[9]);
            d->t2_cs = parse_cs(f[10]);
            d->alt_ft = parse_int(f[12]);
            d->cracked = !strcmp(f[13], "cracked");
            d->target_ft = *f[14] ? atoi(f[14]) : s_set.target_ft;
            char *dash = strchr(f[15], '-');
            d->win_lo_cs = s_set.win_lo_cs;
            d->win_hi_cs = s_set.win_hi_cs;
            if (dash) { *dash = 0; d->win_lo_cs = parse_cs(f[15]); d->win_hi_cs = parse_cs(dash + 1); }
            if (n >= 20) snprintf(d->note, sizeof d->note, "%s", f[19]);
            s_n++;
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* Crash-safe: the log is written to flights.new, and only when that is
 * complete does it replace flights.csv. In 512-byte pieces: the OneWili
 * client WiliBSP ships sends each ow_sd_write() as one burst, and a long
 * burst can overrun the MAIN processor's receive buffer. */
static bool write_file(const char *tmp, const char *path, const char *data, int len) {
    ow_sd_file f;
    bool ok = ow_sd_open(&s_dev, &f, tmp, OW_SD_WRITE) == OW_OK;
    for (int off = 0; ok && off < len; off += 512) {
        int n = len - off < 512 ? len - off : 512;
        ok = ow_sd_write(&f, data + off, (size_t)n) == OW_OK;
        fw2_app_recovery_task();
    }
    if (ow_sd_close(&f) != OW_OK) ok = false;
    if (ok) {
        ow_sd_remove(&s_dev, path);                /* renaming onto an existing file fails on FAT */
        ok = ow_sd_rename(&s_dev, tmp, path) == OW_OK;
    }
    return ok;
}

static void save_now(void) {
    s_save_due = 0;
    s_bar_changed = true;
    if (!s_sd_ok) { s_sd = SD_NONE; return; }
    if (s_read_only) { s_sd = SD_FULL; return; }
    int len = csv_build();
    if (len < 0) { s_sd = SD_FULL; say("the log file is full"); return; }
    if (write_file(CSV_NEW, CSV_PATH, s_csv, len)) {
        s_sd = SD_SAVED;
        DIAG("telemetry: saved %d flights (%d bytes)\n", s_n, len);
    } else {
        s_sd = SD_ERROR;
        DIAG("telemetry: save failed (sdfs %d)\n", (int)ow_sd_last_error());
    }
}

static void settings_save(void) {
    if (!s_sd_ok) return;
    char buf[160], a[CS_LEN], b[CS_LEN];
    int len = snprintf(buf, sizeof buf, "target_ft=%d\nwindow_from_s=%s\nwindow_to_s=%s\nteam=%s\n", s_set.target_ft,
                       fmt_cs(a, s_set.win_lo_cs), fmt_cs(b, s_set.win_hi_cs), s_set.team);
    if (!write_file(DATA_DIR "/settings.new", SET_PATH, buf, len))
        DIAG("telemetry: saving settings failed (sdfs %d)\n", (int)ow_sd_last_error());
}

static void settings_load(void) {
    char buf[256];
    size_t got = 0;
    if (ow_sd_get_mem(&s_dev, SET_PATH, buf, sizeof buf - 1, &got) != OW_OK) return;
    buf[got] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "target_ft") && atoi(eq) > 0) s_set.target_ft = atoi(eq);
        else if (!strcmp(line, "window_from_s") && parse_cs(eq) > 0) s_set.win_lo_cs = parse_cs(eq);
        else if (!strcmp(line, "window_to_s") && parse_cs(eq) > 0) s_set.win_hi_cs = parse_cs(eq);
        else if (!strcmp(line, "team")) snprintf(s_set.team, sizeof s_set.team, "%s", eq);
    }
}

static void sd_connect(void) {
    if (fw2_app_recovery_open_onewili(&s_dev) != OW_OK || fw2_app_recovery_wrap_sd() != OW_OK) {
        DIAG("telemetry: no link to the MAIN processor\n");
        s_sd = SD_NONE;
        return;
    }
    s_link_ok = true;
    ow_sd_mkdir(&s_dev, APP_DIR);                  /* fail harmlessly when they exist */
    ow_sd_mkdir(&s_dev, DATA_DIR);
    fw2_app_recovery_task();
    bool is_dir = false;
    uint32_t size = 0;
    const char *path = CSV_PATH;
    ow_status st = ow_sd_stat(&s_dev, path, &is_dir, &size);
    if (st == OW_ERR_FAILED && ow_sd_stat(&s_dev, CSV_NEW, &is_dir, &size) == OW_OK) {
        path = CSV_NEW;                            /* power went between a save's two steps */
        st = OW_OK;
    }
    if (st != OW_OK) {
        s_sd_ok = st == OW_ERR_FAILED;             /* MAIN answered "not found": the card is there */
        s_sd = s_sd_ok ? SD_SAVED : SD_NONE;
        if (s_sd_ok) settings_load();
        DIAG("telemetry: no flight log yet (ow %d)\n", (int)st);
        return;
    }
    s_sd_ok = true;
    settings_load();
    size_t got = 0;
    if (size > CSV_MAX - 1) {
        s_read_only = true;                        /* never cut a log short: show what fits, write nothing */
        DIAG("telemetry: %s is %lu bytes: read only\n", CSV_PATH, (unsigned long)size);
    }
    if (ow_sd_get_mem(&s_dev, path, s_csv, CSV_MAX - 1, &got) != OW_OK && !s_read_only) {
        DIAG("telemetry: reading the log failed (sdfs %d)\n", (int)ow_sd_last_error());
        s_sd = SD_ERROR;
        return;
    }
    csv_load(s_csv, got);
    s_sd = s_read_only ? SD_FULL : SD_SAVED;
    DIAG("telemetry: loaded %d flights from %s\n", s_n, path);
    if (path == (const char *)CSV_NEW) s_save_due = time_us_64();
}

/* ------------------------------------------------------ typing a value */
typedef enum {
    E_NONE, E_T1, E_T2, E_ALT, E_MOTOR, E_MASS, E_WIND, E_NOTE,
    E_TARGET, E_WIN_LO, E_WIN_HI, E_TEAM, E_CLOCK
} entry_t;

static entry_t s_entry = E_NONE;
static char s_ebuf[41];
static int s_emax;
static char s_eerr[40];

enum { K_SHIFT = 1, K_BKSP, K_MODE, K_CANCEL, K_DONE };
typedef struct { int16_t x, y, w; char code; } kb_key_t;
#define MAX_KEYS 40
static kb_key_t s_keys[2][MAX_KEYS];
static int s_nkeys[2];
static int s_layer;                   /* 0 letters, 1 numbers & symbols */
static bool s_caps;
static int s_down_key = -1;

static void add_key(int layer, int x, int row, int w, char code) {
    kb_key_t *k = &s_keys[layer][s_nkeys[layer]++];
    k->x = (int16_t)x; k->y = (int16_t)(KB_Y + row * KEY_H); k->w = (int16_t)w; k->code = code;
}

static void add_row(int layer, int row, int x, const char *chars) {
    for (; *chars; chars++, x += 48) add_key(layer, x, row, 48, *chars);
}

static void build_keys(void) {
    static const char *const rows[2][3] = {
        { "qwertyuiop", "asdfghjkl", "zxcvbnm" },
        { "1234567890", "-/:;()$&@", "?!'\"=+*" },
    };
    for (int l = 0; l < 2; l++) {
        add_row(l, 0, 0, rows[l][0]);
        add_row(l, 1, 24, rows[l][1]);
        add_key(l, 0, 2, 72, l == 0 ? K_SHIFT : '#');
        add_row(l, 2, 72, rows[l][2]);
        add_key(l, 408, 2, 72, K_BKSP);
        add_key(l, 0, 3, 72, K_MODE);
        add_key(l, 72, 3, 192, ' ');
        add_key(l, 264, 3, 48, '.');
        add_key(l, 312, 3, 72, K_CANCEL);
        add_key(l, 384, 3, 96, K_DONE);
    }
}

static const char *entry_label(entry_t e) {
    switch (e) {
    case E_T1:     return "Timer 1 (s)";
    case E_T2:     return "Timer 2 (s): the other stopwatch";
    case E_ALT:    return "Altitude (ft) from the altimeter";
    case E_MOTOR:  return "Motor";
    case E_MASS:   return "Liftoff mass (g), motor included";
    case E_WIND:   return "Wind (mph)";
    case E_NOTE:   return "Note";
    case E_TARGET: return "Target altitude (ft)";
    case E_WIN_LO: return "Duration window from (s)";
    case E_WIN_HI: return "Duration window to (s)";
    case E_TEAM:   return "Team name";
    case E_CLOCK:  return "Date and time: 2027-05-16 09:41";
    default:       return "";
    }
}

static bool entry_numeric(entry_t e) { return e != E_MOTOR && e != E_NOTE && e != E_TEAM; }

static void entry_open(entry_t e) {
    if (s_run) { say("stop the timer first"); return; }
    flight_t *f = &s_fl[s_cur];
    char a[CS_LEN];
    s_ebuf[0] = 0;
    s_emax = 20;
    switch (e) {
    case E_T1:     if (f->t1_cs >= 0) fmt_cs(s_ebuf, f->t1_cs); s_emax = 7; break;
    case E_T2:     if (f->t2_cs >= 0) fmt_cs(s_ebuf, f->t2_cs); s_emax = 7; break;
    case E_ALT:    if (f->alt_ft >= 0) sprintf(s_ebuf, "%d", f->alt_ft); s_emax = 5; break;
    case E_MOTOR:  strcpy(s_ebuf, f->motor); s_emax = 11; break;
    case E_MASS:   if (f->mass_g >= 0) sprintf(s_ebuf, "%d", f->mass_g); s_emax = 4; break;
    case E_WIND:   if (f->wind_mph >= 0) sprintf(s_ebuf, "%d", f->wind_mph); s_emax = 2; break;
    case E_NOTE:   strcpy(s_ebuf, f->note); s_emax = 40; break;
    case E_TARGET: sprintf(s_ebuf, "%d", s_set.target_ft); s_emax = 5; break;
    case E_WIN_LO: strcpy(s_ebuf, fmt_cs(a, s_set.win_lo_cs)); s_emax = 6; break;
    case E_WIN_HI: strcpy(s_ebuf, fmt_cs(a, s_set.win_hi_cs)); s_emax = 6; break;
    case E_TEAM:   strcpy(s_ebuf, s_set.team); s_emax = 24; break;
    case E_CLOCK:
        if (s_clock_set) snprintf(s_ebuf, sizeof s_ebuf, "%s %s", s_date, s_hm);
        s_emax = 16;
        break;
    default: return;
    }
    s_entry = e;
    s_layer = entry_numeric(e) ? 1 : 0;
    s_caps = e == E_MOTOR;                         /* motors are written F42-8T */
    s_eerr[0] = 0;
    s_page_changed = s_bar_changed = true;
}

static void entry_close(void) {
    s_entry = E_NONE;
    s_down_key = -1;
    s_page_changed = s_bar_changed = true;
}

static bool whole_number(const char *s, int lo, int hi, int *out) {
    if (!*s) return false;
    for (const char *p = s; *p; p++) if (!isdigit((unsigned char)*p)) return false;
    long v = atol(s);
    if (v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static bool seconds(const char *s, int *out) {
    int dots = 0;
    if (!*s) return false;
    for (const char *p = s; *p; p++) {
        if (*p == '.') dots++;
        else if (!isdigit((unsigned char)*p)) return false;
    }
    if (dots > 1 || !strcmp(s, ".")) return false;
    *out = parse_cs(s);
    return *out <= 99999;
}

/* "2027-05-16 09:41": digits and the separators exactly. (Not sscanf:
 * newlib's scanf brings malloc into the app.) */
static const char *digits(const char *p, int n, int *out) {
    *out = 0;
    for (int i = 0; i < n; i++, p++) {
        if (!isdigit((unsigned char)*p)) return NULL;
        *out = *out * 10 + (*p - '0');
    }
    return p;
}

static bool parse_datetime(const char *s, int *y, int *mo, int *d, int *h, int *mi) {
    if (!(s = digits(s, 4, y)) || *s++ != '-' || !(s = digits(s, 2, mo)) || *s++ != '-' ||
        !(s = digits(s, 2, d)) || *s++ != ' ' || !(s = digits(s, 2, h)) || *s++ != ':' || !(s = digits(s, 2, mi)))
        return false;
    return *s == 0;
}

/* Check and store what was typed. Returns false (and says why) to keep the
 * keyboard open. An empty value clears an optional flight field. */
static bool entry_apply(void) {
    flight_t *f = &s_fl[s_cur];
    const char *v = s_ebuf;
    int n = 0;
    char a[CS_LEN];
    bool empty = !*v;
    switch (s_entry) {
    case E_T1:
    case E_T2: {
        int *t = s_entry == E_T1 ? &f->t1_cs : &f->t2_cs;
        if (empty) *t = UNKNOWN;
        else if (!seconds(v, &n)) { strcpy(s_eerr, "seconds, like 38.47"); return false; }
        else *t = n;
        DIAG("telemetry: flight %d timer %d = %s\n", f->num, s_entry == E_T1 ? 1 : 2, empty ? "-" : fmt_cs(a, n));
        break;
    }
    case E_ALT:
        if (empty) f->alt_ft = UNKNOWN;
        else if (!whole_number(v, 0, 99999, &n)) { strcpy(s_eerr, "feet, a whole number"); return false; }
        else f->alt_ft = n;
        DIAG("telemetry: flight %d altitude = %s\n", f->num, v);
        break;
    case E_MASS:
        if (empty) f->mass_g = UNKNOWN;
        else if (!whole_number(v, 1, 5000, &n)) { strcpy(s_eerr, "grams, a whole number"); return false; }
        else f->mass_g = n;
        DIAG("telemetry: flight %d mass = %s\n", f->num, v);
        break;
    case E_WIND:
        if (empty) f->wind_mph = UNKNOWN;
        else if (!whole_number(v, 0, 99, &n)) { strcpy(s_eerr, "mph, 0 to 99"); return false; }
        else f->wind_mph = n;
        break;
    case E_MOTOR:
        snprintf(f->motor, sizeof f->motor, "%.*s", (int)sizeof f->motor - 1, v);
        DIAG("telemetry: flight %d motor = %s\n", f->num, v);
        break;
    case E_NOTE:
        snprintf(f->note, sizeof f->note, "%.*s", (int)sizeof f->note - 1, v);
        break;
    case E_TARGET:
        if (!whole_number(v, 1, 99999, &n)) { strcpy(s_eerr, "feet, a whole number"); return false; }
        s_set.target_ft = n;
        break;
    case E_WIN_LO:
    case E_WIN_HI: {
        if (!seconds(v, &n) || n == 0) { strcpy(s_eerr, "seconds, like 37.00"); return false; }
        int lo = s_entry == E_WIN_LO ? n : s_set.win_lo_cs, hi = s_entry == E_WIN_HI ? n : s_set.win_hi_cs;
        if (lo > hi) { strcpy(s_eerr, "the window would end before it starts"); return false; }
        s_set.win_lo_cs = lo;
        s_set.win_hi_cs = hi;
        break;
    }
    case E_TEAM:
        snprintf(s_set.team, sizeof s_set.team, "%.*s", (int)sizeof s_set.team - 1, v);
        break;
    case E_CLOCK: {
        int y, mo, d, h, mi;
        if (!parse_datetime(v, &y, &mo, &d, &h, &mi) || y < 2026 || y > 2099 ||
            mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) {
            strcpy(s_eerr, "like 2027-05-16 09:41");
            return false;
        }
        if (ow_hardware_set_time(&s_dev, y, mo, d, h, mi, 0) != OW_OK) {
            strcpy(s_eerr, "the board didn't take it");
            return false;
        }
        DIAG("telemetry: clock set to %s\n", v);
        clock_read();
        return true;
    }
    default: return true;
    }
    if (s_entry >= E_TARGET) {                     /* settings: the next flight uses them */
        if (is_draft()) {
            f->target_ft = s_set.target_ft;
            f->win_lo_cs = s_set.win_lo_cs;
            f->win_hi_cs = s_set.win_hi_cs;
        }
        settings_save();
        s_page_changed = true;
    } else {
        changed();
    }
    return true;
}

static char key_char(char code) {
    if (s_layer == 0 && s_caps && code >= 'a' && code <= 'z') return (char)(code - 'a' + 'A');
    return code;
}

static void key_label(const kb_key_t *k, char *out) {
    switch (k->code) {
    case K_SHIFT:  strcpy(out, s_caps ? "ABC" : "abc"); break;
    case K_BKSP:   strcpy(out, "<del"); break;
    case K_MODE:   strcpy(out, s_layer ? "abc" : "123"); break;
    case K_CANCEL: strcpy(out, "back"); break;
    case K_DONE:   strcpy(out, "done"); break;
    case ' ':      strcpy(out, "space"); break;
    default:       out[0] = key_char(k->code); out[1] = 0; break;
    }
}

static void entry_key(int i) {
    char code = s_keys[s_layer][i].code;
    size_t len = strlen(s_ebuf);
    switch (code) {
    case K_SHIFT:  s_caps = !s_caps; break;
    case K_MODE:   s_layer ^= 1; break;
    case K_BKSP:   if (len) s_ebuf[len - 1] = 0; break;
    case K_CANCEL: entry_close(); return;
    case K_DONE:   if (entry_apply()) entry_close(); break;
    default:
        if ((int)len < s_emax) { s_ebuf[len] = key_char(code); s_ebuf[len + 1] = 0; }
        break;
    }
    s_eerr[0] = 0;
    s_page_changed = true;
}

static void draw_entry(void) {
    fb_rect(0, BAR_H, W, H - BAR_H, C_BG);
    fb_text(8, 30, 2, C_DIM, C_BG, entry_label(s_entry), -1);
    fb_rect(8, 56, W - 16, 52, C_PANEL);
    int x = fb_text(20, 66, 4, C_TEXT, C_PANEL, s_ebuf, -1);
    fb_rect(x + 2, 64, 4, 36, C_RUN);                   /* cursor */
    if (s_eerr[0]) fb_text(8, 118, 2, C_WARN, C_BG, s_eerr, -1);
    else if (s_entry <= E_NOTE) fb_text(8, 118, 2, C_DIM, C_BG, "empty = not known", -1);
    for (int i = 0; i < s_nkeys[s_layer]; i++) {
        const kb_key_t *k = &s_keys[s_layer][i];
        bool fn = k->code < ' ' || k->code == ' ';
        uint16_t bg = i == s_down_key ? C_KEY_DOWN : (fn ? C_KEY_FN : C_KEY);
        if (k->code == K_DONE) bg = i == s_down_key ? C_KEY_DOWN : 0x0400;
        fb_rect(k->x + 1, k->y + 1, k->w - 2, KEY_H - 2, bg);
        char label[8];
        key_label(k, label);
        int n = (int)strlen(label), scale = n == 1 ? 3 : 2;
        fb_text(k->x + (k->w - n * 6 * scale) / 2, k->y + (KEY_H - 8 * scale) / 2 + 1, scale, C_TEXT, bg, label, n);
    }
    mark(BAR_H, H);
}

static int key_at(int x, int y) {
    for (int i = 0; i < s_nkeys[s_layer]; i++) {
        const kb_key_t *k = &s_keys[s_layer][i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + KEY_H) return i;
    }
    return -1;
}

/* --------------------------------------------------------- common parts */
static void draw_bar(void) {
    fb_rect(0, 0, W, BAR_H, C_BAR);
    char t[40];
    const flight_t *f = &s_fl[s_cur];
    switch (s_scr) {
    case SCR_FLIGHT: snprintf(t, sizeof t, "FLIGHT %d  %s", f->num, f->qual ? "qualifying" : "practice"); break;
    case SCR_LOG:    snprintf(t, sizeof t, "LOG  %d flight%s", s_n, s_n == 1 ? "" : "s"); break;
    case SCR_TRIM:   snprintf(t, sizeof t, "TRIM"); break;
    case SCR_SETUP:  snprintf(t, sizeof t, "SETUP"); break;
    }
    if (s_scr != SCR_FLIGHT && s_entry == E_NONE && time_us_64() < s_msg_until)
        fb_text(4, 3, 2, C_WARN, C_BAR, s_msg, 22);
    else
        fb_text(4, 3, 2, s_scr == SCR_FLIGHT && f->qual ? C_RUN : C_TEXT, C_BAR, t, -1);
    static const char *const words[] = { "loading", "saved", "edited", "no SD card", "SD error", "log full" };
    static const uint16_t cols[] = { C_DIM, C_OK, C_WARN, C_WARN, C_BAD, C_WARN };
    const char *w = words[s_sd];
    int x = W - 4 - (int)strlen(w) * 12;
    fb_text(x, 3, 2, cols[s_sd], C_BAR, w, -1);
    if (s_hm[0]) fb_text(x - 6 * 12 - 4, 3, 2, C_TEXT, C_BAR, s_hm, -1);
    mark(0, BAR_H);
}

static void draw_softkeys(void) {
    static const char *const names[4] = { "Flight", "Log", "Trim", "Setup" };
    const char *red = s_scr == SCR_FLIGHT ? "New" : s_scr == SCR_LOG ? "Delete" : "";
    if (s_scr == SCR_LOG && time_us_64() < s_delete_armed_until) red = "Sure?";
    fb_rect(0, SOFT_Y, W, H - SOFT_Y, C_BG);
    for (int i = 0; i < 5; i++) {
        const char *label = i < 4 ? names[i] : red;
        bool here = i < 4 && (int)s_scr == i;
        int x = i * SOFT_W;
        if (!*label) continue;
        fb_rect(x + 3, SOFT_Y + 2, SOFT_W - 6, H - SOFT_Y - 4, here ? k_soft_col[i] : C_PANEL);
        fb_rect(x + 3, SOFT_Y + 2, SOFT_W - 6, 4, k_soft_col[i]);
        fb_text_centre(x, SOFT_W, SOFT_Y + 10, 2, here ? k_soft_txt[i] : C_TEXT, here ? k_soft_col[i] : C_PANEL, label);
    }
    mark(SOFT_Y, H);
}

/* ------------------------------------------------------------ Flight page */
enum { F_T1, F_T2, F_ALT, F_EGGS, F_MOTOR, F_MASS, F_TYPE, F_WIND, F_NOTE, F_COUNT };
static const char *const k_field_name[F_COUNT] = {
    "Timer 1", "Timer 2", "Altitude", "Eggs", "Motor", "Mass", "Flight", "Wind", "Note" };

static void field_rect(int i, int *x, int *y, int *w) {
    *x = i == F_NOTE ? 0 : (i % 2) * 240;
    *y = GRID_Y + (i == F_NOTE ? 4 : i / 2) * ROW_H;
    *w = i == F_NOTE ? W : 240;
}

static void field_value(const flight_t *f, int i, char *out, uint16_t *col) {
    *col = C_TEXT;
    out[0] = 0;
    switch (i) {
    case F_T1:    if (f->t1_cs >= 0) { fmt_cs(out, f->t1_cs); strcat(out, " s"); } break;
    case F_T2:    if (f->t2_cs >= 0) { fmt_cs(out, f->t2_cs); strcat(out, " s"); } break;
    case F_ALT:   if (f->alt_ft >= 0) sprintf(out, "%d ft", f->alt_ft); break;
    case F_EGGS:  strcpy(out, f->cracked ? "CRACKED" : "ok"); *col = f->cracked ? C_BAD : C_OK; break;
    case F_MOTOR: strcpy(out, f->motor); break;
    case F_MASS:  if (f->mass_g >= 0) sprintf(out, "%d g", f->mass_g); if (f->mass_g > MASS_LIMIT_G) *col = C_WARN; break;
    case F_TYPE:  strcpy(out, f->qual ? "qualifying" : "practice"); *col = f->qual ? C_RUN : C_TEXT; break;
    case F_WIND:  if (f->wind_mph >= 0) sprintf(out, "%d mph", f->wind_mph); break;
    case F_NOTE:  strcpy(out, f->note); break;
    }
    if (!out[0]) { strcpy(out, "-"); *col = C_DIM; }
}

static void draw_watch(void) {
    const flight_t *f = &s_fl[s_cur];
    int cs;
    uint16_t col = C_TEXT;
    if (s_run && is_draft() == false && s_cur == s_n - 1) {
        cs = (int)((time_us_64() - s_run_t0) / 10000);
        col = C_RUN;
    } else {
        cs = f->t1_cs >= 0 ? f->t1_cs : 0;
        if (f->t1_cs < 0) col = C_DIM;
    }
    if (cs > 99999) cs = 99999;
    int digits[5], nd = 0;
    for (int v = cs; nd < 3 || v; v /= 10) digits[nd++] = v % 10;
    int gap = 10, dot = 16;
    int total = nd * DIG_W + (nd - 1) * gap + dot + gap;
    int x = (W - total) / 2;
    fb_rect(0, WATCH_Y, W, DIG_H, C_BG);
    for (int i = nd - 1; i >= 0; i--) {
        seg_digit(x, WATCH_Y, digits[i], col);
        x += DIG_W + gap;
        if (i == 2) {
            fb_rect(x - gap / 2 + 1, WATCH_Y + DIG_H - DIG_T, DIG_T, DIG_T, col);
            x += dot;
        }
    }
    fb_text(x + 4, WATCH_Y + DIG_H - 16, 2, C_DIM, C_BG, "s", 1);
    mark(WATCH_Y, WATCH_Y + DIG_H);
}

static void draw_flight(void) {
    const flight_t *f = &s_fl[s_cur];
    fb_rect(0, BAR_H, W, SOFT_Y - BAR_H, C_BG);
    draw_watch();

    /* the hint line */
    char hint[48], a[CS_LEN];
    uint16_t hcol = C_DIM;
    int d = duration_cs(f);
    if (time_us_64() < s_msg_until) { snprintf(hint, sizeof hint, "%s", s_msg); hcol = C_WARN; }
    else if (s_run) { strcpy(hint, "OK at touchdown or lost from view"); hcol = C_RUN; }
    else if (f->t1_cs < 0 && f->t2_cs < 0) strcpy(hint, "OK at first motion");
    else if (f->t1_cs >= 0 && f->t2_cs >= 0) snprintf(hint, sizeof hint, "Official %s s, average of 2", fmt_cs(a, d));
    else snprintf(hint, sizeof hint, "Official %s s, one timer", fmt_cs(a, d));
    fb_text_centre(0, W, HINT_Y, 2, hcol, C_BG, hint);

    /* the fields */
    for (int i = 0; i < F_COUNT; i++) {
        int x, y, w;
        field_rect(i, &x, &y, &w);
        uint16_t bg = i == s_focus ? C_FOCUS : C_BG;
        fb_rect(x + 1, y + 1, w - 2, ROW_H - 2, bg);
        char v[48];
        uint16_t col;
        field_value(f, i, v, &col);
        fb_text(x + 6, y + 5, 2, C_DIM, bg, k_field_name[i], -1);
        int max = (w - 116) / 12;
        fb_text(x + 110, y + 5, 2, col, bg, v, max);
    }

    /* the score */
    fb_rect(0, SCORE_Y, W, SOFT_Y - SCORE_Y - 2, C_PANEL);
    score_t sc = score_of(f);
    if (sc.complete) {
        char tot[CS_LEN], t[CS_LEN], line[48];
        fmt_cs(tot, sc.total_cs);
        fb_text(10, SCORE_Y + 12, 2, C_DIM, C_PANEL, "Score", -1);
        fb_text(84, SCORE_Y + 6, 4, f->cracked ? C_DIM : C_TEXT, C_PANEL, tot, -1);
        if (f->cracked) {
            fb_text(290, SCORE_Y + 12, 2, C_BAD, C_PANEL, "DQ: egg cracked", -1);
        } else {
            snprintf(line, sizeof line, "alt %d + time %s", sc.alt, fmt_cs(t, sc.time_cs));
            fb_text(W - 10 - (int)strlen(line) * 12, SCORE_Y + 12, 2, C_DIM, C_PANEL, line, -1);
        }
    } else {
        const char *need = f->alt_ft < 0 && d < 0 ? "Score: needs a time and the altitude"
                         : f->alt_ft < 0 ? "Score: needs the altitude" : "Score: needs a time";
        fb_text(10, SCORE_Y + 12, 2, C_DIM, C_PANEL, need, -1);
    }
    draw_softkeys();
    mark(BAR_H, H);
}

static void timer_start(void) {
    flight_t *f = &s_fl[s_cur];
    if (f->t1_cs >= 0) { say("already timed: RED New for the next"); return; }
    uint64_t t0 = time_us_64();                    /* first: the reading must not wait for the clock */
    if (!is_draft() && s_cur != s_n - 1) { say("open the newest flight to time it"); return; }
    if (is_draft() && s_n >= MAX_FLIGHTS) { say("the log is full (400 flights)"); return; }
    s_run = true;
    s_run_t0 = t0;
    s_focus = -1;
    if (is_draft()) changed();
    s_save_due = 0;                                /* no card writes while timing */
    DIAG("telemetry: flight %d timer started\n", f->num);
    s_page_changed = true;
}

static void timer_stop(void) {
    uint64_t t1 = time_us_64();
    flight_t *f = &s_fl[s_n - 1];
    s_run = false;
    f->t1_cs = (int)((t1 - s_run_t0 + 5000) / 10000);
    char a[CS_LEN];
    DIAG("telemetry: flight %d stopped at %s s\n", f->num, fmt_cs(a, f->t1_cs));
    changed();
}

static void flight_new(void) {
    if (s_run) { say("stop the timer first"); return; }
    if (is_draft() && !has_data(&s_fl[s_cur])) { say("this one is new already"); return; }
    if (s_n >= MAX_FLIGHTS) { say("the log is full (400 flights)"); return; }
    new_draft();
    s_focus = -1;
    DIAG("telemetry: new flight %d\n", s_fl[s_cur].num);
    s_page_changed = s_bar_changed = s_watch_changed = true;
}

static void field_activate(int i) {
    flight_t *f = &s_fl[s_cur];
    switch (i) {
    case F_T1:    entry_open(E_T1); break;
    case F_T2:    entry_open(E_T2); break;
    case F_ALT:   entry_open(E_ALT); break;
    case F_MOTOR: entry_open(E_MOTOR); break;
    case F_MASS:  entry_open(E_MASS); break;
    case F_WIND:  entry_open(E_WIND); break;
    case F_NOTE:  entry_open(E_NOTE); break;
    case F_EGGS:
        f->cracked = !f->cracked;
        DIAG("telemetry: flight %d eggs %s\n", f->num, f->cracked ? "cracked" : "ok");
        changed();
        break;
    case F_TYPE:
        f->qual = !f->qual;
        DIAG("telemetry: flight %d is %s\n", f->num, f->qual ? "qualifying" : "practice");
        changed();
        break;
    }
}

static void flight_move(uartkbd_btn_t b) {
    if (s_focus < 0) { s_focus = 0; s_page_changed = true; return; }
    int row = s_focus == F_NOTE ? 4 : s_focus / 2, col = s_focus == F_NOTE ? 0 : s_focus % 2;
    if (b == UARTKBD_BTN_NAV_UP) row = row ? row - 1 : 0;
    if (b == UARTKBD_BTN_NAV_DOWN) row = row < 4 ? row + 1 : 4;
    if (b == UARTKBD_BTN_NAV_LEFT) col = 0;
    if (b == UARTKBD_BTN_NAV_RIGHT) col = 1;
    s_focus = row == 4 ? F_NOTE : row * 2 + col;
    s_page_changed = true;
}

static int flight_field_at(int x, int y) {
    if (y < GRID_Y || y >= GRID_Y + 5 * ROW_H) return -1;
    int row = (y - GRID_Y) / ROW_H;
    return row == 4 ? F_NOTE : row * 2 + (x >= 240);
}

/* --------------------------------------------------------------- Log page */
#define LOG_Y 44
#define LOG_ROW 18
#define LOG_ROWS 11

static int log_index(int row) { return s_n - 1 - row; }      /* newest first */

/* Best two qualifying flights (complete, not disqualified). */
typedef struct { int flown, scored, a, b; } qual_t;

static qual_t qual_summary(void) {
    qual_t q = { 0, 0, -1, -1 };
    for (int i = 0; i < s_n; i++) {
        const flight_t *f = &s_fl[i];
        if (!f->qual) continue;
        q.flown++;
        score_t sc = score_of(f);
        if (!sc.complete || f->cracked) continue;
        q.scored++;
        if (q.a < 0 || sc.total_cs < score_of(&s_fl[q.a]).total_cs) { q.b = q.a; q.a = i; }
        else if (q.b < 0 || sc.total_cs < score_of(&s_fl[q.b]).total_cs) q.b = i;
    }
    return q;
}

static void draw_log(void) {
    fb_rect(0, BAR_H, W, SOFT_Y - BAR_H, C_BG);
    fb_text(0, 26, 2, C_DIM, C_BG, "  # T Motor    Mass  Alt   Time  Score", -1);
    if (!s_n) fb_text_centre(0, W, 120, 2, C_DIM, C_BG, "No flights yet");
    if (s_log_sel >= s_n) s_log_sel = s_n ? s_n - 1 : 0;
    int top = s_log_sel >= LOG_ROWS ? s_log_sel - LOG_ROWS + 1 : 0;
    for (int r = 0; r < LOG_ROWS && top + r < s_n; r++) {
        const flight_t *f = &s_fl[log_index(top + r)];
        score_t sc = score_of(f);
        char mass[CS_LEN] = "", alt[CS_LEN] = "", tm[CS_LEN] = "", sco[CS_LEN] = "", line[64];
        if (f->mass_g >= 0) sprintf(mass, "%d", f->mass_g);
        if (f->alt_ft >= 0) sprintf(alt, "%d", f->alt_ft);
        if (duration_cs(f) >= 0) fmt_cs(tm, duration_cs(f));
        if (sc.complete) { if (f->cracked) strcpy(sco, "DQ"); else fmt_cs(sco, sc.total_cs); }
        snprintf(line, sizeof line, "%3d %c %-8.8s %4s %4s %6s %6s", f->num, f->qual ? 'Q' : 'P', f->motor,
                 mass, alt, tm, sco);
        bool sel = top + r == s_log_sel;
        uint16_t bg = sel ? C_FOCUS : C_BG;
        int y = LOG_Y + r * LOG_ROW;
        fb_rect(0, y - 1, W, LOG_ROW, bg);
        fb_text(0, y, 2, f->cracked ? C_BAD : f->qual ? C_RUN : C_TEXT, bg, line, -1);
    }
    /* the selected flight, and the qualifying total */
    char line[96], a[CS_LEN], b[CS_LEN], c[CS_LEN];
    if (s_n) {
        const flight_t *f = &s_fl[log_index(s_log_sel)];
        int n = snprintf(line, sizeof line, "%s %s", f->date[0] ? f->date + 5 : "no date", f->tod);
        if (f->temp_f != NO_TEMP) n += snprintf(line + n, sizeof line - (size_t)n, "  %dF %d%%", f->temp_f, f->rh_pct);
        if (f->wind_mph >= 0) n += snprintf(line + n, sizeof line - (size_t)n, "  wind %d", f->wind_mph);
        if (f->note[0] && n < 40) snprintf(line + n, sizeof line - (size_t)n, "  %.*s", 38 - n, f->note);
        fb_text(0, 248, 2, C_DIM, C_BG, line, 40);
    }
    qual_t q = qual_summary();
    uint16_t col = C_DIM;
    if (q.scored >= 2) {
        int sa = score_of(&s_fl[q.a]).total_cs, sb = score_of(&s_fl[q.b]).total_cs;
        snprintf(line, sizeof line, "Best 2 qual: %s + %s = %s", fmt_cs(a, sa), fmt_cs(b, sb), fmt_cs(c, sa + sb));
        col = C_OK;
    } else {
        snprintf(line, sizeof line, "Qualifying: %d scored, need 2", q.scored);
    }
    if (q.flown > 3) { snprintf(line, sizeof line, "%d qualifying flights: ARC allows 3", q.flown); col = C_WARN; }
    fb_text(0, 268, 2, col, C_BG, line, 40);
    draw_softkeys();
    mark(BAR_H, H);
}

static void log_summary_diag(void) {
    qual_t q = qual_summary();
    if (q.scored < 2) return;
    char a[CS_LEN], b[CS_LEN], c[CS_LEN];
    int sa = score_of(&s_fl[q.a]).total_cs, sb = score_of(&s_fl[q.b]).total_cs;
    DIAG("telemetry: qualifying best two: flights %d and %d, %s + %s = %s\n", s_fl[q.a].num, s_fl[q.b].num,
         fmt_cs(a, sa), fmt_cs(b, sb), fmt_cs(c, sa + sb));
}

static void log_open(void) {
    if (!s_n) return;
    s_cur = log_index(s_log_sel);
    s_scr = SCR_FLIGHT;
    s_focus = -1;
    s_page_changed = s_bar_changed = true;
}

static void log_delete(void) {
    if (!s_n || s_run) return;
    uint64_t now = time_us_64();
    if (now >= s_delete_armed_until) {                 /* first press: ask */
        s_delete_armed_until = now + 3000000;
        s_page_changed = true;
        return;
    }
    s_delete_armed_until = 0;
    int i = log_index(s_log_sel);
    DIAG("telemetry: deleted flight %d\n", s_fl[i].num);
    bool was_cur = s_cur == i;
    memmove(&s_fl[i], &s_fl[i + 1], sizeof s_fl[0] * (size_t)(s_n - i));   /* the draft moves down too */
    s_n--;
    if (s_cur > i) s_cur--;
    if (was_cur) s_cur = s_n;                          /* back to the next flight */
    s_save_due = time_us_64();
    s_sd = SD_EDITED;
    s_page_changed = s_bar_changed = true;
}

/* -------------------------------------------------------------- Trim page */
#define MAX_MOTORS 24
static char s_motors[MAX_MOTORS][12];
static int s_nmotors;

static void collect_motors(void) {
    s_nmotors = 0;
    for (int i = s_n - 1; i >= 0; i--) {                /* most recent first */
        const char *m = s_fl[i].motor;
        if (!m[0]) continue;
        bool seen = false;
        for (int k = 0; k < s_nmotors; k++) if (!strcasecmp(s_motors[k], m)) seen = true;
        if (!seen && s_nmotors < MAX_MOTORS) strcpy(s_motors[s_nmotors++], m);
    }
}

/* Least squares y = a + b x. Returns false without two distinct x. */
static bool fit(const float *x, const float *y, int n, float *a, float *b) {
    if (n < 2) return false;
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++) { mx += x[i]; my += y[i]; }
    mx /= (float)n; my /= (float)n;
    float sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) { sxx += (x[i] - mx) * (x[i] - mx); sxy += (x[i] - mx) * (y[i] - my); }
    if (sxx < 0.5f) return false;
    *b = sxy / sxx;
    *a = my - *b * mx;
    return true;
}

static int iround(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }

typedef struct {
    int n_alt, n_dur;
    bool have_alt, have_dur, sensible;
    int mass_for_target, last_mass;
    float ft_per_g, s_per_g, dur_a, dur_b;
} trim_t;

static trim_t trim_compute(const char *motor) {
    static float mx[MAX_FLIGHTS], ay[MAX_FLIGHTS], dx[MAX_FLIGHTS], dy[MAX_FLIGHTS];
    trim_t t;
    memset(&t, 0, sizeof t);
    t.last_mass = -1;
    for (int i = 0; i < s_n; i++) {
        const flight_t *f = &s_fl[i];
        if (strcasecmp(f->motor, motor) || f->mass_g <= 0) continue;
        t.last_mass = f->mass_g;
        if (f->alt_ft >= 0) { mx[t.n_alt] = (float)f->mass_g; ay[t.n_alt++] = (float)f->alt_ft; }
        if (duration_cs(f) >= 0) { dx[t.n_dur] = (float)f->mass_g; dy[t.n_dur++] = (float)duration_cs(f) / 100.0f; }
    }
    float a, b;
    t.have_alt = fit(mx, ay, t.n_alt, &a, &b);
    if (t.have_alt) {
        t.ft_per_g = b;
        t.sensible = b < 0;                            /* heavier must fly lower */
        if (t.sensible) t.mass_for_target = iround(((float)s_set.target_ft - a) / b);
        if (t.mass_for_target <= 0) t.sensible = false;
    }
    t.have_dur = fit(dx, dy, t.n_dur, &t.dur_a, &t.dur_b);
    t.s_per_g = t.dur_b;
    return t;
}

static void draw_trim(void) {
    fb_rect(0, BAR_H, W, SOFT_Y - BAR_H, C_BG);
    collect_motors();
    if (!s_nmotors) {
        fb_text_centre(0, W, 110, 2, C_DIM, C_BG, "Give your flights a motor and a mass,");
        fb_text_centre(0, W, 130, 2, C_DIM, C_BG, "then see what mass hits the target.");
        draw_softkeys();
        mark(BAR_H, H);
        return;
    }
    if (s_trim_motor >= s_nmotors) s_trim_motor = 0;
    const char *motor = s_motors[s_trim_motor];
    char line[64], a[CS_LEN];
    snprintf(line, sizeof line, "< %.11s >", motor);
    fb_text_centre(0, W, 28, 3, C_TEXT, C_BG, line);

    /* this motor's flights, newest first */
    fb_text(40, 58, 2, C_DIM, C_BG, "  #  Mass   Alt    Time", -1);
    int rows = 0;
    for (int i = s_n - 1; i >= 0 && rows < 5; i--) {
        const flight_t *f = &s_fl[i];
        if (strcasecmp(f->motor, motor)) continue;
        char m[CS_LEN] = "-", al[CS_LEN] = "-", tm[CS_LEN] = "-";
        if (f->mass_g > 0) sprintf(m, "%d", f->mass_g);
        if (f->alt_ft >= 0) sprintf(al, "%d", f->alt_ft);
        if (duration_cs(f) >= 0) fmt_cs(tm, duration_cs(f));
        snprintf(line, sizeof line, "%3d %5s %5s %7s", f->num, m, al, tm);
        fb_text(40, 76 + rows * 18, 2, C_TEXT, C_BG, line, -1);
        rows++;
    }

    trim_t t = trim_compute(motor);
    int y = 172;
    if (!t.have_alt) {
        fb_text(8, y, 2, C_DIM, C_BG, "Needs flights at two different", -1);
        fb_text(8, y + 20, 2, C_DIM, C_BG, "masses, with their altitudes.", -1);
    } else if (!t.sensible) {
        fb_text(8, y, 2, C_WARN, C_BG, "Altitude isn't falling with mass", -1);
        fb_text(8, y + 20, 2, C_WARN, C_BG, "yet: fly more, or check the log.", -1);
    } else {
        snprintf(line, sizeof line, "10 g more = %d ft", iround(t.ft_per_g * 10));
        fb_text(8, y, 2, C_DIM, C_BG, line, -1);
        int diff = t.last_mass >= 0 ? t.mass_for_target - t.last_mass : 0;
        snprintf(line, sizeof line, "For %d ft: %d g (%s%d g)", s_set.target_ft, t.mass_for_target,
                 diff >= 0 ? "+" : "", diff);
        fb_text(8, y + 20, 3, t.mass_for_target > MASS_LIMIT_G ? C_WARN : C_OK, C_BG, line, -1);
        if (t.mass_for_target > MASS_LIMIT_G) {
            fb_text(8, y + 48, 2, C_WARN, C_BG, "Over the 650 g liftoff limit", -1);
        } else if (t.have_dur) {
            int d = iround((t.dur_a + t.dur_b * (float)t.mass_for_target) * 100.0f);
            bool in = d >= s_set.win_lo_cs && d <= s_set.win_hi_cs;
            snprintf(line, sizeof line, "Time then: about %s s%s", fmt_cs(a, d), in ? "" : " (outside)");
            fb_text(8, y + 48, 2, in ? C_DIM : C_WARN, C_BG, line, -1);
        }
    }
    fb_text(8, 266, 1, C_DIM, C_BG, "Fitted to your flights with this motor. Weather moves every", -1);
    fb_text(8, 276, 1, C_DIM, C_BG, "flight, so treat it as the next thing to try.", -1);
    draw_softkeys();
    mark(BAR_H, H);
}

static void trim_diag(void) {
    collect_motors();
    if (!s_nmotors) return;
    if (s_trim_motor >= s_nmotors) s_trim_motor = 0;
    trim_t t = trim_compute(s_motors[s_trim_motor]);
    if (t.have_alt && t.sensible)
        DIAG("telemetry: trim %s: %d flights, %d g for %d ft (%d ft per 10 g)\n", s_motors[s_trim_motor], t.n_alt,
             t.mass_for_target, s_set.target_ft, iround(t.ft_per_g * 10));
    else
        DIAG("telemetry: trim %s: %d flights, no estimate yet\n", s_motors[s_trim_motor], t.n_alt);
}

/* ------------------------------------------------------------- Setup page */
enum { S_TARGET, S_WIN_LO, S_WIN_HI, S_TEAM, S_CLOCK, S_COUNT };
#define SET_Y 34
#define SET_ROW 34

static void draw_setup(void) {
    fb_rect(0, BAR_H, W, SOFT_Y - BAR_H, C_BG);
    static const char *const names[S_COUNT] = { "Target altitude", "Window from", "Window to", "Team", "Clock" };
    for (int i = 0; i < S_COUNT; i++) {
        char v[48], a[CS_LEN];
        uint16_t col = C_TEXT;
        switch (i) {
        case S_TARGET: sprintf(v, "%d ft", s_set.target_ft); break;
        case S_WIN_LO: sprintf(v, "%s s", fmt_cs(a, s_set.win_lo_cs)); break;
        case S_WIN_HI: sprintf(v, "%s s", fmt_cs(a, s_set.win_hi_cs)); break;
        case S_TEAM:   snprintf(v, sizeof v, "%s", s_set.team[0] ? s_set.team : "-"); if (!s_set.team[0]) col = C_DIM; break;
        case S_CLOCK:
            if (s_clock_set) snprintf(v, sizeof v, "%s %s", s_date, s_hm);
            else { strcpy(v, s_clock_ok ? "not set: tap to set" : "not available"); col = C_WARN; }
            break;
        }
        int y = SET_Y + i * SET_ROW;
        uint16_t bg = i == s_setup_focus ? C_FOCUS : C_BG;
        fb_rect(0, y, W, SET_ROW - 4, bg);
        fb_text(8, y + 7, 2, C_DIM, bg, names[i], -1);
        fb_text(206, y + 7, 2, col, bg, v, 22);
    }
    fb_text(8, 214, 2, C_DIM, C_BG, "ARC 2027: 800 ft, 37-40 s. New", -1);
    fb_text(8, 232, 2, C_DIM, C_BG, "targets apply from the next flight.", -1);
    char line[48];
    snprintf(line, sizeof line, "%d flights in %s", s_n, CSV_PATH);
    fb_text(8, 258, 1, C_DIM, C_BG, line, -1);
    draw_softkeys();
    mark(BAR_H, H);
}

static void setup_activate(int i) {
    static const entry_t e[S_COUNT] = { E_TARGET, E_WIN_LO, E_WIN_HI, E_TEAM, E_CLOCK };
    if (i == S_CLOCK && !s_clock_ok) { say("the board clock isn't answering"); return; }
    if (i >= 0 && i < S_COUNT) entry_open(e[i]);
}

/* ------------------------------------------------------------------ input */
static void go(screen_t s) {
    if (s_entry != E_NONE) return;
    s_scr = s;
    s_focus = s_setup_focus = -1;
    s_delete_armed_until = 0;
    if (s == SCR_LOG) { s_log_sel = s_cur < s_n ? s_n - 1 - s_cur : 0; log_summary_diag(); }
    if (s == SCR_TRIM) {
        collect_motors();
        for (int k = 0; k < s_nmotors; k++) if (!strcasecmp(s_motors[k], s_fl[s_cur].motor)) s_trim_motor = k;
        trim_diag();
    }
    s_page_changed = s_bar_changed = true;
}

static void soft_key(int i) {
    if (i < 4) { go((screen_t)i); return; }
    if (s_scr == SCR_FLIGHT) flight_new();
    else if (s_scr == SCR_LOG) log_delete();
}

typedef struct { bool held, armed; uint64_t since, next_us; } held_t;
static held_t s_held[UARTKBD_BTN_COUNT];

static void nav(uartkbd_btn_t b) {
    switch (s_scr) {
    case SCR_FLIGHT:
        if (b == UARTKBD_BTN_NAV_CENTER) { if (s_focus >= 0) field_activate(s_focus); }
        else flight_move(b);
        break;
    case SCR_LOG:
        if (b == UARTKBD_BTN_NAV_UP && s_log_sel > 0) s_log_sel--;
        if (b == UARTKBD_BTN_NAV_DOWN && s_log_sel < s_n - 1) s_log_sel++;
        if (b == UARTKBD_BTN_NAV_CENTER) log_open();
        s_page_changed = true;
        break;
    case SCR_TRIM:
        if (s_nmotors && b == UARTKBD_BTN_NAV_LEFT) s_trim_motor = (s_trim_motor + s_nmotors - 1) % s_nmotors;
        if (s_nmotors && b == UARTKBD_BTN_NAV_RIGHT) s_trim_motor = (s_trim_motor + 1) % s_nmotors;
        if (b == UARTKBD_BTN_NAV_LEFT || b == UARTKBD_BTN_NAV_RIGHT) trim_diag();
        s_page_changed = true;
        break;
    case SCR_SETUP:
        if (b == UARTKBD_BTN_NAV_UP) s_setup_focus = s_setup_focus > 0 ? s_setup_focus - 1 : 0;
        if (b == UARTKBD_BTN_NAV_DOWN) s_setup_focus = s_setup_focus < S_COUNT - 1 ? s_setup_focus + 1 : S_COUNT - 1;
        if (b == UARTKBD_BTN_NAV_CENTER && s_setup_focus >= 0) setup_activate(s_setup_focus);
        s_page_changed = true;
        break;
    }
}

static void handle_buttons(uint64_t now) {
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        held_t *h = &s_held[ev.btn];
        h->held = ev.pressed;
        if (ev.pressed) {
            h->since = now;
            h->next_us = now + 450000;
            h->armed = s_entry == E_NONE && s_scr == SCR_FLIGHT;   /* a CANCEL that closes the keyboard never clears */
        }
        if (!ev.pressed) continue;
        /* OK always works the stopwatch while it runs: touchdown can come
         * while you're on another page. */
        if (ev.btn == UARTKBD_BTN_OK && s_run) { timer_stop(); continue; }
        if (s_entry != E_NONE) {
            if (ev.btn == UARTKBD_BTN_OK) { if (entry_apply()) entry_close(); else s_page_changed = true; }
            else if (ev.btn == UARTKBD_BTN_CANCEL) entry_close();
            continue;
        }
        if (ev.btn <= UARTKBD_BTN_RED) { soft_key((int)ev.btn); continue; }
        switch (ev.btn) {
        case UARTKBD_BTN_OK:
            if (s_scr == SCR_FLIGHT) timer_start();
            else if (s_scr == SCR_LOG) log_open();
            else if (s_scr == SCR_SETUP && s_setup_focus >= 0) setup_activate(s_setup_focus);
            break;
        case UARTKBD_BTN_NAV_UP: case UARTKBD_BTN_NAV_DOWN: case UARTKBD_BTN_NAV_LEFT:
        case UARTKBD_BTN_NAV_RIGHT: case UARTKBD_BTN_NAV_CENTER:
            nav(ev.btn);
            break;
        default: break;
        }
    }
    /* D-pad repeats in lists */
    for (int b = UARTKBD_BTN_NAV_UP; b <= UARTKBD_BTN_NAV_DOWN; b++) {
        held_t *h = &s_held[b];
        if (h->held && now >= h->next_us && s_entry == E_NONE && s_scr == SCR_LOG) {
            h->next_us = now + 90000;
            nav((uartkbd_btn_t)b);
        }
    }
    /* CANCEL held 1 s clears the stopwatch (a false start, or a stop too early) */
    held_t *c = &s_held[UARTKBD_BTN_CANCEL];
    if (c->held && c->armed && s_entry == E_NONE && s_scr == SCR_FLIGHT && now - c->since >= 1000000) {
        c->held = c->armed = false;
        flight_t *f = s_run ? &s_fl[s_n - 1] : &s_fl[s_cur];
        if (s_run || f->t1_cs >= 0) {
            s_run = false;
            f->t1_cs = UNKNOWN;
            DIAG("telemetry: flight %d timer cleared\n", f->num);
            say("timer cleared");
            s_cur = (int)(f - s_fl);
            if (s_cur == s_n - 1 && !has_data(f)) {    /* a false start: back to being the next flight */
                f->date[0] = f->tod[0] = 0;
                f->temp_f = NO_TEMP;
                f->rh_pct = UNKNOWN;
                s_n--;
                s_save_due = time_us_64();
                s_page_changed = s_bar_changed = true;
            } else {
                changed();
            }
        }
    }
}

static void handle_touch(void) {
    static bool was_down;
    uint16_t x, y;
    bool down = ft6336_poll(&x, &y);
    if (down && !was_down) {
        if (s_entry != E_NONE) {
            s_down_key = y >= KB_Y ? key_at(x, y) : -1;
            if (s_down_key >= 0) entry_key(s_down_key);
        } else if (y >= SOFT_Y) {
            soft_key(x / SOFT_W);
        } else if (s_scr == SCR_FLIGHT) {
            int i = flight_field_at(x, y);
            if (i >= 0) { s_focus = i; field_activate(i); s_page_changed = true; }
        } else if (s_scr == SCR_LOG && y >= LOG_Y - 1 && y < LOG_Y - 1 + LOG_ROWS * LOG_ROW) {
            int top = s_log_sel >= LOG_ROWS ? s_log_sel - LOG_ROWS + 1 : 0;
            int row = top + (y - (LOG_Y - 1)) / LOG_ROW;
            if (row < s_n) {
                if (row == s_log_sel) log_open();
                else { s_log_sel = row; s_page_changed = true; }
            }
        } else if (s_scr == SCR_TRIM && y < 56) {
            nav(x < W / 2 ? UARTKBD_BTN_NAV_LEFT : UARTKBD_BTN_NAV_RIGHT);
        } else if (s_scr == SCR_SETUP && y >= SET_Y && y < SET_Y + S_COUNT * SET_ROW) {
            s_setup_focus = (y - SET_Y) / SET_ROW;
            setup_activate(s_setup_focus);
            s_page_changed = true;
        }
    } else if (!down && was_down && s_down_key >= 0) {
        s_down_key = -1;
        if (s_entry != E_NONE) s_page_changed = true;
    }
    was_down = down;
}

/* After the About screen (PAGE held 5 s) the whole screen is redrawn. */
static void redraw_all(void) { s_page_changed = s_bar_changed = s_watch_changed = true; }

static void draw_page(void) {
    if (s_entry != E_NONE) { draw_entry(); return; }
    switch (s_scr) {
    case SCR_FLIGHT: draw_flight(); break;
    case SCR_LOG:    draw_log(); break;
    case SCR_TRIM:   draw_trim(); break;
    case SCR_SETUP:  draw_setup(); break;
    }
}

int main(void) {
    board_init();                    /* before the OneWili link: uart_init reads clk_peri */
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd_restore(redraw_all);
    ft6336_init();
    agentio_init();                  /* WiliBSP's fw.py press / touch / screenshot */
    build_keys();
    s_have_sht = sht40_init();

    s_fl[0].num = 1;                 /* something sane to draw before the card is read */
    s_fl[0].t1_cs = s_fl[0].t2_cs = s_fl[0].alt_ft = s_fl[0].mass_g = s_fl[0].wind_mph = UNKNOWN;
    fb_rect(0, 0, W, H, C_BG);
    draw_bar();
    draw_flight();
    st7796_fill_screen(be(C_BG));
    board_backlight_set(1);
    flush();

    sd_connect();
    s_cur = s_n;
    new_draft();
    clock_read();
    DIAG("telemetry: ready (%d flights%s)\n", s_n, s_clock_set ? "" : ", clock not set");
    redraw_all();

    uint64_t last_watch = 0;
    for (;;) {
        fw2_app_recovery_task();
        uint64_t now = time_us_64();
        handle_buttons(now);
        handle_touch();

        if (s_run && now - last_watch >= 30000) { s_watch_changed = true; last_watch = now; }
        if (s_msg_until && now >= s_msg_until) { s_msg_until = 0; s_page_changed = s_bar_changed = true; }
        if (s_delete_armed_until && now >= s_delete_armed_until) { s_delete_armed_until = 0; s_page_changed = true; }
        /* card and clock traffic only while the stopwatch is idle: a button
         * press is timed when the loop sees it */
        if (!s_run && s_save_due && now >= s_save_due) save_now();
        if (!s_run && s_entry == E_NONE && now >= s_clock_next) {
            char before[6];
            strcpy(before, s_hm);
            clock_read();
            if (strcmp(before, s_hm)) s_bar_changed = true;
        }

        if (!st7796_flush_busy()) {               /* draw only between flushes: no tearing */
            if (s_bar_changed)  { draw_bar();  s_bar_changed = false; }
            if (s_page_changed) { draw_page(); s_page_changed = false; s_watch_changed = false; }
            if (s_watch_changed) {
                if (s_scr == SCR_FLIGHT && s_entry == E_NONE) draw_watch();
                s_watch_changed = false;
            }
            flush();
        }
        agentio_task();
        fw2_app_recovery_sleep_ms(2);
    }
}
