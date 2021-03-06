/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include <time.h>

#include <pixelflinger/pixelflinger.h>
#include "../../../gui/placement.h"
#include "minui.h"
#include "graphics.h"

struct GRFont {
    GRSurface* texture;
    int cwidth;
    int cheight;
};

static GRFont* gr_font = NULL;
static minui_backend* gr_backend = NULL;

static int overscan_percent = OVERSCAN_PERCENT;
static int overscan_offset_x = 0;
static int overscan_offset_y = 0;

static unsigned char gr_current_r = 255;
static unsigned char gr_current_g = 255;
static unsigned char gr_current_b = 255;
static unsigned char gr_current_a = 255;
static unsigned char rgb_555[2];
static unsigned char gr_current_r5 = 31;
static unsigned char gr_current_g5 = 63;
static unsigned char gr_current_b5 = 31;

GRSurface* gr_draw = NULL;

static GGLContext *gr_context = 0;
GGLSurface gr_mem_surface;
static int gr_is_curr_clr_opaque = 0;

static bool outside(int x, int y)
{
    return x < 0 || x >= gr_draw->width || y < 0 || y >= gr_draw->height;
}

void blend_16bpp(unsigned char* px, unsigned r5, unsigned g5, unsigned b5, unsigned char a)
{
    unsigned char orig[2];
    orig[0] = px[0];
    orig[1] = px[1];

    /* This code is a little easier to read
    unsigned oldred = (orig[1] >> 3);
    unsigned oldgreen = (((orig[0] >> 5) << 3) + (orig[1] & 0x7));
    unsigned oldblue = (orig[0] & 0x1F);

    unsigned newred = (oldred * (255-a) + r5 * a) / 255;
    unsigned newgreen = (oldgreen * (255-a) + g5 * a) / 255;
    unsigned newblue = (oldblue * (255-a) + b5 * a) / 255;
    */

    unsigned newred = ((orig[1] >> 3) * (255-a) + r5 * a) / 255;
    unsigned newgreen = ((((orig[0] >> 5) << 3) + (orig[1] & 0x7)) * (255-a) + g5 * a) / 255;
    unsigned newblue = ((orig[0] & 0x1F) * (255-a) + b5 * a) / 255;

    *px++ = (newgreen << 5) + (newblue);
    *px++ = (newred << 3) + (newgreen >> 3);
}

static void text_blend(unsigned char* src_p, int src_row_bytes,
                       unsigned char* dst_p, int dst_row_bytes,
                       int width, int height)
{
    for (int j = 0; j < height; ++j) {
        unsigned char* sx = src_p;
        unsigned char* px = dst_p;
        for (int i = 0; i < width; ++i) {
            unsigned char a = *sx++;
            if (gr_current_a < 255) a = ((int)a * gr_current_a) / 255;
            if (a == 255) {
                if (gr_draw->pixel_bytes == 2) {
                    *px++ = rgb_555[0];
                    *px++ = rgb_555[1];
                } else {
                    *px++ = gr_current_r;
                    *px++ = gr_current_g;
                    *px++ = gr_current_b;
                    px++;
                }
            } else if (a > 0) {
                if (gr_draw->pixel_bytes == 2) {
                    blend_16bpp(px, gr_current_r5, gr_current_g5, gr_current_b5, a);
                    px += gr_draw->pixel_bytes;
                } else {
                    *px = (*px * (255-a) + gr_current_r * a) / 255;
                    ++px;
                    *px = (*px * (255-a) + gr_current_g * a) / 255;
                    ++px;
                    *px = (*px * (255-a) + gr_current_b * a) / 255;
                    ++px;
                    ++px;
                }
            } else {
                px += gr_draw->pixel_bytes;
            }
        }
        src_p += src_row_bytes;
        dst_p += dst_row_bytes;
    }
}

void gr_text(int x, int y, const char *s, bool bold)
{
    GRFont* font = gr_font;

    if (!font->texture || gr_current_a == 0) return;

    bold = bold && (font->texture->height != font->cheight);

    x += overscan_offset_x;
    y += overscan_offset_y;

    unsigned char ch;
    while ((ch = *s++)) {
        if (outside(x, y) || outside(x+font->cwidth-1, y+font->cheight-1)) break;

        if (ch < ' ' || ch > '~') {
            ch = '?';
        }

        unsigned char* src_p = font->texture->data + ((ch - ' ') * font->cwidth) +
                               (bold ? font->cheight * font->texture->row_bytes : 0);
        unsigned char* dst_p = gr_draw->data + y*gr_draw->row_bytes + x*gr_draw->pixel_bytes;

        text_blend(src_p, font->texture->row_bytes,
                   dst_p, gr_draw->row_bytes,
                   font->cwidth, font->cheight);

        x += font->cwidth;
    }
}

int gr_textEx_scaleW(int x, int y, const char *s, void* pFont, int max_width, int placement, int scale)
{
    GGLContext *gl = gr_context;
    void* vfont = pFont;
    GRFont *font = (GRFont*) pFont;
    unsigned off;
    unsigned cwidth;
    int y_scale = 0, measured_width, measured_height, ret, new_height;

    if (!s || strlen(s) == 0 || !font)
        return 0;

    measured_height = gr_ttf_getMaxFontHeight(font);

    if (scale) {
        measured_width = gr_ttf_measureEx(s, vfont);
        if (measured_width > max_width) {
            // Adjust font size down until the text fits
            void *new_font = gr_ttf_scaleFont(vfont, max_width, measured_width);
            if (!new_font) {
                printf("gr_textEx_scaleW new_font is NULL\n");
                return 0;
            }
            measured_width = gr_ttf_measureEx(s, new_font);
            // These next 2 lines adjust the y point based on the new font's height
            new_height = gr_ttf_getMaxFontHeight(new_font);
            y_scale = (measured_height - new_height) / 2;
            vfont = new_font;
        }
    } else
        measured_width = gr_ttf_measureEx(s, vfont);

    int x_adj = measured_width;
    if (measured_width > max_width)
        x_adj = max_width;

    if (placement != TOP_LEFT && placement != BOTTOM_LEFT && placement != TEXT_ONLY_RIGHT) {
        if (placement == CENTER || placement == CENTER_X_ONLY)
            x -= (x_adj / 2);
        else
            x -= x_adj;
    }

    if (placement != TOP_LEFT && placement != TOP_RIGHT) {
        if (placement == CENTER || placement == TEXT_ONLY_RIGHT)
            y -= (measured_height / 2);
        else if (placement == BOTTOM_LEFT || placement == BOTTOM_RIGHT)
            y -= measured_height;
    }
    return gr_ttf_textExWH(gl, x, y + y_scale, s, vfont, measured_width + x, -1);
}

void gr_clip(int x, int y, int w, int h)
{
    GGLContext *gl = gr_context;
    gl->scissor(gl, x, y, w, h);
    gl->enable(gl, GGL_SCISSOR_TEST);
}

void gr_noclip()
{
    GGLContext *gl = gr_context;
    gl->scissor(gl, 0, 0, gr_fb_width(), gr_fb_height());
    gl->disable(gl, GGL_SCISSOR_TEST);
}

void gr_line(int x0, int y0, int x1, int y1, int width)
{
    GGLContext *gl = gr_context;

    if(gr_is_curr_clr_opaque)
        gl->disable(gl, GGL_BLEND);

    const int coords0[2] = { x0 << 4, y0 << 4 };
    const int coords1[2] = { x1 << 4, y1 << 4 };
    gl->linex(gl, coords0, coords1, width << 4);

    if(gr_is_curr_clr_opaque)
        gl->enable(gl, GGL_BLEND);
}

gr_surface gr_render_circle(int radius, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    int rx, ry;
    GGLSurface *surface;
    const int diameter = radius*2 + 1;
    const int radius_check = radius*radius + radius*0.8;
    const uint32_t px = (a << 24) | (b << 16) | (g << 8) | r;
    uint32_t *data;

    surface = (GGLSurface *)malloc(sizeof(GGLSurface));
    memset(surface, 0, sizeof(GGLSurface));

    data = (uint32_t *)malloc(diameter * diameter * 4);
    memset(data, 0, diameter * diameter * 4);

    surface->version = sizeof(surface);
    surface->width = diameter;
    surface->height = diameter;
    surface->stride = diameter;
    surface->data = (GGLubyte*)data;
    surface->format = GGL_PIXEL_FORMAT_RGBA_8888;

    for(ry = -radius; ry <= radius; ++ry)
        for(rx = -radius; rx <= radius; ++rx)
            if(rx*rx+ry*ry <= radius_check)
                *(data + diameter*(radius + ry) + (radius+rx)) = px;

    return (gr_surface)surface;
}

void gr_color(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    GGLContext *gl = gr_context;
    GGLint color[4];
#if defined(RECOVERY_ABGR) || defined(RECOVERY_BGRA)
    color[0] = ((b << 8) | r) + 1;
    color[1] = ((g << 8) | g) + 1;
    color[2] = ((r << 8) | b) + 1;
    color[3] = ((a << 8) | a) + 1;
#else
    color[0] = ((r << 8) | r) + 1;
    color[1] = ((g << 8) | g) + 1;
    color[2] = ((b << 8) | b) + 1;
    color[3] = ((a << 8) | a) + 1;
#endif
    gl->color4xv(gl, color);

    gr_is_curr_clr_opaque = (a == 255);
}

void gr_clear()
{
    if (gr_draw->pixel_bytes == 2) {
        gr_fill(0, 0, gr_fb_width(), gr_fb_height());
        return;
    }

    // This code only works on 32bpp devices
    if (gr_current_r == gr_current_g && gr_current_r == gr_current_b) {
        memset(gr_draw->data, gr_current_r, gr_draw->height * gr_draw->row_bytes);
    } else {
        unsigned char* px = gr_draw->data;
        for (int y = 0; y < gr_draw->height; ++y) {
            for (int x = 0; x < gr_draw->width; ++x) {
                *px++ = gr_current_r;
                *px++ = gr_current_g;
                *px++ = gr_current_b;
                px++;
            }
            px += gr_draw->row_bytes - (gr_draw->width * gr_draw->pixel_bytes);
        }
    }
}

void gr_fill(int x, int y, int w, int h)
{
    GGLContext *gl = gr_context;

    if(gr_is_curr_clr_opaque)
        gl->disable(gl, GGL_BLEND);

    gl->recti(gl, x, y, x + w, y + h);

    if(gr_is_curr_clr_opaque)
        gl->enable(gl, GGL_BLEND);
}

void gr_blit(gr_surface source, int sx, int sy, int w, int h, int dx, int dy) {
    if (gr_context == NULL) {
        return;
    }

    GGLContext *gl = gr_context;
    GGLSurface *surface = (GGLSurface*)source;

    if(surface->format == GGL_PIXEL_FORMAT_RGBX_8888)
        gl->disable(gl, GGL_BLEND);

    gl->bindTexture(gl, surface);
    gl->texEnvi(gl, GGL_TEXTURE_ENV, GGL_TEXTURE_ENV_MODE, GGL_REPLACE);
    gl->texGeni(gl, GGL_S, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->texGeni(gl, GGL_T, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->enable(gl, GGL_TEXTURE_2D);
    gl->texCoord2i(gl, sx - dx, sy - dy);
    gl->recti(gl, dx, dy, dx + w, dy + h);
    gl->disable(gl, GGL_TEXTURE_2D);

    if(surface->format == GGL_PIXEL_FORMAT_RGBX_8888)
        gl->enable(gl, GGL_BLEND);
}

unsigned int gr_get_width(gr_surface surface) {
    if (surface == NULL) {
        return 0;
    }
    return ((GGLSurface*) surface)->width;
}

unsigned int gr_get_height(gr_surface surface) {
    if (surface == NULL) {
        return 0;
    }
    return ((GGLSurface*) surface)->height;
}

void gr_flip() {
    gr_draw = gr_backend->flip(gr_backend);
    // On double buffered back ends, when we flip, we need to tell
    // pixel flinger to draw to the other buffer
    gr_mem_surface.data = (GGLubyte*)gr_draw->data;
    gr_context->colorBuffer(gr_context, &gr_mem_surface);
}

static void get_memory_surface(GGLSurface* ms) {
    ms->version = sizeof(*ms);
    ms->width = gr_draw->width;
    ms->height = gr_draw->height;
    ms->stride = gr_draw->row_bytes / gr_draw->pixel_bytes;
    ms->data = (GGLubyte*)gr_draw->data;
    ms->format = gr_draw->format;
}

int gr_init(void)
{
    gr_draw = NULL;

#ifdef MSM_BSP 
    gr_backend = open_overlay();
    if (gr_backend) {
        gr_draw = gr_backend->init(gr_backend);
        if (!gr_draw) {
            gr_backend->exit(gr_backend);
        } else
            printf("Using overlay graphics.\n");
    }
#endif 

#ifdef HAS_ADF
    if (!gr_backend || !gr_draw) { 
        gr_backend = open_adf();
        if (gr_backend) {
            gr_draw = gr_backend->init(gr_backend);
            if (!gr_draw) {
                gr_backend->exit(gr_backend);
            } else
                printf("Using adf graphics.\n");
        }
    }
#else
#ifdef MSM_BSP
	printf("Skipping adf graphics because TW_TARGET_USES_QCOM_BSP := true\n");
#else
    printf("Skipping adf graphics -- not present in build tree\n");
#endif
#endif

#ifdef HAS_DRM
    if (!gr_backend || !gr_draw) { 
        gr_backend = open_drm();
        gr_draw = gr_backend->init(gr_backend);
        if (gr_draw)
            printf("Using drm graphics.\n");
    }
#else
    printf("Skipping drm graphics -- not present in build tree\n");
#endif

    if (!gr_backend || !gr_draw) { 
        gr_backend = open_fbdev();
        gr_draw = gr_backend->init(gr_backend);
        if (gr_draw == NULL) {
            return -1;
        } else
            printf("Using fbdev graphics.\n");
    }

    overscan_offset_x = gr_draw->width * overscan_percent / 100;
    overscan_offset_y = gr_draw->height * overscan_percent / 100;

    // Set up pixelflinger
    get_memory_surface(&gr_mem_surface);
    gglInit(&gr_context);
    GGLContext *gl = gr_context;
    gl->colorBuffer(gl, &gr_mem_surface);

    gl->activeTexture(gl, 0);
    gl->enable(gl, GGL_BLEND);
    gl->blendFunc(gl, GGL_SRC_ALPHA, GGL_ONE_MINUS_SRC_ALPHA);

    gr_flip();
    gr_flip();

#ifdef TW_SCREEN_BLANK_ON_BOOT
    printf("TW_SCREEN_BLANK_ON_BOOT := true\n");
    gr_fb_blank(true);
    gr_fb_blank(false);
#endif

    return 0;
}

void gr_exit(void)
{
    //gr_backend->exit(gr_backend);
}

int gr_fb_width(void)
{
    return gr_draw->width - 2*overscan_offset_x;
}

int gr_fb_height(void)
{
    return gr_draw->height - 2*overscan_offset_y;
}

void gr_fb_blank(bool blank)
{
    gr_backend->blank(gr_backend, blank);
}

int gr_get_surface(gr_surface* surface)
{
    GGLSurface* ms = (GGLSurface*)malloc(sizeof(GGLSurface));
    if (!ms)    return -1;

    // Allocate the data
    get_memory_surface(ms);
    ms->data = (GGLubyte*)malloc(ms->stride * ms->height * gr_draw->pixel_bytes);

    // Now, copy the data
    memcpy(ms->data, gr_mem_surface.data, gr_draw->width * gr_draw->height * gr_draw->pixel_bytes / 8);

    *surface = (gr_surface*) ms;
    return 0;
}

int gr_free_surface(gr_surface surface)
{
    if (!surface)
        return -1;

    GGLSurface* ms = (GGLSurface*) surface;
    free(ms->data);
    free(ms);
    return 0;
}

void gr_write_frame_to_file(int fd)
{
    write(fd, gr_mem_surface.data, gr_draw->width * gr_draw->height * gr_draw->pixel_bytes / 8);
}
