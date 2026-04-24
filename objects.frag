#version 450

layout(set=0, binding=0, std140) uniform World {
    vec3 SKY_DIRECTION; // unused after A2-env
    vec3 SKY_ENERGY; // energy supplied by sky to a surface patch with normal = SKY_DIRECTION; unused after A2-env
    vec3 SUN_DIRECTION;
    vec3 SUN_ENERGY; // energy supplied by sun to a surface patch with normal = SUN_DIRECTION
    vec3 EYE;
};

struct LightData {
    vec3 position; int type;
    vec3 tint; float fov;
    vec3 direction; float blend;
    float angle; float strength; float radius; float power;
    float limit; float _pad0; float _pad1; float _pad2;
    mat4 CLIP_FROM_WORLD;
    int shadow_i; float shadow_map_size; float _pad3; float _pad4; // shadow_i=-1 means no shadow
};

layout(set=1, binding=1, std140) readonly buffer Lights {
    LightData LIGHTS[];
};

layout(set=1, binding=2) uniform sampler2DArray shadowMaps; // A3-shadows
layout(set=2, binding=0) uniform sampler2D TEXTURE;
layout(set=3, binding=0) uniform samplerCube cubeMap; // radiance cubemap (mirror / environment materials); binding=0 was specified at VkDescriptorSetLayoutBinding
layout(set=4, binding=0) uniform samplerCube lambertianCubeMap; // prefiltered irradiance cubemap (X.lambertian.png, lambertian material)
layout(set=5, binding=0) uniform sampler2D normalMap; // A2-normal
layout(set=6, binding=0) uniform samplerCube ggxPrefilteredEnvMap; // A2-pbr, GGX prefiltered mip map (X.ggx.N.png). Credit: Completing the IBL reflectance from https://learnopengl.com/PBR/IBL/Specular-IBL
layout(set=7, binding=0) uniform sampler2D brdfLUT; // BRDF lookup table precomputed by brdf.comp

layout(push_constant) uniform Push {
    uint material_type;
    int exposure; // multiply radiance by 2^exposure
    uint tone_map;
    float roughness; // A2-pbr, [0, 1]
    float metalness; // [0, 1]
    int lights_count; // A3-materials, count of direct lights (sun, sphere, spot)
};

layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texCoord;
layout(location=3) in vec3 tangent;
layout(location=4) in vec3 bitangent;

layout(location=0) out vec4 outColor;

const float PI = 3.14159265359;

// Credit: 
// https://bruop.github.io/tonemapping/
// https://www.shadertoy.com/view/WdjSW3
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/

float tonemap_aces(float x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
}

// Credit: https://learnopengl.com/PBR/IBL/Specular-IBL
// Fresnel-Schlick with roughness: at grazing angles, rougher surfaces reflect less sharply
// Fresnel描述的是：光从一种介质射到另一种介质边界时，有多少比例被反射、多少比例折射进去。
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    // 真实 Fresnel 方程很复杂，实时渲染用Fresnel-Schlick近似: F = F0 + (1 - F0) * (1 - cosθ)^5
    // F0：正视角（θ=0）时的反射率，材质固有属性（非金属（塑料、皮肤）≈ 0.04（约 4% 反射）；金属（铁、金）= albedo 颜色本身（60~90%））  
    // cosTheta: θ=0（正视）→ F = F0（最小反射）(光垂直打到表面，大部分光折射进入材质内部，只有一小部分被反射回来e.g.正对着玻璃窗看，能清楚看到窗外，玻璃几乎透明）
    //           θ=90°（掠射）→ F = 1（全反射）（光几乎平行于表面掠过，几乎没有光能折射进去，几乎全部被反射回来，反射率 → 1（100%））
    // (1 - cosθ)^5：控制从 F0 到 1 的过渡曲线
    // the max() prevents F from going below F0
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Credit: Direct lighting from https://learnopengl.com/PBR/Lighting
float DistributionGGX(float NdotH, float Roughness) {
    float a  = Roughness * Roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / denom;
}

// Credit: https://learnopengl.com/PBR/IBL/Specular-IBL, https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
float GeometrySchlickGGX(float NdotV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}  

struct LightParams {
    vec3 L; // unit direction from surface toward light
    float sinThetaBy2;
    float irradiance;
};

// Computes L, sinThetaBy2, and irradiance for a light at the current frag pos
LightParams get_light_params(LightData light) {
    LightParams light_params;
    if (light.type == 0) { // sun
        light_params.sinThetaBy2 = sin(light.angle / 2.0);
        light_params.L = normalize(-light.direction);
        light_params.irradiance = light.strength;
    } else { // sphere(1) or spot(2)
        vec3 L_vec = light.position - position;
        float d = length(L_vec);
        light_params.L = normalize(L_vec);
        light_params.sinThetaBy2 = light.radius / max(d, light.radius);
        light_params.irradiance = light.power / (4.0 * PI * max(d, light.radius) * max(d, light.radius));

        // Falloff. Credit: Lighting Model (eq9) from https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
        if (light.limit > 0.0) {
            light_params.irradiance *= max(0.0, 1.0 - pow(d / light.limit, 4.0));
        }

        if (light.type == 2) { // spot: sphere light with additional attenuation for cone
            /* Credit: S72 spec
            A point within $(fov * (1-blend))/2$ radians of a *spot*'s $-z$ axis is fully illuminated by that *spot* (i.e., is illuminated as if by a sphere light with the same parameters).
            A point outside $fov/2$ radians of a *spot*'s $-z$ axis is not illuminated by that *spot*.
            A point between $(fov * (1-blend))/2$ and $fov/2$ radians of a *spot*'s $-z$ axis is lit with a linear (w.r.t. angle) blend between full and no illumination.
            */
            float cosTheta = dot(-light_params.L, normalize(light.direction));
            float angle = acos(cosTheta);
            float inner_half = light.fov * (1.0 - light.blend) / 2.0;
            float outer_half = light.fov / 2.0;
            float blend_t = clamp((angle - inner_half) / (outer_half - inner_half), 0.0, 1.0); // angle=inner_half => 0; angle=outer_half => 1
            light_params.irradiance *= 1.0 - blend_t; // angle=inner_half => 1; angle=outer_half => 0
        }
    }
    return light_params;
}

float get_diffuse_irradiance(float NdotL, float sinThetaBy2, float irradiance) {
    if (NdotL >= sinThetaBy2) { // above horizon
        return NdotL * irradiance;
    } else if (NdotL <= -sinThetaBy2) { // below horizon
        return 0.0;
    } else { // crossing horizon: cubic smoothstep
        float t = (NdotL + sinThetaBy2) / (2.0 * sinThetaBy2);
        float t_smooth = t * t * (3.0 - 2.0 * t);
        return t_smooth * sinThetaBy2 * irradiance;
    }
}

// Returns irradiance from one direct light.
vec3 direct_lambertian(LightData light, vec3 n, vec3 albedo) { // Credit: Lights lecture slide
    LightParams light_params = get_light_params(light);
    float NdotL = dot(n, light_params.L); // signed: used to detect which side of the surface we're on
    vec3 e_diffuse = get_diffuse_irradiance(NdotL, light_params.sinThetaBy2, light_params.irradiance) * (albedo / PI);
    return light.tint * e_diffuse;
}

// Returns radiance contribution (diffuse + specular) from one direct light for PBR material.
vec3 direct_pbr(LightData light, vec3 n, vec3 albedo, vec3 F0) {
    LightParams light_params = get_light_params(light);
    vec3 L = light_params.L;
    float sinThetaBy2 = light_params.sinThetaBy2;
    float irradiance = light_params.irradiance;

    // -- diffuse: same as direct_lambertian
    float NdotL_original = dot(n, L);
    float diffuse_irr = get_diffuse_irradiance(NdotL_original, sinThetaBy2, irradiance);

    // -- specular: Representative point method from https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
    vec3 V = normalize(EYE - position); // view direction toward camera

    // find the point with the smallest angle to the reflection ray 
    // by finding the point with the smallest distance to the ray.

    // sun: Find the direction within the sun disc closest to the reflection ray.
    vec3 r = reflect(-V, n); // reflection direction
    vec3 centerToRay = dot(L, r) * r - L;
    float sourceRadius = sinThetaBy2; // Sun is treated as a sphere with radius = sin(theta/2).
    vec3 closestPoint = L + centerToRay * clamp(sourceRadius / length(centerToRay), 0.0, 1.0);
    vec3 L_modified = normalize(closestPoint); // modified light direction

    // GGX BRDF inputs
    float NdotL = max(dot(n, L_modified), 0.0);
    float NdotV = max(dot(n, V),       0.0);
    vec3  H = normalize(V + L_modified);
    float NdotH = max(dot(n, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // eq10 from from https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf. //??
    float roughness_clamped = max(roughness, 0.1); 
    float alpha = roughness_clamped * roughness_clamped; // enforces alpha >= 0.01 to prevent division by 0 in GeometrySchlickGGX
    float distance = 1; // 1 for sun
    float alpha_prime = clamp(alpha + sourceRadius / (2.0 * distance), 0.0, 1.0);
    float normalization = (alpha / alpha_prime) * (alpha / alpha_prime); // energy normalization (Karis Eq.14)

    float D = DistributionGGX(NdotH, alpha_prime);
    float G = GeometrySmith(n, V, L_modified, roughness_clamped);
    vec3 F = FresnelSchlickRoughness(VdotH, F0, roughness);

    vec3 specular_brdf = (D * G * F) / max(4.0 * NdotV * NdotL, 0.0001); // Cook-Torrance, eq2 from from https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
    vec3 e_specular    = specular_brdf * normalization * NdotL * irradiance;

    vec3 kS = F; // kS描述在给定视角下有多少光被镜面反射
    vec3 kD = 1.0 - kS; // 剩余给漫反射
    kD *= 1.0 - metalness; // Metalness 对 kD 的修正：金属没有漫反射（diffuse)，导体会立刻吸收折射进去的光，不会从内部散射出来。所以metalness = 0 (non-metal) => kD stays the same; =1 (metal) => only specular
    vec3 e_diffuse  = kD * (albedo / PI) * diffuse_irr;
    return light.tint * (e_diffuse + e_specular);
}

// A3-shadows: Returns shadow factor [0=fully shadowed, 1=fully lit] using 9-sample PCF for filtering.
// Manual depth comparison because compareEnable=VK_FALSE for macOS compatibility.
float shadow_pcf(int shadow_i, mat4 clip_from_world, float map_size) {
    // Credit: Rendering Shadows from https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping
    //A3-shadows-?? Transform fragment world-space position into light clip space
    vec4 frag_light_clip = clip_from_world * vec4(position, 1.0);
    
    if (frag_light_clip.w <= 0.0) return 1.0; // Guard: fragment behind the light —> treat as no shadow
    
    vec3 frag_light_ndc = frag_light_clip.xyz / frag_light_clip.w; // perform perspective divide, returns the frag's light-space position in [-1, 1]
    vec2 shadow_uv = frag_light_ndc.xy * 0.5 + 0.5; // transform NDC xy -> UV [0,1] to sample from the depth map (depth map is in range [0, 1])
    float current_depth = frag_light_ndc.z; // current fragment depth in light space [0,1]

    float texel_size = 1.0 / map_size;
    const float bias = 0.0001;
    const float sample_cnt = 9.0;

    // Credit: PCF, shadow acne from https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping
    float lit = 0.0; // 0 = no shadow (lit), 1 = shadow
    for (int dx = -1; dx <= 1; dx++) { // dx and dy are: -1, 0, 1 -> take 9 samples
        for (int dy = -1; dy <= 1; dy++) {
            vec2 offset = vec2(float(dx), float(dy)) * texel_size;
            float pcf_depth = texture(shadowMaps, vec3(shadow_uv + offset, float(shadow_i))).r;
            lit += (current_depth <= pcf_depth + bias) ? 1.0 : 0.0;
        }
    }
    return lit / sample_cnt;
}

void main() {
    // A2-normal
    vec3 N = normalize(normal);
    vec3 T = normalize(tangent);
    vec3 B = normalize(bitangent);
    mat3 TBN = mat3(T, B, N);

    // with lighting:
    // Basic hemispherical lighting equation in glsl syntax, where: n is the per-pixel normal (remember to normalize after interpolation!); texCoord is the interpolated texture coordinate; *_DIRECTION are uniforms giving the light directions; *_ENERGY are uniforms giving the light energy in appropriate units; ALBEDO is the albedo texture; and outColor is the value that gets written to the framebuffer.
    // vec3 n = normalize(normal); (before A2-normal)
    // A2-normal
    vec3 n = texture(normalMap, texCoord).rgb; // obtain normal vector from normal map in range [0,1], in tangent space
    n = normalize(n * 2.0 - 1.0); // transform normal vector to range [-1,1]. Need this because texture colors are stored in [0, 1], but directions need to be in [-1, 1].
                                  // A normal map can't store negative numbers as raw bytes. The encoding happens when the normal map texture is created — typically by an artist tool or texture baking software 
    n = normalize(TBN * n);  // rotate into world space, then used by lighting

    vec3 albedo = texture(TEXTURE, texCoord).rgb;

    // Credit: learned from https://learnopengl.com/Advanced-OpenGL/Cubemaps, https://registry.khronos.org/OpenGL-Refpages/gl4/html/texture.xhtml
    vec3 hdr;
    if (material_type == 0) { // pbr. Credit: ApproximateSpecularIBL from https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
        // mat_light = SKY_ENERGY * (0.5 * dot(n, SKY_DIRECTION) + 0.5); // hemisphere sky appoximation, before A2-pbr
        
        vec3 V = normalize(EYE - position); // view direction toward camera
        vec3 R = reflect(-V, N); // reflection direction
        float NdotV = max(dot(n, V), 0.0);

        // highest mip level index 
        const float MAX_REFLECTION_LOD = 4.0; // A2-pbr-TODO: The LearnOpenGL tutorial creates and loads 4 mipmaps at most. maybe we don't need this constraint. In the pre-filter step we only convoluted the environment map up to a maximum of 5 mip levels (0 to 4), which we denote here as MAX_REFLECTION_LOD to ensure we don't sample a mip level where there's no (relevant) data.
        vec3 prefilteredColor = textureLod(ggxPrefilteredEnvMap, R,  roughness * MAX_REFLECTION_LOD).rgb;  // sample the appropriate mip level based on the surface roughness, giving rougher surfaces blurrier specular reflections

        // mix(): linearly interpolate between two values
        // Credit: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#metal-brdf-and-dielectric-brdf
        vec3 F0 = mix(vec3(0.04), albedo, metalness); // specular reflectance at normal incidence, the amount of light reflected when looking directly at a surface (0 degree angle)

        // sample from the BRDF lookup texture given the material's roughness and the angle between the normal and view vector:
        vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness); // the indirect Fresnel results
        // Fresnel 的直觉：掠射角（grazing angle）下几乎所有光都镜面反射（比如水面斜看几乎是镜子），正视角下反射最少。 

        // diffuse IBL part of the reflectance equation:
        // kD 和 kS 是漫反射和镜面反射的能量权重，代表入射光中有多少比例走漫反射、多少比例走镜面反射。
        vec3 kS = F; // kS描述在给定视角下有多少光被镜面反射
        vec3 kD = 1.0 - kS; // 剩余给漫反射
        kD *= 1.0 - metalness; // Metalness 对 kD 的修正：金属没有漫反射（diffuse)，导体会立刻吸收折射进去的光，不会从内部散射出来。所以metalness = 0 (non-metal) => kD stays the same; =1 (metal) => only specular
        vec3 irradiance = texture(lambertianCubeMap, n).rgb; // prefiltered irradiance in direction n
        vec3 diffuse = irradiance * albedo;

        vec2 envBRDF = texture(brdfLUT, vec2(NdotV, roughness)).rg; // scale and bias to F0
        vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y); // combine scale+bias with the prefiltered env color (left portion of the IBL reflectance equation and re-construct the approximated integral result as specular)

        hdr = (kD * diffuse + specular) * pow(2.0, float(exposure)); // combine diffuse + specular + exposure

        // A3-materials: add direct light contributions
        vec3 direct = vec3(0.0);
        for (int i = 0; i < lights_count; i++) {
            float shadow_factor = 1.0;
            if (LIGHTS[i].shadow_i >= 0) { // A3-shadows
                shadow_factor = shadow_pcf(LIGHTS[i].shadow_i, LIGHTS[i].CLIP_FROM_WORLD, LIGHTS[i].shadow_map_size);
            }
            direct += direct_pbr(LIGHTS[i], n, albedo, F0) * shadow_factor;
        }
        hdr += direct * pow(2.0, float(exposure));
    } else {
        vec3 mat_light;
        if (material_type == 1) { // lambertian: sample the prefiltered irradiance cubemap at n; stores (incoming radiance at n)/PI, multiplying by albedo gives Lambertian output
            mat_light = texture(lambertianCubeMap, n).rgb;
            for (int i = 0; i < lights_count; i++) {
                float shadow_factor = 1.0;
                if (LIGHTS[i].shadow_i >= 0) { // A3-shadows
                    shadow_factor = shadow_pcf(LIGHTS[i].shadow_i, LIGHTS[i].CLIP_FROM_WORLD, LIGHTS[i].shadow_map_size);
                }
                mat_light += direct_lambertian(LIGHTS[i], n, albedo) * shadow_factor; // A3-materials: adding contributions from direct lights
            }
        } else if (material_type == 2) { // mirror
            vec3 I = normalize(position - EYE);
            vec3 R = reflect(I, normalize(n));
            mat_light = texture(cubeMap, R).rgb;
        } else if (material_type == 3) {// environmental
            mat_light = texture(cubeMap, n).rgb; // sample the cubemap in the direction of normal; texture() returns vec4 (RGBA), but alpha is not needed for calculating energy, so use .rgb to extract the vec3 (RGB)
        }
        // vec3 e = mat_light + SUN_ENERGY * max(0.0, dot(n, SUN_DIRECTION)); // before A3-materials, treat it as point light, no radius, does not handle near horizon
        hdr = (mat_light * albedo) * pow(2.0, float(exposure));
    }

    // A2-tone mapping: the process of converting HDR light values into LDR values that a display can show.
    vec3 ldr;
    if (tone_map == 0) { // linear
        ldr = vec3(clamp(hdr.r, 0, 1), clamp(hdr.g, 0, 1), clamp(hdr.b, 0, 1));
    } else if (tone_map == 1) { // ACES
        ldr = vec3(tonemap_aces(hdr.r), tonemap_aces(hdr.g), tonemap_aces(hdr.b));
    }

    /*  we need to apply a gamma correction because displays do not expect linear values
        but the swapchain uses VK_FORMAT_B8G8R8A8_SRGB, and Vulkan automatically applies gamma correction when writing to an sRGB swapchain, so I don't need to do it here again.
        Credit: https://www.reddit.com/r/vulkan/comments/135xdra/confused_about_srgb_swapchain_format_the/
        // outColor = vec4(linearToGamma(ldr), 1.0);
    */
    outColor = vec4(ldr, 1.0);
}