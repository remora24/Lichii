#version 450
#define OCARINA_COMPARE_SCENE_DEPTH 1

layout(location = 0) in vec4 previous_clip;
layout(location = 0) out vec2 motion;

#ifdef OCARINA_COMPARE_SCENE_DEPTH
layout(binding = 3) uniform sampler2D scene_depth;
layout(push_constant) uniform DepthComparison {
    uint depth_tolerance;
    uint probe_mesh; // contract trace: counter row of this draw, or ~0 for no counting
    uint probe_pass; // contract trace: 1 = only record the nearest replay depth of each pixel
} comparison;
// Contract trace: per mesh, the replay's visible fragments by their distance from the scene depth.
layout(std430, binding = 4) buffer Probe {
    uint counts[];
} probe;
layout(binding = 5, r32ui) uniform uimage2D nearest;

void count(uint bucket) {
    atomicAdd(probe.counts[comparison.probe_mesh * 12u + bucket], 1u);
}
#endif

layout(std140, binding = 2, row_major) uniform Transforms {
    mat4 current_view_projection;
    mat4 previous_view_projection;
    vec2 extent;
    vec2 jitter;
} transforms;

void main() {
#ifdef OCARINA_COMPARE_SCENE_DEPTH
    uint actual = floatBitsToUint(gl_FragCoord.z);
    if (comparison.probe_pass == 1u) {
        imageAtomicMin(nearest, ivec2(gl_FragCoord.xy), actual);
        discard;
    }
    uint expected = floatBitsToUint(texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r);
    uint difference = max(expected, actual) - min(expected, actual);
    bool probing = comparison.probe_mesh != 0xffffffffu;
    if (probing) {
        count(0u);
        if (imageLoad(nearest, ivec2(gl_FragCoord.xy)).r == actual) {
            // The replay's front surface here: 2 = matches the scene depth, 3-6 = nearer than it
            // by up to 256, 4096, 65536 ULP or more, 7-10 = farther.
            count(1u);
            uint band = difference <= 256u ? 0u : difference <= 4096u ? 1u : difference <= 65536u ? 2u : 3u;
            count(difference <= comparison.depth_tolerance ? 2u : (actual < expected ? 3u : 7u) + band);
        }
    }
    if (difference > comparison.depth_tolerance) {
        discard;
    }
#endif
    if (previous_clip.w <= 1e-6) {
        discard;
    }
    vec2 previous_uv = previous_clip.xy / previous_clip.w * vec2(0.5, -0.5) + 0.5;
    // The draw is offset by the scene's jitter so it covers the same samples as the game's draw;
    // the motion goes from the unjittered position, as FSR expects motion without jitter.
    vec2 displacement = previous_uv * transforms.extent - (gl_FragCoord.xy - transforms.jitter);
    if (!all(lessThanEqual(abs(displacement), vec2(65504)))) {
        discard;
    }
    if (probing) {
        count(11u);
    }
    motion = displacement;
}
