#version 450

struct Transform {
    mat4 CLIP_FROM_LOCAL; // from object's local space to clip space
    mat4 WORLD_FROM_LOCAL; // from local positions to world space
    mat4 WORLD_FROM_LOCAL_NORMAL; // normals
};

layout(set=1, binding=0, std140) readonly buffer Transforms {
    Transform TRANSFORMS[];
};

layout(location = 0) in vec3 Position;
layout(location = 1) in vec3 Normal; // Uppercase variables for attributes (vertex shader stream inputs),
layout(location = 2) in vec2 TexCoord;

layout(location = 0) out vec3 position; // lowercase variables for varyings (vertex shader outputs / fragment shader inputs)
layout(location = 1) out vec3 normal;
layout(location = 2) out vec2 texCoord;

void main() {
    gl_Position = TRANSFORMS[gl_InstanceIndex].CLIP_FROM_LOCAL * vec4(Position, 1.0);
    position = mat4x3(TRANSFORMS[gl_InstanceIndex].WORLD_FROM_LOCAL) * vec4(Position, 1.0);
    normal = mat3(TRANSFORMS[gl_InstanceIndex].WORLD_FROM_LOCAL_NORMAL) * Normal;
    texCoord = TexCoord;
}