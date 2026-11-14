/* The game's own art, from the imported disc at run time (gameart.h). */
#include "frontend/gameart.h"

#include <math.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx/gsc.h"
#include "gfx/tex.h"
#include "gfx/vif.h"
#include "vfs/extract.h"
#include "vfs/rnc.h"
#include "vfs/vfs.h"
#include "platform/audio.h"
#ifdef NW_WITH_GAME
#include "audio/sfx.h"
#include "audio/vag.h"
#include "vfs/dat.h"
#endif

/* ---- files ---------------------------------------------------------------------------------------------------- */

static uint32_t nw_art_rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* A file of the imported disc ("STUFF\\FONT.GSC"), RNC-unpacked; NULL when missing or damaged. */
static uint8_t *nw_art_file(const NwVfs *vfs, const char *path, size_t *size)
{
    char game[NW_VFS_PATH_MAX];
    SDL_snprintf(game, sizeof(game), "cdrom0:\\%s;1", path);
    NwVfsFile *f = nw_vfs_open(vfs, game, NW_VFS_READ);
    int64_t n = f ? nw_vfs_size(f) : -1;
    uint8_t *data = n > 0 && n < (64 << 20) ? SDL_malloc((size_t)n) : NULL;
    bool ok = data && nw_vfs_read(f, data, (size_t)n) == n;
    if (f) {
        nw_vfs_close(f);
    }
    if (!ok) {
        SDL_free(data);
        return NULL;
    }
    uint32_t unpacked, packed;
    if (n >= NW_RNC_HEADER && nw_rnc_header(data, (size_t)n, &unpacked, &packed)) {
        uint8_t *out = packed <= (uint64_t)n - NW_RNC_HEADER && unpacked <= (64u << 20) ? SDL_malloc(unpacked + 16) : NULL;
        int64_t got = out ? nw_rnc_unpack(data + NW_RNC_HEADER, data + NW_RNC_HEADER + packed, out, out + unpacked) : -1;
        SDL_free(data);
        if (got != (int64_t)unpacked) {
            SDL_free(out);
            return NULL;
        }
        data = out;
        n = unpacked;
    }
    *size = (size_t)n;
    return data;
}

/* ---- the software rasteriser ---------------------------------------------------------------------------------- */

typedef struct NwArtPix {
    int w, h;
    uint8_t *rgba; /* straight alpha, 255 = 1 */
} NwArtPix;

typedef struct NwArtVtx {
    float p[3];  /* model space */
    float uv[2];
    float c[4];  /* 1.0 = white */
} NwArtVtx;

typedef struct NwArtTri {
    NwArtVtx v[3];
    int tex;      /* into the model's textures, -1 none */
    bool lit;     /* shade by the face normal */
    int blend;    /* 0 opaque; drawn in the second pass without depth write: 1 alpha, 2 additive */
    int group;    /* who it belongs to (a special, a joint) */
} NwArtTri;

typedef struct NwArtModel {
    NwArtTri *tris;
    size_t ntris, cap;
    NwArtPix *texs;
    int ntexs;
    int remap_from, remap_to; /* draws texture remap_from as remap_to (the font's colours); equal: off */
} NwArtModel;

static void nw_art_model_free(NwArtModel *m)
{
    for (int i = 0; i < m->ntexs; i++) {
        SDL_free(m->texs[i].rgba);
    }
    SDL_free(m->texs);
    SDL_free(m->tris);
    memset(m, 0, sizeof(*m));
}

static NwArtTri *nw_art_tri(NwArtModel *m)
{
    if (m->ntris == m->cap) {
        size_t n = m->cap ? m->cap * 2 : 1024;
        NwArtTri *t = SDL_realloc(m->tris, n * sizeof(*t));
        if (!t) {
            return NULL;
        }
        m->tris = t;
        m->cap = n;
    }
    NwArtTri *t = &m->tris[m->ntris++];
    memset(t, 0, sizeof(*t));
    return t;
}

static bool nw_art_pix_from_tex(NwArtPix *p, const NwTex *t)
{
    p->w = (int)t->levels[0].width;
    p->h = (int)t->levels[0].height;
    p->rgba = SDL_malloc((size_t)p->w * p->h * 4 + 4);
    if (!p->rgba || !p->w || !p->h) {
        SDL_free(p->rgba);
        p->rgba = NULL;
        p->w = p->h = 0;
        return false;
    }
    for (int y = 0; y < p->h; y++) {
        nw_tex_rgba(t, (uint32_t)y, p->rgba + (size_t)y * p->w * 4);
    }
    return true;
}

/* Row-vector transform: p' = p * M, translation in the last row. */
static void nw_art_xform(const float m[16], const float in[3], float out[3])
{
    float x = in[0], y = in[1], z = in[2];
    out[0] = x * m[0] + y * m[4] + z * m[8] + m[12];
    out[1] = x * m[1] + y * m[5] + z * m[9] + m[13];
    out[2] = x * m[2] + y * m[6] + z * m[10] + m[14];
}

static void nw_art_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* A view: screen x and y (up) axes and the depth axis (away from the viewer), each a model-space direction. */
typedef struct NwArtView {
    float x[3], y[3], d[3];
} NwArtView;

static float nw_art_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Turns a view by yaw around its y axis, then pitch around its x axis (radians). */
static NwArtView nw_art_turn(NwArtView v, float yaw, float pitch)
{
    NwArtView r;
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    for (int i = 0; i < 3; i++) {
        float x = v.x[i] * cy + v.d[i] * sy, d = -v.x[i] * sy + v.d[i] * cy;
        r.x[i] = x;
        r.y[i] = v.y[i] * cp - d * sp;
        r.d[i] = v.y[i] * sp + d * cp;
    }
    return r;
}

typedef struct NwArtCanvas {
    int w, h;
    float *rgba; /* premultiplied */
    float *z;
} NwArtCanvas;

static void nw_art_sample(const NwArtPix *t, float u, float v, float out[4])
{
    float fx = u * (float)t->w - 0.5f, fy = v * (float)t->h - 0.5f;
    float x0f = floorf(fx), y0f = floorf(fy);
    float ax = fx - x0f, ay = fy - y0f;
    int x0 = (int)x0f, y0 = (int)y0f;
    float acc[4] = { 0 };
    for (int k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        x = ((x % t->w) + t->w) % t->w;
        y = ((y % t->h) + t->h) % t->h;
        float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
        const uint8_t *p = t->rgba + ((size_t)y * t->w + x) * 4;
        for (int c = 0; c < 4; c++) {
            acc[c] += wgt * (float)p[c] / 255.0f;
        }
    }
    memcpy(out, acc, sizeof(acc));
}

static void nw_art_raster(NwArtCanvas *cv, const NwArtModel *m, const NwArtTri *t, const float sx[3], const float sy[3],
                          const float sz[3], float shade)
{
    float minx = SDL_min(sx[0], SDL_min(sx[1], sx[2])), maxx = SDL_max(sx[0], SDL_max(sx[1], sx[2]));
    float miny = SDL_min(sy[0], SDL_min(sy[1], sy[2])), maxy = SDL_max(sy[0], SDL_max(sy[1], sy[2]));
    int x0 = SDL_max((int)floorf(minx), 0), x1 = SDL_min((int)ceilf(maxx), cv->w - 1);
    int y0 = SDL_max((int)floorf(miny), 0), y1 = SDL_min((int)ceilf(maxy), cv->h - 1);
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-8f || x0 > x1 || y0 > y1) {
        return;
    }
    int ti = t->tex == m->remap_from && m->remap_from != m->remap_to ? m->remap_to : t->tex;
    const NwArtPix *tex = ti >= 0 && ti < m->ntexs && m->texs[ti].rgba ? &m->texs[ti] : NULL;
    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
            float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) {
                continue;
            }
            float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
            size_t at = (size_t)y * cv->w + x;
            if (z >= cv->z[at]) {
                continue;
            }
            float c[4];
            for (int k = 0; k < 4; k++) {
                c[k] = w0 * t->v[0].c[k] + w1 * t->v[1].c[k] + w2 * t->v[2].c[k];
            }
            if (tex) {
                float s[4];
                nw_art_sample(tex, w0 * t->v[0].uv[0] + w1 * t->v[1].uv[0] + w2 * t->v[2].uv[0],
                              w0 * t->v[0].uv[1] + w1 * t->v[1].uv[1] + w2 * t->v[2].uv[1], s);
                for (int k = 0; k < 4; k++) {
                    c[k] *= s[k];
                }
            }
            c[0] *= shade;
            c[1] *= shade;
            c[2] *= shade;
            float a = SDL_clamp(c[3], 0.0f, 1.0f);
            if (a < 0.25f && !t->blend) {
                continue;
            }
            float *d = cv->rgba + at * 4;
            if (t->blend == 2) {
                float lum = SDL_max(c[0], SDL_max(c[1], c[2])) * a;
                for (int k = 0; k < 3; k++) {
                    d[k] = SDL_min(d[k] + SDL_min(c[k], 1.0f) * a, 1.0f);
                }
                d[3] = SDL_min(d[3] + lum, 1.0f);
            } else if (t->blend) {
                for (int k = 0; k < 3; k++) {
                    d[k] = SDL_min(c[k], 1.0f) * a + d[k] * (1 - a);
                }
                d[3] = a + d[3] * (1 - a);
            } else {
                for (int k = 0; k < 3; k++) {
                    d[k] = SDL_min(c[k], 1.0f);
                }
                d[3] = 1.0f;
                cv->z[at] = z;
            }
        }
    }
}

typedef struct NwArtBox {
    float x0, y0, x1, y1;
} NwArtBox;

/* Bounds of the model's triangles of group (-1: all) in the view. */
static NwArtBox nw_art_bounds(const NwArtModel *m, const NwArtView *v, int group)
{
    NwArtBox b = { 1e30f, 1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < m->ntris; i++) {
        if (group >= 0 && m->tris[i].group != group) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            float x = nw_art_dot(m->tris[i].v[k].p, v->x), y = nw_art_dot(m->tris[i].v[k].p, v->y);
            b.x0 = SDL_min(b.x0, x);
            b.x1 = SDL_max(b.x1, x);
            b.y0 = SDL_min(b.y0, y);
            b.y1 = SDL_max(b.y1, y);
        }
    }
    return b;
}

/* Draws group (-1 all) of m in view v so that box (view units) fills w x h, 3x3 supersampled; RGBA8 straight alpha. */
static uint8_t *nw_art_render(const NwArtModel *m, const NwArtView *v, int group, NwArtBox box, int w, int h, int ss)
{
    NwArtCanvas cv = { w * ss, h * ss, NULL, NULL };
    cv.rgba = SDL_calloc((size_t)cv.w * cv.h, 16);
    cv.z = SDL_malloc((size_t)cv.w * cv.h * sizeof(float));
    uint8_t *out = SDL_malloc((size_t)w * h * 4);
    if (!cv.rgba || !cv.z || !out) {
        SDL_free(cv.rgba);
        SDL_free(cv.z);
        SDL_free(out);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)cv.w * cv.h; i++) {
        cv.z[i] = 1e30f;
    }
    float bw = SDL_max(box.x1 - box.x0, 1e-6f), bh = SDL_max(box.y1 - box.y0, 1e-6f);
    float scale = SDL_min((float)cv.w / bw, (float)cv.h / bh);
    float ox = ((float)cv.w - bw * scale) * 0.5f, oy = ((float)cv.h - bh * scale) * 0.5f;
    static const float light[3] = { -0.35f, 0.55f, -0.76f }; /* screen space: from the upper left, towards the viewer */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < m->ntris; i++) {
            const NwArtTri *t = &m->tris[i];
            if ((group >= 0 && t->group != group) || (t->blend != 0) != (pass == 1)) {
                continue;
            }
            float sx[3], sy[3], sz[3], q[3][3];
            for (int k = 0; k < 3; k++) {
                q[k][0] = nw_art_dot(t->v[k].p, v->x);
                q[k][1] = nw_art_dot(t->v[k].p, v->y);
                q[k][2] = nw_art_dot(t->v[k].p, v->d);
                sx[k] = ox + (q[k][0] - box.x0) * scale;
                sy[k] = oy + (box.y1 - q[k][1]) * scale;
                sz[k] = q[k][2];
            }
            float shade = 1.0f;
            if (t->lit) {
                float a[3] = { q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2] };
                float b[3] = { q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2] };
                float n[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
                float len = sqrtf(nw_art_dot(n, n));
                if (len > 0.0f) {
                    for (int k = 0; k < 3; k++) {
                        n[k] /= len;
                    }
                    if (n[2] > 0.0f) { /* face the viewer */
                        n[0] = -n[0];
                        n[1] = -n[1];
                        n[2] = -n[2];
                    }
                    shade = 0.5f + 0.62f * SDL_max(nw_art_dot(n, light), 0.0f);
                }
            }
            nw_art_raster(&cv, m, t, sx, sy, sz, shade);
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float acc[4] = { 0 };
            for (int j = 0; j < ss; j++) {
                for (int i = 0; i < ss; i++) {
                    const float *p = cv.rgba + ((size_t)(y * ss + j) * cv.w + x * ss + i) * 4;
                    for (int k = 0; k < 4; k++) {
                        acc[k] += p[k];
                    }
                }
            }
            uint8_t *o = out + ((size_t)y * w + x) * 4;
            float a = acc[3] / (float)(ss * ss);
            for (int k = 0; k < 3; k++) {
                o[k] = (uint8_t)SDL_clamp(a > 0 ? acc[k] / (float)(ss * ss) / a * 255.0f + 0.5f : 0.0f, 0.0f, 255.0f);
            }
            o[3] = (uint8_t)SDL_clamp(a * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    }
    SDL_free(cv.rgba);
    SDL_free(cv.z);
    return out;
}

/* ---- scenes (.gsc): specials ---------------------------------------------------------------------------------- */

typedef struct NwArtSpecial {
    char name[32];
    uint32_t inst;
} NwArtSpecial;

typedef struct NwArtScene {
    uint8_t *data;
    size_t size;
    NwGsc gsc;
    bool open;
    NwArtSpecial *specials;
    uint32_t nspecials;
} NwArtScene;

static void nw_art_scene_close(NwArtScene *s)
{
    if (s->open) {
        nw_gsc_close(&s->gsc);
    }
    SDL_free(s->specials);
    SDL_free(s->data);
    memset(s, 0, sizeof(*s));
}

static const NwGscChunk *nw_art_chunk(const NwGsc *g, const char id[4])
{
    uint32_t want = NW_GSC_ID(id[0], id[1], id[2], id[3]);
    for (uint32_t i = 0; i < g->nchunks; i++) {
        if (g->chunks[i].id == want) {
            return &g->chunks[i];
        }
    }
    return NULL;
}

static bool nw_art_scene_open(NwArtScene *s, const NwVfs *vfs, const char *path)
{
    memset(s, 0, sizeof(*s));
    s->data = nw_art_file(vfs, path, &s->size);
    if (!s->data || !nw_gsc_open(&s->gsc, s->data, s->size)) {
        SDL_free(s->data);
        s->data = NULL;
        return false;
    }
    s->open = true;
    const NwGscChunk *ntbl = nw_art_chunk(&s->gsc, "NTBL"), *spec = nw_art_chunk(&s->gsc, "SPEC");
    if (ntbl && spec && ntbl->size >= 4 && spec->size >= 8) {
        uint32_t nlen = nw_art_rd32(s->data + ntbl->offset);
        const char *names = (const char *)s->data + ntbl->offset + 4;
        uint32_t n = nw_art_rd32(s->data + spec->offset);
        if (nlen <= ntbl->size - 4 && n <= (spec->size - 8) / 0x50) {
            s->specials = SDL_calloc(n ? n : 1, sizeof(*s->specials));
            for (uint32_t i = 0; s->specials && i < n; i++) {
                const uint8_t *r = s->data + spec->offset + 8 + i * 0x50;
                uint32_t inst = nw_art_rd32(r + 0x40), name = nw_art_rd32(r + 0x44);
                if (name >= nlen || inst >= s->gsc.ninsts) {
                    continue;
                }
                NwArtSpecial *sp = &s->specials[s->nspecials++];
                SDL_strlcpy(sp->name, names + name, SDL_min(sizeof(sp->name), (size_t)(nlen - name)));
                for (char *c = sp->name; *c; c++) {
                    *c = (char)SDL_tolower((unsigned char)*c);
                }
                sp->inst = inst;
            }
        }
    }
    return true;
}

static int nw_art_find(const NwArtScene *s, const char *name)
{
    for (uint32_t i = 0; i < s->nspecials; i++) {
        if (SDL_strcmp(s->specials[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The scene's textures into m (indices as the materials' tid). */
static void nw_art_scene_textures(const NwArtScene *s, NwArtModel *m)
{
    m->texs = SDL_calloc(s->gsc.ntextures ? s->gsc.ntextures : 1, sizeof(*m->texs));
    if (!m->texs) {
        return;
    }
    m->ntexs = (int)s->gsc.ntextures;
    for (uint32_t i = 0; i < s->gsc.ntextures; i++) {
        NwTex t;
        if (nw_gsc_texture(&s->gsc, i, &t)) {
            nw_art_pix_from_tex(&m->texs[i], &t);
            nw_tex_free(&t);
        }
    }
}

/* Appends object objid placed by mtx to m as group. */
static void nw_art_scene_object(const NwArtScene *s, NwArtModel *m, uint32_t objid, const float mtx[16], int group)
{
    const NwGsc *g = &s->gsc;
    if (objid >= g->nobjects) {
        return;
    }
    const NwGscObject *obj = &g->objects[objid];
    for (uint32_t b = obj->first; b < obj->first + obj->count; b++) {
        const NwGscBlob *blob = &g->blobs[b];
        NwVifMesh mesh = { 0 };
        if (blob->faceon || !(blob->ot ? nw_vif_otbatch(&mesh, g->data + blob->offset, blob->size)
                                         : nw_vif_decode(&mesh, g->data + blob->offset, blob->size))) {
            nw_vif_free(&mesh);
            continue;
        }
        NwGscMtl mt = nw_gsc_mtl(g, blob->mtl);
        bool unlit = NW_GSC_ATTRIB_LIGHTING(mt.attrib) == 2;
        bool colour = unlit && NW_GSC_ATTRIB_COLOUR(mt.attrib) != 0; /* lit geoms carry no colours */
        for (uint32_t i = 0; i + 2 < mesh.ntris * 3; i += 3) {
            NwArtTri *t = nw_art_tri(m);
            if (!t) {
                break;
            }
            t->tex = mt.tid;
            t->lit = !unlit;
            t->blend = NW_GSC_ATTRIB_ALPHA(mt.attrib) == 2 ? 2 : NW_GSC_ATTRIB_ALPHA(mt.attrib) != 0;
            t->group = group;
            for (int k = 0; k < 3; k++) {
                const NwVifVertex *v = &mesh.verts[mesh.tris[i + k]];
                nw_art_xform(mtx, v->pos, t->v[k].p);
                t->v[k].uv[0] = (float)v->tex[0] / 4096.0f;
                t->v[k].uv[1] = (float)v->tex[1] / 4096.0f;
                for (int c = 0; c < 3; c++) {
                    t->v[k].c[c] = (colour ? (float)v->col[c] / 128.0f : 1.0f) * (unlit ? 1.0f : mt.diffuse[c] * (mt.tid >= 0 ? 2.0f : 1.0f));
                }
                t->v[k].c[3] = (colour ? (float)v->col[3] / 128.0f : 1.0f) * mt.alpha;
            }
        }
        nw_vif_free(&mesh);
    }
}

static void nw_art_scene_special(const NwArtScene *s, NwArtModel *m, int special, int group)
{
    NwGscInst inst = nw_gsc_inst(&s->gsc, s->specials[special].inst);
    nw_art_scene_object(s, m, (uint32_t)inst.objid, inst.mtx, group);
}

/* ---- models (.ghg) -------------------------------------------------------------------------------------------- */

#define NW_GHG_HEADER 0x90

static int32_t nw_art_i32(const uint8_t *d, size_t size, uint32_t at)
{
    return at + 4 <= size ? (int32_t)nw_art_rd32(d + at) : 0;
}

/* Every rigid and skinned geom of a .ghg in its bind pose; joint world matrices from the t chain. */
static bool nw_art_ghg(const uint8_t *d, size_t size, NwArtModel *m, char *info, size_t info_size)
{
    if (size < NW_GHG_HEADER || nw_art_rd32(d) != size) {
        return false;
    }
    int ntex = nw_art_i32(d, size, 0x08), tids = nw_art_i32(d, size, 0x0C);
    int nmtl = nw_art_i32(d, size, 0x10), mtls = nw_art_i32(d, size, 0x14);
    int nj = nw_art_i32(d, size, 0x18), joints = nw_art_i32(d, size, 0x1C), tm = nw_art_i32(d, size, 0x20);
    int nlayers = nw_art_i32(d, size, 0x48), layers = nw_art_i32(d, size, 0x4C);
    if (ntex < 0 || ntex > 512 || nmtl < 0 || nmtl > 1024 || nj < 0 || nj > 512 || nlayers < 0 || nlayers > 16) {
        return false;
    }
    m->texs = SDL_calloc((size_t)ntex + 1, sizeof(*m->texs));
    m->ntexs = ntex;
    for (int i = 0; m->texs && i < ntex; i++) {
        uint32_t at = (uint32_t)nw_art_i32(d, size, (uint32_t)(tids + 4 * i));
        NwTex t;
        if (at && at < size && nw_tex_decode(&t, d + at, size - at)) {
            nw_art_pix_from_tex(&m->texs[i], &t);
            nw_tex_free(&t);
        }
    }
    float (*world)[16] = SDL_calloc((size_t)nj + 1, sizeof(*world));
    if (!world) {
        return false;
    }
    for (int j = 0; j < nj; j++) {
        float local[16];
        if ((size_t)tm + (size_t)(j + 1) * 64 > size || (size_t)joints + (size_t)(j + 1) * 0x60 > size) {
            SDL_free(world);
            return false;
        }
        memcpy(local, d + tm + j * 64, 64);
        int parent = (int8_t)d[joints + j * 0x60 + 0x50];
        if (parent >= 0 && parent < j) {
            nw_art_mul(local, world[parent], world[j]);
        } else {
            memcpy(world[j], local, 64);
        }
    }
    size_t used = 0;
    if (info) {
        used += (size_t)SDL_snprintf(info + used, info_size - used, "textures %d materials %d joints %d layers %d\n", ntex,
                                     nmtl, nj, nlayers);
        for (int j = 0; j < nj && used < info_size; j++) {
            uint32_t name = (uint32_t)nw_art_i32(d, size, (uint32_t)(joints + j * 0x60 + 0x4C));
            used += (size_t)SDL_snprintf(info + used, info_size - used, "  joint %d %s parent %d at %.2f %.2f %.2f\n", j,
                                         name && name < size ? (const char *)d + name : "-",
                                         (int8_t)d[joints + j * 0x60 + 0x50], world[j][12], world[j][13], world[j][14]);
        }
    }
    int anim[64] = { 0 }; /* a texture-animated material's tid: the last frame */
    int nanims = nw_art_i32(d, size, 0x88), anims = nw_art_i32(d, size, 0x8C);
    for (int i = 0; i >= 0 && i < nanims && i < 64 && (size_t)anims + (size_t)(i + 1) * 0x20 <= size; i++) {
        const uint8_t *r = d + anims + i * 0x20;
        int32_t list = (int32_t)nw_art_rd32(r + 8), mt = (int32_t)nw_art_rd32(r + 0x10);
        int16_t count;
        memcpy(&count, r + 0x0C, 2);
        if (mt >= nmtl) { /* a pointer: find it */
            for (int k = 0; k < nmtl; k++) {
                if (nw_art_i32(d, size, (uint32_t)(mtls + 4 * k)) == mt) {
                    mt = k;
                }
            }
        }
        if (mt >= 0 && mt < 64 && count > 0 && list > 0 && (size_t)list + (size_t)count * 2 <= size) {
            int16_t tid;
            memcpy(&tid, d + list + (count - 1) * 2, 2);
            anim[mt] = tid;
            if (info) {
                used += (size_t)SDL_snprintf(info + used, info_size - used, "  texanim mtl %d frames %d last tid %d\n", mt, count, tid);
            }
        }
    }
    for (int i = 0; info && i < nmtl && used < info_size; i++) {
        uint32_t mo = (uint32_t)nw_art_i32(d, size, (uint32_t)(mtls + 4 * i));
        if (mo && (size_t)mo + 0x1D0 <= size) {
            uint32_t a[2];
            int32_t tid;
            float df[3], al;
            memcpy(a, d + mo + 0x16C, 8);
            memcpy(df, d + mo + 0x180, 12);
            memcpy(&al, d + mo + 0x1A0, 4);
            memcpy(&tid, d + mo + 0x1A4, 4);
            used += (size_t)SDL_snprintf(info + used, info_size - used,
                                         "  mtl %d tid %d lighting %u colour %u alpha %u fx %u uv %u/%u du %.3f dv %.3f diffuse %.2f %.2f %.2f a %.2f\n",
                                         i, tid, NW_GSC_ATTRIB_LIGHTING(a), NW_GSC_ATTRIB_COLOUR(a), NW_GSC_ATTRIB_ALPHA(a),
                                         NW_GSC_ATTRIB_FX(a), d[mo + 0x1AF] & 15, d[mo + 0x1AF] >> 4,
                                         *(const float *)(d + mo + 0x1B0), *(const float *)(d + mo + 0x1B4), df[0], df[1], df[2], al);
        }
    }
    for (int l = 0; l < nlayers; l++) {
        uint32_t lay = (uint32_t)(layers + l * 0x14);
        int32_t gobjs = nw_art_i32(d, size, lay + 4), skin = nw_art_i32(d, size, lay + 8);
        for (int j = -1; j < nj; j++) {
            /* j -1: the skin chain */
            int32_t gobj = j < 0 ? skin : gobjs ? nw_art_i32(d, size, (uint32_t)(gobjs + 4 * j)) : 0;
            for (int guard = 0; gobj > 0 && (size_t)gobj + 0x5C <= size && guard < 4096; guard++) {
                int32_t geom = nw_art_i32(d, size, (uint32_t)gobj + 0x0C);
                for (int g2 = 0; geom > 0 && (size_t)geom + 16 <= size && g2 < 4096; g2++) {
                    int32_t next = nw_art_i32(d, size, (uint32_t)geom), mtl = nw_art_i32(d, size, (uint32_t)geom + 4);
                    int32_t dma = nw_art_i32(d, size, (uint32_t)geom + 8), sk = nw_art_i32(d, size, (uint32_t)geom + 12);
                    uint32_t attrib[2] = { 0, 0 };
                    int32_t tid = 0;
                    float diffuse[3] = { 1, 1, 1 }, alpha = 1.0f;
                    if (mtl >= 0 && mtl < nmtl) {
                        uint32_t mo = (uint32_t)nw_art_i32(d, size, (uint32_t)(mtls + 4 * mtl));
                        if (mo && (size_t)mo + 0x1D0 <= size) {
                            memcpy(attrib, d + mo + 0x16C, 8);
                            memcpy(diffuse, d + mo + 0x180, 12);
                            memcpy(&alpha, d + mo + 0x1A0, 4);
                            memcpy(&tid, d + mo + 0x1A4, 4);
                        }
                    }
                    bool unlit = NW_GSC_ATTRIB_LIGHTING(attrib) == 2, colour = NW_GSC_ATTRIB_COLOUR(attrib) != 0;
                    int blend = NW_GSC_ATTRIB_ALPHA(attrib) == 2 ? 2 : NW_GSC_ATTRIB_ALPHA(attrib) != 0;
                    int tex = mtl >= 0 && mtl < 64 && anim[mtl] ? anim[mtl] - 1 : tid - 1;
                    if (tex >= 0) {
                        for (int c = 0; c < 3; c++) {
                            diffuse[c] *= 2.0f;
                        }
                    }
                    if (info && used < info_size) {
                        used += (size_t)SDL_snprintf(info + used, info_size - used, "  geom joint %d mtl %d %s\n", j, mtl,
                                                     sk > 0 ? "skin" : "rigid");
                    }
                    if (sk > 0 && (size_t)sk < size) {
                        NwVifSkin s;
                        if (nw_vif_skin(&s, d + sk, size - (size_t)sk)) {
                            for (uint32_t i = 0; i + 2 < s.ntris * 3; i += 3) {
                                NwArtTri *t = nw_art_tri(m);
                                if (!t) {
                                    break;
                                }
                                t->tex = tex;
                                t->lit = !unlit;
                                t->blend = blend;
                                t->group = j < 0 ? 0 : j;
                                for (int k = 0; k < 3; k++) {
                                    const NwVifSkinVertex *v = &s.verts[s.tris[i + k]];
                                    memcpy(t->v[k].p, v->pos, 12);
                                    t->v[k].uv[0] = v->uv[0];
                                    t->v[k].uv[1] = v->uv[1];
                                    for (int c = 0; c < 3; c++) {
                                        t->v[k].c[c] = unlit ? 1.0f : diffuse[c];
                                    }
                                    t->v[k].c[3] = alpha;
                                }
                            }
                            nw_vif_skin_free(&s);
                        }
                    } else if (dma > 0 && (size_t)dma < size) {
                        NwVifMesh mesh = { 0 };
                        if (nw_vif_decode(&mesh, d + dma, size - (size_t)dma)) {
                            for (uint32_t i = 0; i + 2 < mesh.ntris * 3; i += 3) {
                                NwArtTri *t = nw_art_tri(m);
                                if (!t) {
                                    break;
                                }
                                t->tex = tex;
                                t->lit = !unlit;
                                t->blend = blend;
                                t->group = j < 0 ? 0 : j;
                                for (int k = 0; k < 3; k++) {
                                    const NwVifVertex *v = &mesh.verts[mesh.tris[i + k]];
                                    nw_art_xform(j >= 0 ? world[j] : (const float[16]){ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
                                                 v->pos, t->v[k].p);
                                    t->v[k].uv[0] = (float)v->tex[0] / 4096.0f;
                                    t->v[k].uv[1] = (float)v->tex[1] / 4096.0f;
                                    for (int c = 0; c < 3; c++) {
                                        t->v[k].c[c] = (colour ? (float)v->col[c] / 128.0f : 1.0f) * (unlit ? 1.0f : diffuse[c]);
                                    }
                                    t->v[k].c[3] = (colour ? (float)v->col[3] / 128.0f : 1.0f) * alpha;
                                }
                            }
                        }
                        nw_vif_free(&mesh);
                    }
                    geom = next;
                }
                gobj = nw_art_i32(d, size, (uint32_t)gobj + 0x4C);
            }
        }
    }
    SDL_free(world);
    return true;
}

/* ---- loading -------------------------------------------------------------------------------------------------- */

typedef struct NwArtState {
    bool loaded;
    NwArtImage images[NW_ART_COUNT];
    NwArtFont font;
    bool have_font;
    float *sounds[NW_SOUND_COUNT]; /* 48 kHz stereo */
    unsigned sound_frames[NW_SOUND_COUNT];
    bool audio;
} NwArtState;

static NwArtState nw_art;

/* What loading makes, before it goes to the GPU (and what --dump-art saves). */
typedef struct NwArtBuilt {
    uint8_t *rgba[NW_ART_COUNT];
    NwArtImage images[NW_ART_COUNT];
    uint8_t *font;
    int font_w, font_h;
    NwArtFont glyphs;
    bool have_font;
} NwArtBuilt;

static void nw_art_built_free(NwArtBuilt *b)
{
    for (int i = 0; i < NW_ART_COUNT; i++) {
        SDL_free(b->rgba[i]);
    }
    SDL_free(b->font);
    memset(b, 0, sizeof(*b));
}

static const NwArtView nw_art_front = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
static const NwArtView nw_art_title = { { 1, 0, 0 }, { 0, 0, 1 }, { 0, -1, 0 } };

static NwArtBox nw_art_pad(NwArtBox b, float k)
{
    float px = (b.x1 - b.x0) * k, py = (b.y1 - b.y0) * k;
    return (NwArtBox){ b.x0 - px, b.y0 - py, b.x1 + px, b.y1 + py };
}

/* A special of s drawn in a three-quarter view (crates) or from the front, size x size. */
static uint8_t *nw_art_special_image(const NwArtScene *s, NwArtModel *m, const char *name, bool turned, int size)
{
    int i = nw_art_find(s, name);
    if (i < 0) {
        return NULL;
    }
    size_t first = m->ntris;
    nw_art_scene_special(s, m, i, i);
    if (m->ntris == first) {
        return NULL;
    }
    NwArtView v = turned ? nw_art_turn(nw_art_front, 0.62f, -0.38f) : nw_art_front;
    return nw_art_render(m, &v, i, nw_art_pad(nw_art_bounds(m, &v, i), 0.03f), size, size, 3);
}

/* The fruit turning around its upright axis: frames of size, cols per row, one box for all. */
static uint8_t *nw_art_spin(const NwArtScene *s, NwArtModel *m, const char *name, int frames, int cols, int size)
{
    int i = nw_art_find(s, name);
    if (i < 0) {
        return NULL;
    }
    size_t first = m->ntris;
    nw_art_scene_special(s, m, i, i);
    int rows = (frames + cols - 1) / cols;
    uint8_t *sheet = m->ntris > first ? SDL_calloc((size_t)cols * size * rows * size, 4) : NULL;
    if (!sheet) {
        return NULL;
    }
    NwArtBox box = { 1e30f, 1e30f, -1e30f, -1e30f };
    for (int f = 0; f < frames; f++) {
        NwArtView v = nw_art_turn(nw_art_front, 6.2831853f * (float)f / (float)frames, -0.25f);
        NwArtBox b = nw_art_bounds(m, &v, i);
        box = (NwArtBox){ SDL_min(box.x0, b.x0), SDL_min(box.y0, b.y0), SDL_max(box.x1, b.x1), SDL_max(box.y1, b.y1) };
    }
    box = nw_art_pad(box, 0.02f);
    for (int f = 0; f < frames; f++) {
        NwArtView v = nw_art_turn(nw_art_front, 6.2831853f * (float)f / (float)frames, -0.25f);
        uint8_t *px = nw_art_render(m, &v, i, box, size, size, 3);
        if (!px) {
            continue;
        }
        int ox = f % cols * size, oy = f / cols * size;
        for (int y = 0; y < size; y++) {
            memcpy(sheet + ((size_t)(oy + y) * cols * size + ox) * 4, px + (size_t)y * size * 4, (size_t)size * 4);
        }
        SDL_free(px);
    }
    return sheet;
}

/* game/font3d.c's Font3DTab: the character and its special. */
static const struct {
    char c;
    const char *name;
} nw_art_glyph_names[] = {
    { '0', "no0" }, { '1', "no1" }, { '2', "no2" }, { '3', "no3" }, { '4', "no4" }, { '5', "no5" }, { '6', "no6" },
    { '7', "no7" }, { '8', "no8" }, { '9', "no9" }, { 'A', "a" }, { 'B', "b" }, { 'C', "c" }, { 'D', "d" },
    { 'E', "e" }, { 'F', "f" }, { 'G', "g" }, { 'H', "h" }, { 'I', "i" }, { 'J', "j" }, { 'K', "k" }, { 'L', "l" },
    { 'M', "m" }, { 'N', "n" }, { 'O', "o" }, { 'P', "p" }, { 'Q', "q" }, { 'R', "r" }, { 'S', "s" }, { 'T', "t" },
    { 'U', "u" }, { 'V', "v" }, { 'W', "w" }, { 'X', "x" }, { 'Y', "y" }, { 'Z', "z" }, { '%', "percent" },
    { '/', "forward_slash" }, { '?', "question_mark" }, { '!', "exclamation" }, { ':', "colon" }, { '.', "fullstop" },
    { '-', "minus" }, { '+', "plus" }, { '_', "under_score" }, { '<', "left_arrow" }, { '>', "right_arrow" },
    { '(', "bracket_l" }, { ')', "bracket_r" }, { ',', "comma" }, { '=', "equals" }, { '\'', "apostrophe" },
};

#define NW_ART_GLYPHS (sizeof(nw_art_glyph_names) / sizeof(nw_art_glyph_names[0]))
#define NW_ART_CELL 64          /* the atlas' glyph height, texels */
#define NW_ART_CELL_UNITS 1.44f /* the font's cell height in its own units (capitals are about 1.15) */
#define NW_ART_ATLAS_W 1024

/* The faces' material (tid 0, Text3D's orange) swapped for each colour's texture. */
static const int nw_art_ink_tex[NW_INK_COUNT] = { 2, 0, 3, 4 };

static bool nw_art_font(const NwArtScene *s, NwArtBuilt *out)
{
    NwArtModel m = { 0 };
    nw_art_scene_textures(s, &m);
    int ids[NW_ART_GLYPHS];
    NwArtBox boxes[NW_ART_GLYPHS];
    struct {
        int g, ink, x, y, w;
    } place[NW_ART_GLYPHS * NW_INK_COUNT];
    int x = 0, y = 0, count = 0;
    for (size_t g = 0; g < NW_ART_GLYPHS; g++) {
        ids[g] = nw_art_find(s, nw_art_glyph_names[g].name);
        if (ids[g] < 0) {
            continue;
        }
        NwGscInst inst = nw_gsc_inst(&s->gsc, s->specials[ids[g]].inst);
        nw_art_scene_object(s, &m, (uint32_t)inst.objid, inst.mtx, (int)g);
        NwArtBox b = nw_art_bounds(&m, &nw_art_front, (int)g);
        if (b.x1 < b.x0) {
            ids[g] = -1;
            continue;
        }
        float oy = inst.mtx[13];
        boxes[g] = (NwArtBox){ b.x0 - 0.05f, oy - NW_ART_CELL_UNITS * 0.5f, b.x1 + 0.05f, oy + NW_ART_CELL_UNITS * 0.5f };
    }
    for (int ink = 0; ink < NW_INK_COUNT; ink++) {
        for (size_t g = 0; g < NW_ART_GLYPHS; g++) {
            if (ids[g] < 0) {
                continue;
            }
            int w = (int)ceilf((boxes[g].x1 - boxes[g].x0) / NW_ART_CELL_UNITS * NW_ART_CELL);
            if (x + w + 2 > NW_ART_ATLAS_W) {
                x = 0;
                y += NW_ART_CELL + 2;
            }
            place[count].g = (int)g;
            place[count].ink = ink;
            place[count].x = x;
            place[count].y = y;
            place[count].w = w;
            count++;
            x += w + 2;
        }
    }
    int h = (y + NW_ART_CELL + 2 + 3) & ~3;
    uint8_t *atlas = count ? SDL_calloc((size_t)NW_ART_ATLAS_W * h, 4) : NULL;
    if (!atlas) {
        nw_art_model_free(&m);
        return false;
    }
    for (int k = 0; k < count; k++) {
        int g = place[k].g, ink = place[k].ink;
        m.remap_from = 0;
        m.remap_to = nw_art_ink_tex[ink];
        NwArtBox b = boxes[g];
        b.x1 = b.x0 + (float)place[k].w / NW_ART_CELL * NW_ART_CELL_UNITS; /* square texels */
        uint8_t *px = nw_art_render(&m, &nw_art_front, g, b, place[k].w, NW_ART_CELL, 3);
        if (!px) {
            continue;
        }
        for (int r = 0; r < NW_ART_CELL; r++) {
            memcpy(atlas + ((size_t)(place[k].y + r) * NW_ART_ATLAS_W + place[k].x) * 4, px + (size_t)r * place[k].w * 4,
                   (size_t)place[k].w * 4);
        }
        SDL_free(px);
        unsigned char c = (unsigned char)nw_art_glyph_names[g].c;
        NwArtGlyph *gl = &out->glyphs.glyphs[ink][c];
        gl->u0 = (float)place[k].x / NW_ART_ATLAS_W;
        gl->v0 = (float)place[k].y / (float)h;
        gl->u1 = (float)(place[k].x + place[k].w) / NW_ART_ATLAS_W;
        gl->v1 = (float)(place[k].y + NW_ART_CELL) / (float)h;
        gl->w = (float)place[k].w / NW_ART_CELL;
        out->glyphs.have[c] = true;
    }
    out->glyphs.advance = 1.0f / 0.7f / NW_ART_CELL_UNITS; /* Text3D: 0.1 a letter at a size of 0.07 */
    out->font = atlas;
    out->font_w = NW_ART_ATLAS_W;
    out->font_h = h;
    out->have_font = true;
    m.remap_from = m.remap_to = 0;
    nw_art_model_free(&m);
    return true;
}

static void nw_art_set(NwArtBuilt *b, NwArtId id, uint8_t *rgba, int w, int h, int frames, int cols)
{
    if (!rgba) {
        return;
    }
    SDL_free(b->rgba[id]);
    b->rgba[id] = rgba;
    b->images[id] = (NwArtImage){ NULL, w, h, frames, cols };
}

static void nw_art_build(const NwVfs *vfs, NwArtBuilt *b)
{
    memset(b, 0, sizeof(*b));
    Uint64 t0 = SDL_GetTicks();
    size_t size;
    uint8_t *d = nw_art_file(vfs, "LEVELS\\B\\LOGOS\\TITLE.GHG", &size);
    if (d) {
        NwArtModel m = { 0 };
        if (nw_art_ghg(d, size, &m, NULL, 0) && m.ntris) {
            NwArtBox box = nw_art_pad(nw_art_bounds(&m, &nw_art_title, -1), 0.01f);
            int w = 900, h = (int)((float)w * (box.y1 - box.y0) / (box.x1 - box.x0));
            h = SDL_clamp(h, 64, 1200);
            nw_art_set(b, NW_ART_LOGO, nw_art_render(&m, &nw_art_title, -1, box, w, h, 2), w, h, 1, 1);
        }
        nw_art_model_free(&m);
        SDL_free(d);
    }
    NwArtScene s;
    if (nw_art_scene_open(&s, vfs, "STUFF\\CRATES.GSC")) {
        static const struct {
            NwArtId id;
            const char *name;
        } crates[] = {
            { NW_ART_CRATE, "crate" },             { NW_ART_CRATE_Q, "question_crate" },
            { NW_ART_CRATE_AKU, "aku_aku_crate" }, { NW_ART_CRATE_NITRO, "nitro_crate" },
            { NW_ART_CRATE_TNT, "tnt_crate" },     { NW_ART_CRATE_CHECK, "checkpoint_crate" },
        };
        NwArtModel m = { 0 };
        nw_art_scene_textures(&s, &m);
        for (size_t i = 0; i < SDL_arraysize(crates); i++) {
            nw_art_set(b, crates[i].id, nw_art_special_image(&s, &m, crates[i].name, true, 192), 192, 192, 1, 1);
        }
        nw_art_model_free(&m);
        nw_art_scene_close(&s);
    }
    if (nw_art_scene_open(&s, vfs, "STUFF\\WUMPA.GSC")) {
        NwArtModel m = { 0 };
        nw_art_scene_textures(&s, &m);
        nw_art_set(b, NW_ART_WUMPA, nw_art_spin(&s, &m, "wumpa", 24, 8, 96), 96, 96, 24, 8);
        nw_art_set(b, NW_ART_CRASH, nw_art_special_image(&s, &m, "crash_head", false, 160), 160, 160, 1, 1);
        nw_art_set(b, NW_ART_COCO, nw_art_special_image(&s, &m, "coco_head", false, 160), 160, 160, 1, 1);
        nw_art_model_free(&m);
        nw_art_scene_close(&s);
    }
    if (nw_art_scene_open(&s, vfs, "STUFF\\PAUSE.GSC")) {
        NwArtModel m = { 0 };
        nw_art_scene_textures(&s, &m);
        for (int i = 0; i < 2 && i < m.ntexs; i++) {
            if (m.texs[i].rgba) {
                nw_art_set(b, i ? NW_ART_PANEL2 : NW_ART_PANEL, m.texs[i].rgba, m.texs[i].w, m.texs[i].h, 1, 1);
                m.texs[i].rgba = NULL;
            }
        }
        nw_art_model_free(&m);
        nw_art_scene_close(&s);
    }
    if (nw_art_scene_open(&s, vfs, "STUFF\\FONT.GSC")) {
        nw_art_font(&s, b);
        nw_art_scene_close(&s);
    }
    SDL_Log("gameart: built in %u ms (logo %s, font %s)", (unsigned)(SDL_GetTicks() - t0),
            b->rgba[NW_ART_LOGO] ? "yes" : "no", b->have_font ? "yes" : "no");
}

#ifdef NW_WITH_GAME
/* A VAG of SFX.DAT through its first END, as 48 kHz stereo floats. */
static float *nw_art_vag(NwDat *dat, const char *name, unsigned *frames)
{
    NwSfxSample s;
    if (!nw_sfx_find(dat, name, &s) || s.vag.size < NW_ADPCM_BLOCK || s.vag.rate < 4000 || s.vag.size > (4u << 20)) {
        SDL_Log("gameart: %s: %s", name, SDL_GetError());
        return NULL;
    }
    uint32_t blocks = s.vag.size / NW_ADPCM_BLOCK;
    uint8_t *adpcm = SDL_malloc(s.vag.size);
    int16_t *pcm = SDL_malloc((size_t)blocks * NW_ADPCM_SAMPLES * 2);
    float *out = NULL;
    if (adpcm && pcm && nw_sfx_read(dat, &s, 0, adpcm, blocks * NW_ADPCM_BLOCK)) {
        NwVagPass pass = nw_vag_decode(adpcm, blocks, pcm);
        uint32_t n = SDL_min(pass.samples, blocks * NW_ADPCM_SAMPLES);
        unsigned count = n ? (unsigned)((uint64_t)n * 48000 / s.vag.rate) : 0;
        out = count ? SDL_malloc((size_t)count * 2 * sizeof(float)) : NULL;
        for (unsigned i = 0; out && i < count; i++) {
            float at = (float)i * (float)s.vag.rate / 48000.0f;
            uint32_t k = (uint32_t)at;
            float f = at - (float)k;
            float a = pcm[SDL_min(k, n - 1)], c = pcm[SDL_min(k + 1, n - 1)];
            out[i * 2] = out[i * 2 + 1] = (a + (c - a) * f) / 32768.0f * 0.7f;
        }
        *frames = out ? count : 0;
    }
    SDL_free(adpcm);
    SDL_free(pcm);
    return out;
}
#endif

static void nw_art_load_sounds(const NwVfs *vfs)
{
#ifdef NW_WITH_GAME
    static const char *const names[NW_SOUND_COUNT] = { "SFX\\GENERIC\\CLET_BP.VAG", "SFX\\GENERIC\\SEL_BP.VAG",
                                                       "SFX\\GENERIC\\UDLT_BP.VAG" };
    NwDat dat;
    if (!nw_dat_open(&dat, vfs, "cdrom0:\\SFX.DAT;1")) {
        SDL_Log("gameart: no SFX.DAT: %s", SDL_GetError());
        return;
    }
    for (int i = 0; i < NW_SOUND_COUNT; i++) {
        nw_art.sounds[i] = nw_art_vag(&dat, names[i], &nw_art.sound_frames[i]);
    }
    nw_dat_close(&dat);
#else
    (void)vfs;
#endif
}

static NwRhiTexture *nw_art_upload(const uint8_t *rgba, int w, int h)
{
    const void *level = rgba;
    return nw_rhi_texture_create(NW_RHI_RGBA8, w, h, 1, 1, NW_RHI_TEX_LINEAR, &level);
}

static bool nw_art_save(const char *path, const uint8_t *rgba, int w, int h)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, (void *)rgba, w * 4);
    bool ok = s && SDL_SavePNG(s, path);
    SDL_DestroySurface(s);
    return ok;
}

bool nw_gameart_loaded(void)
{
    return nw_art.loaded;
}

const NwArtImage *nw_gameart_image(NwArtId id)
{
    return id < NW_ART_COUNT && nw_art.images[id].tex ? &nw_art.images[id] : NULL;
}

const NwArtFont *nw_gameart_font(void)
{
    return nw_art.have_font ? &nw_art.font : NULL;
}

bool nw_gameart_load(const char *root)
{
    if (nw_art.loaded) {
        return true;
    }
    NwVfs vfs;
    if (!root || !nw_extract_has_disc(root) || !nw_vfs_init(&vfs, root)) {
        return false;
    }
    NwArtBuilt b;
    nw_art_build(&vfs, &b);
    for (int i = 0; i < NW_ART_COUNT; i++) {
        if (b.rgba[i]) {
            nw_art.images[i] = b.images[i];
            int cols = SDL_max(b.images[i].cols, 1), rows = (SDL_max(b.images[i].frames, 1) + cols - 1) / cols;
            nw_art.images[i].tex = nw_art_upload(b.rgba[i], b.images[i].w * cols, b.images[i].h * rows);
        }
    }
    if (b.have_font) {
        nw_art.font = b.glyphs;
        nw_art.font.tex = nw_art_upload(b.font, b.font_w, b.font_h);
        nw_art.have_font = nw_art.font.tex != NULL;
    }
    nw_art_built_free(&b);
    nw_art_load_sounds(&vfs);
    nw_vfs_shutdown(&vfs);
    nw_art.loaded = true;
    return true;
}

void nw_gameart_free(void)
{
    nw_gameart_audio_close();
    for (int i = 0; i < NW_ART_COUNT; i++) {
        if (nw_art.images[i].tex) {
            nw_rhi_texture_destroy(nw_art.images[i].tex);
        }
    }
    if (nw_art.font.tex) {
        nw_rhi_texture_destroy(nw_art.font.tex);
    }
    for (int i = 0; i < NW_SOUND_COUNT; i++) {
        SDL_free(nw_art.sounds[i]);
    }
    memset(&nw_art, 0, sizeof(nw_art));
}

bool nw_gameart_audio_open(void)
{
    if (!nw_art.audio && nw_art.sounds[NW_SOUND_MOVE]) {
        nw_art.audio = nw_audio_open();
        if (!nw_art.audio) {
            SDL_Log("gameart: no audio device: %s", SDL_GetError());
        }
    }
    return nw_art.audio;
}

void nw_gameart_audio_close(void)
{
    if (nw_art.audio) {
        nw_audio_close();
        nw_art.audio = false;
    }
}

void nw_gameart_sound(NwArtSound s)
{
    if (nw_art.audio && s < NW_SOUND_COUNT && nw_art.sounds[s]) {
        nw_audio_play(nw_art.sounds[s], nw_art.sound_frames[s]);
    }
}

/* ---- --dump-art ----------------------------------------------------------------------------------------------- */

static void nw_art_dump_name(char *out, size_t size, const char *dir, const char *path, const char *what, int index)
{
    char flat[128];
    SDL_strlcpy(flat, path, sizeof(flat));
    for (char *c = flat; *c; c++) {
        if (*c == '\\' || *c == '/' || *c == '.') {
            *c = '_';
        }
    }
    SDL_snprintf(out, size, "%s/%s_%s%03d.png", dir, flat, what, index);
}

static void nw_art_dump_textures(const NwArtModel *m, const char *dir, const char *path, FILE *list)
{
    for (int i = 0; i < m->ntexs; i++) {
        if (!m->texs[i].rgba) {
            continue;
        }
        char out[512];
        nw_art_dump_name(out, sizeof(out), dir, path, "tex", i);
        nw_art_save(out, m->texs[i].rgba, m->texs[i].w, m->texs[i].h);
        fprintf(list, "%s tex %d %dx%d\n", path, i, m->texs[i].w, m->texs[i].h);
    }
}

int nw_gameart_dump(const char *root, const char *dir)
{
    static const char *const scenes[] = {
        "STUFF\\FONT.GSC", "STUFF\\PAUSE.GSC", "STUFF\\CRATES.GSC", "STUFF\\WUMPA.GSC", "STUFF\\REDBITS.GSC",
        "STUFF\\JEEPBITS.GSC", "LEVELS\\B\\LOGOS\\LOGOS.GSC", "LEVELS\\B\\TITLES\\TITLE.GSC",
        "LEVELS\\B\\TITLES\\TITLESPL.GSC",
    };
    static const char *const models[] = { "LEVELS\\B\\LOGOS\\TITLE.GHG", "LEVELS\\B\\LOGOS\\TTCHAR.GHG" };
    NwVfs vfs;
    if (!root || !nw_extract_has_disc(root) || !nw_vfs_init(&vfs, root)) {
        SDL_Log("dump-art: no imported disc");
        return 2;
    }
    SDL_CreateDirectory(dir);
    char listpath[512];
    SDL_snprintf(listpath, sizeof(listpath), "%s/art.txt", dir);
    FILE *list = fopen(listpath, "w");
    if (!list) {
        nw_vfs_shutdown(&vfs);
        return 1;
    }
    for (size_t f = 0; f < SDL_arraysize(scenes); f++) {
        NwArtScene s;
        if (!nw_art_scene_open(&s, &vfs, scenes[f])) {
            fprintf(list, "%s: cannot open (%s)\n", scenes[f], SDL_GetError());
            continue;
        }
        fprintf(list, "%s objects %u insts %u textures %u specials %u mtls %u\n", scenes[f], s.gsc.nobjects, s.gsc.ninsts,
                s.gsc.ntextures, s.nspecials, s.gsc.nmtls);
        NwArtModel m = { 0 };
        nw_art_scene_textures(&s, &m);
        nw_art_dump_textures(&m, dir, scenes[f], list);
        for (uint32_t i = 0; i < s.gsc.nmtls; i++) {
            NwGscMtl mt = nw_gsc_mtl(&s.gsc, i);
            fprintf(list, "  mtl %u tid %d lighting %u colour %u alpha %u diffuse %.2f %.2f %.2f a %.2f\n", i, mt.tid,
                    NW_GSC_ATTRIB_LIGHTING(mt.attrib), NW_GSC_ATTRIB_COLOUR(mt.attrib), NW_GSC_ATTRIB_ALPHA(mt.attrib),
                    mt.diffuse[0], mt.diffuse[1], mt.diffuse[2], mt.alpha);
        }
        for (uint32_t i = 0; i < s.nspecials; i++) {
            size_t before = m.ntris;
            nw_art_scene_special(&s, &m, (int)i, (int)i);
            NwArtBox b = nw_art_bounds(&m, &nw_art_front, (int)i);
            fprintf(list, "  special %u %s inst %u tris %zu box %.3f %.3f .. %.3f %.3f\n", i, s.specials[i].name,
                    s.specials[i].inst, m.ntris - before, b.x0, b.y0, b.x1, b.y1);
            if (m.ntris > before) {
                float bw = b.x1 - b.x0, bh = b.y1 - b.y0, k = 160.0f / SDL_max(SDL_max(bw, bh), 1e-6f);
                int w = SDL_max((int)(bw * k), 8), h = SDL_max((int)(bh * k), 8);
                NwArtView v = nw_art_front;
                if (f == 2 || f == 3) { /* crates and fruit: a three-quarter view */
                    v = nw_art_turn(v, 0.6f, -0.35f);
                    b = nw_art_bounds(&m, &v, (int)i);
                    w = h = 160;
                }
                uint8_t *px = nw_art_render(&m, &v, (int)i, b, w, h, 3);
                if (px) {
                    char out[512];
                    nw_art_dump_name(out, sizeof(out), dir, scenes[f], s.specials[i].name, (int)i);
                    nw_art_save(out, px, w, h);
                    SDL_free(px);
                }
            }
        }
        if (!s.nspecials && s.gsc.ninsts) {
            for (uint32_t i = 0; i < s.gsc.ninsts; i++) {
                NwGscInst inst = nw_gsc_inst(&s.gsc, i);
                nw_art_scene_object(&s, &m, (uint32_t)inst.objid, inst.mtx, 0);
            }
            NwArtBox b = nw_art_bounds(&m, &nw_art_front, -1);
            uint8_t *px = nw_art_render(&m, &nw_art_front, -1, b, 512, 512, 2);
            if (px) {
                char out[512];
                nw_art_dump_name(out, sizeof(out), dir, scenes[f], "all", 0);
                nw_art_save(out, px, 512, 512);
                SDL_free(px);
            }
        }
        nw_art_model_free(&m);
        nw_art_scene_close(&s);
    }
    for (size_t f = 0; f < SDL_arraysize(models); f++) {
        size_t size;
        uint8_t *d = nw_art_file(&vfs, models[f], &size);
        NwArtModel m = { 0 };
        char *info = SDL_malloc(65536);
        if (d && info && nw_art_ghg(d, size, &m, info, 65536)) {
            fprintf(list, "%s tris %zu\n%s", models[f], m.ntris, info);
            nw_art_dump_textures(&m, dir, models[f], list);
            for (int view = 0; view < 2; view++) {
                const NwArtView *v = view ? &nw_art_front : &nw_art_title;
                NwArtBox b = nw_art_bounds(&m, v, -1);
                float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
                int w = 768, h = SDL_clamp((int)(768 * bh / SDL_max(bw, 1e-6f)), 64, 1024);
                uint8_t *px = nw_art_render(&m, v, -1, b, w, h, 2);
                if (px) {
                    char out[512];
                    nw_art_dump_name(out, sizeof(out), dir, models[f], view ? "front" : "title", 0);
                    nw_art_save(out, px, w, h);
                    SDL_free(px);
                }
            }
        } else {
            fprintf(list, "%s: cannot read\n", models[f]);
        }
        nw_art_model_free(&m);
        SDL_free(info);
        SDL_free(d);
    }
    NwArtBuilt built;
    nw_art_build(&vfs, &built);
    for (int i = 0; i < NW_ART_COUNT; i++) {
        if (built.rgba[i]) {
            char out[512];
            int cols = SDL_max(built.images[i].cols, 1), rows = (SDL_max(built.images[i].frames, 1) + cols - 1) / cols;
            SDL_snprintf(out, sizeof(out), "%s/built_%02d.png", dir, i);
            nw_art_save(out, built.rgba[i], built.images[i].w * cols, built.images[i].h * rows);
        }
    }
    if (built.have_font) {
        char out[512];
        SDL_snprintf(out, sizeof(out), "%s/built_font.png", dir);
        nw_art_save(out, built.font, built.font_w, built.font_h);
    }
    nw_art_built_free(&built);
    fclose(list);
    nw_vfs_shutdown(&vfs);
    SDL_Log("dump-art: %s", listpath);
    return 0;
}
