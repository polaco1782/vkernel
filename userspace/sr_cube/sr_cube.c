#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../include/vk.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { float x, y, z; } vec3_t;
typedef struct { float x, y; } vec2_t;
typedef struct { float x, y, z, u, v; } vtx_t;

typedef struct {
    int a, b, c;
    vec2_t uva, uvb, uvc;
} tri_idx_t;

static inline float fmin3(float a, float b, float c) {
    float m = a < b ? a : b;
    return m < c ? m : c;
}

static inline float fmax3(float a, float b, float c) {
    float m = a > b ? a : b;
    return m > c ? m : c;
}

static inline uint32_t pack_pixel(uint8_t r, uint8_t g, uint8_t b, vk_pixel_format_t fmt) {
    if (fmt == VK_PIXEL_FORMAT_BGRX_8BPP) {
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline float edgef(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void stretch2x(const uint32_t *src, vk_u32 sw, vk_u32 sh, uint32_t *dst, vk_u32 dst_stride) {
    for (vk_u32 y = 0; y < sh; ++y) {
        const uint32_t *srow = src + (vk_usize)y * sw;
        uint32_t *d0 = dst + (vk_usize)(y * 2u) * dst_stride;
        uint32_t *d1 = dst + (vk_usize)(y * 2u + 1u) * dst_stride;
        for (vk_u32 x = 0; x < sw; ++x) {
            uint32_t p = srow[x];
            vk_u32 dx = x * 2u;
            d0[dx] = p;
            d0[dx + 1u] = p;
            d1[dx] = p;
            d1[dx + 1u] = p;
        }
    }
}

static vec3_t v3_sub(vec3_t a, vec3_t b) {
    vec3_t r = {a.x - b.x, a.y - b.y, a.z - b.z};
    return r;
}

static vec3_t v3_cross(vec3_t a, vec3_t b) {
    vec3_t r = {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
    return r;
}

static float v3_dot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3_t rotate_vertex(vec3_t p, float ax, float ay, float az) {
    float sx = sinf(ax), cx = cosf(ax);
    float sy = sinf(ay), cy = cosf(ay);
    float sz = sinf(az), cz = cosf(az);

    vec3_t r = p;

    float y1 = r.y * cx - r.z * sx;
    float z1 = r.y * sx + r.z * cx;
    r.y = y1; r.z = z1;

    float x2 = r.x * cy + r.z * sy;
    float z2 = -r.x * sy + r.z * cy;
    r.x = x2; r.z = z2;

    float x3 = r.x * cz - r.y * sz;
    float y3 = r.x * sz + r.y * cz;
    r.x = x3; r.y = y3;

    return r;
}

static uint32_t plasma_sample(float u, float v, float t, float light, vk_pixel_format_t fmt) {
    float p = 0.0f;
    p += sinf(u * 14.0f + t * 2.3f);
    p += sinf(v * 17.0f - t * 1.7f);
    p += sinf((u + v) * 11.0f + t * 2.9f);
    float cx = u - 0.5f;
    float cy = v - 0.5f;
    float r = sqrtf(cx * cx + cy * cy);
    p += sinf(r * 30.0f - t * 3.4f);
    p *= 0.25f;

    float rr = 0.5f + 0.5f * sinf(6.28318f * (p + t * 0.10f));
    float gg = 0.5f + 0.5f * sinf(6.28318f * (p + 0.33f + t * 0.07f));
    float bb = 0.5f + 0.5f * sinf(6.28318f * (p + 0.66f + t * 0.05f));

    float pulse = 0.72f + 0.28f * sinf(t * 6.0f + u * 8.0f - v * 6.0f);
    float k = light * pulse;

    uint8_t R = (uint8_t)(fminf(255.0f, rr * k * 255.0f));
    uint8_t G = (uint8_t)(fminf(255.0f, gg * k * 255.0f));
    uint8_t B = (uint8_t)(fminf(255.0f, bb * k * 255.0f));

    return pack_pixel(R, G, B, fmt);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    (void)vk_get_api();
    vk_framebuffer_info_t fb = {0};
    VK_CALL(framebuffer_info, &fb);
    if (!fb.valid || fb.base == 0 || fb.width < 64 || fb.height < 64) {
        VK_CALL(puts, "sr_cube: no usable framebuffer\n");
        return 1;
    }

    const vk_u32 rw = fb.width / 2u;
    const vk_u32 rh = fb.height / 2u;
    if (rw == 0 || rh == 0) {
        return 1;
    }

    uint32_t *color = (uint32_t *)malloc((vk_usize)rw * rh * sizeof(uint32_t));
    float *zbuf = (float *)malloc((vk_usize)rw * rh * sizeof(float));
    if (!color || !zbuf) {
        VK_CALL(puts, "sr_cube: out of memory\n");
        if (color) free(color);
        if (zbuf) free(zbuf);
        return 1;
    }

    vec3_t cube[8] = {
        {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1}
    };

    tri_idx_t tris[12] = {
        {0,1,2,{0,0},{1,0},{1,1}}, {0,2,3,{0,0},{1,1},{0,1}},
        {1,5,6,{0,0},{1,0},{1,1}}, {1,6,2,{0,0},{1,1},{0,1}},
        {5,4,7,{0,0},{1,0},{1,1}}, {5,7,6,{0,0},{1,1},{0,1}},
        {4,0,3,{0,0},{1,0},{1,1}}, {4,3,7,{0,0},{1,1},{0,1}},
        {3,2,6,{0,0},{1,0},{1,1}}, {3,6,7,{0,0},{1,1},{0,1}},
        {4,5,1,{0,0},{1,0},{1,1}}, {4,1,0,{0,0},{1,1},{0,1}}
    };

    /* Normalize triangle winding so back-face culling is stable. */
    for (int i = 0; i < 12; ++i) {
        tri_idx_t *tr = &tris[i];
        vec3_t oa = cube[tr->a];
        vec3_t ob = cube[tr->b];
        vec3_t oc = cube[tr->c];
        vec3_t on = v3_cross(v3_sub(ob, oa), v3_sub(oc, oa));
        vec3_t centroid = {
            (oa.x + ob.x + oc.x) * (1.0f / 3.0f),
            (oa.y + ob.y + oc.y) * (1.0f / 3.0f),
            (oa.z + ob.z + oc.z) * (1.0f / 3.0f)
        };
        if (v3_dot(on, centroid) < 0.0f) {
            int tmp_i = tr->b;
            tr->b = tr->c;
            tr->c = tmp_i;
            vec2_t tmp_uv = tr->uvb;
            tr->uvb = tr->uvc;
            tr->uvc = tmp_uv;
        }
    }

    vk_u32 ticks_per_sec = VK_CALL(ticks_per_sec);
    if (!ticks_per_sec) ticks_per_sec = 100;
    vk_u64 frame_ticks = (ticks_per_sec + 59u) / 60u;
    if (frame_ticks == 0) frame_ticks = 1;

    VK_CALL(puts, "sr_cube: Second Reality inspired software cube (ESC exits)\n");

    float t = 0.0f;
    while (1) {
        vk_key_event_t key;
        while (VK_CALL(poll_key, &key)) {
            if (key.pressed && (key.scancode == 0x01u || key.ascii == 'q' || key.ascii == 'Q')) {
                free(color);
                free(zbuf);
                return 0;
            }
        }

        vk_u64 start = VK_CALL(tick_count);

        for (vk_u32 i = 0; i < rw * rh; ++i) {
            zbuf[i] = 1e30f;
        }

        for (vk_u32 y = 0; y < rh; ++y) {
            for (vk_u32 x = 0; x < rw; ++x) {
                float nx = ((float)x / (float)rw) - 0.5f;
                float ny = ((float)y / (float)rh) - 0.5f;
                float bg = sinf(nx * 24.0f + t * 1.2f) + sinf(ny * 22.0f - t * 1.0f);
                bg = 0.5f + 0.25f * bg;
                uint8_t c = (uint8_t)fmaxf(0.0f, fminf(90.0f, 30.0f + bg * 70.0f));
                color[(vk_usize)y * rw + x] = pack_pixel((uint8_t)(c * 0.2f), (uint8_t)(c * 0.6f), c, fb.format);
            }
        }

        vtx_t tv[8];
        float ax = t * 1.1f;
        float ay = t * 0.8f;
        float az = t * 0.53f;
        float dist = 3.4f;
        float scale = (float)rh * 0.48f;

        for (int i = 0; i < 8; ++i) {
            vec3_t r = rotate_vertex(cube[i], ax, ay, az);
            r.z += dist;
            float invz = 1.0f / r.z;
            tv[i].x = r.x * invz * scale + (float)rw * 0.5f;
            tv[i].y = -r.y * invz * scale + (float)rh * 0.5f;
            tv[i].z = r.z;
        }

        vec3_t light_dir = {0.35f, 0.45f, -1.0f};

        for (int ti = 0; ti < 12; ++ti) {
            tri_idx_t tr = tris[ti];
            vtx_t a = tv[tr.a], b = tv[tr.b], c = tv[tr.c];

            float area = edgef(a.x, a.y, b.x, b.y, c.x, c.y);
            if (area >= 0.0f) {
                continue;
            }

            vec3_t wa = rotate_vertex(cube[tr.a], ax, ay, az);
            vec3_t wb = rotate_vertex(cube[tr.b], ax, ay, az);
            vec3_t wc = rotate_vertex(cube[tr.c], ax, ay, az);
            vec3_t n = v3_cross(v3_sub(wb, wa), v3_sub(wc, wa));
            float nl = v3_dot(n, light_dir);
            float light = 0.45f + 0.55f * fmaxf(0.0f, nl * 0.25f);

            int minx = (int)fmaxf(0.0f, floorf(fmin3(a.x, b.x, c.x)));
            int maxx = (int)fminf((float)(rw - 1u), ceilf(fmax3(a.x, b.x, c.x)));
            int miny = (int)fmaxf(0.0f, floorf(fmin3(a.y, b.y, c.y)));
            int maxy = (int)fminf((float)(rh - 1u), ceilf(fmax3(a.y, b.y, c.y)));

            if (minx > maxx || miny > maxy) continue;

            float inv_area = 1.0f / area;
            for (int y = miny; y <= maxy; ++y) {
                float py = (float)y + 0.5f;
                for (int x = minx; x <= maxx; ++x) {
                    float px = (float)x + 0.5f;

                    float w0 = edgef(b.x, b.y, c.x, c.y, px, py);
                    float w1 = edgef(c.x, c.y, a.x, a.y, px, py);
                    float w2 = edgef(a.x, a.y, b.x, b.y, px, py);

                    if (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f) {
                        w0 *= inv_area;
                        w1 *= inv_area;
                        w2 *= inv_area;

                        float z = w0 * a.z + w1 * b.z + w2 * c.z;
                        vk_usize idx = (vk_usize)y * rw + (vk_usize)x;
                        if (z < zbuf[idx]) {
                            zbuf[idx] = z;

                            float u = w0 * tr.uva.x + w1 * tr.uvb.x + w2 * tr.uvc.x;
                            float v = w0 * tr.uva.y + w1 * tr.uvb.y + w2 * tr.uvc.y;
                            color[idx] = plasma_sample(u, v, t, light, fb.format);
                        }
                    }
                }
            }
        }

        uint32_t *fbpix = (uint32_t *)(uintptr_t)fb.base;
        stretch2x(color, rw, rh, fbpix, fb.stride);

        t += 1.0f / 60.0f;

        while ((VK_CALL(tick_count) - start) < frame_ticks) {
            VK_CALL(yield);
        }
    }
}
