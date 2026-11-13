#if PROGRAM == SKIN || PROGRAM == SKINXUV || PROGRAM == SKNGLASS || PROGRAM == SKNGEM
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec4 a_weight0; // FTOI15
layout(location = 4) in uvec4 a_weight1;
layout(location = 5) in uvec4 a_joint0;
layout(location = 6) in uvec4 a_joint1;
layout(location = 7) in uvec2 a_nrmjoint; // the normal's joint, the delta count
layout(location = 8) in uint a_delta; // the first delta
NW_BINDING(3) uniform samplerBuffer u_pool; // the packet's palettes, a matrix as its four rows
NW_BINDING(4) uniform samplerBuffer u_weights; // the packet's blend-shape weights
NW_BINDING(5) uniform samplerBuffer u_deltas; // the geom's deltas: x, y, z, shape
uniform int u_joints; // the draw's first row
uniform ivec2 u_blend; // the draw's first weight and their count
#else
layout(location = 0) in vec4 a_pos; // VF06: x, y, z, the normal's x
#ifdef NW_SPIRV
// Direct3D has no integer-to-float vertex formats: the texcoords (int16 in a mesh, float in a sprite) and the colour
// (uint8) arrive as the buffer holds them and convert here, as GL's attribute fetch does (tools/gen_shaders.py).
layout(location = 1) in NW_TEX_TYPE a_tex_raw;
layout(location = 2) in uvec4 a_col_raw;
#define a_tex vec4(a_tex_raw)
#define a_col vec4(a_col_raw)
#else
layout(location = 1) in vec4 a_tex; // VF08 before ITOF12: u, v, the normal's y, z
layout(location = 2) in vec4 a_col; // VF31 before ITOF0
#endif
#endif
uniform mat4 u_lv;
uniform vec4 u_diffuse;
uniform vec4 u_ambient;
uniform vec4 u_lightdir[3];
uniform vec4 u_lightcol[3];
uniform vec4 u_uvoff; // VU 0x14 u, v; the STROW row u, v (x4096) the texcoords add under STMOD 1
uniform vec4 u_sprite;
uniform vec4 u_uv[4]; // the xuv programs' VF24-27
uniform vec4 u_vu[2]; // VU 0x14-0x15
#if PARTICLE
uniform uvec4 u_ptl[132]; // VU 0x3C-0xBF
uniform ivec4 u_atest;
uniform ivec2 u_texalpha; // conservative GS alpha range of the particle texture
uniform int u_zero; // this pass leaves all writable buffers unchanged when As is zero
#elif PROGRAM == WATER || PROGRAM == WATERUV
uniform vec4 u_wave[32]; // VU 0x1B2-0x1D1
#endif
out vec4 v_col;
out vec2 v_uv;
#if GEMS
uniform mat4 u_world;
out vec3 v_wpos;
out vec3 v_wnrm;
out vec3 v_lnrm;
#endif
#if PROGRAM == SKIN || PROGRAM == SKINXUV || PROGRAM == SKNGLASS || PROGRAM == SKNGEM
vec3 row(uint joint, int r)
{
    return texelFetch(u_pool, u_joints + int(joint) * 4 + r).xyz;
}
#endif
void main()
{
// ptl.vsm: entry k = rate * t (t = time - birth) gives the corner offsets in GS units before the divide, lerped to the next but the colour not; dead before birth or past entry 62.9.
#if PARTICLE
    const int strip[6] = int[6](0, 1, 2, 1, 2, 3);
    int corner = strip[gl_VertexID];
    vec4 head = uintBitsToFloat(u_ptl[0]);
    float t = head.w - a_pos.w;
    float k = a_tex.w * t;
    if (t < 0.0 || k > 62.9) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    int i = int(k) & 63;
    float f = k - float(int(k));
    vec4 v01 = mix(uintBitsToFloat(u_ptl[4 + 2 * i]), uintBitsToFloat(u_ptl[4 + 2 * min(i + 1, 63)]), f);
    vec2 v2 = mix(uintBitsToFloat(u_ptl[5 + 2 * i].xy), uintBitsToFloat(u_ptl[5 + 2 * min(i + 1, 63)].xy), f);
    vec2 offset = (corner & 1) != 0 ? v2 : v01.xy;
    if (corner >= 2) {
        offset += v01.zw;
    }
    vec3 p = a_pos.xyz + a_tex.xyz * t + vec3(0.0, head.y * t * t, 0.0);
    gl_Position = u_lv * vec4(p, 1.0);
    gl_Position.xy += offset * u_sprite.xy;
    const ivec2 uv[4] = ivec2[4](ivec2(1, 0), ivec2(2, 0), ivec2(1, 2), ivec2(2, 2));
    uvec4 corners = u_ptl[uv[corner].x];
    uvec2 st = uv[corner].y == 0 ? corners.xy : corners.zw;
    v_uv = vec2(st & 0x3FFFu) * u_sprite.zw;
    v_col = vec4(u_ptl[4 + 2 * i] & 0xFFu);
// A fading particle often cannot pass AREF at all. Reject its primitive before rasterizing that pass, rather than sampling the texture over a large near-camera quad just to discard every fragment.
    int lo = min(int(v_col.a) * u_texalpha.x / 128, 255);
    int hi = min(int(v_col.a) * u_texalpha.y / 128, 255);
    bool anypass, allpass;
    switch (u_atest.x) {
    case 0: anypass = false; allpass = false; break;
    case 1: anypass = true; allpass = true; break;
    case 2: anypass = lo < u_atest.y; allpass = hi < u_atest.y; break;
    case 3: anypass = lo <= u_atest.y; allpass = hi <= u_atest.y; break;
    case 4: anypass = lo <= u_atest.y && hi >= u_atest.y; allpass = lo == u_atest.y && hi == lo; break;
    case 5: anypass = hi >= u_atest.y; allpass = lo >= u_atest.y; break;
    case 6: anypass = hi > u_atest.y; allpass = lo > u_atest.y; break;
    default: anypass = lo != u_atest.y || hi != u_atest.y; allpass = hi < u_atest.y || lo > u_atest.y; break;
    }
    if ((u_atest.z != 0 ? allpass : !anypass) || (u_zero != 0 && hi == 0))
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
// faceon adds its corners in GS units before the divide; facey spans its quad along VF15 and the object's y.
#elif PROGRAM == FACEON || PROGRAM == FACEY
    vec2 corner = 1.0 - 2.0 * a_tex.xy;
#if PROGRAM == FACEON
    gl_Position = u_lv * vec4(a_pos.xyz, 1.0);
    gl_Position.xy += corner * a_tex.zw * u_sprite.xy;
#else
    vec3 p = a_pos.xyz - corner.x * a_tex.z * u_sprite.xyz + vec3(0.0, corner.y * a_tex.w, 0.0);
    gl_Position = u_lv * vec4(p, 1.0);
#endif
    v_uv = a_tex.xy;
    v_col = a_col;
#elif PROGRAM == SKIN || PROGRAM == SKINXUV || PROGRAM == SKNGLASS || PROGRAM == SKNGEM
// skinmod.vsm, then skin.vsm: the weights are not normalised, and the normal turns under the heaviest joint alone. skin lights the material diffuse without vertex colour; skinxuv and sknglass work as trixuv and triglass.
    vec3 q = a_pos;
    for (uint k = 0u; k < a_nrmjoint.y && u_blend.y > 0; k++) {
        vec4 d = texelFetch(u_deltas, int(a_delta + k));
        if (int(d.w) < u_blend.y) {
            q += texelFetch(u_weights, u_blend.x + int(d.w)).x * d.xyz;
        }
    }
    vec3 p = vec3(0.0);
    for (int k = 0; k < 8; k++) {
        uint j = k < 4 ? a_joint0[k] : a_joint1[k - 4];
        float w = float(k < 4 ? a_weight0[k] : a_weight1[k - 4]) / 32768.0;
        p += w * (q.x * row(j, 0) + q.y * row(j, 1) + q.z * row(j, 2) + row(j, 3));
    }
    gl_Position = u_lv * vec4(p, 1.0);
    vec3 n = a_nrm.x * row(a_nrmjoint.x, 0) + a_nrm.y * row(a_nrmjoint.x, 1) + a_nrm.z * row(a_nrmjoint.x, 2);
#if PROGRAM == SKNGEM
    v_wpos = (u_world * vec4(p, 1.0)).xyz;
    v_wnrm = mat3(u_world) * n;
    v_lnrm = a_nrm;
    v_col = u_diffuse;
    v_uv = a_uv;
#elif PROGRAM == SKIN
    vec3 nl = max(vec3(dot(u_lightdir[0].xyz, n), dot(u_lightdir[1].xyz, n), dot(u_lightdir[2].xyz, n)), 0.0);
    vec4 lit = u_lightcol[0] * nl.x + u_lightcol[1] * nl.y + u_lightcol[2] * nl.z;
    v_col = vec4(min(u_diffuse.rgb * (u_ambient.rgb + lit.rgb), 255.0), u_diffuse.a);
    v_uv = a_uv + u_uvoff.xy;
#else
    v_col = u_diffuse;
    v_uv = vec2(dot(u_uv[0].xyz, n), dot(u_uv[1].xyz, n)) + u_uv[3].xy;
#if PROGRAM == SKNGLASS
    v_uv *= u_vu[0].z;
#endif
#endif
// water.vsm, wateruv.vsm: the normal's y picks the wave; water takes the material colour and the frame copy (in the fragment program), wateruv its vertex colour and texcoords.
#elif PROGRAM == WATER || PROGRAM == WATERUV
    vec4 wave = u_wave[int(a_tex.z) & 31];
    gl_Position = u_lv * vec4(a_pos.x, a_pos.y + wave.w, a_pos.z, 1.0);
    v_uv = wave.xy * u_vu[0].z;
#if PROGRAM == WATER
    v_col = u_diffuse;
#else
    v_uv += (a_tex.xy + u_uvoff.zw) / 4096.0;
    v_col = a_col;
#endif
#elif PROGRAM == GEM
    gl_Position = u_lv * vec4(a_pos.xyz, 1.0);
    vec3 n = vec3(a_pos.w, a_tex.zw / 4096.0);
    v_wpos = (u_world * vec4(a_pos.xyz, 1.0)).xyz;
    v_wnrm = mat3(u_world) * n;
    v_lnrm = n;
    v_col = a_col;
    v_uv = (a_tex.xy + u_uvoff.zw) / 4096.0;
#elif PROGRAM == TRIXUV || PROGRAM == TRIGLASS
// trixuv.vsm: the material colour unlit, the texcoords the normal in the UV matrix (specular, environment). triglass.vsm: those times VU 0x14.z, to which the fragment program adds the frame copy at the vertex's pixel.
    gl_Position = u_lv * vec4(a_pos.xyz, 1.0);
    vec3 n = vec3(a_pos.w, a_tex.zw / 4096.0);
    v_col = u_diffuse;
    v_uv = vec2(dot(u_uv[0].xyz, n), dot(u_uv[1].xyz, n)) + u_uv[3].xy;
#if PROGRAM == TRIGLASS
    v_uv *= u_vu[0].z;
#endif
#else
    gl_Position = u_lv * vec4(a_pos.xyz, 1.0);
    vec3 n = vec3(a_pos.w, a_tex.zw / 4096.0);
    vec3 nl = max(vec3(dot(u_lightdir[0].xyz, n), dot(u_lightdir[1].xyz, n), dot(u_lightdir[2].xyz, n)), 0.0);
    vec4 lit = u_lightcol[0] * nl.x + u_lightcol[1] * nl.y + u_lightcol[2] * nl.z;
    v_uv = (a_tex.xy + u_uvoff.zw) / 4096.0;
#if PROGRAM == TRIC // tri, tric: vertex colour plus the lit material diffuse
    v_col = vec4(min(a_col.rgb + u_diffuse.rgb * (u_ambient.rgb + lit.rgb), 255.0), a_col.a);
#elif PROGRAM == TRICU // tricu: unlit
    v_col = a_col;
#elif PROGRAM == TRICA // trica: the lights' w scale the alpha
    v_col = vec4(a_col.rgb, a_col.a * min(lit.a, 1.0));
    v_uv += u_uvoff.xy;
#else // tricdv: the light modulates the vertex colour
    v_col = vec4(min((u_ambient.rgb + lit.rgb) * a_col.rgb, 255.0), a_col.a);
    v_uv += u_uvoff.xy;
#endif
#endif
}
