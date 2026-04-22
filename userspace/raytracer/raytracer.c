/*
 * vkernel userspace - path tracer demo
 * Adapted from "Ray Tracing in One Weekend" by Shirley, Black, Hollasch.
 * https://raytracing.github.io/books/RayTracingInOneWeekend.html
 *
 * raytracer.c - High-quality progressive render with double-buffering
 *               and on-screen HUD.
 *
 * Double-buffer design
 * ────────────────────
 *  g_back[]     vk_u32 pixel back-buffer in BSS, flat stride = render_w.
 *               The accumulation pass and HUD renderer write ONLY here.
 *               The hardware framebuffer (fb.base) is never written
 *               mid-scanline.
 *
 *  blit_row()   Copies one row of g_back → fb, honouring fb.stride.
 *               Called after every scanline so the image builds visibly
 *               from top to bottom with no tearing.
 *
 *  blit_all()   Full-screen blit used after each pass to push the
 *               freshly-rendered HUD bar.
 *
 *  HUD          Drawn into g_back using the glyph engine from
 *               framebuffer_text.c (adapted to write to g_back).
 *               Shows: resolution, pass N/total, SPP, current scanline.
 *               No VK_CALL(puts/put_dec) is used after rendering starts.
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

#ifdef _MSC_VER
int _fltused = 0;
#endif

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

#define SAMPLES_PER_PIXEL  256   /* total accumulated samples per pixel  */
#define SAMPLES_PER_PASS   4     /* samples added per progressive pass   */
#define MAX_DEPTH          16    /* maximum ray-bounce depth             */
#define MAX_SPHERES        520   /* upper bound on scene spheres         */

/* Back-buffer / accumulation buffer size cap.
   At 1920x1080: 3 float planes = ~24 MB, 1 vk_u32 plane = ~8 MB.
   Adjust ACC_MAX_* down if your kernel BSS budget is tighter.         */
#define ACC_MAX_W  1920
#define ACC_MAX_H  1080

/* HUD bar height in pixels at the bottom of the image. */
#define HUD_H  32

/* ================================================================== */
/* vec3                                                                */
/* ================================================================== */

typedef struct { float x, y, z; } vec3_t;

static vec3_t v3(float x, float y, float z) {
    vec3_t r; r.x=x; r.y=y; r.z=z; return r;
}
static vec3_t v3_add  (vec3_t a, vec3_t b){ return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static vec3_t v3_sub  (vec3_t a, vec3_t b){ return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static vec3_t v3_mul  (vec3_t a, vec3_t b){ return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static vec3_t v3_scale(vec3_t v, float s) { return v3(v.x*s,   v.y*s,   v.z*s  ); }
static vec3_t v3_neg  (vec3_t v)          { return v3(-v.x,   -v.y,    -v.z   ); }
static float  v3_dot  (vec3_t a, vec3_t b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static float  v3_len2 (vec3_t v)          { return v3_dot(v,v); }
static float  v3_len  (vec3_t v)          { return sqrtf(v3_len2(v)); }

static vec3_t v3_unit(vec3_t v){
    float l = v3_len(v);
    return l > 0.0f ? v3_scale(v, 1.0f/l) : v3(0,0,0);
}
static vec3_t v3_cross(vec3_t u, vec3_t v){
    return v3(u.y*v.z - u.z*v.y,
              u.z*v.x - u.x*v.z,
              u.x*v.y - u.y*v.x);
}
static int v3_near_zero(vec3_t v){
    const float e=1e-8f;
    return fabsf(v.x)<e && fabsf(v.y)<e && fabsf(v.z)<e;
}
static vec3_t v3_reflect(vec3_t v, vec3_t n){
    return v3_sub(v, v3_scale(n, 2.0f*v3_dot(v,n)));
}
static vec3_t v3_refract(vec3_t uv, vec3_t n, float etai_over_etat){
    float cos_theta = fminf(v3_dot(v3_neg(uv),n), 1.0f);
    vec3_t r_perp   = v3_scale(v3_add(uv, v3_scale(n,cos_theta)), etai_over_etat);
    float para_len2 = 1.0f - v3_len2(r_perp);
    vec3_t r_para   = v3_scale(n, -sqrtf(fabsf(para_len2)));
    return v3_add(r_perp, r_para);
}

/* ================================================================== */
/* RNG  (xorshift32)                                                   */
/* ================================================================== */

static unsigned int g_rng = 0x853C49E6u;

static float rand_f(void){
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng >> 8) * (1.0f/16777216.0f);
}
static float  rand_range(float lo, float hi){ return lo+(hi-lo)*rand_f(); }
static vec3_t rand_v3(void){ return v3(rand_f(),rand_f(),rand_f()); }
static vec3_t rand_v3_range(float lo, float hi){
    return v3(rand_range(lo,hi), rand_range(lo,hi), rand_range(lo,hi));
}
static vec3_t rand_unit_vector(void){
    for(;;){
        vec3_t p = rand_v3_range(-1.0f, 1.0f);
        float lsq = v3_len2(p);
        if(lsq > 1e-30f && lsq <= 1.0f) return v3_scale(p, 1.0f/sqrtf(lsq));
    }
}
static vec3_t rand_in_unit_disk(void){
    for(;;){
        vec3_t p = v3(rand_range(-1,1), rand_range(-1,1), 0);
        if(v3_len2(p) < 1.0f) return p;
    }
}

/* ================================================================== */
/* Ray                                                                 */
/* ================================================================== */

typedef struct { vec3_t origin, dir; } ray_t;
static vec3_t ray_at(ray_t r, float t){ return v3_add(r.origin, v3_scale(r.dir,t)); }

/* ================================================================== */
/* Materials                                                           */
/* ================================================================== */

typedef enum { MAT_LAMBERTIAN, MAT_METAL, MAT_DIELECTRIC } mat_type_t;
typedef struct {
    mat_type_t type; vec3_t albedo; float fuzz, ri;
} material_t;

static material_t mat_lambertian(vec3_t albedo){
    material_t m; m.type=MAT_LAMBERTIAN; m.albedo=albedo; m.fuzz=0; m.ri=0; return m;
}
static material_t mat_metal(vec3_t albedo, float fuzz){
    material_t m; m.type=MAT_METAL; m.albedo=albedo;
    m.fuzz=fuzz<1.0f?fuzz:1.0f; m.ri=0; return m;
}
static material_t mat_dielectric(float ri){
    material_t m; m.type=MAT_DIELECTRIC; m.albedo=v3(0,0,0); m.fuzz=0; m.ri=ri; return m;
}

/* ================================================================== */
/* Sphere / hit record                                                 */
/* ================================================================== */

typedef struct { vec3_t center; float radius; material_t mat; } sphere_t;
typedef struct {
    vec3_t p, normal; material_t mat; float t; int front_face;
} hit_record_t;

static void set_face_normal(hit_record_t* rec, ray_t r, vec3_t outward_n){
    rec->front_face = v3_dot(r.dir, outward_n) < 0.0f;
    rec->normal     = rec->front_face ? outward_n : v3_neg(outward_n);
}

/* ================================================================== */
/* Scene                                                               */
/* ================================================================== */

static sphere_t g_spheres[MAX_SPHERES];
static int      g_sphere_count;

static void scene_add(vec3_t center, float radius, material_t mat){
    if(g_sphere_count < MAX_SPHERES){
        sphere_t* s = &g_spheres[g_sphere_count++];
        s->center = center;
        s->radius = radius < 0.0f ? 0.0f : radius;
        s->mat    = mat;
    }
}

static int hit_sphere(ray_t r, const sphere_t* s, float t_min, float t_max,
                      hit_record_t* rec){
    vec3_t oc   = v3_sub(s->center, r.origin);
    float  a    = v3_len2(r.dir);
    float  h    = v3_dot(r.dir, oc);
    float  c    = v3_len2(oc) - s->radius*s->radius;
    float  disc = h*h - a*c;
    if(disc < 0.0f) return 0;
    float sqrtd = sqrtf(disc);
    float root  = (h - sqrtd) / a;
    if(root <= t_min || root >= t_max){
        root = (h + sqrtd) / a;
        if(root <= t_min || root >= t_max) return 0;
    }
    rec->t = root;
    rec->p = ray_at(r, root);
    rec->mat = s->mat;
    set_face_normal(rec, r, v3_scale(v3_sub(rec->p, s->center), 1.0f/s->radius));
    return 1;
}

static int hit_world(ray_t r, float t_min, float t_max, hit_record_t* out){
    hit_record_t tmp; int found=0; float closest=t_max;
    for(int i=0; i<g_sphere_count; ++i)
        if(hit_sphere(r, &g_spheres[i], t_min, closest, &tmp)){
            found=1; closest=tmp.t; *out=tmp;
        }
    return found;
}

/* ================================================================== */
/* Material scattering                                                 */
/* ================================================================== */

static float schlick(float cosine, float ri){
    float r0 = (1.0f-ri)/(1.0f+ri); r0=r0*r0;
    float x  = 1.0f-cosine;
    return r0 + (1.0f-r0)*(x*x*x*x*x);
}

static int scatter(material_t mat, ray_t r_in, hit_record_t* rec,
                   vec3_t* atten, ray_t* scattered){
    if(mat.type == MAT_LAMBERTIAN){
        vec3_t dir = v3_add(rec->normal, rand_unit_vector());
        if(v3_near_zero(dir)) dir = rec->normal;
        scattered->origin = rec->p; scattered->dir = dir;
        *atten = mat.albedo; return 1;
    }
    if(mat.type == MAT_METAL){
        vec3_t ref = v3_reflect(v3_unit(r_in.dir), rec->normal);
        if(mat.fuzz > 0.0f) ref = v3_add(ref, v3_scale(rand_unit_vector(), mat.fuzz));
        scattered->origin = rec->p; scattered->dir = ref;
        *atten = mat.albedo;
        return v3_dot(scattered->dir, rec->normal) > 0.0f;
    }
    if(mat.type == MAT_DIELECTRIC){
        *atten = v3(1,1,1);
        float ri_ratio  = rec->front_face ? (1.0f/mat.ri) : mat.ri;
        vec3_t unit_dir = v3_unit(r_in.dir);
        float cos_theta = fminf(v3_dot(v3_neg(unit_dir), rec->normal), 1.0f);
        float sin_theta = sqrtf(1.0f - cos_theta*cos_theta);
        vec3_t direction;
        if(ri_ratio*sin_theta > 1.0f || schlick(cos_theta, ri_ratio) > rand_f())
            direction = v3_reflect(unit_dir, rec->normal);
        else
            direction = v3_refract(unit_dir, rec->normal, ri_ratio);
        scattered->origin = rec->p; scattered->dir = direction; return 1;
    }
    return 0;
}

/* ================================================================== */
/* Path tracer  (iterative — no deep call stack)                      */
/* ================================================================== */

static vec3_t sky_color(vec3_t dir){
    vec3_t ud = v3_unit(dir);
    float  a  = 0.5f*(ud.y + 1.0f);
    return v3_add(v3_scale(v3(1,1,1), 1.0f-a), v3_scale(v3(0.5f,0.7f,1.0f), a));
}

static vec3_t ray_color(ray_t r){
    vec3_t color=v3(0,0,0), throughput=v3(1,1,1);
    for(int depth=0; depth<MAX_DEPTH; ++depth){
        hit_record_t rec;
        if(!hit_world(r, 0.001f, 1.0e30f, &rec)){
            color = v3_add(color, v3_mul(throughput, sky_color(r.dir))); break;
        }
        vec3_t atten; ray_t scattered;
        if(!scatter(rec.mat, r, &rec, &atten, &scattered)) break;
        throughput = v3_mul(throughput, atten);
        r = scattered;
    }
    return color;
}

/* ================================================================== */
/* Camera                                                              */
/* ================================================================== */

typedef struct {
    vec3_t center, pixel00_loc, pixel_delta_u, pixel_delta_v;
    vec3_t defocus_disk_u, defocus_disk_v;
    float  defocus_angle;
} camera_t;

static camera_t camera_init(int img_w, int img_h,
    vec3_t lookfrom, vec3_t lookat, vec3_t vup,
    float vfov, float defocus_angle, float focus_dist)
{
    camera_t cam;
    cam.center = lookfrom; cam.defocus_angle = defocus_angle;
    float theta      = degrees_to_radians(vfov);
    float h          = tanf(theta*0.5f);
    float viewport_h = 2.0f*h*focus_dist;
    float viewport_w = viewport_h*((float)img_w/(float)img_h);
    vec3_t w = v3_unit(v3_sub(lookfrom, lookat));
    vec3_t u = v3_unit(v3_cross(vup, w));
    vec3_t v = v3_cross(w, u);
    vec3_t viewport_u = v3_scale(u, viewport_w);
    vec3_t viewport_v = v3_scale(v3_neg(v), viewport_h);
    cam.pixel_delta_u = v3_scale(viewport_u, 1.0f/img_w);
    cam.pixel_delta_v = v3_scale(viewport_v, 1.0f/img_h);
    vec3_t vp_ul = v3_sub(v3_sub(v3_sub(lookfrom, v3_scale(w,focus_dist)),
                                  v3_scale(viewport_u,0.5f)),
                           v3_scale(viewport_v,0.5f));
    cam.pixel00_loc = v3_add(vp_ul,
        v3_scale(v3_add(cam.pixel_delta_u, cam.pixel_delta_v), 0.5f));
    float defocus_r    = focus_dist * tanf(degrees_to_radians(defocus_angle*0.5f));
    cam.defocus_disk_u = v3_scale(u, defocus_r);
    cam.defocus_disk_v = v3_scale(v, defocus_r);
    return cam;
}

static ray_t camera_get_ray(const camera_t* cam, int i, int j){
    float off_x = rand_f()-0.5f, off_y = rand_f()-0.5f;
    vec3_t pixel_sample = v3_add(cam->pixel00_loc,
        v3_add(v3_scale(cam->pixel_delta_u, (float)i+off_x),
               v3_scale(cam->pixel_delta_v, (float)j+off_y)));
    vec3_t origin;
    if(cam->defocus_angle <= 0.0f){
        origin = cam->center;
    } else {
        vec3_t p = rand_in_unit_disk();
        origin = v3_add(cam->center,
            v3_add(v3_scale(cam->defocus_disk_u, p.x),
                   v3_scale(cam->defocus_disk_v, p.y)));
    }
    ray_t r; r.origin=origin; r.dir=v3_sub(pixel_sample,origin); return r;
}

/* ================================================================== */
/* Pixel format + colour helpers                                       */
/* ================================================================== */

/*
 * Format-aware packer — mirrors framebuffer_text.c so the HUD glyphs
 * and the ray-traced image use the same colour encoding.
 */
static vk_u32 pack_pixel(unsigned char r, unsigned char g, unsigned char b, vk_pixel_format_t fmt){
    return ((vk_u32)r<<16)|((vk_u32)g<<8)|(vk_u32)b;
}

/* Proper IEC 61966-2-1 sRGB transfer function. */
static unsigned char linear_to_srgb(float v){
    if(v <= 0.0f) return 0;
    if(v >= 1.0f) return 255;
    float enc = (v <= 0.0031308f) ? 12.92f*v : 1.055f*powf(v,1.0f/2.4f)-0.055f;
    return (unsigned char)(enc*255.0f + 0.5f);
}

/* ================================================================== */
/* Static buffers  (BSS — zeroed by the kernel loader)                */
/* ================================================================== */

static vk_u32 g_back [ACC_MAX_W * ACC_MAX_H];  /* vk_u32 back-buffer        */
static float  g_acc_r[ACC_MAX_W * ACC_MAX_H];  /* per-pixel radiance sum R  */
static float  g_acc_g[ACC_MAX_W * ACC_MAX_H];  /* per-pixel radiance sum G  */
static float  g_acc_b[ACC_MAX_W * ACC_MAX_H];  /* per-pixel radiance sum B  */

/* Set once in main(), used by all helpers. */
static vk_u32 g_render_w;
static vk_u32 g_render_h;

/* ================================================================== */
/* Back-buffer draw primitives                                         */
/*                                                                     */
/* Everything draws into g_back[].  fb.base is never written except    */
/* inside blit_row() / blit_all().                                     */
/* ================================================================== */

static void bb_put_pixel(vk_u32 x, vk_u32 y, vk_u32 color){
    if(x < g_render_w && y < g_render_h)
        g_back[(vk_usize)y * g_render_w + x] = color;
}

static void bb_fill_rect(vk_u32 x, vk_u32 y, vk_u32 w, vk_u32 h, vk_u32 color){
    for(vk_u32 row=0; row<h; ++row)
        for(vk_u32 col=0; col<w; ++col)
            bb_put_pixel(x+col, y+row, color);
}

/* ================================================================== */
/* Glyph engine  (from framebuffer_text.c, extended for HUD strings)  */
/* ================================================================== */

typedef struct { char ch; unsigned char rows[8]; } glyph_entry_t;

static const glyph_entry_t k_font[] = {
    { ' ', { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* ---- uppercase ---- */
    { 'A', { 0x18,0x24,0x42,0x7E,0x42,0x42,0x42,0x00 } },
    { 'B', { 0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00 } },
    { 'C', { 0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00 } },
    { 'D', { 0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00 } },
    { 'E', { 0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00 } },
    { 'F', { 0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00 } },
    { 'G', { 0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00 } },
    { 'H', { 0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00 } },
    { 'I', { 0x3E,0x08,0x08,0x08,0x08,0x08,0x3E,0x00 } },
    { 'J', { 0x1E,0x04,0x04,0x04,0x04,0x44,0x38,0x00 } },
    { 'K', { 0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00 } },
    { 'L', { 0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00 } },
    { 'M', { 0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x00 } },
    { 'N', { 0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00 } },
    { 'O', { 0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00 } },
    { 'P', { 0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00 } },
    { 'Q', { 0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00 } },
    { 'R', { 0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00 } },
    { 'S', { 0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00 } },
    { 'T', { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 } },
    { 'U', { 0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00 } },
    { 'V', { 0x42,0x42,0x42,0x42,0x24,0x24,0x18,0x00 } },
    { 'W', { 0x42,0x42,0x42,0x5A,0x5A,0x66,0x42,0x00 } },
    { 'X', { 0x42,0x24,0x18,0x18,0x18,0x24,0x42,0x00 } },
    { 'Y', { 0x42,0x42,0x24,0x18,0x18,0x18,0x18,0x00 } },
    { 'Z', { 0x7E,0x02,0x04,0x18,0x20,0x40,0x7E,0x00 } },
    /* ---- digits ---- */
    { '0', { 0x3C,0x42,0x46,0x4A,0x52,0x62,0x3C,0x00 } },
    { '1', { 0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00 } },
    { '2', { 0x3C,0x42,0x02,0x0C,0x30,0x40,0x7E,0x00 } },
    { '3', { 0x7E,0x04,0x08,0x1C,0x02,0x42,0x3C,0x00 } },
    { '4', { 0x0C,0x14,0x24,0x44,0x7E,0x04,0x04,0x00 } },
    { '5', { 0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00 } },
    { '6', { 0x3C,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00 } },
    { '7', { 0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00 } },
    { '8', { 0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00 } },
    { '9', { 0x3C,0x42,0x42,0x3E,0x02,0x02,0x3C,0x00 } },
    /* ---- punctuation ---- */
    { ':', { 0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00 } },
    { '.', { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 } },
    { '/', { 0x02,0x04,0x08,0x10,0x20,0x40,0x00,0x00 } },
    { '-', { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 } },
    { '(', { 0x0C,0x10,0x20,0x20,0x20,0x10,0x0C,0x00 } },
    { ')', { 0x30,0x08,0x04,0x04,0x04,0x08,0x30,0x00 } },
    { '[', { 0x3C,0x20,0x20,0x20,0x20,0x20,0x3C,0x00 } },
    { ']', { 0x3C,0x04,0x04,0x04,0x04,0x04,0x3C,0x00 } },
    { '|', { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00 } },
    { '%', { 0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00 } },
    { '>', { 0x40,0x20,0x10,0x08,0x10,0x20,0x40,0x00 } },
    { '+', { 0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00 } },
    { '=', { 0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00 } },
};

static const unsigned int k_font_count = sizeof(k_font)/sizeof(k_font[0]);

static const unsigned char* glyph_for(char ch){
    for(unsigned int i=0; i<k_font_count; ++i)
        if(k_font[i].ch == ch) return k_font[i].rows;
    return k_font[0].rows; /* fallback: space */
}

/* Draw one 8×8 glyph into g_back. */
static void bb_draw_char(vk_u32 x, vk_u32 y, char ch, vk_u32 fg, vk_u32 bg){
    const unsigned char* g = glyph_for(ch);
    for(vk_u32 row=0; row<8; ++row){
        unsigned char bits = g[row];
        for(vk_u32 col=0; col<8; ++col)
            bb_put_pixel(x+col, y+row, (bits & (0x80u>>col)) ? fg : bg);
    }
}

/* Draw a NUL-terminated string into g_back. */
static void bb_draw_text(vk_u32 x, vk_u32 y, const char* text,
                         vk_u32 fg, vk_u32 bg){
    vk_u32 cx = x;
    for(const char* p=text; *p; ++p){ bb_draw_char(cx, y, *p, fg, bg); cx+=8; }
}

/* ================================================================== */
/* Freestanding integer → string helpers                              */
/* ================================================================== */

/* Write decimal of v into buf (>= 12 bytes), NUL-terminated. */
static char* u32_to_str(char* buf, vk_u32 v){
    char tmp[12]; int len=0;
    if(v==0){ tmp[len++]='0'; }
    else { while(v){ tmp[len++]='0'+(char)(v%10u); v/=10u; } }
    int i; for(i=0; i<len; ++i) buf[i]=tmp[len-1-i];
    buf[i]='\0'; return buf;
}

/* Append src to dst (freestanding strcat). */
static char* str_append(char* dst, const char* src){
    char* p=dst; while(*p) ++p;
    while(*src) *p++=*src++;
    *p='\0'; return dst;
}

/* ================================================================== */
/* Blit helpers  (ONLY place fb.base is written)                      */
/* ================================================================== */

/* Copy one back-buffer row to the hardware framebuffer. */
static void blit_row(vk_u32* fb_pixels, vk_u32 fb_stride, vk_u32 row){
    const vk_u32* src = g_back + (vk_usize)row * g_render_w;
    vk_u32*       dst = fb_pixels + (vk_usize)row * fb_stride;
    for(vk_u32 x=0; x<g_render_w; ++x) dst[x] = src[x];
}

/* Blit every row of g_back to the hardware framebuffer. */
static void blit_all(vk_u32* fb_pixels, vk_u32 fb_stride){
    for(vk_u32 row=0; row<g_render_h; ++row)
        blit_row(fb_pixels, fb_stride, row);
}

/* ================================================================== */
/* HUD renderer                                                        */
/*                                                                     */
/* Redraws the bottom HUD_H rows of g_back.  Two text lines:          */
/*   Line 0:  "RAY TRACER | WxH | PASS N/T | SPP N"                  */
/*   Line 1:  row progress label  +  graphical bar                    */
/*                                                                     */
/* cur_row == 0xFFFFFFFF means the pass is complete (bar goes gold).  */
/* ================================================================== */

static void hud_draw(vk_pixel_format_t fmt,
                     int pass, int total_passes, int samples_so_far,
                     vk_u32 cur_row)
{
    /* Palette. */
    vk_u32 c_bg     = pack_pixel( 10,  10,  18, fmt);
    vk_u32 c_border = pack_pixel( 64, 128, 200, fmt);
    vk_u32 c_text   = pack_pixel(220, 220, 220, fmt);
    vk_u32 c_hi     = pack_pixel( 96, 196, 255, fmt);
    vk_u32 c_bar_fg = pack_pixel( 64, 200,  96, fmt);
    vk_u32 c_bar_bg = pack_pixel( 30,  40,  50, fmt);
    vk_u32 c_done   = pack_pixel(255, 210,  64, fmt);

    vk_u32 bar_y = g_render_h - (vk_u32)HUD_H;
    vk_u32 w     = g_render_w;

    /* Background + top border. */
    bb_fill_rect(0, bar_y,    w, 1,          c_border);
    bb_fill_rect(0, bar_y+1,  w, HUD_H-1,   c_bg);

    /* ---- Top text line  (y = bar_y + 4) ---- */
    vk_u32 ty0 = bar_y + 4u;
    char   buf[128]; char tmp[12];
    buf[0] = '\0';

    str_append(buf, "RAY TRACER | ");
    str_append(buf, u32_to_str(tmp, g_render_w));
    str_append(buf, "X");
    str_append(buf, u32_to_str(tmp, g_render_h));
    str_append(buf, " | PASS ");
    str_append(buf, u32_to_str(tmp, (vk_u32)(pass+1)));
    str_append(buf, "/");
    str_append(buf, u32_to_str(tmp, (vk_u32)total_passes));
    str_append(buf, " | SPP ");
    str_append(buf, u32_to_str(tmp, (vk_u32)samples_so_far));

    bb_draw_text(8u, ty0, buf, c_hi, c_bg);

    /* ---- Bottom text + progress bar  (y = bar_y + 14) ---- */
    vk_u32 ty1  = bar_y + 14u;
    int    done = (cur_row == 0xFFFFFFFFu);

    /* Row counter label (10 chars wide = 80 px). */
    char lbl[32]; lbl[0]='\0';
    if(done){
        str_append(lbl, "DONE      ");
    } else {
        str_append(lbl, "ROW ");
        str_append(lbl, u32_to_str(tmp, cur_row));
        str_append(lbl, "/");
        str_append(lbl, u32_to_str(tmp, g_render_h - (vk_u32)HUD_H));
        /* Pad to fixed width so the bar doesn't jitter left/right. */
        vk_usize llen=0; while(lbl[llen]) ++llen;
        while(llen < 14){ lbl[llen++]=' '; lbl[llen]='\0'; }
    }
    bb_draw_text(8u, ty1, lbl, done ? c_done : c_text, c_bg);

    /* Graphical progress bar. */
    vk_u32 bar_x0 = 8u + 14u*8u;               /* right of label            */
    vk_u32 bar_x1 = w > 16u ? w-16u : w;        /* right margin              */
    if(bar_x1 > bar_x0 + 4u){
        vk_u32 bar_w      = bar_x1 - bar_x0;
        vk_u32 inner_w    = bar_w > 2u ? bar_w-2u : 0u;
        vk_u32 render_rows = (g_render_h > (vk_u32)HUD_H)
                           ? g_render_h - (vk_u32)HUD_H : 1u;
        vk_u32 filled;
        if(done){
            filled = inner_w;
        } else {
            filled = (vk_u32)(((vk_u64)cur_row * inner_w) / render_rows);
            if(filled > inner_w) filled = inner_w;
        }
        /* Border caps. */
        bb_fill_rect(bar_x0,             ty1, 1,       8u, c_border);
        bb_fill_rect(bar_x0+bar_w-1u,    ty1, 1,       8u, c_border);
        /* Filled portion. */
        if(filled > 0u)
            bb_fill_rect(bar_x0+1u,      ty1, filled,          8u,
                         done ? c_done : c_bar_fg);
        /* Empty portion. */
        if(inner_w > filled)
            bb_fill_rect(bar_x0+1u+filled, ty1, inner_w-filled, 8u, c_bar_bg);
    }
}

/* ================================================================== */
/* Scene builder                                                       */
/* ================================================================== */

static void build_scene(void){
    g_sphere_count = 0;
    scene_add(v3(0,-1000,0), 1000.0f, mat_lambertian(v3(0.5f,0.5f,0.5f)));
    for(int a=-11; a<11; ++a){
        for(int b=-11; b<11; ++b){
            float choose = rand_f();
            vec3_t center = v3((float)a+0.9f*rand_f(), 0.2f, (float)b+0.9f*rand_f());
            if(v3_len(v3_sub(center, v3(4,0.2f,0))) <= 0.9f) continue;
            material_t mat;
            if(choose < 0.8f)
                mat = mat_lambertian(v3_mul(rand_v3(), rand_v3()));
            else if(choose < 0.95f)
                mat = mat_metal(rand_v3_range(0.5f,1.0f), rand_range(0,0.5f));
            else
                mat = mat_dielectric(1.5f);
            scene_add(center, 0.2f, mat);
        }
    }
    scene_add(v3( 0,1,0), 1.0f, mat_dielectric(1.5f));
    scene_add(v3(-4,1,0), 1.0f, mat_lambertian(v3(0.4f,0.2f,0.1f)));
    scene_add(v3( 4,1,0), 1.0f, mat_metal(v3(0.7f,0.6f,0.5f), 0.0f));
}

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */

int main(char **argv, int argc){
    (void)argv; (void)argc;

    vk_framebuffer_info_t fb = {0};
    VK_CALL(framebuffer_info, &fb);

    if(!fb.valid || fb.base==0 || fb.width==0 || fb.height==0){
        /* Only safe VK_CALL: framebuffer not available, nothing to corrupt. */
        VK_CALL(puts, "raytracer: no framebuffer\n");
        return 1;
    }

    g_render_w = fb.width  < ACC_MAX_W ? fb.width  : ACC_MAX_W;
    g_render_h = fb.height < ACC_MAX_H ? fb.height : ACC_MAX_H;
    if(g_render_w < 1u) g_render_w = 1u;
    if(g_render_h < (vk_u32)HUD_H + 1u) g_render_h = (vk_u32)HUD_H + 1u;

    /*
     * Single serial log line emitted BEFORE the framebuffer is touched.
     * After this: zero VK_CALL(puts/put_dec) — the kernel console writes
     * directly to fb.base and would corrupt the rendered image.
     * All status from here on is rendered into g_back via the HUD.
     */
    VK_CALL(puts, "raytracer: starting — all progress shown on-screen\n");

    build_scene();

    camera_t cam = camera_init(
        (int)g_render_w, (int)g_render_h,
        v3(13,2,3),    /* lookfrom      */
        v3(0,0,0),     /* lookat        */
        v3(0,1,0),     /* vup           */
        20.0f,         /* vfov (deg)    */
        0.6f,          /* defocus_angle */
        10.0f          /* focus_dist    */
    );

    vk_u32* fb_pixels = (vk_u32*)(unsigned long long)fb.base;

    /* Zero accumulator and back-buffer, show black screen. */
    vk_usize npixels = (vk_usize)g_render_w * g_render_h;
    for(vk_usize k=0; k<npixels; ++k){
        g_back [k] = 0u;
        g_acc_r[k] = 0.0f;
        g_acc_g[k] = 0.0f;
        g_acc_b[k] = 0.0f;
    }
    blit_all(fb_pixels, fb.stride);

    int total_passes  = SAMPLES_PER_PIXEL / SAMPLES_PER_PASS;
    if(total_passes < 1) total_passes = 1;

    /*
     * render_rows: the number of image rows the path tracer fills.
     * The bottom HUD_H rows are reserved for the status bar and are
     * never overwritten by the accumulation pass.
     */
    vk_u32 render_rows = g_render_h - (vk_u32)HUD_H;

    for(int pass=0; pass<total_passes; ++pass){

        int samples_so_far = (pass+1) * SAMPLES_PER_PASS;

        /* Show the HUD immediately when the pass starts so the user
           sees "PASS N" before any pixels are traced.                */
        hud_draw(fb.format, pass, total_passes, samples_so_far - SAMPLES_PER_PASS, 0);
        blit_all(fb_pixels, fb.stride);

        /* ---- Accumulate + blit one scanline at a time ---- */
        for(vk_u32 j=0; j<render_rows; ++j){

            /* Trace all pixels in this scanline. */
            for(vk_u32 i=0; i<g_render_w; ++i){
                vk_usize idx = (vk_usize)j * g_render_w + i;

                for(int s=0; s<SAMPLES_PER_PASS; ++s){
                    ray_t r  = camera_get_ray(&cam, (int)i, (int)j);
                    vec3_t c = ray_color(r);
                    g_acc_r[idx] += c.x;
                    g_acc_g[idx] += c.y;
                    g_acc_b[idx] += c.z;
                }

                /* Tonemap and write into back-buffer. */
                float inv = 1.0f / (float)samples_so_far;
                g_back[idx] = pack_pixel(
                    linear_to_srgb(g_acc_r[idx]*inv),
                    linear_to_srgb(g_acc_g[idx]*inv),
                    linear_to_srgb(g_acc_b[idx]*inv),
                    fb.format);
            }

            /* Redraw HUD with updated row counter into g_back. */
            hud_draw(fb.format, pass, total_passes, samples_so_far, j);

            /* Push this image scanline to hardware. */
            blit_row(fb_pixels, fb.stride, j);

            /* Push the HUD rows to hardware (always HUD_H rows). */
            for(vk_u32 hy = g_render_h-(vk_u32)HUD_H; hy<g_render_h; ++hy)
                blit_row(fb_pixels, fb.stride, hy);
        }

        /* Pass complete — flip progress bar to gold "DONE" state. */
        hud_draw(fb.format, pass, total_passes, samples_so_far, 0xFFFFFFFFu);
        blit_all(fb_pixels, fb.stride);
    }

    /* Final frame: all passes done, bar fully gold. */
    hud_draw(fb.format,
             total_passes-1, total_passes,
             SAMPLES_PER_PIXEL,
             0xFFFFFFFFu);
    blit_all(fb_pixels, fb.stride);

    return 0;
}