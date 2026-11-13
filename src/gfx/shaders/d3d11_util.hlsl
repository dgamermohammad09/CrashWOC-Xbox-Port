// Direct3D 11 helpers of src/gfx/rhi_d3d11.c: what glBlitFramebuffer and a masked or scissored glClear do, as draws
// of one triangle over the viewport (the destination rectangle). Positions are render-target pixels, rows from the
// top of the texture's memory, which src/gfx/rhi.h keeps the same as GL's rows from row 0; only the window is
// flipped (flip = its height).

cbuffer Blit : register(b0)
{
    float4 map;    // xy: the source rectangle's corner, zw: the destination's (GL rows)
    float4 scale;  // xy: source pixels per destination pixel, z: flip (0 none), w: the source's mip level
    float4 colour; // the clear's
    float4 depth;  // x: the clear's
};

Texture2D<float4> src_colour : register(t0);
Texture2D<float> src_depth : register(t1);
Texture2DMS<float> src_depth_ms : register(t2);
SamplerState linear_clamp : register(s0);

float4 vs_full(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// The source position a destination pixel centre maps to, as glBlitFramebuffer maps them.
float2 src_pos(float4 p)
{
    float y = scale.z > 0.0 ? scale.z - p.y : p.y;
    return map.xy + (float2(p.x, y) - map.zw) * scale.xy;
}

float4 ps_point(float4 p : SV_Position) : SV_Target
{
    return src_colour.Load(int3(floor(src_pos(p)), (int)scale.w));
}

float4 ps_linear(float4 p : SV_Position) : SV_Target
{
    uint w, h, n;
    src_colour.GetDimensions((uint)scale.w, w, h, n);
    return src_colour.SampleLevel(linear_clamp, src_pos(p) / float2(w, h), scale.w);
}

float ps_depth(float4 p : SV_Position) : SV_Depth
{
    return src_depth.Load(int3(floor(src_pos(p)), 0));
}

// An MSAA resolve of depth takes sample 0, as GL's nearest blit of a multisampled depth buffer.
float ps_depth_ms(float4 p : SV_Position) : SV_Depth
{
    return src_depth_ms.Load(int2(floor(src_pos(p))), 0);
}

void ps_clear(float4 p : SV_Position, out float4 c : SV_Target, out float d : SV_Depth)
{
    c = colour;
    d = depth.x;
}
