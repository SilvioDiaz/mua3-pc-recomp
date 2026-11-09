#version 450
// Stands in for MUA3's depth-prepass culling geometry shader (runtime/vk_exec_draw.c gs_list_all). The game's shader is
// a passthrough one that tests each triangle (outside the view, back-facing, too small) and appends the survivors'
// three vertex indices to a per-draw index list, whose indexed indirect record (count first) the G-buffer pass draws.
// Replayed, it lists only about 5% of the triangles and mixes a few up, so the G-buffer came out in shards. This one
// lists every triangle: the G-buffer tests depth for equality against this prepass, so the extra triangles change no
// pixel, they only cost GPU time.
//
// Bindings follow the game shader's translation (the .res.json text in vk_exec_draw.c): binding 1 the draw's indirect
// record, binding 2 its index list, both NVN storage buffers addressed from the geometry stage's c0 table (0x410,
// 0x420) and bound at the address rounded down to 16 bytes; binding 3 c0 itself, whose table gives the bytes to skip.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

out gl_PerVertex {
    vec4 gl_Position;   // only the position: no clip distances, as the game's vertex shaders write none
};

layout(location = 0) in vec4 attr0[];   // the vertex shader's output 0: x holds gl_VertexIndex as float bits
layout(location = 0) out vec4 out0;     // the prepass fragment shader reads it

layout(set = 0, binding = 1, std430) buffer Record { uint w[]; } record;
layout(set = 0, binding = 2, std430) buffer Indices { uint w[]; } indices;
layout(set = 0, binding = 3, std140) uniform C0 { uvec4 v[128]; } c0;

void main() {
    uint record_at = (c0.v[0x410 / 16].x & 15u) >> 2;    // words between the bound address and the record
    uint indices_at = (c0.v[0x420 / 16].x & 15u) >> 2;   // and the draw's index list
    uint at = indices_at + atomicAdd(record.w[record_at], 3u);
    for (int k = 0; k < 3; k++) indices.w[at + uint(k)] = floatBitsToUint(attr0[k].x);
    for (int k = 0; k < 3; k++) {
        gl_Position = gl_in[k].gl_Position;
        out0 = attr0[k];
        EmitVertex();
    }
    EndPrimitive();
}
