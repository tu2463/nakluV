#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>

struct PosNorTexTanVertex {
    struct { float x,y,z; } Position;
    struct { float x,y,z; } Normal;
    struct { float s, t; } TexCoord; // s  horizontal (like u), t = vertical (like v). OpenGL convention for texture coordinates.
    struct { float x,y,z, w; } Tangent; // optional, only if mesh has TANGENT attribute

    // a pipeline vertex input state that works with a buffer holding a PosNorTexTanVertex[] array:
    static const VkPipelineVertexInputStateCreateInfo array_input_state;
};

static_assert(sizeof(PosNorTexTanVertex) == 3*4 + 3*4 + 2*4 + 4*4, "PosNorTexTanVertex is packed.");