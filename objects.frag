#version 450

layout(set=2,binding=0) uniform sampler2D TEXTURE;

layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texCoord;

layout(location = 0) out vec4 outColor;

void main() {
    // // without lighting:
    // outColor = vec4(fract(texCoord), 0.0, 1.0);

    // with lighting:
    vec3 n = normalize(normal);
    vec3 l = vec3(0.0, 0.0, 1.0);
    vec3 albedo = texture(TEXTURE, texCoord).rgb;
    // vec3 albedo = vec3(1.0); // all white

    //hemisphere lighting from direction l
    vec3 e = vec3(0.5 * dot(n, l) + 0.5);

    outColor = vec4(e * albedo, 1.0);
}