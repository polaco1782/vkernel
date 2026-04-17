/*
 * vkernel userspace - raytracer demo
 * Copyright (C) 2026 vkernel authors
 *
 * raytracer.c - Freestanding realtime raytracing example
 *
 * Build: see Makefile (Linux) or raytracer.vcxproj (Visual Studio).
 * Run:   vk> run raytracer.elf
 */

#include "../include/vk.h"

#ifdef _MSC_VER
int _fltused = 0;
#endif

typedef struct vec3 {
    float x;
    float y;
    float z;
} vec3_t;

typedef struct ray {
    vec3_t origin;
    vec3_t direction;
} ray_t;

typedef struct sphere {
    vec3_t center;
    float radius;
    vec3_t color;
} sphere_t;

static vec3_t vec3_make(float x, float y, float z) {
    vec3_t v = { x, y, z };
    return v;
}

static vec3_t vec3_add(vec3_t a, vec3_t b) {
    return vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}

static vec3_t vec3_sub(vec3_t a, vec3_t b) {
    return vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}

static vec3_t vec3_scale(vec3_t v, float s) {
    return vec3_make(v.x * s, v.y * s, v.z * s);
}

static float vec3_dot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float vk_absf(float value) {
    return value < 0.0f ? -value : value;
}

static float vk_sqrtf(float value) {
    if (value <= 0.0f) return 0.0f;

    float guess = value > 1.0f ? value : 1.0f;
    for (int i = 0; i < 8; ++i) {
        guess = 0.5f * (guess + value / guess);
    }
    return guess;
}

static float vk_floorf(float value);

static vec3_t vec3_normalize(vec3_t v) {
    float len = vk_sqrtf(vec3_dot(v, v));
    if (len <= 0.0f) return vec3_make(0.0f, 0.0f, 0.0f);
    return vec3_scale(v, 1.0f / len);
}

static float clampf(float value, float min_value, float max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static vk_u32 pack_pixel(unsigned char r, unsigned char g, unsigned char b, vk_pixel_format_t format) {
    switch (format) {
        case VK_PIXEL_FORMAT_BGRX_8BPP:
            return ((vk_u32)b << 16) | ((vk_u32)g << 8) | (vk_u32)r;
        case VK_PIXEL_FORMAT_RGBX_8BPP:
        case VK_PIXEL_FORMAT_BITMASK:
        case VK_PIXEL_FORMAT_BLT_ONLY:
        default:
            return ((vk_u32)r << 16) | ((vk_u32)g << 8) | (vk_u32)b;
    }
}

static unsigned char to_byte(float value) {
    value = clampf(value, 0.0f, 1.0f);
    return (unsigned char)(value * 255.0f + 0.5f);
}

static float triangle_wave(int frame, int period) {
    if (period <= 0) return 0.0f;

    int phase = frame % period;
    float t = (float)phase / (float)period;
    if (t < 0.5f) {
        return t * 4.0f - 1.0f;
    }
    return 3.0f - t * 4.0f;
}

static int hit_sphere(ray_t ray, sphere_t sphere, float* out_t, vec3_t* out_normal) {
    vec3_t oc = vec3_sub(ray.origin, sphere.center);
    float a = vec3_dot(ray.direction, ray.direction);
    float b = 2.0f * vec3_dot(oc, ray.direction);
    float c = vec3_dot(oc, oc) - sphere.radius * sphere.radius;
    float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) {
        return 0;
    }

    float root = vk_sqrtf(discriminant);
    float t0 = (-b - root) / (2.0f * a);
    float t1 = (-b + root) / (2.0f * a);
    float t = t0;
    if (t < 0.001f) {
        t = t1;
    }
    if (t < 0.001f) {
        return 0;
    }

    if (out_t != NULL) {
        *out_t = t;
    }
    if (out_normal != NULL) {
        vec3_t hit_point = vec3_add(ray.origin, vec3_scale(ray.direction, t));
        *out_normal = vec3_normalize(vec3_sub(hit_point, sphere.center));
    }
    return 1;
}

static int hit_floor(ray_t ray, float* out_t, vec3_t* out_normal) {
    const float plane_y = -1.0f;
    if (vk_absf(ray.direction.y) < 0.0001f) {
        return 0;
    }

    float t = (plane_y - ray.origin.y) / ray.direction.y;
    if (t < 0.001f) {
        return 0;
    }

    if (out_t != NULL) {
        *out_t = t;
    }
    if (out_normal != NULL) {
        *out_normal = vec3_make(0.0f, 1.0f, 0.0f);
    }
    return 1;
}

static float sphere_shadow(ray_t ray, sphere_t sphere) {
    float t;
    if (!hit_sphere(ray, sphere, &t, NULL)) {
        return 0.0f;
    }
    return 1.0f;
}

static float floor_shadow(ray_t ray) {
    float t;
    if (!hit_floor(ray, &t, NULL)) {
        return 0.0f;
    }
    return 1.0f;
}

static vec3_t shade_scene(ray_t ray, sphere_t* spheres, int sphere_count, float frame_phase) {
    float closest_t = 1.0e30f;
    vec3_t closest_normal = vec3_make(0.0f, 1.0f, 0.0f);
    vec3_t closest_color = vec3_make(0.12f, 0.14f, 0.18f);
    int hit_anything = 0;
    int hit_floor_surface = 0;

    for (int i = 0; i < sphere_count; ++i) {
        float t;
        vec3_t normal;
        if (hit_sphere(ray, spheres[i], &t, &normal) && t < closest_t) {
            closest_t = t;
            closest_normal = normal;
            closest_color = spheres[i].color;
            hit_anything = 1;
            hit_floor_surface = 0;
        }
    }

    {
        float t;
        vec3_t normal;
        if (hit_floor(ray, &t, &normal) && t < closest_t) {
            closest_t = t;
            closest_normal = normal;
            hit_anything = 1;
            hit_floor_surface = 1;
        }
    }

    vec3_t sky_top = vec3_make(0.10f, 0.16f, 0.33f);
    vec3_t sky_bottom = vec3_make(0.72f, 0.86f, 1.00f);

    if (!hit_anything) {
        float t = 0.5f * (ray.direction.y + 1.0f);
        return vec3_add(vec3_scale(sky_bottom, t), vec3_scale(sky_top, 1.0f - t));
    }

    vec3_t light_dir = vec3_normalize(vec3_make(-0.8f, 1.3f, -0.6f));
    vec3_t hit_point = vec3_add(ray.origin, vec3_scale(ray.direction, closest_t));
    vec3_t shadow_origin = vec3_add(hit_point, vec3_scale(closest_normal, 0.01f));
    ray_t shadow_ray;
    shadow_ray.origin = shadow_origin;
    shadow_ray.direction = light_dir;

    float blocked = 0.0f;
    for (int i = 0; i < sphere_count; ++i) {
        blocked = blocked > 0.0f ? blocked : sphere_shadow(shadow_ray, spheres[i]);
    }
    blocked = blocked > 0.0f ? blocked : floor_shadow(shadow_ray);

    float diffuse = clampf(vec3_dot(closest_normal, light_dir), 0.0f, 1.0f);
    if (blocked > 0.0f) {
        diffuse *= 0.18f;
    }

    float spec = clampf(vec3_dot(vec3_normalize(vec3_add(light_dir, vec3_scale(ray.direction, -1.0f))), closest_normal), 0.0f, 1.0f);
    spec = spec * spec;
    spec = spec * spec;

    vec3_t ambient = vec3_make(0.10f, 0.10f, 0.12f);
    vec3_t direct = vec3_scale(closest_color, 0.85f * diffuse + 0.25f);
    vec3_t highlight = vec3_scale(vec3_make(1.0f, 1.0f, 1.0f), 0.2f * spec);
    vec3_t color = vec3_add(vec3_add(ambient, direct), highlight);

    if (hit_floor_surface) {
        int checker = (((int)(vk_floorf(hit_point.x * 2.0f) + vk_floorf(hit_point.z * 2.0f))) & 1);
        vec3_t checker_a = vec3_make(0.22f, 0.24f, 0.26f);
        vec3_t checker_b = vec3_make(0.74f, 0.77f, 0.83f);
        vec3_t checker_color = checker ? checker_b : checker_a;
        color = vec3_add(vec3_scale(checker_color, 0.8f), vec3_scale(vec3_make(0.6f, 0.6f, 0.7f), 0.2f * diffuse));
    }

    float fog = clampf((closest_t - 2.0f) / 10.0f, 0.0f, 1.0f);
    vec3_t sky = vec3_add(vec3_scale(sky_bottom, 0.6f + 0.4f * frame_phase), vec3_scale(sky_top, 0.4f - 0.2f * frame_phase));
    return vec3_add(vec3_scale(color, 1.0f - fog), vec3_scale(sky, fog));
}

/* Provide floor helpers without stdlib. */
static float vk_floorf(float value) {
    int integer = (int)value;
    if ((float)integer > value) {
        --integer;
    }
    return (float)integer;
}

int _start(const vk_api_t* api) {
    vk_init(api);

    printf("Realtime raytracer demo\n");

    vk_framebuffer_info_t fb = { 0 };
    vk_get_framebuffer_info(&fb);

    if (!fb.valid || fb.base == 0 || fb.width == 0 || fb.height == 0) {
        printf("  No framebuffer available.\n");
        return 1;
    }

    vk_u32 scale = 4u;
    if (fb.width < 800u || fb.height < 600u) {
        scale = 2u;
    }
    if (fb.width < 400u || fb.height < 300u) {
        scale = 1u;
    }

    vk_u32 render_w = fb.width / scale;
    vk_u32 render_h = fb.height / scale;
    if (render_w == 0u) render_w = 1u;
    if (render_h == 0u) render_h = 1u;

    printf("  Framebuffer        : %ux%u\n", fb.width, fb.height);
    printf("  Render size        : %ux%u\n", render_w, render_h);
    printf("  Scale              : %u\n", scale);
    printf("  Rendering...\n");

    vk_u32* pixels = (vk_u32*)(unsigned long long)fb.base;

    for (int frame = 0; frame < 240; ++frame) {
        float pulse = triangle_wave(frame, 120);
        float pulse2 = triangle_wave(frame + 40, 180);

        sphere_t spheres[3];
        spheres[0].center = vec3_make(-1.2f + pulse * 0.7f, -0.1f + pulse2 * 0.2f, 3.8f);
        spheres[0].radius = 0.85f;
        spheres[0].color  = vec3_make(0.95f, 0.28f, 0.25f);

        spheres[1].center = vec3_make(0.0f, -0.35f + pulse * 0.35f, 2.9f + pulse2 * 0.3f);
        spheres[1].radius = 0.65f;
        spheres[1].color  = vec3_make(0.20f, 0.72f, 0.95f);

        spheres[2].center = vec3_make(1.3f - pulse * 0.6f, 0.05f + pulse2 * 0.15f, 4.4f);
        spheres[2].radius = 0.95f;
        spheres[2].color  = vec3_make(0.92f, 0.82f, 0.28f);

        vec3_t camera = vec3_make(0.0f, 0.2f, -3.0f);

        for (vk_u32 y = 0; y < render_h; ++y) {
            for (vk_u32 x = 0; x < render_w; ++x) {
                float u = ((float)x / (float)(render_w - 1u)) * 2.0f - 1.0f;
                float v = ((float)y / (float)(render_h - 1u)) * 2.0f - 1.0f;
                float aspect = (float)fb.width / (float)fb.height;

                ray_t ray;
                ray.origin = camera;
                ray.direction = vec3_normalize(vec3_make(u * aspect * 1.15f, -v * 0.95f, 1.0f));

                vec3_t color = shade_scene(ray, spheres, 3, 0.5f + 0.5f * pulse);
                vk_u32 pixel = pack_pixel(to_byte(color.x), to_byte(color.y), to_byte(color.z), fb.format);

                vk_u32 base_x = x * scale;
                vk_u32 base_y = y * scale;
                for (vk_u32 yy = 0; yy < scale && base_y + yy < fb.height; ++yy) {
                    for (vk_u32 xx = 0; xx < scale && base_x + xx < fb.width; ++xx) {
                        pixels[(vk_usize)(base_y + yy) * fb.stride + (base_x + xx)] = pixel;
                    }
                }
            }
        }

        vk_sleep(1);
    }

    printf("  Raytracing done.\n");
    return 0;
}
