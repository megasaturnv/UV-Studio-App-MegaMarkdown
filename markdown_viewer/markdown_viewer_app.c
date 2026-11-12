/*
 * MegaMarkdown - a small overlay-app viewer for the UV-K1/K5 v3 v6.3 firmware.
 *
 * By MegaSaturnv
 * SPDX-License-Identifier: Apache-2.0
 *
 * Target: v6.3.0 ("Labs" / "Fusion_"), ABI 1, API level 3.
 *
 * The markdown files in ../markdown files are converted to plain paragraphs
 * and compressed at build time (see gen_assets.py).  The reader wraps each
 * paragraph to 18 columns (6x8 font) or 32 columns (3x5 font) at runtime.
 * This app only has to open a file, decompress the next paragraph, and draw
 * the rows it needs; it does not carry a markdown parser or an inflate
 * implementation in the 4 KiB overlay.
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

enum {
    MODE_LIST = 0,
    MODE_READ,
};

/* One decompressor instance, kept in .bss so its input buffer does not sit on
 * the radio's stack. */
typedef struct {
    uint16_t src_pos;
    uint16_t src_left;
    uint8_t  src_buf[32];
    uint8_t  src_buf_pos;
    uint8_t  src_buf_left;
    uint8_t  bits;
    uint8_t  bits_left;
    uint16_t out_left;
    bool     error;
} huff_t;

static huff_t huff;

/* Runtime word-wrap state.  The asset stores one unwrapped logical line per
 * Markdown block; this walks it and emits rows for the selected font width. */
typedef struct {
    char     line[MD_WRAP_MAX + 1u];
    uint16_t len;
    uint16_t pos;
} wrap_t;

static wrap_t wrap;

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
/*  Huffman decoder                                                           */
/* -------------------------------------------------------------------------- */

static uint8_t huff_comp_byte(void)
{
    if (!huff.src_buf_left) {
        if (!huff.src_left) {
            huff.error = true;
            return 0;
        }
        const uint16_t want = huff.src_left > sizeof huff.src_buf
                            ? (uint16_t)sizeof huff.src_buf : huff.src_left;
        const uint16_t got = A->asset_read(huff.src_pos, huff.src_buf, want);
        if (got != want) {
            huff.error = true;
            return 0;
        }
        huff.src_pos = (uint16_t)(huff.src_pos + want);
        huff.src_left = (uint16_t)(huff.src_left - want);
        huff.src_buf_pos = 0;
        huff.src_buf_left = (uint8_t)want;
    }
    huff.src_buf_left--;
    return huff.src_buf[huff.src_buf_pos++];
}

static int huff_next(void)
{
    if (!huff.out_left)
        return -1;

    uint16_t code = 0;
    for (uint8_t length = 1; length <= MD_HUFF_MAX_BITS; length++) {
        if (!huff.bits_left) {
            huff.bits = huff_comp_byte();
            if (huff.error)
                return -1;
            huff.bits_left = 8;
        }
        code = (uint16_t)((code << 1) | (huff.bits >> 7));
        huff.bits = (uint8_t)(huff.bits << 1);
        huff.bits_left--;

        const uint16_t first = md_huff_first[length];
        if (code >= first) {
            const uint16_t index = (uint16_t)(code - first);
            if (index < md_huff_count[length]) {
                huff.out_left--;
                return md_huff_symbols[md_huff_index[length] + index];
            }
        }
    }

    huff.error = true;
    return -1;
}

static void huff_start(const md_file_t *file)
{
    huff.src_pos = file->off;
    huff.src_left = file->clen;
    huff.src_buf_pos = 0;
    huff.src_buf_left = 0;
    huff.bits = 0;
    huff.bits_left = 0;
    huff.out_left = file->ulen;
    huff.error = false;

    wrap.len = 0;
    wrap.pos = 0;
}

/* Return one line, or -1 at end of document / on a corrupt stream.  An empty
 * line returns 0, which is different from end of document. */
static int huff_read_line(char *dst, uint16_t size)
{
    uint16_t n = 0;
    int c;

    while ((c = huff_next()) >= 0) {
        if (c == '\n') {
            dst[n] = '\0';
            return (int)n;
        }
        if (n + 1u < size)
            dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return n ? (int)n : -1;
}

/* Wrap the current logical line to the selected font width. */
static int reader_next_row(char *dst, uint8_t size)
{
    const uint8_t cols = g.tiny ? MD_TINY_COLS : MD_LINE_MAX;

    for (;;) {
        if (wrap.pos >= wrap.len) {
            const int n = huff_read_line(wrap.line,
                                       (uint16_t)sizeof wrap.line);
            if (n < 0)
                return -1;

            wrap.len = (uint16_t)n;
            wrap.pos = 0;
            if (!n) {
                dst[0] = '\0';
                return 0;
            }
            if ((uint8_t)wrap.line[0] == MD_MARK_RULE) {
                uint8_t i = 0;
                while (i < cols && (uint8_t)(i + 1u) < size)
                    dst[i] = '-', i++;
                dst[i] = '\0';
                wrap.pos = wrap.len;
                return (int)i;
            }
        }

        uint16_t start = wrap.pos;
        while (start < wrap.len && wrap.line[start] == ' ')
            start++;
        wrap.pos = start;

        const uint16_t avail = (uint16_t)(wrap.len - start);
        uint16_t take = avail > cols ? cols : avail;

        if (take < avail) {
            uint16_t cut = take;
            while (cut > 0 && wrap.line[start + cut] != ' ')
                cut--;
            if (cut > 0)
                take = cut;
        }

        uint8_t n = 0;
        while (n < take && (uint8_t)(n + 1u) < size)
            dst[n] = wrap.line[start + n], n++;
        dst[n] = '\0';

        wrap.pos = (uint16_t)(start + take);
        while (wrap.pos < wrap.len && wrap.line[wrap.pos] == ' ')
            wrap.pos++;
        return (int)n;
    }
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

static void update_total_rows(void)
{
    const md_file_t *file = &md_files[g.selected];
    g.total_rows = g.tiny ? file->trows : file->lines;
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
    huff_start(&md_files[g.selected]);

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

    if (huff.error)
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
