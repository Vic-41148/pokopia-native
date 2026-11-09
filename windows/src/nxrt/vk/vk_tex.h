// Textures (docs/m5-vulkan.md §4): NVN texture objects -> VkImage / VkImageView. Images are shared by storage, shape, format and usage; textures whose
// content comes from guest memory (packaged layouts, CPU-written ones, and block-compressed ones in a CPU-writable pool even when flagged IMAGE: the icon
// fix, DESIGN D10) are uploaded through s2tex and uploaded again when their pages are written. The texture and sampler registrations (descriptor pools)
// are kept for the replay.
#pragma once
#include "vk_internal.h"
#include "vk_fmt.h"
#include "vk_present.h"
#include "nvn_obj.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VKT_GPU = 0, VKT_STORAGE = 1, VKT_NVIEWS = 48 };   // content owned by the GPU (render targets, storage images, copies) / read from guest memory
typedef struct vkt_image {
    // identity
    const pool_t *pool; int64_t off; uint8_t *mem; nvn_texdesc d; int cls, has_layout;
    uint64_t storage;                    // bytes of storage it reads (t->L.size)
    // Vulkan
    const vkf_info *fi; VkImage img; vk_alloc alloc; VkImageViewType vtype; VkImageAspectFlags aspect;
    uint32_t w, h, depth, layers, levels; VkSampleCountFlagBits samples;
    // F-5: a render target drawn larger (NXRT_VK_SCALE) is vw x vh at level 0; w x h stay the game's size, which every NVN coordinate is in
    uint32_t vw, vh; float scale; const char *noscale_why;   // why an image is not scaled (for the log)
    int scalable;                        // F-5: the rule takes it (it follows the resolution when that changes in the game)
    VkImageUsageFlags usage; VkImageLayout rest;   // the layout it is left in between submissions
    VkImageView view;                    // all levels and layers, the format's base mapping
    VkImageView present_view;            // UNORM view for presentation (sRGB content shown as stored)
    struct { uint64_t key; VkImageView v; } views[VKT_NVIEWS]; int nviews;
    // content
    int content;                         // 1 once uploaded, cleared or written by the GPU
    uint64_t stamp; const void *span;    // VKT_STORAGE: dirty-tracker stamp of the last upload
    uint64_t *phash; uint32_t npg;       // VKT_STORAGE: each storage page's hash at the last upload (pages written with the same bytes skip the upload)
    uint64_t uploads, last_frame, serial;
    int astc_fast;                       // levels / layers still at the fast ASTC pass
} vkt_image;

int vkt_init(void);
void vkt_shutdown(void);
void vkt_tex_init(const tex_t *t);
void vkt_tex_release(const tex_t *t);
void vkt_tex_cpu_written(const tex_t *t);
int vkt_tex_gpu_owned(uint64_t serial);
void vkt_drop_pool(const void *pool);
void vkt_reg_tex(int id, const tex_t *t, const view_t *v, int image);
void vkt_reg_tex_gone(int id);
void vkt_reg_samp(int id, const samp_par *p);
void vkt_set_filter(float aniso_min, float lod_bias);   // the in-game menu's Anisotropic and Texture bias rows
void vkt_set_pool_n(int n);
void vkt_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);

// The replay's side (M5-C). vkt_resolve: the host image of a texture, created, uploaded or uploaded again as needed (inside a submission the uploads go into
// its upload command buffer; outside one they are submitted and waited for)
vkt_image *vkt_resolve(const tex_t *t);
VkImageView vkt_view(vkt_image *im, const view_t *v, int set_swz, uint16_t swz);   // NULL view = the default; fallbacks apply (vkf_view_format)
VkSampler vkt_sampler(const samp_par *p);
typedef struct { tex_t t; view_t v; uint8_t has_view, image, live, gone; } vkt_reg;
// The replay's views of exact parameters: identity swizzle unless m is given (attachments and storage images must have it), cached with the image's views
VkImageView vkt_view_raw_(vkt_image *im, VkFormat f, VkImageViewType vt, uint32_t base_level, uint32_t nlevels, uint32_t base_layer, uint32_t nlayers, VkImageAspectFlags aspect, const VkComponentMapping *m);
int vkt_reg_copy(int id, vkt_reg *out);          // a registration copied under the table's lock (the game thread may grow the table meanwhile); 0 when none
int vkt_samp_copy(int id, samp_par *out);
int vkt_pool_n(void);
// The image of a serial (the textures the replay resolved), or NULL
vkt_image *vkt_by_serial(uint64_t serial);
const vkt_reg *vkt_reg_get(int id);
const samp_par *vkt_samp_get(int id);
void vkt_in_submission(void);   // at gfx_sub_begin: finished ASTC high-quality passes go into this submission (within the upload budget)
void vkt_frame(uint64_t frame);
int vkt_present_source(uint64_t serial, vkp_source *out);   // vk_present.c's resolver
// Readback of an image's subresource into host memory (tests, ReadTexels of GPU-owned textures); synchronous. 0 on success
int vkt_readback(vkt_image *im, unsigned level, unsigned layer, uint8_t *dst, size_t size);
// The same for a region of one level, x / y / z offsets and size in texels, rows packed; the image's own format and aspect (depth when it has one). A scaled
// image is read at the game's size (the nearest of its pixels); _raw reads its own pixels (r in them)
int vkt_readback_region(vkt_image *im, unsigned level, unsigned layer, const NVNcopyRegion *r, uint8_t *dst, size_t size);
int vkt_readback_region_raw(vkt_image *im, unsigned level, unsigned layer, const NVNcopyRegion *r, uint8_t *dst, size_t size);

// F-5 resolution scaling: the factor the game's screen-sized render targets are drawn at (NXRT_VK_SCALE: 1, the game's own size, 1.5, 2 or 3)
float vkt_scale(void);
void vkt_request_scale(float s);           // the in-game menu: a new factor, applied at the next submission (vkt_apply_scale)
void vkt_apply_scale(VkCommandBuffer cb);   // the replay, at a submission's start: every scalable image re-made at the requested factor, its content blitted over
static inline uint32_t vkt_vlw(const vkt_image *im, unsigned level) { uint32_t s = im->vw >> level; return s ? s : 1; }   // the image's own level size
static inline uint32_t vkt_vlh(const vkt_image *im, unsigned level) { uint32_t s = im->vh >> level; return s ? s : 1; }
// An NVN coordinate on level `level` in the image's own pixels (both edges of a rectangle rounded alike, so neighbouring rectangles still meet)
static inline int32_t vkt_sx(const vkt_image *im, unsigned level, int64_t x) { uint32_t n = im->w >> level; n = n ? n : 1; return im->vw == im->w ? (int32_t)x : (int32_t)((x * vkt_vlw(im, level) * 2 + n) / (2 * (int64_t)n)); }
static inline int32_t vkt_sy(const vkt_image *im, unsigned level, int64_t y) { uint32_t n = im->h >> level; n = n ? n : 1; return im->vh == im->h ? (int32_t)y : (int32_t)((y * vkt_vlh(im, level) * 2 + n) / (2 * (int64_t)n)); }
// The image drawn at the game's size from now on, its content scaled down into it (recorded into cb, outside a pass): a scaled image bound for stores (a compute
// shader's dispatch covers the game's size), or attached beside one that is not scaled. why for the log; 0 when it was done or the image was not scaled
int vkt_unscale(vkt_image *im, VkCommandBuffer cb, const char *why);
typedef struct { uint64_t images, image_bytes, uploads, reuploads, upload_bytes, upload_ns, shared_hits, astc_fast, astc_high, cpu_written, evicted, samplers; } vkt_stats_t;
void vkt_stats(vkt_stats_t *s);

#ifdef __cplusplus
}
#endif
