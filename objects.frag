#version 450

layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texCoord;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fract(texCoord), 0.0, 1.0);
}