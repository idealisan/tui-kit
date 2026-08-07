/*
 * png.c - write an RGBA pixel buffer to a PNG file using libpng
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <png.h>

/*
 * `pixels` is a uint32 array packed as 0xAARRGGBB (native endian).
 * Convert to network-order RGBA bytes and write as PNG.
 */
int png_write(const char *path, void *pixels, int width, int height)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);

    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height,
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    png_bytep *rows = malloc(sizeof(png_bytep) * (size_t)height);
    if (!rows) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    uint32_t *src = pixels;

    for (int y = 0; y < height; y++) {
        rows[y] = malloc(sizeof(png_byte) * (size_t)width * 4);
        if (!rows[y]) {
            for (int i = 0; i < y; i++) free(rows[i]);
            free(rows);
            png_destroy_write_struct(&png, &info);
            fclose(fp);
            return -1;
        }
        png_bytep out = rows[y];
        for (int x = 0; x < width; x++) {
            uint32_t px = src[(size_t)y * width + x];
            out[x * 4 + 0] = (png_byte)((px >> 16) & 0xFF);
            out[x * 4 + 1] = (png_byte)((px >> 8) & 0xFF);
            out[x * 4 + 2] = (png_byte)(px & 0xFF);
            out[x * 4 + 3] = (png_byte)((px >> 24) & 0xFF);
        }
    }

    png_write_image(png, rows);
    png_write_end(png, info);

    for (int y = 0; y < height; y++) {
        free(rows[y]);
    }
    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}
