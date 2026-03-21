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
/*
Credit: https://learnopengl.com/Advanced-Lighting/Normal-Mapping
Think of the tangent space as the local space of the normal map's vectors.
In tangent space, the normal map stores (x, y, z) relative to the surface itself. To use it in world-space lighting, you need
  a 3×3 rotation matrix TBN = [T, B, N] where:                                                                                           
  - N = Normal vevtor, up vector along the surface of the normalp map
  - T = Tangent vector (already stored in your vertex data at location 3, .xyz), right vector along the surface of the normalp map
  - B = bitangent = the third axis of the tangent-space coordinate frame
    //      N  ↑  (perpendicular to surface)                                                                                                 
    //     |                                                                                                                               
    //     +----→  T  (along UV's U axis)                                                                                                  
    //    /                                                                                                                                
    //  B  (along UV's V axis, could go "forward" or "backward", direction determined by tangent.w) 
*/
layout(location=3) in vec4 Tangent; // A2-normal // s72 format "TANGENT": { "format":"R32G32B32A32_SFLOAT" }

layout(location = 0) out vec3 position; // lowercase variables for varyings (vertex shader outputs / fragment shader inputs)
layout(location = 1) out vec3 normal;
layout(location = 2) out vec2 texCoord;
layout(location = 3) out vec3 tangent;
layout(location = 4) out vec3 bitangent;

void main() {
    gl_Position = TRANSFORMS[gl_InstanceIndex].CLIP_FROM_LOCAL * vec4(Position, 1.0);
    position = mat4x3(TRANSFORMS[gl_InstanceIndex].WORLD_FROM_LOCAL) * vec4(Position, 1.0);
    normal = normalize(mat3(TRANSFORMS[gl_InstanceIndex].WORLD_FROM_LOCAL_NORMAL) * Normal);
    tangent = normalize(mat3(TRANSFORMS[gl_InstanceIndex].WORLD_FROM_LOCAL) * Tangent.xyz);

    // Credit: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html: 
    //         The bitangent vectors MUST be computed by taking the cross product of the normal and tangent XYZ vectors and multiplying it against the W component of the tangent: bitangent = cross(normal.xyz, tangent.xyz) * tangent.w.
    bitangent = cross(normal, tangent) * Tangent.w;
    texCoord = TexCoord;
}