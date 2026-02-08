#pragma once

// A small matrix math library for 4x4 matrices only

#include <array>
#include <cmath>
#include <cstdint>

// Note: column-major storage order (like in OpenGL / GLSL)
using mat4 = std::array< float, 16 >; // what does "using" mean //vv
static_assert(sizeof(mat4) == 16 * 4, "mat4 is exactly 16 32-bit floats");

using vec4 = std::array< float, 4 >;
static_assert(sizeof(vec4) == 4 * 4, "vec4 is exactly 4 32-bit floats");

inline vec4 operator*(mat4 const &A, vec4 const &b) { // what does "inline" mean //vv what does the syntax "operator*" mean //??
    vec4 ret;
    // compute ret = A * b
    for (uint32_t r = 0; r < 4; ++r) {
        ret[r] = A[0 * 4 + r] * b[0]; // ret[r] = A[r][0] * b[0] (start of dot product for row r)
        for (uint32_t k = 1; k < 4; ++k) {
            ret[r] += A[k * 4 + r] * b[k]; // ret[r] += A[r][k] * b[k] (continue dot product of row r with vector b
        }
    }
    return ret;
}

inline mat4 operator*(mat4 const &A, mat4 const &B) {
    mat4 ret;
    // compute ret = A * B;
    for (uint32_t c = 0; c < 4; ++c) {
        for (uint32_t r = 0; r < 4; ++r) {
            ret[c * 4 + r] = A[0 * 4 + r] * B[c * 4 + 0]; // C[r][c] = A[r][0] * B[0][c] (start of dot product)
            for (uint32_t k = 1; k < 4; ++k) {
                ret[c * 4 + r] += A[k * 4 + r] * B[c * 4 + k]; // C[r][c] += A[r][k] * B[k][c] (row r of A · column c of B)
            } 
        }
    }
    return ret;
}

// perspective projection matrix,
// - vfov is fov in radians
// - near maps to 0, far maps to 1
// looks down -z with +y up and +x right
inline mat4 perspective(float vfov, float aspect, float near, float far) {
    //as per https://www.terathon.com/gdc07_lengyel.pdf
	// (with modifications for Vulkan-style coordinate system)
	//  notably: flip y (vulkan device coords are y-down)
	//       and rescale z (vulkan device coords are z-[0,1])
    const float e = 1.0f / std::tan(vfov / 2.0f);
    const float a = aspect;
    const float n = near;
    const float f = far;
    return mat4{ // note: column-major storage order!
        e/a,  0.0f,                       0.0f, 0.0f,
        0.0f,   -e,                       0.0f, 0.0f,
        0.0f, 0.0f, -0.5f - 0.5f * (f+n)/(f-n),-1.0f,
        0.0f, 0.0f,              - (f*n)/(f-n), 0.0f,
    };
}

// look at matrix:
// makes a camera-space-from-world matrix for a camera at eye looking forward
// target with up-vecvtor pointing (as-close-as-possible) along up
// that is, it maps:
// - eye_xyz to the origin
// - the unit length vector from eye_xyz to target_xyz to -z
// - an as-close-as-possible unit-length vector to up to +y
inline mat4 look_at(
    float eye_x, float eye_y, float eye_z,
    float target_x, float target_y, float target_z,
    float up_x, float up_y, float up_z) {
    
    // NOTE: this would be a lot cleaner with a vec3 type and some overloads!

    // compute vector from eye to target:
    float in_x = target_x - eye_x;
    float in_y = target_y - eye_y;
    float in_z = target_z - eye_z;

   //normalize 'in' vector:
	float inv_in_len = 1.0f / std::sqrt(in_x*in_x + in_y*in_y + in_z*in_z);
	in_x *= inv_in_len;
	in_y *= inv_in_len;
	in_z *= inv_in_len;

	//make 'up' orthogonal to 'in':
	float in_dot_up = in_x*up_x + in_y*up_y +in_z*up_z;
	up_x -= in_dot_up * in_x;
	up_y -= in_dot_up * in_y;
	up_z -= in_dot_up * in_z;

	//normalize 'up' vector:
	float inv_up_len = 1.0f / std::sqrt(up_x*up_x + up_y*up_y + up_z*up_z);
	up_x *= inv_up_len;
	up_y *= inv_up_len;
	up_z *= inv_up_len;

	//compute 'right' vector as 'in' x 'up'
	float right_x = in_y*up_z - in_z*up_y;
	float right_y = in_z*up_x - in_x*up_z;
	float right_z = in_x*up_y - in_y*up_x;

	//compute dot products of right, in, up with eye:
	float right_dot_eye = right_x*eye_x + right_y*eye_y + right_z*eye_z;
	float up_dot_eye = up_x*eye_x + up_y*eye_y + up_z*eye_z;
	float in_dot_eye = in_x*eye_x + in_y*eye_y + in_z*eye_z;

	//final matrix: (computes (right . (v - eye), up . (v - eye), -in . (v-eye), v.w )
	return mat4{ //note: column-major storage order
		right_x, up_x, -in_x, 0.0f,
		right_y, up_y, -in_y, 0.0f,
		right_z, up_z, -in_z, 0.0f,
		-right_dot_eye, -up_dot_eye, in_dot_eye, 1.0f,
	};
}
//orbit camera matrix:
// makes a camera-from-world matrix for a camera orbiting target_{x,y,z}
//   at distance radius with angles azimuth and elevation.
// azimuth is counterclockwise angle in the xy plane from the x axis
// elevation is angle up from the xy plane
// both are in radians
inline mat4 orbit(
		float target_x, float target_y, float target_z,
		float azimuth, float elevation, float radius
	) {

    // shorthand for some useful trig values:
    float ca = std::cos(azimuth);
    float sa = std::sin(azimuth);
    float ce = std::cos(elevation);
    float se = std::sin(elevation);

    // camera's right direction is azimuth rotated by 90 degrees:
    float right_x = -sa;
    float right_y = ca;
    float right_z = 0.0f;

	// camera's up direction is elevation rotated 90 degrees:
	// (and points in the same xy direction as azimuth)
	float up_x = -se * ca;
	float up_y = -se * sa;
	float up_z = ce;

	// compute out direction:
    //direction to the camera from the target:
	float out_x = ce * ca;
	float out_y = ce * sa;
	float out_z = se;

	// compute camera position
	float eye_x = target_x + radius * out_x;
	float eye_y = target_y + radius * out_y;
	float eye_z = target_z + radius * out_z;

	//camera's position projected onto the various vectors:
	float right_dot_eye = right_x*eye_x + right_y*eye_y + right_z*eye_z;
	float up_dot_eye    =    up_x*eye_x +    up_y*eye_y +    up_z*eye_z;
	float out_dot_eye   =   out_x*eye_x +   out_y*eye_y +   out_z*eye_z;

	//the final local-from-world transformation (column-major):
	return mat4{
		right_x, up_x, out_x, 0.0f,
		right_y, up_y, out_y, 0.0f,
		right_z, up_z, out_z, 0.0f,
		-right_dot_eye, -up_dot_eye, -out_dot_eye, 1.0f,
	};
}

// --

constexpr mat4 mat4_identity{                                                                                                                                                                                            
    1.0f, 0.0f, 0.0f, 0.0f,                                                                                                                                                                                              
    0.0f, 1.0f, 0.0f, 0.0f,                                                                                                                                                                                              
    0.0f, 0.0f, 1.0f, 0.0f,                                                                                                                                                                                              
    0.0f, 0.0f, 0.0f, 1.0f,                                                                                                                                                                                              
}; 