#version 450

layout(location = 0) in vec3 position;
layout(location = 4) in vec4 weights;
layout(location = 5) in vec4 extra_weights;
layout(location = 6) in uvec4 joints;
layout(location = 7) in uvec4 extra_joints;
layout(location = 8) in uint pose_index;
layout(location = 0) out vec4 previous_clip;

struct ActorPose {
    mat4x3 bones[280];
    vec4 origin;
};
layout(std430, binding = 0, row_major) readonly buffer CurrentPoses {
    ActorPose poses[];
} current_poses;
layout(std430, binding = 1, row_major) readonly buffer PreviousPoses {
    ActorPose poses[];
} previous_poses;
layout(std140, binding = 2, row_major) uniform Transforms {
    mat4 current_view_projection;
    mat4 previous_view_projection;
    vec2 extent;
    vec2 jitter;
} transforms;

void blend_joint(vec4 local, uint joint, float weight, inout vec3 current, inout vec3 previous) {
    if (weight != 0 && joint < 280u) {
        current += (current_poses.poses[pose_index].bones[joint] * local) * weight;
        previous += (previous_poses.poses[pose_index].bones[joint] * local) * weight;
    }
}

void main() {
    vec4 local = vec4(position, 1);
    vec3 current = vec3(0);
    vec3 previous = vec3(0);
    for (int i = 0; i < weights.length(); ++i) {
        blend_joint(local, joints[i], weights[i], current, previous);
    }
    for (int i = 0; i < 4; ++i) {
        blend_joint(local, extra_joints[i], extra_weights[i], current, previous);
    }
    gl_Position = transforms.current_view_projection *
        vec4(current + current_poses.poses[pose_index].origin.xyz, 1);
    previous_clip = transforms.previous_view_projection *
        vec4(previous + previous_poses.poses[pose_index].origin.xyz, 1);
    // Match Zelda's [-1, 1] clip depth before Vulkan rasterization.
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
