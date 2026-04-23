#version 450

// Vertex shader for shadow map pass, transforms transforms the vertices to spot light space
// Credit: https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping
layout(push_constant) uniform Push {
    mat4 CLIP_FROM_LOCAL;
};

layout(location=0) in vec3 Position;

// the remaining attributes are unused. they are declared to match the PosNorTexTanVertex stride
layout(location=1) in vec3 in_normal;
layout(location=2) in vec2 in_texCoord;
layout(location=3) in vec3 in_tangent;  

void main() {
    gl_Position = CLIP_FROM_LOCAL * vec4(Position, 1.0);
}
