/*
 * MegaMarkdown - a small overlay-app viewer for the UV-K1/K5 v3 v6.3 firmware.
 *
 * By MegaSaturnv
 * SPDX-License-Identifier: Apache-2.0
 *
 * Target: v6.3.0 ("Labs" / "Fusion_"), ABI 1, API level 3.
 *
 * The markdown files in ../markdown files are converted to 18-column display
 * lines and compressed at build time (see gen_assets.py).  The reader can
 * reflow those lines to the radio's 32-column 3x5 font.  This app only has to
 * open a file, decompress the next few lines, and draw them; it does not carry
 * a markdown parser or an inflate implementation in the 4 KiB overlay.
 *
 * Keys:
 *   file list:  UP/DOWN (or LEFT/RIGHT)  previous/next file
 *               MENU                     open the selected file
 *               STAR / F                 first / last file
 *               EXIT                     quit to the Apps menu
 *   reader:     UP/DOWN (or LEFT/RIGHT)  scroll one line
 *               STAR / F                 jump to the top / bottom
 *               0                        toggle normal/tiny font
 *               MENU                     back to the file list
 *               EXIT                     quit to the Apps menu
 *
 * The app never transmits and never writes flash.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "app_api.h"
#include "app_overlay.h"
#include "markdown_assets.h"

_Static_assert(MD_FILE_COUNT > 0u, "at least one document is required");
_Static_assert(MD_ASSET_SIZE <= APP_ASSET_MAX,
               "documents exceed the app's asset area");

#define TICK_MS 20u

#define MD_WINDOW_SIZE  (1u << MD_LZ_WINDOW_BITS)
#define MD_WINDOW_MASK  (MD_WINDOW_SIZE - 1u)
#define MD_OFFSET_MASK  ((1u << MD_LZ_OFFSET_BITS) - 1u)

enum {
    MODE_LIST = 0,
    MODE_READ,
};

/* One decompressor instance, kept in .bss so its ring buffer does not sit on
 * the radio's stack.  The window must be a power of two. */
typedef struct {
    uint16_t src_pos;
    uint16_t src_left;
    uint8_t  src_buf[32];
    uint8_t  src_buf_pos;
    uint8_t  src_buf_left;
    uint8_t  flags;
    uint8_t  flags_left;
    uint8_t  match_left;
    uint16_t match_dist;
    uint16_t out_left;
    uint16_t produced;
    uint16_t win_pos;
    bool     error;
    uint8_t  window[MD_WINDOW_SIZE];
} lz_t;

static lz_t lz;

/* Tiny-font reflow state.  The asset stores 18-column lines with markers at
 * the start of wrapped continuations; this joins them into one logical line
 * and re-wraps it to MD_TINY_COLS. */
typedef struct {
    char     logical[MD_WRAP_MAX + 1u];
    char     pending[MD_LINE_MAX + 2u];
    uint8_t  logical_len;
    uint8_t  pos;
    uint8_t  pending_len;
    bool     have_pending;
} tiny_t;

static tiny_t tiny;

typedef struct {
    uint8_t  mode;
    uint8_t  selected;
    uint8_t  previous_key;
    uint8_t  saver;
    uint8_t  redraw;
    uint8_t  running;
    uint8_t  wait_release;
    uint8_t  tiny;
    uint16_t top;
    uint16_t total_rows;
    uint16_t repeat_ms;
    const app_api_t *api;
} globals_t;

static globals_t g;
#define A (g.api)

/* -------------------------------------------------------------------------- */
/*  Toolchain support                                                         */
/* -------------------------------------------------------------------------- */

/* The Cortex-M0+ has no divide instruction.  Forward the compiler's run-time
 * helpers to the resident ones instead of linking libgcc's larger versions
 * (API level 2).  The aliases return the quotient in r0, as the AEABI expects. */
uint64_t __aeabi_idivmod(int32_t n, int32_t d)
{
    return A->idivmod(n, d);
}
uint64_t __aeabi_idiv(int32_t n, int32_t d) __attribute__((alias("__aeabi_idivmod")));

uint64_t __aeabi_uidivmod(uint32_t n, uint32_t d)
{
    return A->uidivmod(n, d);
}
uint64_t __aeabi_uidiv(uint32_t n, uint32_t d) __attribute__((alias("__aeabi_uidivmod")));

/* -------------------------------------------------------------------------- */
/*  LZSS decoder                                                              */
/* -------------------------------------------------------------------------- */

static uint8_t lz_comp_byte(void)
{
    if (!lz.src_buf_left) {
        if (!lz.src_left) {
            lz.error = true;
            return 0;
        }
        const uint16_t want = lz.src_left > sizeof lz.src_buf
                            ? (uint16_t)sizeof lz.src_buf : lz.src_left;
        const uint16_t got = A->asset_read(lz.src_pos, lz.src_buf, want);
        if (got != want) {
            lz.error = true;
            return 0;
        }
        lz.src_pos = (uint16_t)(lz.src_pos + want);
        lz.src_left = (uint16_t)(lz.src_left - want);
        lz.src_buf_pos = 0;
        lz.src_buf_left = (uint8_t)want;
    }
    lz.src_buf_left--;
    return lz.src_buf[lz.src_buf_pos++];
}

static void lz_emit(uint8_t c)
{
    lz.window[lz.win_pos] = c;
    lz.win_pos = (uint16_t)((lz.win_pos + 1u) & MD_WINDOW_MASK);
    lz.out_left--;
    lz.produced++;
}

static int lz_next(void)
{
    while (!lz.match_left) {
        if (!lz.out_left)
            return -1;

        if (!lz.flags_left) {
            lz.flags = lz_comp_byte();
            if (lz.error)
                return -1;
            lz.flags_left = 8;
        }

        if (lz.flags & 1u) {
            lz.flags >>= 1;
            lz.flags_left--;
            const uint8_t c = lz_comp_byte();
            if (lz.error)
                return -1;
            lz_emit(c);
            return c;
        }

        lz.flags >>= 1;
        lz.flags_left--;
        uint16_t token = lz_comp_byte();
        token |= (uint16_t)lz_comp_byte() << 8;
        if (lz.error)
            return -1;

        lz.match_dist = (uint16_t)((token & MD_OFFSET_MASK) + 1u);
        lz.match_left = (uint8_t)((token >> MD_LZ_OFFSET_BITS) + MD_LZ_MIN_MATCH);
        if (lz.match_dist > lz.produced) {
            lz.error = true;
            return -1;
        }
        if (lz.match_left > lz.out_left)
            lz.match_left = (uint8_t)lz.out_left;
    }

    const uint16_t index =
        (uint16_t)(lz.win_pos - lz.match_dist) & (uint16_t)MD_WINDOW_MASK;
    const uint8_t c = lz.window[index];
    lz.match_left--;
    lz_emit(c);
    return c;
}

static void lz_start(const md_file_t *file)
{
    lz.src_pos = file->off;
    lz.src_left = file->clen;
    lz.src_buf_pos = 0;
    lz.src_buf_left = 0;
    lz.flags = 0;
    lz.flags_left = 0;
    lz.match_left = 0;
    lz.out_left = file->ulen;
    lz.produced = 0;
    lz.win_pos = 0;
    lz.error = false;

    tiny.logical_len = 0;
    tiny.pos = 0;
    tiny.pending_len = 0;
    tiny.have_pending = false;
}

/* Return one line, or -1 at end of document / on a corrupt stream.  An empty
 * line returns 0, which is different from end of document. */
static int lz_read_line(char *dst, uint8_t size)
{
    uint8_t n = 0;
    int c;

    while ((c = lz_next()) >= 0) {
        if (c == '\n') {
            dst[n] = '\0';
            return (int)n;
        }
        if ((uint8_t)(n + 1u) < size)
            dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return n ? (int)n : -1;
}

/* Normal (6x8) rows are stored directly; strip the continuation marker. */
static int normal_next_row(char *dst, uint8_t size)
{
    const int n = lz_read_line(dst, size);
    if (n <= 0)
        return n;

    if (dst[0] == MD_MARK_RULE) {
        for (uint8_t i = 0; i < MD_LINE_MAX; i++)
            dst[i] = '-';
        dst[MD_LINE_MAX] = '\0';
        return MD_LINE_MAX;
    }
    if (dst[0] == MD_MARK_SPACE || dst[0] == MD_MARK_HARD) {
        for (uint8_t i = 1; i <= (uint8_t)n; i++)
            dst[i - 1u] = dst[i];
        return n - 1;
    }
    return n;
}

static int tiny_raw(char *dst, uint8_t size)
{
    if (tiny.have_pending) {
        tiny.have_pending = false;
        uint8_t n = tiny.pending_len;
        if (n >= size)
            n = (uint8_t)(size - 1u);
        for (uint8_t i = 0; i <= n; i++)
            dst[i] = tiny.pending[i];
        return (int)n;
    }
    return lz_read_line(dst, size);
}

/* Return one logical line with its continuations joined, or 0 for a blank
 * line, or -1 at end of document. */
static int tiny_read_logical(void)
{
    char line[MD_LINE_MAX + 2u];
    int n = tiny_raw(line, (uint8_t)sizeof line);

    if (n < 0)
        return -1;
    if (n == 0)
        return 0;

    if (line[0] == MD_MARK_RULE) {
        tiny.logical_len = MD_TINY_COLS;
        for (uint8_t i = 0; i < MD_TINY_COLS; i++)
            tiny.logical[i] = '-';
        tiny.logical[tiny.logical_len] = '\0';
        return tiny.logical_len;
    }

    uint8_t start = 0;
    if (line[0] == MD_MARK_SPACE || line[0] == MD_MARK_HARD)
        start = 1;

    tiny.logical_len = 0;
    for (uint8_t i = start; line[i] && tiny.logical_len < MD_WRAP_MAX; i++)
        tiny.logical[tiny.logical_len++] = line[i];

    for (;;) {
        n = tiny_raw(line, (uint8_t)sizeof line);
        if (n < 0)
            break;
        if (n == 0) {
            tiny.pending[0] = '\0';
            tiny.pending_len = 0;
            tiny.have_pending = true;
            break;
        }
        if (line[0] == MD_MARK_SPACE || line[0] == MD_MARK_HARD) {
            if (line[0] == MD_MARK_SPACE && tiny.logical_len < MD_WRAP_MAX)
                tiny.logical[tiny.logical_len++] = ' ';
            for (uint8_t i = 1; line[i] && tiny.logical_len < MD_WRAP_MAX; i++)
                tiny.logical[tiny.logical_len++] = line[i];
            continue;
        }
        tiny.pending_len = (uint8_t)n;
        for (uint8_t i = 0; i <= (uint8_t)n; i++)
            tiny.pending[i] = line[i];
        tiny.have_pending = true;
        break;
    }

    tiny.logical[tiny.logical_len] = '\0';
    return tiny.logical_len ? (int)tiny.logical_len : 0;
}

/* Greedy word wrap to MD_TINY_COLS, mirroring the build-time renderer. */
static int tiny_next_row(char *dst, uint8_t size)
{
    for (;;) {
        if (tiny.pos >= tiny.logical_len) {
            const int n = tiny_read_logical();
            if (n < 0)
                return -1;
            if (n == 0) {
                dst[0] = '\0';
                return 0;
            }
            tiny.pos = 0;
        }

        uint8_t start = tiny.pos;
        while (start < tiny.logical_len && tiny.logical[start] == ' ')
            start++;
        tiny.pos = start;

        const uint8_t avail = (uint8_t)(tiny.logical_len - start);
        uint8_t take = avail > MD_TINY_COLS ? MD_TINY_COLS : avail;

        if (take < avail) {
            uint8_t cut = take;
            while (cut > 0 && tiny.logical[start + cut] != ' ')
                cut--;
            if (cut > 0)
                take = cut;
        }

        uint8_t n = 0;
        while (n < take && (uint8_t)(n + 1u) < size)
            dst[n] = tiny.logical[start + n], n++;
        dst[n] = '\0';

        tiny.pos = (uint8_t)(start + take);
        while (tiny.pos < tiny.logical_len && tiny.logical[tiny.pos] == ' ')
            tiny.pos++;
        return (int)n;
    }
}

static int reader_next_row(char *dst, uint8_t size)
{
    if (g.tiny)
        return tiny_next_row(dst, size);
    return normal_next_row(dst, size);
}

/* -------------------------------------------------------------------------- */
/*  Drawing helpers                                                           */
/* -------------------------------------------------------------------------- */

static void begin_frame(void)
{
    A->display_clear();
    A->status_clear();
}

static void put_status(const char *s)
{
    A->print_tiny(s, 0, 0, true, true);
}

static void end_frame(void)
{
    A->draw_battery();
    A->blit_status();
    A->blit_full();
}

static char *put_text(char *out, const char *s)
{
    while (*s)
        *out++ = *s++;
    return out;
}

static char *put_u16(char *out, uint16_t value)
{
    char tmp[5];
    uint8_t n = 0;

    do {
        tmp[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < sizeof tmp);

    while (n)
        *out++ = tmp[--n];
    return out;
}

static char *put_name(char *out, const char *name, uint8_t max)
{
    uint8_t n = 0;
    while (*name && n < max) {
        *out++ = *name++;
        n++;
    }
    return out;
}

static uint8_t row_count(void)
{
    return g.tiny ? MD_TINY_ROWS : MD_ROWS;
}

static uint16_t count_rows(void)
{
    char line[MD_TINY_COLS + 1u];
    uint16_t rows = 0;

    lz_start(&md_files[g.selected]);
    while (reader_next_row(line, (uint8_t)sizeof line) >= 0)
        rows++;
    return rows;
}

static void update_total_rows(void)
{
    g.total_rows = g.tiny ? count_rows() : md_files[g.selected].lines;
}

static uint16_t max_top(void)
{
    const uint8_t rows = row_count();
    return g.total_rows > rows ? (uint16_t)(g.total_rows - rows) : 0;
}

/* -------------------------------------------------------------------------- */
/*  Pages                                                                     */
/* -------------------------------------------------------------------------- */

static void status_list(void)
{
    char s[32];
    char *o = s;

    o = put_text(o, "MegaMarkdown - files ");
    o = put_u16(o, (uint16_t)MD_FILE_COUNT);
    *o = '\0';
    put_status(s);
}

static void status_reader(void)
{
    char s[32];
    char *o = s;

    o = put_name(o, md_files[g.selected].name, 10u);
    o = put_text(o, " L");
    o = put_u16(o, (uint16_t)(g.top + 1u));
    *o++ = '/';
    o = put_u16(o, g.total_rows);
    o = put_text(o, g.tiny ? " S" : " N");
    *o = '\0';
    put_status(s);
}

static void draw_list(void)
{
    char line[MD_LINE_MAX + 1u];
    uint8_t first = 0;

    begin_frame();

    if (g.selected >= MD_ROWS)
        first = (uint8_t)(g.selected - (MD_ROWS - 1u));

    for (uint8_t row = 0; row < MD_ROWS; row++) {
        const uint8_t index = (uint8_t)(first + row);
        if (index >= MD_FILE_COUNT)
            break;

        uint8_t n = 0;
        line[n++] = index == g.selected ? '>' : ' ';
        const char *name = md_files[index].name;
        while (*name && n < MD_LINE_MAX)
            line[n++] = *name++;
        line[n] = '\0';
        A->print_normal(line, 0, 0, row);
    }

    status_list();
    end_frame();
}

static void draw_reader(void)
{
    char line[MD_TINY_COLS + 1u];
    uint16_t skip = g.top;

    begin_frame();
    lz_start(&md_files[g.selected]);

    while (skip) {
        if (reader_next_row(line, (uint8_t)sizeof line) < 0)
            break;
        skip--;
    }

    for (uint8_t row = 0; row < row_count(); row++) {
        const int n = reader_next_row(line, (uint8_t)sizeof line);
        if (n < 0)
            break;
        if (!n)
            continue;
        if (g.tiny)
            A->print_tiny(line, 0, (uint8_t)(row * 6u), false, true);
        else
            A->print_normal(line, 0, 0, row);
    }

    if (lz.error)
        put_status("DOCUMENT ERROR");
    else
        status_reader();
    end_frame();
}

static void draw(void)
{
    if (g.mode == MODE_READ)
        draw_reader();
    else
        draw_list();
}

/* -------------------------------------------------------------------------- */
/*  Input                                                                     */
/* -------------------------------------------------------------------------- */

static void list_move(int8_t direction)
{
    int index = (int)g.selected + direction;
    if (index < 0)
        index = (int)MD_FILE_COUNT - 1;
    else if (index >= (int)MD_FILE_COUNT)
        index = 0;
    g.selected = (uint8_t)index;
    g.redraw = 1;
}

static void reader_move(int16_t delta)
{
    int32_t top = (int32_t)g.top + delta;
    if (top < 0)
        top = 0;
    const int32_t limit = (int32_t)max_top();
    if (top > limit)
        top = limit;
    g.top = (uint16_t)top;
    g.redraw = 1;
}

static void key_action(uint8_t key)
{
    if (key == APP_KEY_EXIT) {
        g.running = 0;
        return;
    }

    if (g.mode == MODE_LIST) {
        if (key == APP_KEY_MENU) {
            g.mode = MODE_READ;
            g.top = 0;
            update_total_rows();
            g.redraw = 1;
            return;
        }
        if (key == APP_KEY_STAR) {
            g.selected = 0;
            g.redraw = 1;
            return;
        }
        if (key == APP_KEY_F) {
            g.selected = (uint8_t)(MD_FILE_COUNT - 1u);
            g.redraw = 1;
            return;
        }
        /* nav_dir() returns +1 for UP (respecting the SET_NAV setting).  In a
         * list, +1 selects the previous file. */
        const int8_t direction = A->nav_dir(key);
        if (direction)
            list_move(-direction);
        return;
    }

    if (key == APP_KEY_0) {
        g.tiny = !g.tiny;
        g.top = 0;
        update_total_rows();
        g.redraw = 1;
        return;
    }
    if (key == APP_KEY_MENU) {
        g.mode = MODE_LIST;
        g.redraw = 1;
        return;
    }
    if (key == APP_KEY_STAR) {
        g.top = 0;
        g.redraw = 1;
        return;
    }
    if (key == APP_KEY_F) {
        g.top = max_top();
        g.redraw = 1;
        return;
    }
    /* nav_dir() +1 is UP: move to the earlier line. */
    const int8_t direction = A->nav_dir(key);
    if (direction)
        reader_move(-direction);
}

static void poll_keys(void)
{
    uint8_t key = A->get_key();

    if (key == APP_KEY_SAVER) {
        g.saver = 1;
        return;
    }
    if (key == APP_KEY_WAKE) {
        g.saver = 0;
        g.redraw = 1;
        key = APP_KEY_INVALID;
    }
    if (key == APP_KEY_INVALID) {
        g.previous_key = key;
        g.repeat_ms = 0;
        g.wait_release = 0;
        return;
    }

    /* The key that launched the app from the Apps menu is still held when
     * app_main() starts: wait for it to be released before acting. */
    if (g.wait_release) {
        g.previous_key = key;
        return;
    }

    /* Font toggle is a single-shot action, never a repeating key. */
    if (key == APP_KEY_0) {
        if (key != g.previous_key) {
            A->backlight_on();
            key_action(key);
        }
        g.previous_key = key;
        g.repeat_ms = 0;
        return;
    }

    if (key != g.previous_key) {
        A->backlight_on();
        key_action(key);
        g.repeat_ms = 300u;
    } else if (g.repeat_ms > TICK_MS) {
        g.repeat_ms = (uint16_t)(g.repeat_ms - TICK_MS);
    } else {
        key_action(key);
        g.repeat_ms = 80u;
    }
    g.previous_key = key;
}

/* -------------------------------------------------------------------------- */
/*  Entry point                                                               */
/* -------------------------------------------------------------------------- */

/* Must be first in the blob: app.ld puts .text.entry at offset 0.  Never name a
 * function "entry", or GCC may place it in .text.entry ahead of app_main. */
__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    A = api;

    g.mode = MODE_LIST;
    g.selected = 0;
    g.top = 0;
    g.total_rows = 0;
    g.tiny = 0;
    g.saver = 0;
    g.redraw = 1;
    g.running = 1;

    /* Ignore a key still held from the Apps menu. */
    g.previous_key = A->get_key();
    g.wait_release = g.previous_key != APP_KEY_INVALID;

    A->led(false);
    A->backlight_on();

    while (g.running) {
        poll_keys();
        if (!g.running)
            break;

        if (!g.saver && g.redraw) {
            draw();
            g.redraw = 0;
        }

        A->battery_sample();
        A->backlight_update();
        A->delay_ms(TICK_MS);
    }

    A->led(false);
}
