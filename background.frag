#version 450

layout(location = 0) out vec4 outColor;
layout(location = 0) in vec2 position;

vec2 brickTile(vec2 _st, float _zoom) {
    _st *= _zoom;
    _st.x += step(1., mod(_st.y,2.0)) * 0.5;
    return fract(_st);
}

float box(vec2 _st, vec2 _size) {
    _size = vec2(0.5)-_size*0.5;
    vec2 uv = smoothstep(_size,_size+vec2(0.0000001),_st);
    uv *= smoothstep(_size,_size+vec2(0.000001),vec2(1.0)-_st);
    return uv.x*uv.y;
}

void main() {
    vec2 tiledSt = vec2(gl_FragCoord.x / 800, gl_FragCoord.y / 600);
    tiledSt = brickTile(tiledSt,5.0);
    float boxMask = box(tiledSt, vec2(0.9));

    // Create a brick color gradient based on position within each tile
    vec3 brickColor = vec3(0.8, 0.3, 0.1); // Base brick color
    vec3 brickColor2 = vec3(0.5, 0.2, 0.05); // Darker brick color
    
    // Gradient from top to bottom (or left to right)
    vec3 gradient = mix(brickColor, brickColor2, tiledSt.y); // Vertical gradient

    // Apply gradient to the brick, with mortar (non-box areas) being darker
    vec3 mortarColor = vec3(0.2, 0.15, 0.05);
    vec3 color = mix(mortarColor, gradient, boxMask);

    // outColor = vec4(color,1.0);

    outColor = vec4(position, 0.0, 1.0);

    // code example in tutorial: outColor = vec4( fract(gl_FragCoord.x / 100), gl_FragCoord.y / 400, 0.2, 1.0);
}