/* Host-side smoke test for the Huffman decoder and line readers.
 *
 * Not part of the radio build.  It includes the app source so the static
 * decoder functions are visible, feeds the generated asset blob through a
 * fake asset_read(), and prints the decoded text.  test/run.sh compares the
 * normal output with the Python 18-column renderer and the tiny output with
 * the 32-column renderer.
 *
 * Usage: host_decode <markdown_assets.bin> [normal|tiny]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "markdown_viewer_app.c"

static uint8_t *asset_blob;
static size_t asset_len;

static uint16_t fake_asset_read(uint16_t off, void *buf, uint16_t len)
{
    if (off >= asset_len)
        return 0;
    if (len > asset_len - off)
        len = (uint16_t)(asset_len - off);
    memcpy(buf, asset_blob + off, len);
    return len;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <markdown_assets.bin> [normal|tiny]\n",
                argv[0]);
        return 2;
    }
    const bool tiny_mode = argc == 3 && strcmp(argv[2], "tiny") == 0;
    if (argc == 3 && !tiny_mode && strcmp(argv[2], "normal") != 0) {
        fprintf(stderr, "unknown mode: %s\n", argv[2]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    asset_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    asset_blob = malloc(asset_len);
    if (!asset_blob || fread(asset_blob, 1, asset_len, f) != asset_len) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);

    static app_api_t fake;
    fake.asset_read = fake_asset_read;
    g.api = &fake;
    g.tiny = tiny_mode;

    char line[MD_TINY_COLS + 1u];
    unsigned total_rows = 0;

    for (uint8_t i = 0; i < MD_FILE_COUNT; i++) {
        unsigned rows = 0;
        huff_start(&md_files[i]);
        int n;
        while ((n = reader_next_row(line, (uint8_t)sizeof line)) >= 0) {
            if (tiny_mode && strlen(line) > MD_TINY_COLS) {
                fprintf(stderr, "%s: tiny row is over %u columns\n",
                        md_files[i].name, MD_TINY_COLS);
                return 1;
            }
            printf("%s\n", line);
            rows++;
        }
        if (huff.error) {
            fprintf(stderr, "decode error in %s\n", md_files[i].name);
            return 1;
        }
        if (!tiny_mode && rows != md_files[i].lines) {
            fprintf(stderr, "%s: decoded %u lines, header says %u\n",
                    md_files[i].name, rows, md_files[i].lines);
            return 1;
        }
        total_rows += rows;
    }

    fprintf(stderr, "OK: %u files, %u %s rows, %zu asset bytes\n",
            MD_FILE_COUNT, total_rows, tiny_mode ? "tiny" : "normal",
            asset_len);
    return 0;
}
