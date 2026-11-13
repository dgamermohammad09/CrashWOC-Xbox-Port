#!/usr/bin/env python3
"""Builds the renderer's GLSL (src/gfx/shaders/) into Direct3D 11 bytecode for src/gfx/rhi_d3d11.c.

    gen_shaders.py --glslang <glslang> --spirv-cross <spirv-cross> --fxc <fxc> --shaders <src/gfx/shaders>
                   --work <dir> --out <file.c>

Per vu program (src/gfx/vu.h's NwVuProgram, the same #defines src/gfx/vu.c gives OpenGL):
  GLSL --glslang -V -R--> SPIR-V --spirv-cross --hlsl--> HLSL --fxc--> DXBC
The vertex stage flips y and maps z from -w..w to 0..w (--flip-vert-y --fixup-clipspace), which keeps every texture
the same as under OpenGL (src/gfx/rhi.h). Direct3D has no integer-to-float vertex formats, so the vertex stage is
built twice: texcoords as float (sprites, particles) and as int16 (meshes), NW_TEX_TYPE in vu.vs.glsl. The default
uniform block of each stage becomes cbuffer b0; its members' offsets come from spirv-cross --reflect. The output C
file holds the bytecode, the uniform offsets, the vertex inputs, and d3d11_util.hlsl's blit and clear shaders.
A Vulkan backend can take the SPIR-V of the same step; a D3D12 one the same DXBC (or dxc's DXIL from the HLSL).
"""
import argparse
import json
import os
import subprocess
import sys

PROGRAMS = ['TRIC', 'TRICU', 'TRICA', 'TRICDV', 'FACEON', 'FACEY', 'SKIN', 'TRIXUV', 'SKINXUV', 'PTL', 'WATER',
            'WATERUV', 'TRIGLASS', 'SKNGLASS', 'PTL_GENERAL', 'GEM', 'SKNGEM']
GEMS = ('GEM', 'SKNGEM')
SHADER_MODEL = '5_0'
UTIL = [  # (name, entry, profile)
    ('blit_vs', 'vs_full', 'vs'),
    ('blit_point', 'ps_point', 'ps'),
    ('blit_linear', 'ps_linear', 'ps'),
    ('blit_depth', 'ps_depth', 'ps'),
    ('blit_depth_ms', 'ps_depth_ms', 'ps'),
    ('clear', 'ps_clear', 'ps'),
]


def defines(program):
    """The text src/gfx/vu.c's nw_vu_link puts after #version: keep the two the same."""
    t = ''.join('#define %s %d\n' % (n, i) for i, n in enumerate(PROGRAMS))
    t += ('#define PROGRAM %d\n#define PARTICLE (PROGRAM == PTL || PROGRAM == PTL_GENERAL)\n'
          '#define GEMS (PROGRAM == GEM || PROGRAM == SKNGEM)\n') % PROGRAMS.index(program)
    return t


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('%s failed:\n%s\n%s' % (' '.join(cmd), r.stdout[-4000:], r.stderr[-4000:]))
    return r.stdout


def build_stage(a, program, stage, body, tex_type, tag):
    base = os.path.join(a.work, '%s_%s' % (program.lower(), tag))
    src = base + ('.vert' if stage == 'vs' else '.frag')
    text = ('#version 450\n' + defines(program) + '#define NW_SPIRV 1\n#define NW_BINDING(n) layout(binding = n)\n' +
            '#define NW_TEX_TYPE %s\n' % tex_type + body)
    with open(src, 'w', newline='\n') as f:
        f.write(text)
    spv = base + '.spv'
    run([a.glslang, '-V', '-R', '--aml', '--sdub', 'NwUniforms', '0', '0', '--quiet', '-o', spv, src])
    hlsl = base + '.hlsl'
    cross = [a.spirv_cross, spv, '--hlsl', '--shader-model', SHADER_MODEL.replace('_', ''), '--output', hlsl]
    if stage == 'vs':
        cross += ['--flip-vert-y', '--fixup-clipspace']
    run(cross)
    reflect = json.loads(run([a.spirv_cross, spv, '--reflect']))
    obj = base + '.dxbc'
    run([a.fxc, '/nologo', '/T', '%s_%s' % (stage, SHADER_MODEL), '/E', 'main', '/O3', '/Qstrip_reflect',
         '/Qstrip_debug', '/Fo', obj, hlsl])
    with open(obj, 'rb') as f:
        code = f.read()
    return code, reflect


def block(reflect):
    """The default uniform block: (size, {name: (offset, array length)})."""
    for t in reflect.get('types', {}).values():
        if t.get('name') == 'NwUniforms':
            members = {}
            size = 0
            for m in t['members']:
                n = m.get('array', [1])[0]
                members[m['name']] = (m['offset'], n)
                stride = m.get('array_stride', 0)
                width = {'mat4': 64, 'mat3': 48}.get(m['type'], 16)
                size = max(size, m['offset'] + (stride * n if stride else width))
            return (size + 15) // 16 * 16, members
    return 0, {}


def inputs(reflect):
    out = []
    for i in reflect.get('inputs', []):
        t = i['type']
        kind = 1 if t.startswith('u') else 2 if t.startswith('i') else 0
        comps = int(t[-1]) if t[-1].isdigit() else 1
        out.append((i['location'], kind, comps))
    return sorted(out)


def carray(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append('    ' + ','.join('0x%02x' % b for b in data[i:i + 24]) + ',')
    return 'static const unsigned char %s[%d] = {\n%s\n};\n' % (name, len(data), '\n'.join(lines))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--glslang', required=True)
    p.add_argument('--spirv-cross', required=True)
    p.add_argument('--fxc', required=True)
    p.add_argument('--shaders', required=True)
    p.add_argument('--work', required=True)
    p.add_argument('--out', required=True)
    a = p.parse_args()
    os.makedirs(a.work, exist_ok=True)
    read = lambda n: open(os.path.join(a.shaders, n), newline='').read()
    vs_body, fs_body, gem_body = read('vu.vs.glsl'), read('vu.fs.glsl'), read('gem.fs.glsl')

    out = ['/* Generated by tools/gen_shaders.py from src/gfx/shaders/: do not edit. */',
           '#include "gfx/rhi_d3d11_shaders.h"', '']
    progs = []
    for program in PROGRAMS:
        name = program.lower()
        vs = []
        for v, tex in enumerate(('vec4', 'ivec4')):
            code, refl = build_stage(a, program, 'vs', vs_body, tex, 'vs%d' % v)
            ins = inputs(refl)
            if v == 1 and not any(l == 1 and k == 2 for l, k, _ in ins):
                vs.append(vs[0])  # no integer texcoords to take: the float build serves both
                continue
            vs.append((code, refl, ins))
        ps_code, ps_refl = build_stage(a, program, 'ps', gem_body if program in GEMS else fs_body, 'vec4', 'ps')
        sizes = [block(vs[0][1])[0], block(ps_refl)[0]]
        vmembers, pmembers = block(vs[0][1])[1], block(ps_refl)[1]
        # both VS builds share the block layout (they differ in inputs only)
        if vs[1] is not vs[0] and block(vs[1][1])[1] != vmembers:
            sys.exit('%s: the vertex builds disagree on uniforms' % program)
        for v in range(2):
            if v == 1 and vs[1] is vs[0]:
                continue
            out.append(carray('vs_%s_%d' % (name, v), vs[v][0]))
            out.append('static const NwD3dInput in_%s_%d[] = { %s };' % (
                name, v, ', '.join('{ %d, %d, %d }' % t for t in vs[v][2]) or '{ 0, 0, 0 }'))
        out.append(carray('ps_%s' % name, ps_code))
        names = sorted(set(vmembers) | set(pmembers))
        rows = []
        for n in names:
            vo, va = vmembers.get(n, (0xFFFF, 0))
            po, pa = pmembers.get(n, (0xFFFF, 0))
            rows.append('    { "%s", { %d, %d }, %d },' % (n, vo, po, max(va, pa, 1)))
        out.append('static const NwD3dUniform un_%s[] = {\n%s\n    { NULL, { 0, 0 }, 0 }\n};' % (name, '\n'.join(rows)))
        v1 = 1 if vs[1] is not vs[0] else 0
        progs.append('    { { { vs_%s_0, sizeof(vs_%s_0) }, { vs_%s_%d, sizeof(vs_%s_%d) } },\n'
                     '      { in_%s_0, in_%s_%d }, { %d, %d },\n'
                     '      { ps_%s, sizeof(ps_%s) }, { %d, %d }, un_%s, %d },' % (
                         name, name, name, v1, name, v1, name, name, v1, len(vs[0][2]), len(vs[v1][2]), name, name,
                         sizes[0], sizes[1], name, len(names)))
        print('gen_shaders: %s: vs %d/%d bytes, ps %d bytes, %d uniforms' % (
            program, len(vs[0][0]), len(vs[v1][0]), len(ps_code), len(names)))
    out.append('static const NwD3dProgram vu_programs[] = {\n%s\n};' % '\n'.join(progs))
    out.append('const NwD3dSet nw_d3d11_sets[] = { { "vu", vu_programs, %d } };' % len(PROGRAMS))
    out.append('const int nw_d3d11_nsets = 1;')

    util = os.path.join(a.shaders, 'd3d11_util.hlsl')
    entries = []
    for n, entry, prof in UTIL:
        obj = os.path.join(a.work, 'util_%s.dxbc' % n)
        run([a.fxc, '/nologo', '/T', '%s_%s' % (prof, SHADER_MODEL), '/E', entry, '/O3', '/Qstrip_reflect',
             '/Qstrip_debug', '/Fo', obj, util])
        out.append(carray('util_%s' % n, open(obj, 'rb').read()))
        entries.append('{ util_%s, sizeof(util_%s) }' % (n, n))
    out.append('const NwD3dShader nw_d3d11_util[NW_D3D_UTILS] = { %s };' % ', '.join(entries))
    tmp = a.out + '.tmp'
    with open(tmp, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    os.replace(tmp, a.out)


if __name__ == '__main__':
    main()
