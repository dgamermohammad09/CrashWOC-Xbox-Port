in vec4 v_col;
in vec2 v_uv;
NW_BINDING(0) uniform usampler2D u_tex;
NW_BINDING(1) uniform usampler2D u_clut;
NW_BINDING(2) uniform sampler2D u_surf;
NW_BINDING(6) uniform sampler2D u_depth; // a depth CLUT's: the target's Z
NW_BINDING(7) uniform usampler2D u_zpal; // its entries
uniform uvec4 u_zclut; // shift, base, far, near
uniform uvec3 u_zclear; // NuLightFogClear: on, the two Z
uniform int u_zalpha; // its alpha alone over the texture's
uniform int u_zad; // the blend's Ad from it: the fog composite's vertex alpha; -1 none
NW_BINDING(8) uniform sampler2D u_dst; // the target, for the GS blend
uniform ivec4 u_gsblend; // ALPHA's A, B, C, D; -1: GL's blend
uniform ivec4 u_texmode; // 0 untextured, 1 CLUT, 2 direct colour, 3 surface, 4 depth CLUT; MMAG; CLAMP u, v
uniform ivec4 u_texlod; // MMIN, MXL, L, K (1/16)
uniform int u_filter; // NwTexFilter
uniform int u_tcc;
uniform int u_ascolour; // the blend needs As as the source colour
uniform ivec4 u_atest; // ATST (1 ALWAYS without ATE), AREF, the pass keeps the failing pixels
uniform vec4 u_vu[2]; // VU 0x14-0x15
uniform vec4 u_frag; // the pixel's GS coordinates
out vec4 o_col;
ivec2 tex_size(int lod)
{
#if PARTICLE
    return textureSize(u_tex, lod);
#else
    return u_texmode.x == 3 ? textureSize(u_surf, 0) : textureSize(u_tex, lod);
#endif
}
// % of a negative is undefined; a surface at an internal scale of 3 is no power of two.
int wrap(int p, int n)
{
#if PROGRAM == PTL
    return p & (n - 1);
#else
    return p >= 0 ? p % n : n - 1 - (-1 - p) % n;
#endif
}
vec4 texel(ivec2 p, int lod)
{
    ivec2 size = tex_size(lod);
    p = ivec2(u_texmode.z != 0 ? clamp(p.x, 0, size.x - 1) : wrap(p.x, size.x),
              u_texmode.w != 0 ? clamp(p.y, 0, size.y - 1) : wrap(p.y, size.y));
#if PARTICLE
    return vec4(texelFetch(u_tex, p, lod));
#else
    if (u_texmode.x == 3) {
        vec4 s = texelFetch(u_surf, ivec2(p.x, size.y - 1 - p.y), 0);
        return floor(vec4(s.rgb * 255.0, s.a * 128.0) + 0.5);
    }
    uvec4 c = texelFetch(u_tex, p, lod);
    return vec4(u_texmode.x == 2 ? c : texelFetch(u_clut, ivec2(int(c.r), 0), 0));
#endif
}
vec4 sample_level(vec2 uv, int lod, bool bilinear)
{
    vec2 t = uv * vec2(tex_size(lod));
    if (!bilinear) {
        return texel(ivec2(floor(t)), lod);
    }
    t -= 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    return mix(mix(texel(i, lod), texel(i + ivec2(1, 0), lod), f.x),
               mix(texel(i + ivec2(0, 1), lod), texel(i + 1, lod), f.x), f.y);
}
bool bilerp(bool gs)
{
    return u_filter == 0 || u_texmode.x >= 3 ? gs : u_filter == 2;
}
// TEX1 with LCM 0: LOD = log2(1/Q) << L + K; MMAG up to LOD 0, MMIN past it.
vec4 sample_gs(vec2 uv)
{
#if PROGRAM == PTL
    return sample_level(uv, 0, true);
#else
    float lod = log2(1.0 / gl_FragCoord.w) * float(1 << u_texlod.z) + float(u_texlod.w) / 16.0;
    if (lod <= 0.0 || u_texlod.x < 2) {
        return sample_level(uv, 0, bilerp((lod <= 0.0 ? u_texmode.y : u_texlod.x) == 1));
    }
    bool bilinear = bilerp(u_texlod.x >= 4);
    lod = min(lod, float(u_texlod.y));
    if (u_texlod.x == 3 || u_texlod.x == 5) {
        int l0 = int(lod);
        return mix(sample_level(uv, l0, bilinear), sample_level(uv, min(l0 + 1, u_texlod.y), bilinear),
                   fract(lod));
    }
    return sample_level(uv, min(int(lod + 0.5), u_texlod.y), bilinear);
#endif
}
// The GS Z under the pixel, back from the depth the GS matrix (src/gfx/backend.c) gave it.
vec4 zclut()
{
    float d = texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0).r;
    uint z = uint((1.0 - d) * 8388607.0 + 0.5);
    uint i = z < u_zclut.y ? u_zclut.z
           : z - u_zclut.y >= (256u << u_zclut.x) ? u_zclut.w : (z >> u_zclut.x) & 0xFFu;
    vec4 t = vec4(texelFetch(u_zpal, ivec2(int(i & 0xFFu), 0), 0));
    if (i == 0x100u) {
        t = vec4(0.0);
    } else if (i == 0x200u) {
        t = vec4(texelFetch(u_zpal, ivec2(int((z >> u_zclut.x) & 0xFFu), 0), 0));
    }
    if (u_zclear.x == 1u && t.a < 128.0 && z <= u_zclear.y) {
        t.a = 128.0;
    }
    if (u_zclear.x == 1u && t.a >= 128.0 && z >= u_zclear.z) {
        t.a = 0.0;
    }
    if (u_zclear.x == 2u && t.a >= 128.0 && ((u_zclear.y != 0u && z <= u_zclear.y) ||
                                           (u_zclear.z != 0u && z >= u_zclear.z))) {
        t = vec4(0.0);
    }
    return t;
}
bool atest(int a)
{
    switch (u_atest.x) {
    case 0: return false;
    case 1: return true;
    case 2: return a < u_atest.y;
    case 3: return a <= u_atest.y;
    case 4: return a == u_atest.y;
    case 5: return a >= u_atest.y;
    case 6: return a > u_atest.y;
    default: return a != u_atest.y;
    }
}
// TFX MODULATE in GS units (0x80 = 1.0): Cf = Cv * Ct >> 7, Af the same with TCC; untextured Cf = Cv.
void main()
{
    vec4 c = clamp(floor(v_col + 1.0 / 256.0), 0.0, 255.0);
// The frame-copy texcoords (water's size in VU 0x15.zw, the glass offset in VU 0x14.xy), per pixel rather than per vertex as the VU, so they hold on vertices the GPU's clipping makes.
#if PROGRAM == WATER || PROGRAM == TRIGLASS || PROGRAM == SKNGLASS
    vec2 gs = gl_FragCoord.xy * u_frag.xy + u_frag.zw;
#if PROGRAM == WATER
    vec2 st = min((gs + u_vu[1].zw * 8.0 - 32768.0) * u_vu[1].xy + v_uv, (u_vu[1].zw - 1.0) * u_vu[1].xy * 16.0);
#else
    vec2 st = min(gs * u_vu[1].xy + u_vu[0].xy + v_uv, u_vu[1].zw);
#endif
#else
    vec2 st = v_uv;
#endif
    if (u_texmode.x != 0) {
#if PARTICLE
        vec4 t = floor(sample_gs(st));
#else
        vec4 t = u_texmode.x == 4 ? zclut() : floor(sample_gs(st));
        if (u_zalpha != 0) {
            t.a = zclut().a;
        }
#endif
        c.rgb = min(floor(c.rgb * t.rgb / 128.0), 255.0);
        c.a = u_tcc != 0 ? min(floor(c.a * t.a / 128.0), 255.0) : c.a;
    }
    if (atest(int(c.a)) == (u_atest.z != 0)) {
        discard;
    }
// ((A - B) * C >> 7) + D clamped, A, B and D Cs, Cd or 0, C As, Ad or FIX (0 here); the frame keeps As.
#if !PARTICLE
    if (u_gsblend.x >= 0) {
        vec4 d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0);
        vec3 cd = floor(d.rgb * 255.0 + 0.5);
        vec3 col[3] = vec3[3](c.rgb, cd, vec3(0.0));
        float ad = u_zad >= 0 ? min(floor(float(u_zad) * zclut().a / 128.0), 255.0) : floor(d.a * 128.0 + 0.5);
        float k[3] = float[3](c.a, ad, 0.0);
        vec3 r = floor((col[u_gsblend.x] - col[u_gsblend.y]) * k[u_gsblend.z] / 128.0) + col[u_gsblend.w];
        o_col = vec4(clamp(r, 0.0, 255.0) / 255.0, c.a / 128.0);
        return;
    }
#endif
    o_col = vec4(u_ascolour != 0 ? vec3(c.a / 128.0) : c.rgb / 255.0, c.a / 128.0);
}
