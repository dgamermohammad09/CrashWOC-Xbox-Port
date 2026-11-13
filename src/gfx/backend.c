#include "gfx/backend.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "app.h"
#include "data/image_tables.h"
#include "frontend/debug.h"
#include "gfx/gem.h"
#include "gfx/interp.h"
#include "gfx/rhi.h"
#include "gfx/vu.h"
#include "platform/paths.h"

#define NW_BE_SURFS 16

/* A GS surface (back or front buffer, render target) as a power-of-two texture at scale n, GS row y at rows from
   n * (th - 1 - y). Its draws go to tex under zatt (its own depth or the back buffer's), or with MSAA to ms_col under
   ms_zatt, resolved into tex and zatt before a read. */
typedef struct NwBeSurf {
    int32_t addr; /* NW_SURF_BACK, NW_SURF_FRONT, a GS address, 0 for the targets the port does not draw */
    int tw, th;   /* GS pixels */
    int n;        /* the internal scale it was made at */
    NwRhiTexture *tex, *depth, *zatt;
    NwRhiTexture *ms_col, *ms_depth, *ms_zatt;
    int ms_w, ms_h, samples; /* the MSAA buffers' storage */
    bool ms;                 /* the draws go to ms_col */
    bool dirty;              /* ms_col holds draws tex and zatt lack */
} NwBeSurf;

static struct {
    NwBeSurf surf[NW_BE_SURFS];
    uint32_t nsurf;
    NwBeSurf copy;         /* a target a draw also reads, copied */
    NwBeSurf zcopy;        /* the target's Z a depth CLUT reads, copied */
    NwRhiTexture *zclut;   /* a depth CLUT's entries */
    NwBeSurf *target;      /* what the draws go to */
    bool alpha;            /* the target keeps alpha (not PSMCT24) */
    int width, height;     /* the target's pixels */
    int shown_w, shown_h;  /* those of the back buffer */
    uint64_t drawn;        /* the last packet run */
    float drawn_t;         /* at that blend */
    NwGpuJob *jobs;
    uint32_t jobcap;
    NwRhiVertexArray *rect_vao;           /* the packet's sprites, four vertices each */
    NwRhiBuffer *rect_vbo, *rect_ibo;
    NwRhiBuffer *ot_ibo;                  /* the packet's OT triangles */
    NwRhiBuffer *pool;                    /* the packet's joint palettes, a texel buffer on unit 3 */
    NwRhiBuffer *weights;                 /* its blend-shape weights, on unit 4 */
    NwRhiVertexArray *verts_vao;          /* its VERTS triangles */
    NwRhiBuffer *verts_vbo;
    NwRhiVertexArray *ptl_vao;            /* its particles */
    NwRhiBuffer *ptl_vbo;
    NwVifVertex *rect_verts;
    uint32_t rect_cap;
    bool wireframe;
    NwRenderOptions opts;
    uint64_t runs;           /* packets run */
    const NwBeSurf *copy_of; /* the target copy holds */
    uint64_t copy_run;       /* in that run */
    uint32_t held[512];      /* the world's [video] gems draws, in NwPacket.cmds, till the scene behind them is drawn */
    uint32_t nheld;
    bool flushing; /* drawing those */
    int scale;          /* the frame's */
    int samples;        /* the frame's MSAA, 0 off */
    int max_tex;        /* the GPU's largest texture */
    int max_samples;    /* its most MSAA samples */
    double clear_z;     /* the depth and stencil a new surface clears to: the last NW_CMD_CLEAR's and volumes' */
    uint8_t clear_s;
} nw_be;

/* A command with no material: GS defaults, untextured. */
static const NwVuMaterial nw_be_default = { { 0 }, { 0 }, { 0 }, NULL, { .ztst = 2 }, NULL, NULL };

static void nw_be_resolve(NwBeSurf *s);

static NwBeSurf *nw_be_back(void)
{
    for (uint32_t i = 0; i < nw_be.nsurf; i++) {
        if (nw_be.surf[i].addr == NW_SURF_BACK) {
            return &nw_be.surf[i];
        }
    }
    return NULL;
}

bool nw_backend_save(const char *path)
{
    NwBeSurf *back = nw_be_back();
    SDL_Surface *shot = NULL;
    uint8_t *rows = NULL;
    bool read = false;
    if (!back) {
        SDL_SetError("nothing drawn yet");
    } else if ((shot = SDL_CreateSurface(nw_be.shown_w * back->n, nw_be.shown_h * back->n, SDL_PIXELFORMAT_RGBX32)) &&
               (rows = SDL_malloc((size_t)shot->w * 4 * (size_t)shot->h))) {
        /* The shown rows are the texture's top ones, the picture's top row last. */
        nw_be_resolve(back);
        int y0 = back->th * back->n - shot->h;
        read = nw_rhi_texture_read(back->tex, 0, y0, shot->w, shot->h, rows, shot->w * 4);
        for (int y = 0; read && y < shot->h; y++) {
            memcpy((uint8_t *)shot->pixels + (size_t)y * (size_t)shot->pitch,
                   rows + (size_t)(shot->h - 1 - y) * (size_t)shot->w * 4, (size_t)shot->w * 4);
        }
    }
    bool ok = shot && read && SDL_SavePNG(shot, path);
    SDL_free(rows);
    SDL_DestroySurface(shot);
    return ok;
}

static void nw_be_screenshot(void *user)
{
    (void)user;
    char *path = nw_debug_file(NW_PATHS_SCREENSHOTS, "png");
    if (path && nw_backend_save(path)) {
        printf("screenshot: %s\n", path);
    } else {
        fprintf(stderr, "screenshot: %s\n", SDL_GetError());
    }
    SDL_free(path);
}

bool nw_backend_init(void)
{
    nw_be.opts.scale = 1;
    nw_be.clear_z = 1.0;
    nw_be.max_tex = nw_rhi_caps()->max_texture;
    nw_be.max_samples = nw_rhi_caps()->max_samples;
    return nw_vu_init();
}

void nw_backend_debug_menu(void)
{
    nw_debug_toggle("Debug", "Wireframe", &nw_be.wireframe);
    nw_debug_action("Debug", "Screenshot", SDL_SCANCODE_F12, nw_be_screenshot, NULL);
}

/* GL re-specifies a texture in place and every framebuffer it is attached to keeps it: the surfaces that drew under
   was draw under now. */
static void nw_be_rename(const NwRhiTexture *was, NwRhiTexture *now)
{
    NwBeSurf *all[NW_BE_SURFS + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nw_be.nsurf; i++) {
        all[n++] = &nw_be.surf[i];
    }
    all[n++] = &nw_be.copy;
    all[n++] = &nw_be.zcopy;
    for (uint32_t i = 0; was && i < n; i++) {
        if (all[i]->zatt == was) {
            all[i]->zatt = now;
        }
        if (all[i]->ms_zatt == was) {
            all[i]->ms_zatt = now;
        }
    }
}

static void nw_be_ms_free(NwBeSurf *s)
{
    nw_be_rename(s->ms_depth, NULL);
    nw_rhi_texture_destroy(s->ms_col);
    nw_rhi_texture_destroy(s->ms_depth);
    s->ms_col = s->ms_depth = s->ms_zatt = NULL;
    s->ms_w = s->ms_h = s->samples = 0;
}

static void nw_be_surf_free(NwBeSurf *s)
{
    nw_be_rename(s->depth, NULL);
    nw_rhi_texture_destroy(s->tex);
    nw_rhi_texture_destroy(s->depth);
    nw_be_ms_free(s);
    memset(s, 0, sizeof(*s));
}

/* What s's draws go to. */
static void nw_be_set(const NwBeSurf *s)
{
    if (s->ms) {
        nw_rhi_set_target(s->ms_col, s->ms_zatt);
    } else {
        nw_rhi_set_target(s->tex, s->zatt);
    }
}

/* The draws go back to the target after a surface was set for a clear. */
static void nw_be_rebind(void)
{
    if (nw_be.target) {
        nw_be_set(nw_be.target);
    }
}

/* Same-size blit, as a resolving blit needs; the target stays set. */
static void nw_be_resolve(NwBeSurf *s)
{
    if (!s->dirty) {
        return;
    }
    int w = s->tw * s->n, h = s->th * s->n;
    nw_rhi_blit(&(NwRhiBlit){ .src_colour = s->ms_col, .src_depth = s->ms_zatt, .dst_colour = s->tex,
                              .dst_depth = s->zatt, .src = { 0, 0, w, h }, .dst = { 0, 0, w, h }, .colour = true,
                              .depth = true, .stencil = true });
    s->dirty = false;
}

/* Black everywhere, as a new GL buffer is cleared: colour, depth and stencil to the values GL last cleared to. */
static void nw_be_clear_new(void)
{
    nw_rhi_clear(&(NwRhiClear){ .colour = true, .depth = true, .stencil = true, .rgba = { 0.0f, 0.0f, 0.0f, 1.0f },
                                .z = nw_be.clear_z, .s = nw_be.clear_s, .mask = { true, true, true, true },
                                .depth_write = true, .stencil_write = 0xFF });
}

/* s's multisampled buffers, black, under its own Z or shared's (the back buffer's); leaves them set. */
static void nw_be_ms(NwBeSurf *s, const NwBeSurf *shared)
{
    int w = s->tw * s->n, h = s->th * s->n;
    if (!s->ms_col || s->ms_w != w || s->ms_h != h || s->samples != nw_be.samples) {
        NwRhiTexture *col = nw_rhi_texture_create(NW_RHI_RGBA8, w, h, 1, nw_be.samples, NW_RHI_TEX_TARGET, NULL);
        NwRhiTexture *depth = nw_rhi_texture_create(NW_RHI_D24S8, w, h, 1, nw_be.samples, NW_RHI_TEX_TARGET, NULL);
        nw_be_rename(s->ms_depth, depth);
        nw_rhi_texture_destroy(s->ms_col);
        nw_rhi_texture_destroy(s->ms_depth);
        s->ms_col = col;
        s->ms_depth = s->ms_zatt = depth;
        s->ms_w = w;
        s->ms_h = h;
        s->samples = nw_be.samples;
        s->dirty = false;
        nw_rhi_set_target(s->ms_col, s->ms_depth);
        nw_be_clear_new();
    }
    s->ms_zatt = shared ? shared->ms_depth : s->ms_depth;
    nw_rhi_set_target(s->ms_col, s->ms_zatt);
}

void nw_backend_shutdown(void)
{
    for (uint32_t i = 0; i < nw_be.nsurf; i++) {
        nw_be_surf_free(&nw_be.surf[i]);
    }
    nw_be_surf_free(&nw_be.copy);
    nw_be_surf_free(&nw_be.zcopy);
    nw_rhi_texture_destroy(nw_be.zclut);
    nw_rhi_vertex_array_destroy(nw_be.rect_vao);
    nw_rhi_buffer_destroy(nw_be.rect_vbo);
    nw_rhi_buffer_destroy(nw_be.rect_ibo);
    nw_rhi_buffer_destroy(nw_be.ot_ibo);
    nw_rhi_buffer_destroy(nw_be.pool);
    nw_rhi_buffer_destroy(nw_be.weights);
    nw_rhi_vertex_array_destroy(nw_be.verts_vao);
    nw_rhi_buffer_destroy(nw_be.verts_vbo);
    nw_rhi_vertex_array_destroy(nw_be.ptl_vao);
    nw_rhi_buffer_destroy(nw_be.ptl_vbo);
    nw_vu_shutdown();
    SDL_free(nw_be.jobs);
    SDL_free(nw_be.rect_verts);
    memset(&nw_be, 0, sizeof(nw_be));
}

/* A vertex array of a static vertex buffer and index buffer. */
static NwRhiVertexArray *nw_be_static(const NwRhiVertexLayout *layout, const void *verts, size_t vsize,
                                      const void *idx, size_t isize, NwRhiBuffer **vbo, NwRhiBuffer **ibo)
{
    *vbo = nw_rhi_buffer_create(NW_RHI_VERTICES, NW_RHI_RGBA8);
    *ibo = nw_rhi_buffer_create(NW_RHI_INDICES, NW_RHI_RGBA8);
    NwRhiVertexArray *va = nw_rhi_vertex_array_create();
    nw_rhi_buffer_data(*vbo, vsize, verts, false);
    nw_rhi_buffer_data(*ibo, isize, idx, false);
    nw_rhi_vertex_array_set(va, layout, *vbo, 0, *ibo);
    return va;
}

/* Each sprite as a quad of two triangles, s and t from (0, 0) to (1, 1), its size in tex.zw. */
static void nw_be_sprites_upload(NwGpuSprites *s)
{
    uint32_t n = s->sprites.count;
    NwVuSpriteVertex *v = SDL_malloc((size_t)n * 4 * sizeof(*v) + 1);
    uint32_t *idx = SDL_malloc((size_t)n * 6 * sizeof(*idx) + 1);
    if (!v || !idx) {
        SDL_free(v);
        SDL_free(idx);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        static const uint32_t quad[6] = { 0, 1, 2, 1, 3, 2 };
        const NwVifSprite *sp = &s->sprites.sprites[i];
        for (int k = 0; k < 4; k++) {
            NwVuSpriteVertex *c = &v[i * 4 + k];
            const float pos[4] = { sp->pos[0], sp->pos[1], sp->pos[2], 0.0f };
            const float tex[4] = { (float)(k & 1), (float)(k >> 1), sp->size[0], sp->size[1] };
            memcpy(c->pos, pos, sizeof(pos));
            memcpy(c->tex, tex, sizeof(tex));
            memcpy(c->col, sp->col, 4);
        }
        for (int k = 0; k < 6; k++) {
            idx[i * 6 + k] = i * 4 + quad[k];
        }
    }
    s->vao = nw_be_static(nw_vu_sprite_layout(), v, (size_t)n * 4 * sizeof(*v), idx, (size_t)n * 6 * sizeof(*idx),
                          &s->vbo, &s->ibo);
    SDL_free(v);
    SDL_free(idx);
}

static void nw_be_skin_upload(NwGpuSkin *s)
{
    s->vao = nw_be_static(nw_vu_skin_layout(), s->skin.verts, (size_t)s->skin.nverts * sizeof(NwVifSkinVertex),
                          s->skin.tris, (size_t)s->skin.ntris * 12, &s->vbo, &s->ibo);
    if (s->skin.ndeltas) {
        s->deltas = nw_rhi_buffer_create(NW_RHI_TEXELS, NW_RHI_RGBA32F);
        nw_rhi_buffer_data(s->deltas, (size_t)s->skin.ndeltas * sizeof(*s->skin.deltas), s->skin.deltas, false);
    }
}

static void nw_be_job(const NwGpuJob *job)
{
    NwGpuMesh *m = job->obj;
    NwGpuTex *t = job->obj;
    NwGpuSkin *sk = job->obj;
    NwGpuSprites *sp = job->obj;
    switch (job->op) {
    case NW_GPU_UPLOAD_MESH:
        m->vao = nw_be_static(nw_vu_layout(), m->mesh.verts, (size_t)m->mesh.nverts * sizeof(NwVifVertex),
                              m->mesh.tris, (size_t)m->mesh.ntris * 12, &m->vbo, &m->ibo);
        break;
    case NW_GPU_UPLOAD_TEX:
        nw_vu_texture(&t->gl, &t->tex);
        break;
    case NW_GPU_FREE_MESH:
        nw_rhi_vertex_array_destroy(m->vao);
        nw_rhi_buffer_destroy(m->vbo);
        nw_rhi_buffer_destroy(m->ibo);
        nw_vif_free(&m->mesh);
        SDL_free(m);
        break;
    case NW_GPU_FREE_TEX:
        nw_vu_texture_free(&t->gl);
        nw_tex_free(&t->tex);
        SDL_free(t);
        break;
    case NW_GPU_UPLOAD_SPRITES:
        nw_be_sprites_upload(sp);
        break;
    case NW_GPU_FREE_SPRITES:
        nw_rhi_vertex_array_destroy(sp->vao);
        nw_rhi_buffer_destroy(sp->vbo);
        nw_rhi_buffer_destroy(sp->ibo);
        nw_vif_sprites_free(&sp->sprites);
        SDL_free(sp);
        break;
    case NW_GPU_UPLOAD_SKIN:
        nw_be_skin_upload(sk);
        break;
    case NW_GPU_FREE_SKIN:
        nw_rhi_vertex_array_destroy(sk->vao);
        nw_rhi_buffer_destroy(sk->vbo);
        nw_rhi_buffer_destroy(sk->ibo);
        nw_rhi_buffer_destroy(sk->deltas);
        nw_vif_skin_free(&sk->skin);
        SDL_free(sk);
        break;
    case NW_GPU_FREE_HOST:
        SDL_free(job->obj);
        break;
    }
}

/* A black surface with a 24-bit Z like the GS's, as a texture a depth CLUT can read (plus stencil); a rescale keeps
   the picture. Leaves it set. */
static void nw_be_surf_make(NwBeSurf *s, int32_t addr, int tw, int th)
{
    int n = nw_be.scale;
    bool remake = s->tw != tw || s->th != th || s->n != n;
    if (remake) {
        nw_be_resolve(s);
    }
    NwBeSurf old = *s;
    bool rescale = s->tex && s->tw == tw && s->th == th && s->n != n;
    if (remake) {
        NwRhiTexture *tex =
            nw_rhi_texture_create(NW_RHI_RGBA8, tw * n, th * n, 1, 1, NW_RHI_TEX_TARGET | NW_RHI_TEX_MIPGEN, NULL);
        NwRhiTexture *depth = nw_rhi_texture_create(NW_RHI_D24S8, tw * n, th * n, 1, 1, NW_RHI_TEX_TARGET, NULL);
        nw_be_rename(s->depth, depth);
        if (!rescale) {
            nw_rhi_texture_destroy(s->tex);
            nw_rhi_texture_destroy(s->depth);
        }
        s->tex = tex;
        s->depth = s->zatt = depth;
        s->tw = tw;
        s->th = th;
        s->n = n;
        s->dirty = false;
        nw_rhi_set_target(s->tex, s->zatt);
        nw_be_clear_new();
        if (rescale) {
            nw_rhi_blit(&(NwRhiBlit){ .src_colour = old.tex, .dst_colour = s->tex,
                                      .src = { 0, 0, tw * old.n, th * old.n }, .dst = { 0, 0, tw * n, th * n },
                                      .colour = true, .linear = true });
            nw_rhi_texture_destroy(old.tex);
            nw_rhi_texture_destroy(old.depth);
        }
    } else {
        nw_rhi_set_target(s->tex, s->zatt);
    }
    s->addr = addr;
}

/* The surface at a GS address, made at tw x th if it is new or its size changed; NULL with none left. */
static NwBeSurf *nw_be_surf(int32_t addr, int tw, int th)
{
    NwBeSurf *s = NULL;
    for (uint32_t i = 0; i < nw_be.nsurf && !s; i++) {
        s = nw_be.surf[i].addr == addr ? &nw_be.surf[i] : NULL;
    }
    if (!s && nw_be.nsurf < NW_BE_SURFS) {
        s = &nw_be.surf[nw_be.nsurf++];
    }
    if (s) {
        nw_be_surf_make(s, addr, tw, th);
    }
    return s;
}

/* The draws go to s, width x height GS pixels at its top left. */
static void nw_be_bind(NwBeSurf *s, int width, int height)
{
    nw_be_set(s);
    s->dirty |= s->ms;
    nw_rhi_viewport(0, (s->th - height) * s->n, width * s->n, height * s->n);
    nw_be.target = s;
    nw_be.width = width;
    nw_be.height = height;
    if (s->addr == NW_SURF_BACK) {
        nw_be.shown_w = width;
        nw_be.shown_h = height;
    }
}

/* A target drawn under the back buffer's Z at its size shares it (and its MSAA, except the front buffer, which the
   back buffer is copied into); otherwise it has its own. */
static void nw_be_target(const NwCmd *c)
{
    NwSurf surf = c->target.surf;
    if (!surf.addr) {
        surf.tw = (uint16_t)c->target.width;
        surf.th = (uint16_t)c->target.height;
    }
    NwBeSurf *back = nw_be_back();
    NwBeSurf *s = nw_be_surf(surf.addr, surf.tw, surf.th);
    if (s) {
        bool shared =
            c->target.zback && back && back != s && back->tw == s->tw && back->th == s->th && back->n == s->n;
        bool ms = shared && back->ms && s->addr != NW_SURF_FRONT;
        if (!ms) {
            nw_be_resolve(s);
            if (!nw_be.samples) {
                nw_be_ms_free(s);
            }
            if (shared) {
                nw_be_resolve(back);
            }
            s->zatt = shared ? back->depth : s->depth;
            nw_rhi_set_target(s->tex, s->zatt);
        } else {
            nw_be_ms(s, back);
        }
        s->ms = ms;
        nw_be_bind(s, c->target.width, c->target.height);
        nw_be.alpha = surf.tcc || !surf.addr;
        nw_vu_alpha_writes(nw_be.alpha);
    }
}

/* The target's colour copied into nw_be.copy, as a texture cannot be both. */
static void nw_be_copy_target(NwBeSurf *s)
{
    nw_be_surf_make(&nw_be.copy, 0, s->tw, s->th);
    int w = s->tw * s->n, h = s->th * s->n;
    nw_rhi_blit(&(NwRhiBlit){ .src_colour = s->ms ? s->ms_col : s->tex, .dst_colour = nw_be.copy.tex,
                              .src = { 0, 0, w, h }, .dst = { 0, 0, w, h }, .colour = true });
    nw_be_rebind();
    nw_be.copy_of = s;
    nw_be.copy_run = nw_be.runs;
}

/* A surface for a draw to read: the target itself is read from a copy, as a texture cannot be both. */
static const NwVuTexture *nw_be_surf_tex(const NwSurf *surf, bool tcc)
{
    static NwVuTexture tex;
    NwBeSurf *s = NULL;
    for (uint32_t i = 0; i < nw_be.nsurf && !s; i++) {
        s = nw_be.surf[i].addr == surf->addr && nw_be.surf[i].tex ? &nw_be.surf[i] : NULL;
    }
    if (!s) {
        return NULL;
    }
    if (s == nw_be.target) {
        nw_be_copy_target(s);
        s = &nw_be.copy;
    } else {
        nw_be_resolve(s);
    }
    tex = (NwVuTexture){ .index = s->tex, .tcc = tcc, .surface = true };
    return &tex;
}

/* The target as a texture a draw blends over, copied. */
static const NwVuTexture *nw_be_dst(void)
{
    static NwVuTexture tex;
    nw_be_copy_target(nw_be.target);
    tex = (NwVuTexture){ .index = nw_be.copy.tex, .tcc = true, .surface = true };
    return &tex;
}

/* A depth CLUT for a draw on the target: its Z copied, as a texture cannot be both, and the entries uploaded. */
static const NwVuZClut *nw_be_zclut(const NwZClut *z)
{
    static NwVuZClut vz;
    NwBeSurf *s = nw_be.target;
    nw_be_surf_make(&nw_be.zcopy, 0, s->tw, s->th);
    int w = s->tw * s->n, h = s->th * s->n;
    nw_rhi_blit(&(NwRhiBlit){ .src_colour = s->ms ? s->ms_col : s->tex, .src_depth = s->ms ? s->ms_zatt : s->zatt,
                              .dst_colour = nw_be.zcopy.tex, .dst_depth = nw_be.zcopy.zatt, .src = { 0, 0, w, h },
                              .dst = { 0, 0, w, h }, .depth = true });
    nw_be_rebind();
    if (!nw_be.zclut) {
        nw_be.zclut = nw_rhi_texture_create(NW_RHI_RGBA8UI, 256, 1, 1, 1, 0, NULL);
    }
    nw_rhi_texture_update(nw_be.zclut, 0, 0, 0, 256, 1, z->clut);
    vz = (NwVuZClut){ nw_be.zcopy.depth, nw_be.zclut, z->shift, z->base, z->far, z->near,
                      { z->spot ? 2u : z->clear ? 1u : 0u, z->clear_z[0], z->clear_z[1] }, z->alpha, z->ad };
    return &vz;
}

/* GS coordinates (12.4 from XYOFFSET, Z 0 .. 2^23 - 1, larger nearer) to GL clip space (smaller nearer). */
static void nw_be_gs(const NwView *v, int width, int height, float g[16])
{
    memset(g, 0, 16 * sizeof(float));
    g[0] = 1.0f / (8.0f * (float)width);
    g[5] = -1.0f / (8.0f * (float)height);
    g[10] = -2.0f / 8388607.0f;
    g[12] = -(float)v->xyoffset[0] / (8.0f * (float)width) - 1.0f;
    g[13] = (float)v->xyoffset[1] / (8.0f * (float)height) + 1.0f;
    g[14] = 1.0f;
    g[15] = 1.0f;
}

/* The view's vs, which ends in GS coordinates, then to GL clip space. */
static void nw_be_screen(const NwView *v, int width, int height, float out[16])
{
    float g[16];
    nw_be_gs(v, width, height, g);
    NwVuObject o;
    nw_vu1init(&o, v->vs, g);
    memcpy(out, o.lv, sizeof(o.lv));
}

static NwVuMaterial nw_be_material(const NwPacket *p, const NwCmd *c, bool uvrow)
{
    NwVuMaterial m = nw_be_default;
    if (c->mtl != NW_MTL_NONE) {
        const NwMtlSnap *s = &p->mtls[c->mtl];
        memcpy(m.diffuse, s->diffuse, sizeof(m.diffuse));
        memcpy(m.vu, s->vu, sizeof(m.vu));
        m.uvoff[0] = s->vu[0];
        m.uvoff[1] = s->vu[1];
        m.uvoff[2] = uvrow ? (float)s->row[0] : 0.0f;
        m.uvoff[3] = uvrow ? (float)s->row[1] : 0.0f;
        m.tex = s->surf.addr ? nw_be_surf_tex(&s->surf, s->surf.tcc) : s->tex ? &s->tex->gl : NULL;
        m.state = s->gs;
    }
    return m;
}

/* The program's passes over count indices from first, of va's index buffer or of indices. */
static void nw_be_passes(NwVuProgram program, const NwVuObject *o, const NwVuMaterial *m, NwRhiVertexArray *va,
                         NwRhiBuffer *indices, uint32_t first, uint32_t count)
{
    NwMtlPass pass[2];
    uint32_t n = nw_vu_use(program, o, m, pass);
    for (uint32_t k = 0; k < n; k++) {
        nw_vu_pass(program, m, &pass[k]);
        nw_rhi_draw_indexed(va, indices, first, count);
    }
}

/* As the GS clears: sprites over the scissor with ZTST ALWAYS, the alpha test masking colour or Z; 0x80 alpha = 1.0. */
static void nw_be_clear(const NwPacket *p, const NwCmd *c)
{
    const int32_t *sc = p->views[c->view].scissor;
    int n = nw_be.target->n;
    const int scissor[4] = { sc[0] * n, (nw_be.target->th - 1 - sc[3]) * n, (sc[2] - sc[0] + 1) * n,
                             (sc[3] - sc[1] + 1) * n };
    bool rgb = c->clear.mask & 1, z = c->clear.mask & 2;
    nw_be.clear_z = 1.0 - c->clear.z / 8388607.0;
    nw_rhi_clear(&(NwRhiClear){ .colour = true, .depth = true,
                                .rgba = { c->clear.rgba[0] / 255.0f, c->clear.rgba[1] / 255.0f,
                                          c->clear.rgba[2] / 255.0f, SDL_min(c->clear.rgba[3] / 128.0f, 1.0f) },
                                .z = nw_be.clear_z, .mask = { rgb, rgb, rgb, rgb && nw_be.alpha }, .depth_write = z,
                                .scissor = scissor });
}

/* Uploads the packet's sprites as quads: GS coordinates for the unlit program, s and t x4096. */
static void nw_be_rects(const NwPacket *p)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->ncmds; i++) {
        n += p->cmds[i].op == NW_CMD_RECT;
    }
    if (!n) {
        return;
    }
    if (n > nw_be.rect_cap) {
        NwVifVertex *verts = SDL_realloc(nw_be.rect_verts, (size_t)n * 4 * sizeof(*verts));
        uint32_t *idx = SDL_malloc((size_t)n * 6 * sizeof(*idx));
        if (!verts || !idx) {
            SDL_free(idx);
            return;
        }
        for (uint32_t r = 0; r < n; r++) {
            static const uint32_t quad[6] = { 0, 1, 2, 1, 3, 2 };
            for (int k = 0; k < 6; k++) {
                idx[r * 6 + k] = r * 4 + quad[k];
            }
        }
        if (!nw_be.rect_vao) {
            nw_be.rect_vao = nw_rhi_vertex_array_create();
            nw_be.rect_vbo = nw_rhi_buffer_create(NW_RHI_VERTICES, NW_RHI_RGBA8);
            nw_be.rect_ibo = nw_rhi_buffer_create(NW_RHI_INDICES, NW_RHI_RGBA8);
        }
        nw_rhi_buffer_data(nw_be.rect_ibo, (size_t)n * 6 * sizeof(*idx), idx, false);
        SDL_free(idx);
        nw_be.rect_verts = verts;
        nw_be.rect_cap = n;
    }
    NwVifVertex *v = nw_be.rect_verts;
    for (uint32_t i = 0; i < p->ncmds; i++) {
        const NwCmd *c = &p->cmds[i];
        if (c->op != NW_CMD_RECT) {
            continue;
        }
        for (int k = 0; k < 4; k++, v++) {
            memset(v, 0, sizeof(*v));
            v->pos[0] = (float)c->rect.xy[k & 1 ? 2 : 0];
            v->pos[1] = (float)c->rect.xy[k & 2 ? 3 : 1];
            v->tex[0] = (int16_t)(c->rect.st[k & 1 ? 2 : 0] * 4096.0f);
            v->tex[1] = (int16_t)(c->rect.st[k & 2 ? 3 : 1] * 4096.0f);
            memcpy(v->col, c->rect.rgba, 4);
        }
    }
    nw_rhi_buffer_data(nw_be.rect_vbo, (size_t)(v - nw_be.rect_verts) * sizeof(*v), nw_be.rect_verts, true);
    nw_rhi_vertex_array_set(nw_be.rect_vao, nw_vu_layout(), nw_be.rect_vbo, 0, nw_be.rect_ibo);
}

static void nw_be_rect(const NwPacket *p, const NwCmd *c, uint32_t index)
{
    NwVuObject o;
    float g[16];
    nw_be_gs(&p->views[c->view], nw_be.width, nw_be.height, g);
    memset(&o, 0, sizeof(o));
    memcpy(o.lv, g, sizeof(o.lv));
    NwVuMaterial m = nw_be_material(p, c, false);
    nw_be_passes(NW_VU_TRICU, &o, &m, nw_be.rect_vao, NULL, index * 6, 6);
}

static void nw_be_verts(const NwPacket *p)
{
    if (!p->nverts) {
        return;
    }
    if (!nw_be.verts_vao) {
        nw_be.verts_vao = nw_rhi_vertex_array_create();
        nw_be.verts_vbo = nw_rhi_buffer_create(NW_RHI_VERTICES, NW_RHI_RGBA8);
    }
    nw_rhi_buffer_data(nw_be.verts_vbo, (size_t)p->nverts * sizeof(*p->verts), p->verts, true);
    nw_rhi_vertex_array_set(nw_be.verts_vao, nw_vu_sprite_layout(), nw_be.verts_vbo, 0, NULL);
}

static void nw_be_ptls(const NwPacket *p)
{
    if (!p->nptls) {
        return;
    }
    if (!nw_be.ptl_vao) {
        nw_be.ptl_vao = nw_rhi_vertex_array_create();
        nw_be.ptl_vbo = nw_rhi_buffer_create(NW_RHI_VERTICES, NW_RHI_RGBA8);
    }
    nw_rhi_buffer_data(nw_be.ptl_vbo, (size_t)p->nptls * sizeof(*p->ptls), p->ptls, true);
}

static void nw_be_undrawn(const NwVuProg *mpg, bool skin)
{
    static const NwVuProg *seen[2][16];
    for (int i = 0; i < 16 && seen[skin][i] != mpg; i++) {
        if (!seen[skin][i]) {
            seen[skin][i] = mpg;
            fprintf(stderr, NW_APP_NAME ": %s: its %s program is not drawn\n", mpg ? mpg->name : "no package",
                    skin ? "skin" : "triangle");
            break;
        }
    }
}

/* vu1init's state for a draw: local times screen, the VU 0x0C block (lights, or the UV matrix for xuv and glass), and
   frag mapping GS 12.4 (x, y) from XYOFFSET to GL (x, th - 1 - y), pixel centres, n x n pixels each. */
static void nw_be_object(const NwPacket *p, const NwCmd *c, NwVuProgram program, NwVuObject *o)
{
    float screen[16];
    const NwView *v = &p->views[c->view];
    nw_be_screen(v, nw_be.width, nw_be.height, screen);
    nw_vu1init(o, c->local, screen);
    o->frag[0] = 16.0f / (float)nw_be.target->n;
    o->frag[1] = -16.0f / (float)nw_be.target->n;
    o->frag[2] = (float)v->xyoffset[0] - 8.0f;
    o->frag[3] = (float)(nw_be.target->th * 16 + v->xyoffset[1]) - 8.0f;
    if (c->light != NW_LIGHT_NONE && (program == NW_VU_TRIXUV || program == NW_VU_SKINXUV ||
                                      program == NW_VU_TRIGLASS || program == NW_VU_SKNGLASS)) {
        nw_vu1init_uv(o, c->local, &p->lights[c->light]);
    } else if (c->light != NW_LIGHT_NONE) {
        nw_vu1init_lights(o, c->local, &p->lights[c->light]);
    }
}

/* A VERTS command's triangles, drawn by its NwVertsMode. */
static void nw_be_tris(const NwPacket *p, const NwCmd *c)
{
    NwVuObject o;
    NwVuProgram program = NW_VU_TRICU;
    if (c->mode == NW_VERTS_GS) {
        memset(&o, 0, sizeof(o));
        nw_be_gs(&p->views[c->view], nw_be.width, nw_be.height, o.lv);
        o.lv[12] += 1.0f / (float)nw_be.width;
        o.lv[13] -= 1.0f / (float)nw_be.height;
    } else if (c->mode == NW_VERTS_SPRITE) {
        program = NW_VU_FACEON;
        nw_be_object(p, c, program, &o);
        o.sprite[0] = 1.0f / (8.0f * (float)nw_be.width);
        o.sprite[1] = -1.0f / (8.0f * (float)nw_be.height);
    } else {
        const NwVuProg *mpg = c->mpg;
        program = nw_vu_program(mpg ? mpg->name : NULL);
        nw_be_object(p, c, program, &o);
    }
    NwVuMaterial m = nw_be_material(p, c, false);
    if (c->alpha) {
        m.state.alpha = (uint8_t)c->alpha;
    }
    if (c->zclut && c->zclut <= p->nzcluts) {
        m.zclut = nw_be_zclut(&p->zcluts[c->zclut - 1]);
    }
    if (c->exact) {
        /* a draw that reads the target has its copy already */
        m.dst = m.tex && nw_be.copy.tex && m.tex->index == nw_be.copy.tex ? m.tex : nw_be_dst();
    }
    NwVuTexture clut;
    if (c->clut && c->clut->gl.clut && m.tex && m.tex->clut) {
        clut = *m.tex;
        clut.clut = c->clut->gl.clut;
        m.tex = &clut;
    }
    NwMtlPass pass[2];
    uint32_t n = nw_vu_use(program, &o, &m, pass);
    if (c->stencil == NW_STENCIL_SHADOWED) {
        nw_rhi_stencil(true, NW_RHI_LEQUAL, 0x80, 0xFF, NW_RHI_KEEP, NW_RHI_KEEP, NW_RHI_KEEP);
    } else if (c->stencil) {
        nw_rhi_stencil(true, NW_RHI_ALWAYS, 0, 0xFF, NW_RHI_KEEP, NW_RHI_KEEP,
                       c->stencil > 0 ? NW_RHI_INCR_WRAP : NW_RHI_DECR_WRAP);
        nw_vu_pass(program, &m, &pass[0]);
        nw_rhi_colour_mask(false, false, false, false);
        nw_rhi_depth_write(false);
        nw_rhi_draw(nw_be.verts_vao, c->first, c->count);
        nw_rhi_stencil(false, NW_RHI_ALWAYS, 0, 0xFF, NW_RHI_KEEP, NW_RHI_KEEP, NW_RHI_KEEP);
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        nw_vu_pass(program, &m, &pass[k]);
        nw_rhi_draw(nw_be.verts_vao, c->first, c->count);
    }
    nw_rhi_stencil(false, NW_RHI_ALWAYS, 0, 0xFF, NW_RHI_KEEP, NW_RHI_KEEP, NW_RHI_KEEP);
}

/* Retail's NuLgtHSurf[1]: the back buffer's Z squeezed to the volumes' width into the target's Z, and the stencil at
   0x7F, as retail's 0x7F sprites start its colour. */
static void nw_be_volumes(const NwCmd *c)
{
    NwBeSurf *back = nw_be_back(), *s = nw_be.target;
    if (back && back != s && !s->ms) { /* a scaling blit neither reads nor writes multisampled buffers */
        nw_be_resolve(back);
        nw_rhi_blit(&(NwRhiBlit){ .src_colour = back->tex, .src_depth = back->zatt, .dst_colour = s->tex,
                                  .dst_depth = s->zatt,
                                  .src = { 0, (back->th - nw_be.shown_h) * back->n, nw_be.shown_w * back->n,
                                           back->th * back->n },
                                  .dst = { 0, (s->th - nw_be.height) * s->n, c->volumes.width * s->n, s->th * s->n },
                                  .depth = true });
        nw_rhi_set_target(s->tex, s->zatt);
    }
    nw_be.clear_s = 0x7F;
    nw_rhi_stencil_write_mask(0xFF);
    nw_rhi_clear(&(NwRhiClear){ .stencil = true, .s = 0x7F, .stencil_write = 0xFF });
}

/* A quad per particle: corners in GS units before the divide, texel coordinates in 1/16 of the texture's level 0. */
static void nw_be_particles(const NwPacket *p, const NwCmd *c)
{
    _Static_assert(sizeof(NwPtlList) == 132 * 16, "NwPtlList is VU 0x3C-0xBF");
    NwVuObject o;
    nw_be_object(p, c, NW_VU_PTL, &o);
    o.sprite[0] = 1.0f / (8.0f * (float)nw_be.width);
    o.sprite[1] = -1.0f / (8.0f * (float)nw_be.height);
    NwVuMaterial m = nw_be_material(p, c, false);
    const NwGpuTex *tex = c->mtl != NW_MTL_NONE ? p->mtls[c->mtl].tex : NULL;
    if (tex && !nw_vu_texture_rgba((NwVuTexture *)&tex->gl, &tex->tex)) {
        return;
    }
    if (tex && tex->tex.nlevels) {
        o.sprite[2] = 1.0f / (16.0f * (float)tex->tex.levels[0].width);
        o.sprite[3] = 1.0f / (16.0f * (float)tex->tex.levels[0].height);
    }
    NwMtlPass pass[2];
    nw_rhi_vertex_array_set(nw_be.ptl_vao, nw_vu_ptl_layout(), nw_be.ptl_vbo, (size_t)c->first * sizeof(NwPtl), NULL);
    NwVuProgram program = nw_vu_particle_program(m.tex, &m.state);
    uint32_t n = nw_vu_use(program, &o, &m, pass);
    nw_vu_ptl(program, (const uint32_t(*)[4])&p->ptllists[c->ptllist]);
    for (uint32_t k = 0; k < n; k++) {
        nw_vu_pass(program, &m, &pass[k]);
        nw_rhi_draw_instanced(nw_be.ptl_vao, 0, 6, c->count);
    }
}

static bool nw_be_inverse(const float m[16], float out[16])
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        out[i] = inv[i] / det;
    }
    return true;
}

/* A clip-space point (row vector) back into the world. */
static void nw_be_unproject(const float inv[16], float x, float y, float z, float w, float out[4])
{
    for (int c = 0; c < 4; c++) {
        out[c] = x * inv[c] + y * inv[4 + c] + z * inv[8 + c] + w * inv[12 + c];
    }
}

static void nw_be_unit(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; l > 0.0f && i < 3; i++) {
        v[i] /= l;
    }
}

/* Menus and the HUD draw under the identity camera (DrawPanel's pNuCam). */
static bool nw_be_panel_view(const NwView *v)
{
    for (int i = 0; i < 16; i++) {
        if (SDL_fabsf(v->cam[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-4f) {
            return false;
        }
    }
    return true;
}

/* What the gems see behind them: the target as the first of them since the last flush found it. */
static NwRhiTexture *nw_be_gem_backdrop(void)
{
    if (nw_be.copy_of != nw_be.target || nw_be.copy_run != nw_be.runs) {
        nw_be_dst();
    }
    return nw_be.copy.tex;
}

/* A [video] gems material in the gem programs, opaque; false when the view gives no eye (draw it as the PS2). */
static bool nw_be_gem(const NwPacket *p, const NwCmd *c, NwVuProgram program, const NwGemInfo *gem,
                      NwRhiVertexArray *va, NwRhiBuffer *indices, uint32_t first, uint32_t count)
{
    NwVuGem g;
    memset(&g, 0, sizeof(g));
    nw_be_screen(&p->views[c->view], nw_be.width, nw_be.height, g.vp);
    float inv[16], e[4], a[4], b[4];
    if (!nw_be_inverse(g.vp, inv)) {
        return false;
    }
    /* The eye maps to w 0 on the axis; right and up from the screen's centre at some depth. */
    nw_be_unproject(inv, 0.0f, 0.0f, 1.0f, 0.0f, e);
    nw_be_unproject(inv, 0.0f, 0.0f, 0.5f, 1.0f, a);
    if (SDL_fabsf(e[3]) < 1e-12f || SDL_fabsf(a[3]) < 1e-12f) {
        return false;
    }
    float centre[3];
    for (int i = 0; i < 3; i++) {
        g.eye[i] = e[i] / e[3];
        centre[i] = a[i] / a[3];
        g.cam[6 + i] = centre[i] - g.eye[i];
    }
    for (int axis = 0; axis < 2; axis++) {
        nw_be_unproject(inv, axis ? 0.0f : 0.5f, axis ? 0.5f : 0.0f, 0.5f, 1.0f, b);
        for (int i = 0; i < 3; i++) {
            g.cam[axis * 3 + i] = b[i] / b[3] - centre[i];
        }
    }
    for (int axis = 0; axis < 3; axis++) {
        nw_be_unit(g.cam + axis * 3);
    }
    memcpy(g.world, c->local, sizeof(g.world));
    memcpy(g.tint, gem->tint, sizeof(g.tint));
    g.kind = gem->kind;
    g.role = gem->role;
    g.panel = nw_be_panel_view(&p->views[c->view]);
    nw_rhi_viewport_get(g.viewport);

    NwRhiTexture *backdrop = nw_be_gem_backdrop();
    NwVuObject o;
    nw_be_object(p, c, program, &o);
    NwVuMaterial m = nw_be_material(p, c, false);
    m.state.abe = false;
    NwMtlPass pass[2];
    nw_vu_use(program, &o, &m, pass);
    nw_vu_gem(program, &g);
    nw_rhi_depth(true, NW_RHI_LEQUAL);
    nw_rhi_colour_mask(true, true, true, true);
    nw_rhi_depth_write(true);
    nw_rhi_bind_texture(8, backdrop);
    if (program == NW_VU_SKNGEM) {
        const NwGpuSkin *s = c->skin;
        nw_vu_skin(program, c->first, c->weights, s->deltas ? c->nweights : 0);
        if (s->deltas) {
            nw_rhi_bind_texels(5, s->deltas);
        }
    }
    nw_rhi_draw_indexed(va, indices, first, count);
    return true;
}

/* The draw's [video] gems role, NW_GEM_KEEP when it draws as the PS2. */
static NwGemRole nw_be_gem_of(const NwPacket *p, const NwCmd *c, NwGemInfo *gem)
{
    if (!nw_be.opts.gems || c->mtl == NW_MTL_NONE) {
        return NW_GEM_KEEP;
    }
    *gem = p->mtls[c->mtl].gem;
    return gem->kind == NW_GEM_NONE ? NW_GEM_KEEP : (NwGemRole)gem->role;
}

/* Whether a draw waits for the rest of the scene: the PS2 sorts the world's gems ahead of what lies behind them, which
   their stone then would not see. Menus and the HUD keep their order. */
static bool nw_be_gem_hold(const NwPacket *p, const NwCmd *c)
{
    if (nw_be.flushing || !nw_be.opts.gems || c->mtl == NW_MTL_NONE || p->mtls[c->mtl].gem.kind == NW_GEM_NONE ||
        nw_be.nheld == SDL_arraysize(nw_be.held) || nw_be_panel_view(&p->views[c->view])) {
        return false;
    }
    nw_be.held[nw_be.nheld++] = (uint32_t)(c - p->cmds);
    return true;
}

static void nw_be_draw(const NwPacket *p, const NwCmd *c);
static void nw_be_skin(const NwPacket *p, const NwCmd *c);

/* The held gems over one copy of the scene, before c: a target change, a clear, the shadows' Z, the HUD, or anything
   under the identity camera; NULL at the run's end. */
static void nw_be_gems_flush(const NwPacket *p, const NwCmd *c)
{
    if (!nw_be.nheld) {
        return;
    }
    if (c) {
        switch (c->op) {
        case NW_CMD_TARGET:
        case NW_CMD_CLEAR:
        case NW_CMD_RECT:
        case NW_CMD_VOLUMES:
            break;
        case NW_CMD_WAVES:
            return;
        default:
            if (!nw_be_panel_view(&p->views[c->view])) {
                return;
            }
        }
    }
    nw_be.copy_of = NULL;
    nw_be.flushing = true;
    for (uint32_t i = 0; i < nw_be.nheld; i++) {
        const NwCmd *h = &p->cmds[nw_be.held[i]];
        if (h->op == NW_CMD_SKIN) {
            nw_be_skin(p, h);
        } else {
            nw_be_draw(p, h);
        }
    }
    nw_be.flushing = false;
    nw_be.nheld = 0;
    nw_be.copy_of = NULL;
}

/* A geom's mesh; an OT batch draws its triangles in the packet's order through the packet's index buffer. */
static void nw_be_draw(const NwPacket *p, const NwCmd *c)
{
    const NwGpuMesh *mesh = c->mesh;
    const NwVuProg *mpg = c->mpg;
    NwVuProgram program = nw_vu_program(mpg ? mpg->name : NULL);
    if (!mesh || !mesh->vao) {
        return;
    }
    if (program == NW_VU_PROGRAMS) {
        nw_be_undrawn(mpg, false);
        return;
    }
    NwGemInfo gem;
    NwGemRole role = nw_be_gem_of(p, c, &gem);
    if (role == NW_GEM_HIDE || nw_be_gem_hold(p, c)) {
        return;
    }
    bool ot = c->op == NW_CMD_DRAW_OT;
    NwRhiBuffer *indices = ot ? nw_be.ot_ibo : NULL;
    uint32_t first = ot ? c->first : 0, count = ot ? c->count : mesh->mesh.ntris * 3;
    if (role != NW_GEM_KEEP && nw_be_gem(p, c, NW_VU_GEM, &gem, mesh->vao, indices, first, count)) {
        return;
    }
    NwVuObject o;
    nw_be_object(p, c, program, &o);
    NwVuMaterial m = nw_be_material(p, c, mesh->mesh.uvrow);
    nw_be_passes(program, &o, &m, mesh->vao, indices, first, count);
}

/* A skin geom under its draw's palette and blend-shape weights, in the local matrix's space as vu1init sets it up. */
static void nw_be_skin(const NwPacket *p, const NwCmd *c)
{
    const NwGpuSkin *s = c->skin;
    const NwVuProg *mpg = c->mpg;
    NwVuProgram program = nw_vu_skin_program(mpg ? mpg->name : NULL);
    if (!s || !s->vao || c->first + c->count > p->njoints || c->weights + c->nweights > p->nweights) {
        return;
    }
    if (program == NW_VU_PROGRAMS) {
        nw_be_undrawn(mpg, true);
        return;
    }
    NwGemInfo gem;
    NwGemRole role = nw_be_gem_of(p, c, &gem);
    if (role == NW_GEM_HIDE || nw_be_gem_hold(p, c)) {
        return;
    }
    if (role != NW_GEM_KEEP && nw_be_gem(p, c, NW_VU_SKNGEM, &gem, s->vao, NULL, 0, s->skin.ntris * 3)) {
        return;
    }
    NwVuObject o;
    nw_be_object(p, c, program, &o);
    NwVuMaterial m = nw_be_material(p, c, false);
    NwMtlPass pass[2];
    uint32_t n = nw_vu_use(program, &o, &m, pass);
    nw_vu_skin(program, c->first, c->weights, s->deltas ? c->nweights : 0);
    if (s->deltas) {
        nw_rhi_bind_texels(5, s->deltas);
    }
    for (uint32_t k = 0; k < n; k++) {
        nw_vu_pass(program, &m, &pass[k]);
        nw_rhi_draw_indexed(s->vao, NULL, 0, s->skin.ntris * 3);
    }
}

static void nw_be_weights(const NwPacket *p)
{
    if (!p->nweights) {
        return;
    }
    if (!nw_be.weights) {
        nw_be.weights = nw_rhi_buffer_create(NW_RHI_TEXELS, NW_RHI_R32F);
    }
    nw_rhi_buffer_data(nw_be.weights, (size_t)p->nweights * sizeof(*p->weights), p->weights, true);
    nw_rhi_bind_texels(4, nw_be.weights);
}

/* The packet's palettes as RGBA32F texels, four rows a matrix. */
static void nw_be_pool(const NwPacket *p)
{
    nw_be_weights(p);
    if (!p->njoints) {
        return;
    }
    if (!nw_be.pool) {
        nw_be.pool = nw_rhi_buffer_create(NW_RHI_TEXELS, NW_RHI_RGBA32F);
    }
    nw_rhi_buffer_data(nw_be.pool, (size_t)p->njoints * sizeof(*p->joints), p->joints, true);
    nw_rhi_bind_texels(3, nw_be.pool);
}

/* faceon: corner sizes in GS units before the divide, scaled by VU 0x16; facey: quads along VF15, the camera's right
   (cos yaw, 0, -sin yaw) dotted with the local rows and normalised (faceinit). */
static void nw_be_sprites(const NwPacket *p, const NwCmd *c)
{
    const NwGpuSprites *s = c->sprites;
    if (!s || !s->vao) {
        return;
    }
    const NwView *v = &p->views[c->view];
    float screen[16];
    nw_be_screen(v, nw_be.width, nw_be.height, screen);
    NwVuObject o;
    nw_vu1init(&o, c->local, screen);
    if (s->sprites.facey) {
        float len = 0.0f;
        for (int r = 0; r < 3; r++) {
            o.sprite[r] = c->local[r * 4] * v->faceon[0] + c->local[r * 4 + 2] * v->faceon[2];
            len += o.sprite[r] * o.sprite[r];
        }
        for (int r = 0; r < 3; r++) {
            o.sprite[r] /= sqrtf(len);
        }
    } else {
        o.sprite[0] = v->faceon[1] / (8.0f * (float)nw_be.width);
        o.sprite[1] = -v->faceon[3] / (8.0f * (float)nw_be.height);
    }
    NwVuMaterial m = nw_be_material(p, c, false);
    nw_be_passes(s->sprites.facey ? NW_VU_FACEY : NW_VU_FACEON, &o, &m, s->vao, NULL, 0, s->sprites.count * 6);
}

/* Runs once into GS memory, which keeps what it drew; the front buffer is the back buffer as it ends. */
static void nw_be_run(const NwPacket *p, NwBeSurf *back)
{
    nw_be.runs++;
    back->ms = nw_be.samples > 1;
    if (back->ms) {
        nw_be_ms(back, NULL);
    } else {
        nw_be_resolve(back);
        nw_be_ms_free(back);
    }
    nw_be_bind(back, nw_be.shown_w, nw_be.shown_h);
    nw_be.alpha = true;
    nw_vu_alpha_writes(true);
    nw_rhi_depth(true, NW_RHI_LESS); /* each draw sets its test */
    nw_rhi_wireframe(nw_be.wireframe);
    nw_be_rects(p);
    nw_be_verts(p);
    nw_be_ptls(p);
    nw_be_pool(p);
    if (p->nidx) {
        if (!nw_be.ot_ibo) {
            nw_be.ot_ibo = nw_rhi_buffer_create(NW_RHI_INDICES, NW_RHI_RGBA8);
        }
        nw_rhi_buffer_data(nw_be.ot_ibo, (size_t)p->nidx * sizeof(uint32_t), p->idx, true);
    }
    uint32_t rects = 0;
    nw_be.nheld = 0;
    for (uint32_t i = 0; i < p->ncmds; i++) {
        const NwCmd *c = &p->cmds[i];
        nw_be_gems_flush(p, c);
        switch (c->op) {
        case NW_CMD_DRAW:
        case NW_CMD_DRAW_OT:
            nw_be_draw(p, c);
            break;
        case NW_CMD_CLEAR:
            nw_be_clear(p, c);
            break;
        case NW_CMD_RECT:
            nw_be_rect(p, c, rects++);
            break;
        case NW_CMD_SPRITES:
            nw_be_sprites(p, c);
            break;
        case NW_CMD_TARGET:
            nw_be_target(c);
            break;
        case NW_CMD_SKIN:
            nw_be_skin(p, c);
            break;
        case NW_CMD_VERTS:
            nw_be_tris(p, c);
            break;
        case NW_CMD_PARTICLES:
            nw_be_particles(p, c);
            break;
        case NW_CMD_VOLUMES:
            nw_be_volumes(c);
            break;
        case NW_CMD_WAVES:
            if (c->first < p->nwaves) {
                nw_vu_waves(p->waves[c->first].wave);
            }
            break;
        }
    }
    nw_be_gems_flush(p, NULL);
    nw_rhi_wireframe(false);
    nw_be_resolve(back);
    NwBeSurf *front = nw_be_surf(NW_SURF_FRONT, back->tw, back->th);
    if (front) {
        nw_rhi_blit(&(NwRhiBlit){ .src_colour = back->tex, .dst_colour = front->tex,
                                  .src = { 0, 0, back->tw * back->n, back->th * back->n },
                                  .dst = { 0, 0, front->tw * front->n, front->th * front->n }, .colour = true });
    }
    nw_be.drawn = p->seq;
}

/* Ps2TxValToPowerOfTwo: the exponent of the power of two at or above n. */
static int nw_be_log2(int n)
{
    int shift = 0;
    while ((1 << shift) < n) {
        shift++;
    }
    return shift;
}

void nw_backend_set_options(const NwRenderOptions *o)
{
    nw_be.opts = *o;
    nw_vu_filter(o->filter);
}

int nw_backend_scale(void)
{
    return nw_be.scale;
}

int nw_backend_msaa(void)
{
    return nw_be.samples;
}

static int nw_be_samples(void)
{
    int n = 1;
    while (n * 2 <= nw_be.opts.msaa && n * 2 <= nw_be.max_samples) {
        n *= 2;
    }
    return n > 1 ? n : 0;
}

/* Auto (scale 0) is the least that covers h window rows with bh GS rows. */
static int nw_be_scale(int h, int bh, int tw, int th)
{
    int n = nw_be.opts.scale ? nw_be.opts.scale : (h + bh - 1) / bh;
    n = SDL_clamp(n, 1, NW_BACKEND_SCALE_MAX);
    while (n > 1 && (tw * n > nw_be.max_tex || th * n > nw_be.max_tex)) {
        n--;
    }
    return n;
}

/* Shrinking more than 2x blits from a mip level at least the window's size, as glGenerateMipmap averages. */
static void nw_be_present(NwBeSurf *back, int bw, int bh, int x, int y, int w, int h)
{
    int n = back->n, level = 0;
    nw_be_resolve(back);
    while (level < 12 && (bw * n) >> (level + 1) >= w && (bh * n) >> (level + 1) >= h) {
        level++;
    }
    if (level) {
        nw_rhi_generate_mips(back->tex);
    }
    nw_rhi_blit(&(NwRhiBlit){ .src_colour = back->tex, .src_level = level,
                              .src = { 0, ((back->th - bh) * n) >> level, (bw * n) >> level, (back->th * n) >> level },
                              .dst = { x, y, x + w, y + h }, .colour = true, .linear = true });
}

const NwPacket *nw_backend_frame(int width, int height, double now)
{
    const NwPacket *prev;
    const NwPacket *p = nw_packet_take_pair(&prev);
    const NwPacket *run = p;
    float t = 1.0f;
    if (p && now >= 0.0 && nw_interp_redraws(p)) {
        t = nw_interp_t(prev, p, now);
        run = nw_interp_blend(prev, p, t);
    }
    uint32_t njobs = nw_gpu_take(p ? p->seq : 0, &nw_be.jobs, &nw_be.jobcap);
    for (uint32_t i = 0; i < njobs; i++) {
        nw_be_job(&nw_be.jobs[i]);
    }
    /* The back buffer at the size of the packet's first back-buffer target, 640 x 512 before one. */
    int bw = nw_be.shown_w ? nw_be.shown_w : 640, bh = nw_be.shown_h ? nw_be.shown_h : 512;
    for (uint32_t i = 0; p && i < p->ncmds; i++) {
        if (p->cmds[i].op == NW_CMD_TARGET && p->cmds[i].target.surf.addr == NW_SURF_BACK) {
            bw = p->cmds[i].target.width;
            bh = p->cmds[i].target.height;
            break;
        }
    }
    nw_be.shown_w = bw;
    nw_be.shown_h = bh;

    /* The PS2 shows 640 pixels across at 4:3 whatever the height; a wider frame (port/wide.h) keeps that pixel shape.
       Within 1% of the window's aspect it fills the window. */
    float aspect = 4.0f / 3.0f * (float)bw / 640.0f;
    int w = width, h = height;
    if (h > 0 && SDL_fabsf((float)w / (float)h - aspect) > 0.01f * aspect) {
        if ((float)w > (float)h * aspect) {
            w = (int)((float)h * aspect + 0.5f);
        } else {
            h = (int)((float)w / aspect + 0.5f);
        }
    }
    int tw = 1 << nw_be_log2(bw), th = 1 << nw_be_log2(bh);
    nw_be.scale = nw_be_scale(h, bh, tw, th);
    nw_be.samples = nw_be_samples();
    nw_rhi_colour_mask(true, true, true, true); /* the last pass's masks */
    nw_rhi_depth_write(true);
    NwBeSurf *back = nw_be_surf(NW_SURF_BACK, tw, th);
    if (back && p && (p->seq != nw_be.drawn || t != nw_be.drawn_t)) {
        nw_be_run(run, back);
        nw_be.drawn_t = t;
        nw_rhi_colour_mask(true, true, true, true);
        nw_rhi_depth_write(true);
    }
    nw_rhi_set_target(NULL, NULL);
    nw_rhi_viewport(0, 0, width, height);
    nw_rhi_clear(&(NwRhiClear){ .colour = true, .depth = true, .rgba = { 0.0f, 0.0f, 0.0f, 1.0f },
                                .z = nw_be.clear_z, .mask = { true, true, true, true }, .depth_write = true });
    if (back) {
        nw_be_present(back, bw, bh, (width - w) / 2, (height - h) / 2, w, h);
    }
    return p;
}
