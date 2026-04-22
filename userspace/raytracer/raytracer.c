/*
 * vkernel userspace - path tracer demo
 * Adapted from "Ray Tracing in One Weekend" by Shirley, Black, Hollasch.
 * https://raytracing.github.io/books/RayTracingInOneWeekend.html
 *
 * raytracer.c - Final render: ~480 randomly placed Lambertian, Metal, and
 *               Dielectric spheres with a positionable camera and defocus blur.
 *
 * Build: see Makefile (Linux) or raytracer.vcxproj (Visual Studio).
 * Run:   vk> run raytracer.elf
 */

#include <math.h>
#include <float.h>
#include "../include/vk.h"

static inline float degrees_to_radians(float deg) {
    return deg * 3.14159265358979323846f / 180.0f;
}

static inline float clampf(float v, float lo, float hi) {
    return fminf(fmaxf(v, lo), hi);
}

#ifdef _MSC_VER
int _fltused = 0;
#endif

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

/*
 * SAMPLES_PER_PIXEL: total samples accumulated per pixel.
 *   4   → fast preview (noisy)
 *   64  → good quality
 *   256 → production quality (slow on a single core freestanding kernel)
 *
 * The render loop is split into PASSES of SAMPLES_PER_PASS samples each,
 * flushing the framebuffer after every pass so you see the image sharpen
 * progressively rather than waiting for the full render to complete.
 */
#define SAMPLES_PER_PIXEL  256    /* total accumulated samples             */
#define SAMPLES_PER_PASS   4      /* samples per progressive-flush pass    */
#define MAX_DEPTH          16     /* maximum ray-bounce depth              */
#define MAX_SPHERES        520    /* upper bound on scene spheres          */

/* Progress: print a dot every N scanlines during each pass. */
#define PROGRESS_INTERVAL  16

/* ================================================================== */
/* vec3                                                                */
/* ================================================================== */

typedef struct { float x, y, z; } vec3_t;

static vec3_t v3(float x, float y, float z) {
    vec3_t r; r.x = x; r.y = y; r.z = z; return r;
}
static vec3_t v3_add(vec3_t a, vec3_t b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static vec3_t v3_sub(vec3_t a, vec3_t b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static vec3_t v3_mul(vec3_t a, vec3_t b) { return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static vec3_t v3_scale(vec3_t v, float s){ return v3(v.x*s, v.y*s, v.z*s); }
static vec3_t v3_neg(vec3_t v)           { return v3(-v.x, -v.y, -v.z); }
static float  v3_dot(vec3_t a, vec3_t b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float  v3_len2(vec3_t v)          { return v3_dot(v, v); }
static float  v3_len(vec3_t v)           { return sqrtf(v3_len2(v)); }

static vec3_t v3_unit(vec3_t v) {
    float l = v3_len(v);
    return l > 0.0f ? v3_scale(v, 1.0f / l) : v3(0.0f, 0.0f, 0.0f);
}

static vec3_t v3_cross(vec3_t u, vec3_t v) {
    return v3(u.y*v.z - u.z*v.y,
              u.z*v.x - u.x*v.z,
              u.x*v.y - u.y*v.x);
}

static int v3_near_zero(vec3_t v) {
    const float e = 1e-8f;
    return fabsf(v.x) < e && fabsf(v.y) < e && fabsf(v.z) < e;
}

static vec3_t v3_reflect(vec3_t v, vec3_t n) {
    return v3_sub(v, v3_scale(n, 2.0f * v3_dot(v, n)));
}

static vec3_t v3_refract(vec3_t uv, vec3_t n, float etai_over_etat) {
    float cos_theta  = fminf(v3_dot(v3_neg(uv), n), 1.0f);
    vec3_t r_perp    = v3_scale(v3_add(uv, v3_scale(n, cos_theta)), etai_over_etat);
    float  para_len2 = 1.0f - v3_len2(r_perp);
    vec3_t r_para    = v3_scale(n, -sqrtf(fabsf(para_len2)));
    return v3_add(r_perp, r_para);
}

/* ================================================================== */
/* Random number generator (xorshift32)                               */
/* ================================================================== */

static unsigned int g_rng = 0x853C49E6u;

static float rand_f(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng >> 8) * (1.0f / 16777216.0f);
}

static float rand_range(float lo, float hi) { return lo + (hi - lo) * rand_f(); }
static vec3_t rand_v3(void)                 { return v3(rand_f(), rand_f(), rand_f()); }

static vec3_t rand_v3_range(float lo, float hi) {
    return v3(rand_range(lo, hi), rand_range(lo, hi), rand_range(lo, hi));
}

static vec3_t rand_unit_vector(void) {
    for (;;) {
        vec3_t p   = rand_v3_range(-1.0f, 1.0f);
        float  lsq = v3_len2(p);
        if (lsq > 1e-30f && lsq <= 1.0f)
            return v3_scale(p, 1.0f / sqrtf(lsq));
    }
}

static vec3_t rand_in_unit_disk(void) {
    for (;;) {
        vec3_t p = v3(rand_range(-1.0f, 1.0f), rand_range(-1.0f, 1.0f), 0.0f);
        if (v3_len2(p) < 1.0f)
            return p;
    }
}

/* ================================================================== */
/* Ray                                                                 */
/* ================================================================== */

typedef struct { vec3_t origin, dir; } ray_t;

static vec3_t ray_at(ray_t r, float t) {
    return v3_add(r.origin, v3_scale(r.dir, t));
}

/* ================================================================== */
/* Materials                                                           */
/* ================================================================== */

typedef enum { MAT_LAMBERTIAN, MAT_METAL, MAT_DIELECTRIC } mat_type_t;

typedef struct {
    mat_type_t type;
    vec3_t     albedo;
    float      fuzz;
    float      ri;
} material_t;

static material_t mat_lambertian(vec3_t albedo) {
    material_t m;
    m.type   = MAT_LAMBERTIAN;
    m.albedo = albedo;
    m.fuzz   = 0.0f;
    m.ri     = 0.0f;
    return m;
}

static material_t mat_metal(vec3_t albedo, float fuzz) {
    material_t m;
    m.type   = MAT_METAL;
    m.albedo = albedo;
    m.fuzz   = fuzz < 1.0f ? fuzz : 1.0f;
    m.ri     = 0.0f;
    return m;
}

static material_t mat_dielectric(float ri) {
    material_t m;
    m.type   = MAT_DIELECTRIC;
    m.albedo = v3(0.0f, 0.0f, 0.0f);
    m.fuzz   = 0.0f;
    m.ri     = ri;
    return m;
}

/* ================================================================== */
/* Sphere and hit record                                               */
/* ================================================================== */

typedef struct {
    vec3_t     center;
    float      radius;
    material_t mat;
} sphere_t;

typedef struct {
    vec3_t     p;
    vec3_t     normal;
    material_t mat;
    float      t;
    int        front_face;
} hit_record_t;

static void set_face_normal(hit_record_t* rec, ray_t r, vec3_t outward_n) {
    rec->front_face = v3_dot(r.dir, outward_n) < 0.0f;
    rec->normal     = rec->front_face ? outward_n : v3_neg(outward_n);
}

/* ================================================================== */
/* Scene storage                                                       */
/* ================================================================== */

static sphere_t g_spheres[MAX_SPHERES];
static int      g_sphere_count;

static void scene_add(vec3_t center, float radius, material_t mat) {
    if (g_sphere_count < MAX_SPHERES) {
        sphere_t* s = &g_spheres[g_sphere_count++];
        s->center = center;
        s->radius = radius < 0.0f ? 0.0f : radius;
        s->mat    = mat;
    }
}

/* ================================================================== */
/* Ray-sphere intersection                                             */
/* ================================================================== */

static int hit_sphere(ray_t r, const sphere_t* s, float t_min, float t_max,
                      hit_record_t* rec) {
    vec3_t oc   = v3_sub(s->center, r.origin);
    float  a    = v3_len2(r.dir);
    float  h    = v3_dot(r.dir, oc);
    float  c    = v3_len2(oc) - s->radius * s->radius;
    float  disc = h*h - a*c;
    if (disc < 0.0f) return 0;

    float sqrtd = sqrtf(disc);
    float root  = (h - sqrtd) / a;
    if (root <= t_min || root >= t_max) {
        root = (h + sqrtd) / a;
        if (root <= t_min || root >= t_max) return 0;
    }

    rec->t   = root;
    rec->p   = ray_at(r, root);
    rec->mat = s->mat;
    vec3_t outward_n = v3_scale(v3_sub(rec->p, s->center), 1.0f / s->radius);
    set_face_normal(rec, r, outward_n);
    return 1;
}

static int hit_world(ray_t r, float t_min, float t_max, hit_record_t* out) {
    hit_record_t tmp;
    int          found   = 0;
    float        closest = t_max;
    for (int i = 0; i < g_sphere_count; ++i) {
        if (hit_sphere(r, &g_spheres[i], t_min, closest, &tmp)) {
            found   = 1;
            closest = tmp.t;
            *out    = tmp;
        }
    }
    return found;
}

/* ================================================================== */
/* Material scattering                                                 */
/* ================================================================== */

static float schlick(float cosine, float ri) {
    float r0 = (1.0f - ri) / (1.0f + ri);
    r0       = r0 * r0;
    float x  = 1.0f - cosine;
    return r0 + (1.0f - r0) * (x * x * x * x * x);
}

static int scatter(material_t mat, ray_t r_in, hit_record_t* rec,
                   vec3_t* atten, ray_t* scattered) {
    if (mat.type == MAT_LAMBERTIAN) {
        vec3_t dir = v3_add(rec->normal, rand_unit_vector());
        if (v3_near_zero(dir)) dir = rec->normal;
        scattered->origin = rec->p;
        scattered->dir    = dir;
        *atten            = mat.albedo;
        return 1;
    }

    if (mat.type == MAT_METAL) {
        vec3_t reflected = v3_reflect(v3_unit(r_in.dir), rec->normal);
        if (mat.fuzz > 0.0f)
            reflected = v3_add(reflected, v3_scale(rand_unit_vector(), mat.fuzz));
        scattered->origin = rec->p;
        scattered->dir    = reflected;
        *atten            = mat.albedo;
        return v3_dot(scattered->dir, rec->normal) > 0.0f;
    }

    if (mat.type == MAT_DIELECTRIC) {
        *atten          = v3(1.0f, 1.0f, 1.0f);
        float ri_ratio  = rec->front_face ? (1.0f / mat.ri) : mat.ri;
        vec3_t unit_dir = v3_unit(r_in.dir);
        float cos_theta = fminf(v3_dot(v3_neg(unit_dir), rec->normal), 1.0f);
        float sin_theta = sqrtf(1.0f - cos_theta * cos_theta);
        int must_reflect = ri_ratio * sin_theta > 1.0f;

        vec3_t direction;
        if (must_reflect || schlick(cos_theta, ri_ratio) > rand_f())
            direction = v3_reflect(unit_dir, rec->normal);
        else
            direction = v3_refract(unit_dir, rec->normal, ri_ratio);

        scattered->origin = rec->p;
        scattered->dir    = direction;
        return 1;
    }

    return 0;
}

/* ================================================================== */
/* Path tracer (iterative -- avoids deep call-stack recursion)        */
/* ================================================================== */

static vec3_t sky_color(vec3_t dir) {
    vec3_t ud = v3_unit(dir);
    float  a  = 0.5f * (ud.y + 1.0f);
    return v3_add(v3_scale(v3(1.0f, 1.0f, 1.0f), 1.0f - a),
                  v3_scale(v3(0.5f, 0.7f, 1.0f), a));
}

static vec3_t ray_color(ray_t r) {
    vec3_t color      = v3(0.0f, 0.0f, 0.0f);
    vec3_t throughput = v3(1.0f, 1.0f, 1.0f);

    for (int depth = 0; depth < MAX_DEPTH; ++depth) {
        hit_record_t rec;
        if (!hit_world(r, 0.001f, 1.0e30f, &rec)) {
            color = v3_add(color, v3_mul(throughput, sky_color(r.dir)));
            break;
        }

        vec3_t atten;
        ray_t  scattered;
        if (!scatter(rec.mat, r, &rec, &atten, &scattered))
            break;

        throughput = v3_mul(throughput, atten);
        r          = scattered;
    }

    return color;
}

/* ================================================================== */
/* Camera                                                              */
/* ================================================================== */

typedef struct {
    vec3_t center;
    vec3_t pixel00_loc;
    vec3_t pixel_delta_u;
    vec3_t pixel_delta_v;
    vec3_t defocus_disk_u;
    vec3_t defocus_disk_v;
    float  defocus_angle;
} camera_t;

static camera_t camera_init(
    int img_w, int img_h,
    vec3_t lookfrom, vec3_t lookat, vec3_t vup,
    float vfov, float defocus_angle, float focus_dist)
{
    camera_t cam;
    cam.center        = lookfrom;
    cam.defocus_angle = defocus_angle;

    float theta      = degrees_to_radians(vfov);
    float h          = tanf(theta * 0.5f);
    float viewport_h = 2.0f * h * focus_dist;
    float viewport_w = viewport_h * ((float)img_w / (float)img_h);

    vec3_t w = v3_unit(v3_sub(lookfrom, lookat));
    vec3_t u = v3_unit(v3_cross(vup, w));
    vec3_t v = v3_cross(w, u);

    vec3_t viewport_u = v3_scale(u, viewport_w);
    vec3_t viewport_v = v3_scale(v3_neg(v), viewport_h);

    cam.pixel_delta_u = v3_scale(viewport_u, 1.0f / img_w);
    cam.pixel_delta_v = v3_scale(viewport_v, 1.0f / img_h);

    vec3_t vp_upper_left = v3_sub(
        v3_sub(v3_sub(lookfrom, v3_scale(w, focus_dist)),
               v3_scale(viewport_u, 0.5f)),
        v3_scale(viewport_v, 0.5f));
    cam.pixel00_loc = v3_add(vp_upper_left,
        v3_scale(v3_add(cam.pixel_delta_u, cam.pixel_delta_v), 0.5f));

    float defocus_r    = focus_dist * tanf(degrees_to_radians(defocus_angle * 0.5f));
    cam.defocus_disk_u = v3_scale(u, defocus_r);
    cam.defocus_disk_v = v3_scale(v, defocus_r);

    return cam;
}

static ray_t camera_get_ray(const camera_t* cam, int i, int j) {
    float off_x = rand_f() - 0.5f;
    float off_y = rand_f() - 0.5f;
    vec3_t pixel_sample = v3_add(cam->pixel00_loc,
        v3_add(v3_scale(cam->pixel_delta_u, (float)i + off_x),
               v3_scale(cam->pixel_delta_v, (float)j + off_y)));

    vec3_t origin;
    if (cam->defocus_angle <= 0.0f) {
        origin = cam->center;
    } else {
        vec3_t p = rand_in_unit_disk();
        origin = v3_add(cam->center,
            v3_add(v3_scale(cam->defocus_disk_u, p.x),
                   v3_scale(cam->defocus_disk_v, p.y)));
    }

    ray_t r;
    r.origin = origin;
    r.dir    = v3_sub(pixel_sample, origin);
    return r;
}

/* ================================================================== */
/* Output helpers                                                      */
/* ================================================================== */

/*
 * Proper sRGB transfer function.
 * Converts a linear [0,1] light value to an 8-bit gamma-corrected value.
 * This is more accurate than the gamma-2 sqrt approximation: dark tones
 * are linearised below 0.0031308, and the standard power curve (1/2.4)
 * is applied above that threshold.
 */
static unsigned char linear_to_srgb(float v) {
    if (v <= 0.0f)           return 0;
    if (v >= 1.0f)           return 255;
    float enc;
    if (v <= 0.0031308f)
        enc = 12.92f * v;
    else
        enc = 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (unsigned char)(enc * 255.0f + 0.5f);
}

static vk_u32 pack_pixel(unsigned char r, unsigned char g, unsigned char b) {
    return ((vk_u32)r << 16) | ((vk_u32)g << 8) | (vk_u32)b;
}

/*
 * Write a pixel from the float accumulation buffer to the framebuffer.
 * acc_r/g/b are the summed raw radiance values; total_samples is the
 * number of samples accumulated so far.
 */
static void flush_pixel(vk_u32* pixels, vk_usize stride,
                        vk_u32 x, vk_u32 y,
                        float acc_r, float acc_g, float acc_b,
                        int total_samples)
{
    float inv = 1.0f / (float)total_samples;
    unsigned char R = linear_to_srgb(acc_r * inv);
    unsigned char G = linear_to_srgb(acc_g * inv);
    unsigned char B = linear_to_srgb(acc_b * inv);
    pixels[(vk_usize)y * stride + x] = pack_pixel(R, G, B);
}

/* ================================================================== */
/* Scene: RTIOW final render                                          */
/* ================================================================== */

static void build_scene(void) {
    g_sphere_count = 0;

    scene_add(v3(0.0f, -1000.0f, 0.0f), 1000.0f,
              mat_lambertian(v3(0.5f, 0.5f, 0.5f)));

    for (int a = -11; a < 11; ++a) {
        for (int b = -11; b < 11; ++b) {
            float  choose = rand_f();
            vec3_t center = v3((float)a + 0.9f * rand_f(),
                                0.2f,
                               (float)b + 0.9f * rand_f());

            if (v3_len(v3_sub(center, v3(4.0f, 0.2f, 0.0f))) <= 0.9f)
                continue;

            material_t mat;
            if (choose < 0.8f) {
                mat = mat_lambertian(v3_mul(rand_v3(), rand_v3()));
            } else if (choose < 0.95f) {
                mat = mat_metal(rand_v3_range(0.5f, 1.0f),
                                rand_range(0.0f, 0.5f));
            } else {
                mat = mat_dielectric(1.5f);
            }
            scene_add(center, 0.2f, mat);
        }
    }

    scene_add(v3( 0.0f, 1.0f, 0.0f), 1.0f, mat_dielectric(1.5f));
    scene_add(v3(-4.0f, 1.0f, 0.0f), 1.0f, mat_lambertian(v3(0.4f, 0.2f, 0.1f)));
    scene_add(v3( 4.0f, 1.0f, 0.0f), 1.0f, mat_metal(v3(0.7f, 0.6f, 0.5f), 0.0f));
}

static void clear_screen(vk_framebuffer_info_t *fb) {
    if (fb->valid && fb->base != 0 && fb->width > 0 && fb->height > 0) {
        vk_u32* pixels = (vk_u32*)(unsigned long long)fb->base;
        for (vk_u32 j = 0; j < fb->height; ++j)
            for (vk_u32 i = 0; i < fb->width; ++i)
                pixels[(vk_usize)j * fb->stride + i] = 0x00000000u;
    }
}

/* ================================================================== */
/* Float accumulation buffer                                           */
/*                                                                     */
/* We keep a per-pixel (R, G, B) float accumulator on the stack (or   */
/* in BSS if large).  Each progressive pass adds SAMPLES_PER_PASS new  */
/* samples to every pixel, then converts to sRGB and flushes the full  */
/* framebuffer so the image visibly sharpens over time.               */
/*                                                                     */
/* Maximum supported resolution: 4096 x 2160 (8 M pixels, 96 MB).    */
/* Increase if your kernel provides more heap/BSS space.              */
/* ================================================================== */

#define ACC_MAX_W  4096
#define ACC_MAX_H  2160

/*
 * Stored as three separate planes so the compiler can vectorise
 * the per-channel arithmetic more easily.
 */
static float g_acc_r[ACC_MAX_W * ACC_MAX_H];
static float g_acc_g[ACC_MAX_W * ACC_MAX_H];
static float g_acc_b[ACC_MAX_W * ACC_MAX_H];

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */

int main(char **argv, int argc) {

    VK_CALL(puts, "Ray Tracing in One Weekend -- High-Quality Render\n");
    VK_CALL(puts, "  Total samples : "); VK_CALL(put_dec, SAMPLES_PER_PIXEL); VK_CALL(puts, "\n");
    VK_CALL(puts, "  Samples/pass  : "); VK_CALL(put_dec, SAMPLES_PER_PASS);  VK_CALL(puts, "\n");
    VK_CALL(puts, "  Max depth     : "); VK_CALL(put_dec, MAX_DEPTH);          VK_CALL(puts, "\n");

    vk_framebuffer_info_t fb = { 0 };
    VK_CALL(framebuffer_info, &fb);

    if (!fb.valid || fb.base == 0 || fb.width == 0 || fb.height == 0) {
        VK_CALL(puts, "  No framebuffer available.\n");
        return 1;
    }

    clear_screen(&fb);

    /* Render at full framebuffer resolution (scale = 1). */
    vk_u32 render_w = fb.width;
    vk_u32 render_h = fb.height;

    /* Guard against a resolution larger than our accumulation buffer. */
    if (render_w > ACC_MAX_W) render_w = ACC_MAX_W;
    if (render_h > ACC_MAX_H) render_h = ACC_MAX_H;
    if (render_w == 0u) render_w = 1u;
    if (render_h == 0u) render_h = 1u;

    VK_CALL(puts, "  Framebuffer   : ");
    VK_CALL(put_dec, fb.width);  VK_CALL(puts, "x");
    VK_CALL(put_dec, fb.height); VK_CALL(puts, "\n");

    VK_CALL(puts, "  Render size   : ");
    VK_CALL(put_dec, render_w);  VK_CALL(puts, "x");
    VK_CALL(put_dec, render_h);  VK_CALL(puts, "\n");

    build_scene();
    VK_CALL(puts, "  Spheres       : ");
    VK_CALL(put_dec, (vk_u64)g_sphere_count);
    VK_CALL(puts, "\n");

    camera_t cam = camera_init(
        (int)render_w, (int)render_h,
        v3(13.0f, 2.0f,  3.0f),   /* lookfrom      */
        v3( 0.0f, 0.0f,  0.0f),   /* lookat        */
        v3( 0.0f, 1.0f,  0.0f),   /* vup           */
        20.0f,                      /* vfov (deg)    */
        0.6f,                       /* defocus_angle */
        10.0f                       /* focus_dist    */
    );

    vk_u32* pixels = (vk_u32*)(unsigned long long)fb.base;

    /* Zero the accumulation buffer. */
    vk_usize npixels = (vk_usize)render_w * render_h;
    for (vk_usize k = 0; k < npixels; ++k) {
        g_acc_r[k] = 0.0f;
        g_acc_g[k] = 0.0f;
        g_acc_b[k] = 0.0f;
    }

    /* ----------------------------------------------------------------
     * Progressive multi-pass render loop.
     *
     * Each outer iteration (pass) fires SAMPLES_PER_PASS new rays per
     * pixel, accumulates into g_acc_*, then converts and flushes the
     * entire framebuffer so you can see the noise level drop in real
     * time.
     * ---------------------------------------------------------------- */

    int total_passes = SAMPLES_PER_PIXEL / SAMPLES_PER_PASS;
    if (total_passes < 1) total_passes = 1;

    for (int pass = 0; pass < total_passes; ++pass) {

        // VK_CALL(puts, "  Pass ");
        // VK_CALL(put_dec, (vk_u64)(pass + 1));
        // VK_CALL(puts, " / ");
        // VK_CALL(put_dec, (vk_u64)total_passes);
        // VK_CALL(puts, " ...\n");

        /* -- Accumulate new samples for this pass -- */
        for (vk_u32 j = 0; j < render_h; ++j) {

            // /* Progress dot every PROGRESS_INTERVAL scanlines. */
            // if ((j % PROGRESS_INTERVAL) == 0) {
            //     VK_CALL(puts, "    row ");
            //     VK_CALL(put_dec, (vk_u64)j);
            //     VK_CALL(puts, " / ");
            //     VK_CALL(put_dec, (vk_u64)render_h);
            //     VK_CALL(puts, "\n");
            // }

            for (vk_u32 i = 0; i < render_w; ++i) {
                vk_usize idx = (vk_usize)j * render_w + i;

                for (int s = 0; s < SAMPLES_PER_PASS; ++s) {
                    ray_t r = camera_get_ray(&cam, (int)i, (int)j);
                    vec3_t c = ray_color(r);
                    g_acc_r[idx] += c.x;
                    g_acc_g[idx] += c.y;
                    g_acc_b[idx] += c.z;
                }
            }
        }

        /* -- Flush the whole framebuffer with current sample count -- */
        int samples_so_far = (pass + 1) * SAMPLES_PER_PASS;

        for (vk_u32 j = 0; j < render_h; ++j) {
            for (vk_u32 i = 0; i < render_w; ++i) {
                vk_usize idx = (vk_usize)j * render_w + i;
                flush_pixel(pixels, fb.stride,
                            i, j,
                            g_acc_r[idx], g_acc_g[idx], g_acc_b[idx],
                            samples_so_far);
            }
        }
    }

    return 0;
}