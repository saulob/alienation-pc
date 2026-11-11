#include <stdio.h>
#include <stdlib.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: svg_raster input.svg output.rgba width\n");
        return 2;
    }
    NSVGimage *image = nsvgParseFromFile(argv[1], "px", 96.0f);
    if (!image) {
        fprintf(stderr, "cannot parse %s\n", argv[1]);
        return 1;
    }
    const float width = (float)atoi(argv[3]);
    if (width <= 0 || image->width <= 0) {
        nsvgDelete(image);
        return 1;
    }
    const float scale = width / image->width;
    const int w = (int)(image->width * scale + 0.5f);
    const int h = (int)(image->height * scale + 0.5f);
    unsigned char *pixels = calloc((size_t)w * h, 4);
    NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
    if (!pixels || !rasterizer) {
        free(pixels);
        nsvgDeleteRasterizer(rasterizer);
        nsvgDelete(image);
        return 1;
    }
    nsvgRasterize(rasterizer, image, 0, 0, scale, pixels, w, h, w * 4);
    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(pixels, 4, (size_t)w * h, out) != (size_t)w * h) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        if (out) fclose(out);
        free(pixels);
        nsvgDeleteRasterizer(rasterizer);
        nsvgDelete(image);
        return 1;
    }
    fclose(out);
    fprintf(stderr, "%d %d\n", w, h);
    free(pixels);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
    return 0;
}
