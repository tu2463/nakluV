#version 450

layout(set=0, binding=0, std140) uniform World {
    vec3 SKY_DIRECTION; // unused after A2-env
    vec3 SKY_ENERGY; // energy supplied by sky to a surface patch with normal = SKY_DIRECTION; unused after A2-env
    vec3 SUN_DIRECTION;
    vec3 SUN_ENERGY; // energy supplied by sun to a surface patch with normal = SUN_DIRECTION
    vec3 EYE;
};

layout(set=2, binding=0) uniform sampler2D TEXTURE;
layout(set=3, binding=0) uniform samplerCube cubeMap; // cubeMap texture sampler; binding=0 was specified at VkDescriptorSetLayoutBinding

layout(push_constant) uniform Push {
    uint material_type;
};

layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texCoord;

layout(location = 0) out vec4 outColor;

void main() {
    // with lighting:
    // Basic hemispherical lighting equation in glsl syntax, where: n is the per-pixel normal (remember to normalize after interpolation!); texCoord is the interpolated texture coordinate; *_DIRECTION are uniforms giving the light directions; *_ENERGY are uniforms giving the light energy in appropriate units; ALBEDO is the albedo texture; and outColor is the value that gets written to the framebuffer.
    vec3 n = normalize(normal);
    vec3 albedo = texture(TEXTURE, texCoord).rgb;

    // Credit: learned from https://learnopengl.com/Advanced-OpenGL/Cubemaps, https://registry.khronos.org/OpenGL-Refpages/gl4/html/texture.xhtml
    
    vec3 mat_light;
    if (material_type == 0 || material_type == 1) { // pbr, lambertian
        mat_light = SKY_ENERGY * (0.5 * dot(n, SKY_DIRECTION) + 0.5); // hemisphere sky
    } else if (material_type == 2) { // mirror
        vec3 I = normalize(position - EYE);
        vec3 R = reflect(I, normalize(n));
        mat_light = texture(cubeMap, R).rgb;
    } else if (material_type == 3) {// environmental
        mat_light = texture(cubeMap, n).rgb; // sample the cubemap in the direction of normal; texture() returns vec4 (RGBA), but alpha is not needed for calculating energy, so use .rgb to extract the vec3 (RGB)
    }
    
    vec3 e = mat_light + SUN_ENERGY * max(0.0, dot(n, SUN_DIRECTION));

    outColor = vec4(e * albedo, 1.0);
}