#version 450
layout(location = 0) in vec3 Position;
layout(location = 1) in vec4 Color; // Uppercase variables for attributes (vertex shader stream inputs),

layout(location = 0) out vec4 color; // lowercase variables for varyings (vertex shader outputs / fragment shader inputs)

void main() {
    gl_Position = vec4(Position, 1.0);
    color = Color;
}