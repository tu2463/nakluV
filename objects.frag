#version 450

layout(set=0, binding=0, std140) uniform World {
    vec3 SKY_DIRECTION;
    vec3 SKY_ENERGY; // energy supplied by sky to a surface patch with normal = SKY_DIRECTION
    vec3 SUN_DIRECTION;
    vec3 SUN_ENERGY; // energy supplied by sun to a surface patch with normal = SUN_DIRECTION
};

layout(set=2, binding=0) uniform sampler2D TEXTURE;

layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texCoord;

// Credit: adapted from https://satellitnorden.wordpress.com/2018/01/23/vulkan-adventures-cube-map-tutorial/
in vec3 textureDir; // direction vector representing a 3D texture coordinate
layout(location=3) uniform samplerCube cubeMap; // cubeMap texture sampler

vec4 textureSample = texture(cubeMapTexture, directionVector);

layout(location = 0) out vec4 outColor;

void main() {
    // with lighting:
    // Basic hemispherical lighting equation in glsl syntax, where: n is the per-pixel normal (remember to normalize after interpolation!); texCoord is the interpolated texture coordinate; *_DIRECTION are uniforms giving the light directions; *_ENERGY are uniforms giving the light energy in appropriate units; ALBEDO is the albedo texture; and outColor is the value that gets written to the framebuffer.
    vec3 n = normalize(normal);
    vec3 albedo = texture(TEXTURE, texCoord).rgb;
    // hemisphere sky + directional sun TODO: understand this
    vec3 e = SKY_ENERGY * (0.5 * dot(n, SKY_DIRECTION) + 0.5)
           + SUN_ENERGY * max(0.0, dot(n, SUN_DIRECTION));

    outColor = vec4(e * albedo, 1.0);

    FragColor = texture(cubeMap, textureDir);
}