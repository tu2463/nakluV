/*
 * cube utility — pre-integrates a radiance environment cubemap into a
 * lambertian irradiance cubemap.
 *
 * Usage: cube in.png --lambertian out.png
 *
 * Both files are in RGBE encoding (R,G,B mantissa bytes + shared exponent in
 * alpha), with 6 faces stacked vertically (total height = 6 * face_edge_length).
 *
 * Assignment specification:
 *   "when run with the command cube in.png --lambertian out.png reads a cubemap
 *    from in.png, integrates over it to produce a lambertian lookup table cube
 *    map, and stores this in out.png."
 */

#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cmath>

#include <glm/glm.hpp>

#include "RGBE.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// stb_image_write uses sprintf internally, which clang marks deprecated on macOS.
// Suppress that warning only around this header so -Werror doesn't trip on it.
#pragma clang diagnostic push // saves the current warning state
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#define STB_IMAGE_WRITE_IMPLEMENTATION // silences the sprintf deprecation warning
#include "stb_image_write.h"
#pragma clang diagnostic pop // restores -Werror and all other warnings for the rest of your code

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Solid angle helpers
// ---------------------------------------------------------------------------

// Antiderivative used by TexelCoordSolidAngle below.
// Credit: Rory Driscoll, "Cubemap Texel Solid Angle"
//   https://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/
// (Referenced in #A2 > Rendering Equation, 15-472 Spring '26 Zulip,
//  and discussed further in #A2 > More on Texel Areas by Jim McCann.)
static float AreaElement(float x, float y) {
    return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0f));
}

// Returns the solid angle (steradians) subtended by input texel (u, v) on a
// cubemap face of edge length `size`. u and v are integer texel coordinates.
//
// This is the AMD CubeMapGen formula (via Rory Driscoll's blog). Jim McCann
// confirmed in #A2 > More on Texel Areas (15-472 Spring '26 Zulip) that this
// method is essentially exact — it matches the spherical polygon area formula
// to within floating-point noise, and is within 0.1% of the Jacobian approx.
//
// Note: the face index is not needed; solid angle depends only on (u, v, size).
static float texel_solid_angle(int u, int v, int size) {
    // Map integer texel index to [-1,1] NDC at texel center
    float U = (2.0f * (u + 0.5f) / float(size)) - 1.0f;
    float V = (2.0f * (v + 0.5f) / float(size)) - 1.0f;
    float inv = 1.0f / float(size); // half-texel width in NDC
    // Solid angle = integral of the differential area element over the texel's
    // projected footprint on the unit sphere, evaluated via the antiderivative.
    return AreaElement(U - inv, V - inv)
         - AreaElement(U - inv, V + inv)
         - AreaElement(U + inv, V - inv)
         + AreaElement(U + inv, V + inv);
}

// ---------------------------------------------------------------------------
// Cubemap face direction mapping
// ---------------------------------------------------------------------------

// Convert face index [0..5] and NDC coords (s, t) in [-1, 1] (s = horizontal,
// t = vertical in image/face space, both increasing downward/rightward) to a
// unit direction vector in world space.
//
// Convention matches Vulkan/OpenGL cubemap layer order (OpenGL 4.5 spec §8.13):
//   0 = +X,  1 = -X,  2 = +Y,  3 = -Y,  4 = +Z,  5 = -Z
static glm::vec3 face_to_direction(int face, float s, float t) {
    glm::vec3 dir;
    switch (face) {
        case 0:  dir = glm::vec3( 1.0f,   -t,   -s); break; // +X
        case 1:  dir = glm::vec3(-1.0f,   -t,    s); break; // -X
        case 2:  dir = glm::vec3(   s,  1.0f,    t); break; // +Y
        case 3:  dir = glm::vec3(   s, -1.0f,   -t); break; // -Y
        case 4:  dir = glm::vec3(   s,    -t, 1.0f); break; // +Z
        case 5:  dir = glm::vec3(  -s,    -t,-1.0f); break; // -Z
        default: dir = glm::vec3(0.0f);                break;
    }
    return glm::normalize(dir);
}

// ---------------------------------------------------------------------------
// Core baking function
// ---------------------------------------------------------------------------

// Integrates the input radiance cubemap into an output lambertian irradiance
// cubemap using a direct summation over all input texels.
//
// Integration formula (lambertian irradiance, divided by π so the shader just
// multiplies by albedo):
//
//   L_irr(n) = (1/π) · Σ_{input texels i}  L_in(ωᵢ) · max(0, n·ωᵢ) · dωᵢ
//
// where dωᵢ is the solid angle of input texel i.
//
// Approach: direct integral (sum over all input texels, no Monte Carlo), as
// recommended by Jim McCann in #A2 > Checking approach to cube utility
// (15-472 Spring '26 Zulip, 2026-02-22):
//   "Just write a loop over all input texels and accumulate the contribution
//    from each one!"
//
// in_pixels : RGBE pixel data, width=in_size, height=6*in_size, 4 ch/px
// out_pixels: filled with RGBE data, width=out_size, height=6*out_size, 4 ch/px
void bake_radiance_into_lambertian(
    const uint8_t *in_pixels,  int in_size,
    std::vector<uint8_t> &out_pixels, int out_size)
{
    // Allocate output: 6 faces × out_size × out_size pixels, 4 bytes each (RGBE)
    out_pixels.assign(size_t(6) * out_size * out_size * 4, 0u);

    const char *face_names[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    // long long total_input_texels = 6LL * in_size * in_size;
    // std::cout << "Baking: " << 6 << " faces × " << out_size << "×" << out_size
    //           << " output texels, each summing " << total_input_texels
    //           << " input texels (" << in_size << "px/face)." << std::endl;

    for (int f_out = 0; f_out < 6; ++f_out) {
        std::cout << "  Face " << f_out << " (" << face_names[f_out] << ") ..." << std::flush;
        for (int ty = 0; ty < out_size; ++ty) {
            for (int tx = 0; tx < out_size; ++tx) {

                // Map output texel (f_out, tx, ty) → unit outgoing direction n
                float s_out = (2.0f * (tx + 0.5f) / float(out_size)) - 1.0f;
                float t_out = (2.0f * (ty + 0.5f) / float(out_size)) - 1.0f;
                glm::vec3 n = face_to_direction(f_out, s_out, t_out);

                // Accumulate: Σ L_in(ωᵢ) · cos(θᵢ) · dωᵢ  over all input texels
                glm::vec3 accum(0.0f);

                for (int f_in = 0; f_in < 6; ++f_in) {
                    for (int iy = 0; iy < in_size; ++iy) {
                        for (int ix = 0; ix < in_size; ++ix) {

                            // Map input texel (f_in, ix, iy) → unit incoming direction ω
                            float s_in = (2.0f * (ix + 0.5f) / float(in_size)) - 1.0f;
                            float t_in = (2.0f * (iy + 0.5f) / float(in_size)) - 1.0f;
                            glm::vec3 omega = face_to_direction(f_in, s_in, t_in);

                            // cos(θ) = n · ω ; skip texels below the hemisphere
                            float cos_theta = glm::dot(n, omega);
                            if (cos_theta <= 0.0f) continue;

                            // Solid angle subtended by this input texel on the unit sphere
                            // Credit: AMD/Rory Driscoll formula (see texel_solid_angle above)
                            float d_omega = texel_solid_angle(ix, iy, in_size);

                            // Decode RGBE → linear float RGB
                            // Input image layout: face f_in occupies rows [f_in*in_size .. (f_in+1)*in_size - 1]
                            int in_row    = f_in * in_size + iy;
                            int in_offset = (in_row * in_size + ix) * 4;
                            glm::u8vec4 in_rgbe(
                                in_pixels[in_offset + 0],
                                in_pixels[in_offset + 1],
                                in_pixels[in_offset + 2],
                                in_pixels[in_offset + 3]);
                            glm::vec3 L_in = rgbe_to_float(in_rgbe);

                            accum += L_in * cos_theta * d_omega;
                        }
                    }
                }

                // Divide by π: stores L_irr = E/π so shader computes L_out = albedo · L_irr
                // (Lambertian BRDF = albedo/π, so L_out = albedo/π · E = albedo · E/π)
                glm::vec3 L_irr = accum * (1.0f / float(M_PI));

                // Encode linear float RGB → RGBE and write to output buffer
                // Output layout: face f_out occupies rows [f_out*out_size .. (f_out+1)*out_size - 1]
                glm::u8vec4 out_rgbe = float_to_rgbe(L_irr);
                int out_row    = f_out * out_size + ty;
                int out_offset = (out_row * out_size + tx) * 4;
                out_pixels[out_offset + 0] = out_rgbe.r; // R mantissa
                out_pixels[out_offset + 1] = out_rgbe.g; // G mantissa
                out_pixels[out_offset + 2] = out_rgbe.b; // B mantissa
                out_pixels[out_offset + 3] = out_rgbe.a; // shared exponent (e + 128)
            }
        }
        std::cout << " done." << std::endl;
    }
    std::cout << "Bake complete." << std::endl;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    try {
        // Parse: cube in.png --lambertian out.png
        std::string in_file;
        std::string out_file;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--lambertian") {
                if (i + 1 >= argc)
                    throw std::runtime_error("--lambertian requires an output filename.");
                out_file = argv[++i];
            } else if (in_file.empty()) {
                in_file = arg;
            } else {
                throw std::runtime_error("Unrecognized argument '" + arg + "'.");
            }
        }

        if (in_file.empty())
            throw std::runtime_error("No input file given. Usage: cube in.png --lambertian out.png");
        if (out_file.empty())
            throw std::runtime_error("No output file given. Usage: cube in.png --lambertian out.png");

        // Load input cubemap (stb_image always returns 4 channels when asked)
        int in_w, in_h, orig_channels;
        uint8_t *in_data = stbi_load(in_file.c_str(), &in_w, &in_h, &orig_channels, 4);
        if (!in_data)
            throw std::runtime_error("Failed to load '" + in_file + "': " + stbi_failure_reason());

        // Validate: 6-face vertically stacked cubemap requires height == 6 * width
        if (in_h != 6 * in_w) {
            stbi_image_free(in_data);
            throw std::runtime_error(
                "Expected height == 6 * width for cubemap, got "
                + std::to_string(in_w) + "x" + std::to_string(in_h));
        }
        int in_size = in_w; // per-face edge length

        std::cout << "Loaded " << in_file
                  << " (" << in_w << "x" << in_h
                  << ", " << in_size << "px per face)" << std::endl;

        // Bake: output is 16×16 per face (sufficient for lambertian — very low frequency)
        // Credit: Jim McCann, #A2 > Checking approach to cube utility (15-472 Spring '26 Zulip):
        //   "16x16 is enough for the lambertian map."
        constexpr int OUT_SIZE = 16;
        std::vector<uint8_t> out_pixels;
        bake_radiance_into_lambertian(in_data, in_size, out_pixels, OUT_SIZE);
        stbi_image_free(in_data);

        // Write output PNG: width=OUT_SIZE, height=6*OUT_SIZE, 4 channels (RGBE), stride=OUT_SIZE*4
        int out_w = OUT_SIZE;
        int out_h = OUT_SIZE * 6;
        int ok = stbi_write_png(out_file.c_str(), out_w, out_h, 4,
                                out_pixels.data(), out_w * 4);
        if (!ok)
            throw std::runtime_error("stbi_write_png failed for '" + out_file + "'.");

        std::cout << "Wrote " << out_file
                  << " (" << out_w << "x" << out_h << " RGBE)" << std::endl;

    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
