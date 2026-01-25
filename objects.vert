#version 450

layout(set=0, binding=0, std140) uniform Camera {
    mat4 CLIP_FROM_WORLD;
};

layout(location = 0) in vec3 Position;
layout(location = 1) in vec3 Normal; // Uppercase variables for attributes (vertex shader stream inputs),
layout(location = 2) in vec2 TexCoord;

layout(location = 0) out vec3 position; // lowercase variables for varyings (vertex shader outputs / fragment shader inputs)
layout(location = 1) out vec3 normal;
layout(location = 2) out vec2 texCoord;

void main() {
    gl_Position = CLIP_FROM_WORLD * vec4(Position, 1.0);
    position = Position;
    normal = Normal;
    texCoord = TexCoord;
}