#include "gfx/vu.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx/rhi.h"
#include "gfx/vif.h"

/* The GLSL, src/gfx/shaders/ embedded by CMakeLists.txt: the VU programs' vertex and fragment stages, and the gem
   programs' fragment stage. Row-major PS2 matrices are uploaded as they are: GL reads them transposed, which makes
   u_lv * v the VU's v x LV. tools/gen_shaders.py builds the same sources with the same defines for the backends that
   take bytecode. */
extern const unsigned char nw_shader_vu_vs[], nw_shader_vu_fs[], nw_shader_gem_fs[];
extern const size_t nw_shader_vu_vs_size, nw_shader_vu_fs_size, nw_shader_gem_fs_size;

/* The texture units by sampler name (gen_shaders.py binds the same). */
static const char *const nw_vu_samplers[] = { "u_tex",     "u_clut",   "u_surf",  "u_pool", "u_weights",
                                              "u_deltas", "u_depth", "u_zpal", "u_dst",  NULL };

static struct {
    NwRhiProgram *prog;
    int lv, diffuse, ambient, lightdir, lightcol, uvoff, sprite, texmode, texlod, tcc, ascolour, atest, joints, blend,
        uv, ptl, zclut, zclear, zalpha, zad, gsblend, vu, frag, wave, filter, texalpha, zero;
    int world, vp, eye, cam, tint, gem, viewport;
} nw_vu[NW_VU_PROGRAMS];
static NwTexFilter nw_vu_filter_mode;

static void nw_vu_f(NwVuProgram p, int u, int components, int count, const float *v)
{
    nw_rhi_set_uniform(nw_vu[p].prog, u, NW_RHI_FLOAT, components, count, v);
}

static void nw_vu_i(NwVuProgram p, int u, int components, int a, int b, int c, int d)
{
    const int v[4] = { a, b, c, d };
    nw_rhi_set_uniform(nw_vu[p].prog, u, NW_RHI_INT, components, 1, v);
}

static void nw_vu_u(NwVuProgram p, int u, int components, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t v[4] = { a, b, c, d };
    nw_rhi_set_uniform(nw_vu[p].prog, u, NW_RHI_UINT, components, 1, v);
}

static bool nw_vu_link(NwVuProgram program)
{
    static const char *const names[NW_VU_PROGRAMS] = { "TRIC",    "TRICU", "TRICA", "TRICDV",  "FACEON",
                                                       "FACEY",   "SKIN",  "TRIXUV", "SKINXUV", "PTL",
                                                       "WATER", "WATERUV", "TRIGLASS", "SKNGLASS", "PTL_GENERAL",
                                                       "GEM",     "SKNGEM" };
    char head[512];
    int n = 0;
    for (int i = 0; i < NW_VU_PROGRAMS; i++) {
        n += SDL_snprintf(head + n, sizeof(head) - (size_t)n, "#define %s %d\n", names[i], i);
    }
    SDL_snprintf(head + n, sizeof(head) - (size_t)n,
                  "#define PROGRAM %d\n#define PARTICLE (PROGRAM == PTL || PROGRAM == PTL_GENERAL)\n"
                  "#define GEMS (PROGRAM == GEM || PROGRAM == SKNGEM)\n", (int)program);
    bool gem = program == NW_VU_GEM || program == NW_VU_SKNGEM;
    const NwRhiProgramDesc desc = {
        .set = "vu",
        .variant = (int)program,
        .defines = head,
        .vs = (const char *)nw_shader_vu_vs,
        .vs_size = nw_shader_vu_vs_size,
        .fs = (const char *)(gem ? nw_shader_gem_fs : nw_shader_vu_fs),
        .fs_size = gem ? nw_shader_gem_fs_size : nw_shader_vu_fs_size,
        .samplers = nw_vu_samplers,
    };
    NwRhiProgram *p = nw_vu[program].prog = nw_rhi_program_create(&desc);
    if (!p) {
        return false;
    }
    nw_vu[program].lv = nw_rhi_uniform(p, "u_lv");
    nw_vu[program].diffuse = nw_rhi_uniform(p, "u_diffuse");
    nw_vu[program].ambient = nw_rhi_uniform(p, "u_ambient");
    nw_vu[program].lightdir = nw_rhi_uniform(p, "u_lightdir");
    nw_vu[program].lightcol = nw_rhi_uniform(p, "u_lightcol");
    nw_vu[program].uvoff = nw_rhi_uniform(p, "u_uvoff");
    nw_vu[program].sprite = nw_rhi_uniform(p, "u_sprite");
    nw_vu[program].texmode = nw_rhi_uniform(p, "u_texmode");
    nw_vu[program].texlod = nw_rhi_uniform(p, "u_texlod");
    nw_vu[program].tcc = nw_rhi_uniform(p, "u_tcc");
    nw_vu[program].ascolour = nw_rhi_uniform(p, "u_ascolour");
    nw_vu[program].atest = nw_rhi_uniform(p, "u_atest");
    nw_vu[program].joints = nw_rhi_uniform(p, "u_joints");
    nw_vu[program].blend = nw_rhi_uniform(p, "u_blend");
    nw_vu[program].uv = nw_rhi_uniform(p, "u_uv");
    nw_vu[program].ptl = nw_rhi_uniform(p, "u_ptl");
    nw_vu[program].zclut = nw_rhi_uniform(p, "u_zclut");
    nw_vu[program].zclear = nw_rhi_uniform(p, "u_zclear");
    nw_vu[program].zalpha = nw_rhi_uniform(p, "u_zalpha");
    nw_vu[program].zad = nw_rhi_uniform(p, "u_zad");
    nw_vu[program].gsblend = nw_rhi_uniform(p, "u_gsblend");
    nw_vu[program].vu = nw_rhi_uniform(p, "u_vu");
    nw_vu[program].frag = nw_rhi_uniform(p, "u_frag");
    nw_vu[program].wave = nw_rhi_uniform(p, "u_wave");
    nw_vu[program].filter = nw_rhi_uniform(p, "u_filter");
    nw_vu[program].texalpha = nw_rhi_uniform(p, "u_texalpha");
    nw_vu[program].zero = nw_rhi_uniform(p, "u_zero");
    nw_vu[program].world = nw_rhi_uniform(p, "u_world");
    nw_vu[program].vp = nw_rhi_uniform(p, "u_vp");
    nw_vu[program].eye = nw_rhi_uniform(p, "u_eye");
    nw_vu[program].cam = nw_rhi_uniform(p, "u_cam");
    nw_vu[program].tint = nw_rhi_uniform(p, "u_tint");
    nw_vu[program].gem = nw_rhi_uniform(p, "u_gem");
    nw_vu[program].viewport = nw_rhi_uniform(p, "u_viewport");
    return true;
}

bool nw_vu_init(void)
{
    for (int i = 0; i < NW_VU_PROGRAMS; i++) {
        if (!nw_vu_link((NwVuProgram)i)) {
            nw_vu_shutdown();
            return false;
        }
    }
    return true;
}

void nw_vu_shutdown(void)
{
    for (int i = 0; i < NW_VU_PROGRAMS; i++) {
        nw_rhi_program_destroy(nw_vu[i].prog);
        nw_vu[i].prog = NULL;
    }
}

NwVuProgram nw_vu_program(const char *package)
{
    static const struct {
        const char *name;
        NwVuProgram program;
    } map[] = {
        { "vu_load_tricu_mpg", NW_VU_TRICU },
        { "vu_load_trica_mpg", NW_VU_TRICA },
        { "vu_load_tricdv_mpg", NW_VU_TRICDV },
        { "vu_load_xformuv_mpg", NW_VU_TRIXUV },
        { "vu_load_fx2_mpg", NW_VU_TRICU }, /* its triangles at MSCAL 0x306: vertex colour, unlit */
        { "vu_load_water_mpg", NW_VU_WATER },
        { "vu_load_wateruv_mpg", NW_VU_WATERUV },
        { "vu_load_glass_mpg", NW_VU_TRIGLASS },
    };
    for (size_t i = 0; package && i < SDL_arraysize(map); i++) {
        if (strcmp(package, map[i].name) == 0) {
            return map[i].program;
        }
    }
    return NW_VU_TRIC;
}

NwVuProgram nw_vu_skin_program(const char *package)
{
    if (package && strcmp(package, "vu_load_tri_mpg") == 0) {
        return NW_VU_SKIN;
    }
    if (package && strcmp(package, "vu_load_glass_mpg") == 0) {
        return NW_VU_SKNGLASS;
    }
    return package && strcmp(package, "vu_load_xformuv_mpg") == 0 ? NW_VU_SKINXUV : NW_VU_PROGRAMS;
}

void nw_vu_skin(NwVuProgram program, uint32_t joints, uint32_t weights, uint32_t nweights)
{
    nw_vu_i(program, nw_vu[program].joints, 1, (int)joints * 4, 0, 0, 0);
    nw_vu_i(program, nw_vu[program].blend, 2, (int)weights, (int)nweights, 0, 0);
}

void nw_vu_waves(const float waves[32][4])
{
    for (int program = NW_VU_WATER; program <= NW_VU_WATERUV; program++) {
        nw_rhi_use_program(nw_vu[program].prog);
        nw_vu_f((NwVuProgram)program, nw_vu[program].wave, 4, 32, waves[0]);
    }
}

void nw_vu_ptl(NwVuProgram program, const uint32_t image[132][4])
{
    nw_rhi_set_uniform(nw_vu[program].prog, nw_vu[program].ptl, NW_RHI_UINT, 4, 132, image[0]);
}

NwVuProgram nw_vu_particle_program(const NwVuTexture *texture, const NwMtlState *state)
{
    bool bilinear = nw_vu_filter_mode == NW_FILTER_BILINEAR ||
                    (nw_vu_filter_mode == NW_FILTER_RETAIL && state->mmag == 1 && state->mmin == 1);
    return texture && texture->power_of_two && state->mmin < 2 && bilinear ? NW_VU_PTL : NW_VU_PTL_GENERAL;
}

uint32_t nw_vu_use(NwVuProgram program, const NwVuObject *o, const NwVuMaterial *m, NwMtlPass pass[2])
{
    NwRhiProgram *p = nw_vu[program].prog;
    nw_rhi_use_program(p);
    nw_rhi_set_uniform(p, nw_vu[program].lv, NW_RHI_MAT4, 4, 1, o->lv);
    nw_vu_f(program, nw_vu[program].diffuse, 4, 1, m->diffuse);
    nw_vu_f(program, nw_vu[program].ambient, 4, 1, o->ambient);
    nw_vu_f(program, nw_vu[program].lightdir, 4, 3, o->lightdir[0]);
    nw_vu_f(program, nw_vu[program].lightcol, 4, 3, o->lightcol[0]);
    nw_vu_f(program, nw_vu[program].uvoff, 4, 1, m->uvoff);
    nw_vu_f(program, nw_vu[program].sprite, 4, 1, o->sprite);
    nw_vu_f(program, nw_vu[program].uv, 4, 4, o->uv[0]);
    nw_vu_f(program, nw_vu[program].vu, 4, 2, m->vu);
    nw_vu_f(program, nw_vu[program].frag, 4, 1, o->frag);
    const NwVuZClut *z = m->zclut;
    bool zonly = z && !z->alpha && !z->ad;
    bool textured = !zonly && m->tex && m->tex->index;
    nw_vu_i(program, nw_vu[program].texmode, 4, zonly ? 4 : textured ? (m->tex->surface ? 3 : m->tex->clut ? 1 : 2) : 0,
            m->state.mmag, m->state.wms == 1, m->state.wmt == 1);
    nw_vu_i(program, nw_vu[program].texlod, 4, m->state.mmin, m->state.mxl, m->state.l, m->state.k);
    nw_vu_i(program, nw_vu[program].filter, 1, (int)nw_vu_filter_mode, 0, 0, 0);
    nw_vu_i(program, nw_vu[program].tcc, 1, (z && !z->ad) || (textured && m->tex->tcc), 0, 0, 0);
    bool particle = program == NW_VU_PTL || program == NW_VU_PTL_GENERAL;
    if (particle) {
        bool alpha = textured && m->tex->tcc;
        nw_vu_i(program, nw_vu[program].texalpha, 2, alpha ? m->tex->alpha_min : 128, alpha ? m->tex->alpha_max : 128,
                0, 0);
    }
    nw_vu_i(program, nw_vu[program].zalpha, 1, z && z->alpha, 0, 0, 0);
    nw_vu_i(program, nw_vu[program].zad, 1, z && z->ad ? z->ad - 1 : -1, 0, 0, 0);
    if (z) {
        nw_vu_u(program, nw_vu[program].zclut, 4, z->shift, z->base, z->far, z->near);
        nw_vu_u(program, nw_vu[program].zclear, 3, z->clear[0], z->clear[1], z->clear[2], 0);
        nw_rhi_bind_texture(6, z->depth);
        nw_rhi_bind_texture(7, z->clut);
    }
    NwMtlBlend b = { 0 };
    uint8_t a = m->state.alpha;
    if (m->state.abe && m->dst) {
        nw_rhi_blend(false, NW_RHI_ADD, NW_RHI_ONE, NW_RHI_ZERO, NW_RHI_ADD, NW_RHI_ONE, NW_RHI_ZERO);
        nw_vu_i(program, nw_vu[program].gsblend, 4, a & 3, (a >> 2) & 3, (a >> 4) & 3, (a >> 6) & 3);
        nw_rhi_bind_texture(8, m->dst->index);
    } else if (m->state.abe && nw_mtl_blend(m->state.alpha, &b)) {
        /* The GS writes As to the frame's alpha unblended. */
        nw_rhi_blend(true, (NwRhiBlendOp)b.op, (NwRhiBlendFactor)b.src, (NwRhiBlendFactor)b.dst, NW_RHI_ADD,
                     NW_RHI_ONE, NW_RHI_ZERO);
    } else {
        nw_rhi_blend(false, NW_RHI_ADD, NW_RHI_ONE, NW_RHI_ZERO, NW_RHI_ADD, NW_RHI_ONE, NW_RHI_ZERO);
    }
    if (!m->state.abe || !m->dst) {
        nw_vu_i(program, nw_vu[program].gsblend, 4, -1, -1, -1, -1);
    }
    nw_vu_i(program, nw_vu[program].ascolour, 1, m->state.abe && b.as_colour, 0, 0, 0);
    /* The GS keeps the larger z; the screen matrices give GL the smaller one for the nearer point. */
    static const NwRhiCompare ztst[4] = { NW_RHI_NEVER, NW_RHI_ALWAYS, NW_RHI_LEQUAL, NW_RHI_LESS };
    nw_rhi_depth(true, ztst[m->state.ztst & 3]);
    if (textured && particle && m->tex->rgba) {
        nw_rhi_bind_texture(0, m->tex->rgba);
    } else if (textured && m->tex->surface) {
        nw_rhi_bind_texture(2, m->tex->index);
    } else if (textured) {
        nw_rhi_bind_texture(1, m->tex->clut);
        nw_rhi_bind_texture(0, m->tex->index);
    }
    return nw_mtl_passes(&m->state, pass);
}

static bool nw_vu_alpha = true;

void nw_vu_gem(NwVuProgram program, const NwVuGem *g)
{
    NwRhiProgram *p = nw_vu[program].prog;
    const float viewport[4] = { (float)g->viewport[0], (float)g->viewport[1], (float)g->viewport[2],
                                (float)g->viewport[3] };
    nw_rhi_set_uniform(p, nw_vu[program].world, NW_RHI_MAT4, 4, 1, g->world);
    nw_rhi_set_uniform(p, nw_vu[program].vp, NW_RHI_MAT4, 4, 1, g->vp);
    nw_vu_f(program, nw_vu[program].eye, 3, 1, g->eye);
    nw_rhi_set_uniform(p, nw_vu[program].cam, NW_RHI_MAT3, 3, 1, g->cam);
    nw_vu_f(program, nw_vu[program].tint, 3, 1, g->tint);
    nw_vu_i(program, nw_vu[program].gem, 3, g->kind, g->role, g->panel, 0);
    nw_vu_f(program, nw_vu[program].viewport, 4, 1, viewport);
}

void nw_vu_alpha_writes(bool on)
{
    nw_vu_alpha = on;
}

void nw_vu_filter(NwTexFilter filter)
{
    nw_vu_filter_mode = filter;
}

void nw_vu_pass(NwVuProgram program, const NwVuMaterial *m, const NwMtlPass *pass)
{
    nw_vu_i(program, nw_vu[program].atest, 4, m->state.ate ? m->state.atst : 1, m->state.aref, pass->failed, 0);
    if (program == NW_VU_PTL || program == NW_VU_PTL_GENERAL) {
        NwMtlBlend blend;
        uint8_t a = m->state.alpha;
        bool keep = m->state.abe && ((a >> 4) & 3) == 0 && (a >> 6) == 1 && nw_mtl_blend(a, &blend);
        nw_vu_i(program, nw_vu[program].zero, 1, keep && !pass->z && !(pass->alpha && nw_vu_alpha), 0, 0, 0);
    }
    nw_rhi_colour_mask(pass->rgb, pass->rgb, pass->rgb, pass->alpha && nw_vu_alpha);
    nw_rhi_depth_write(pass->z);
}

/* Integer textures with their mip levels, read by texelFetch alone. */
static NwRhiTexture *nw_vu_upload(NwRhiFormat format, const NwTexLevel *levels, uint32_t n)
{
    const void *data[NW_TEX_LEVELS];
    for (uint32_t i = 0; i < n && i < NW_TEX_LEVELS; i++) {
        data[i] = levels[i].texels;
    }
    return nw_rhi_texture_create(format, (int)levels[0].width, (int)levels[0].height, (int)n, 1, 0, data);
}

/* A texture of no levels is a CLUT alone, which a draw puts over its material texture's. */
void nw_vu_texture(NwVuTexture *tex, const NwTex *t)
{
    NwRhiTexture *index = NULL, *clut = NULL;
    if (t->nlevels) {
        index = nw_vu_upload(t->ncolours ? NW_RHI_R8UI : NW_RHI_RGBA8UI, t->levels, t->nlevels);
    }
    if (t->ncolours) {
        const NwTexLevel level = { 256, 1, (uint8_t *)t->clut };
        clut = nw_vu_upload(NW_RHI_RGBA8UI, &level, 1);
    }
    *tex = (NwVuTexture){ .index = index, .clut = clut, .tcc = t->tcc };
}

void nw_vu_texture_free(NwVuTexture *tex)
{
    if (tex->rgba != tex->index) {
        nw_rhi_texture_destroy(tex->rgba);
    }
    nw_rhi_texture_destroy(tex->index);
    nw_rhi_texture_destroy(tex->clut);
    tex->index = tex->clut = tex->rgba = NULL;
}

bool nw_vu_texture_rgba(NwVuTexture *tex, const NwTex *t)
{
    if (tex->rgba || !t->nlevels) {
        return true;
    }
    NwTexLevel expanded[NW_TEX_LEVELS];
    unsigned alpha_min = 255, alpha_max = 0;
    bool ok = true;
    tex->power_of_two = true;
    for (uint32_t l = 0; l < t->nlevels; l++) {
        const NwTexLevel *level = &t->levels[l];
        size_t pixels = (size_t)level->width * level->height;
        tex->power_of_two &= !(level->width & (level->width - 1)) && !(level->height & (level->height - 1));
        uint8_t *rgba = t->ncolours ? SDL_malloc(pixels * 4) : level->texels;
        expanded[l] = (NwTexLevel){ level->width, level->height, rgba };
        if (!rgba) {
            ok = false;
            continue;
        }
        if (t->ncolours) {
            for (size_t i = 0; i < pixels; i++) {
                memcpy(rgba + i * 4, t->clut[level->texels[i]], 4);
            }
        }
        for (size_t i = 0; i < pixels; i++) {
            alpha_min = SDL_min(alpha_min, rgba[i * 4 + 3]);
            alpha_max = SDL_max(alpha_max, rgba[i * 4 + 3]);
        }
    }
    NwRhiTexture *name = tex->index;
    if (ok && t->ncolours) {
        name = nw_vu_upload(NW_RHI_RGBA8UI, expanded, t->nlevels);
        ok = name != NULL;
    }
    for (uint32_t l = 0; t->ncolours && l < t->nlevels; l++) {
        SDL_free(expanded[l].texels);
    }
    if (!ok) {
        return false;
    }
    tex->rgba = name;
    tex->alpha_min = alpha_min;
    tex->alpha_max = alpha_max;
    return true;
}

const NwRhiVertexLayout *nw_vu_layout(void)
{
    static const NwRhiVertexLayout l = {
        .stride = sizeof(NwVifVertex),
        .count = 3,
        .attribs = {
            { 0, NW_RHI_F32, 4, offsetof(NwVifVertex, pos) },
            { 1, NW_RHI_S16_FLOAT, 4, offsetof(NwVifVertex, tex) },
            { 2, NW_RHI_U8_FLOAT, 4, offsetof(NwVifVertex, col) },
        },
    };
    return &l;
}

/* The weights as FTOI15 integers, the joints and the delta range as integers. */
const NwRhiVertexLayout *nw_vu_skin_layout(void)
{
    static const NwRhiVertexLayout l = {
        .stride = sizeof(NwVifSkinVertex),
        .count = 9,
        .attribs = {
            { 0, NW_RHI_F32, 3, offsetof(NwVifSkinVertex, pos) },
            { 1, NW_RHI_F32, 3, offsetof(NwVifSkinVertex, nrm) },
            { 2, NW_RHI_F32, 2, offsetof(NwVifSkinVertex, uv) },
            { 3, NW_RHI_U16_INT, 4, offsetof(NwVifSkinVertex, weight) },
            { 5, NW_RHI_U8_INT, 4, offsetof(NwVifSkinVertex, joint) },
            { 4, NW_RHI_U16_INT, 4, offsetof(NwVifSkinVertex, weight) + 8 },
            { 6, NW_RHI_U8_INT, 4, offsetof(NwVifSkinVertex, joint) + 4 },
            { 7, NW_RHI_U8_INT, 2, offsetof(NwVifSkinVertex, nrmjoint) },
            { 8, NW_RHI_U32_INT, 1, offsetof(NwVifSkinVertex, delta) },
        },
    };
    return &l;
}

const NwRhiVertexLayout *nw_vu_ptl_layout(void)
{
    static const NwRhiVertexLayout l = {
        .stride = 8 * sizeof(float),
        .count = 2,
        .instanced = true,
        .attribs = {
            { 0, NW_RHI_F32, 4, 0 },
            { 1, NW_RHI_F32, 4, 4 * sizeof(float) },
        },
    };
    return &l;
}

const NwRhiVertexLayout *nw_vu_sprite_layout(void)
{
    static const NwRhiVertexLayout l = {
        .stride = sizeof(NwVuSpriteVertex),
        .count = 3,
        .attribs = {
            { 0, NW_RHI_F32, 4, offsetof(NwVuSpriteVertex, pos) },
            { 1, NW_RHI_F32, 4, offsetof(NwVuSpriteVertex, tex) },
            { 2, NW_RHI_U8_FLOAT, 4, offsetof(NwVuSpriteVertex, col) },
        },
    };
    return &l;
}
