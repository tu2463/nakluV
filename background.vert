#version 450 // GLSL version 4.5
layout(location = 0) out vec2 position;

void main() {
    vec2 POSITION = vec2(2 * (gl_VertexIndex & 2) - 1, 4 * (gl_VertexIndex & 1) - 1); // the three vertices are: (-1, -1), (-1, 3), and (3, -1) in clip space.
   
    // Z = 0.0: Places the triangle at a neutral depth (middle of the depth range). Since we're drawing a full-screen quad for post-processing, depth doesn't matter.
    // W = 1.0: This is the homogeneous coordinate used for perspective division. Setting it to 1.0 means "no perspective distortion"—the coordinates stay as-is after the divide. (The GPU divides X, Y, Z by W later in the pipeline.)
    gl_Position = vec4(POSITION, 0.0, 1.0); // ets the vertex's clip-space position with Z=0 and W=1 (so no perspective division changes the coordinates).

    // transforms clip space coordinates (range -1 to +1) into texture/UV coordinates (range 0 to 1).
    // transforms the clip-space coordinates (range -1 to 3) into a more useful range for the fragment shader. It maps:
    //     -1 → 0
    //     1 → 1
    //     3 → 2
    position = POSITION * 0.5 + 0.5;
}