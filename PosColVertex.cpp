#include "PosColVertex.hpp"

#include <array>

static std::array< VkVertexInputBindingDescription, 1 > bindings{
    VkVertexInputBindingDescription{
        .binding = 0,
        .stride = sizeof(PosColVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    }
};

static std::array< VkVertexInputAttributeDescription, 2 > attributes{
    VkVertexInputAttributeDescription{
        .location = 0, 
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT, // Reads 12 bytes as 3 × 32-bit floats (x, y, z). Shader receives: vec3
        .offset = offsetof(PosColVertex, Position), // offsetof() macro: Automatically calculates the byte offset of a member in a struct. This is safer than hardcoding numbers.
    },
    VkVertexInputAttributeDescription{
        .location = 1, 
        .binding = 0,
        .format = VK_FORMAT_R8G8B8A8_UNORM, // Reads 4 bytes as 4 × 8-bit unsigned bytes. Automatically normalizes 0-255 → 0.0-1.0. Shader receives: vec4 with values in range [0.0, 1.0]
        .offset = offsetof(PosColVertex, Color),
    },
};

const VkPipelineVertexInputStateCreateInfo PosColVertex::array_input_state{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = uint32_t(bindings.size()),
    .pVertexBindingDescriptions = bindings.data(),
    .vertexAttributeDescriptionCount = uint32_t(attributes.size()),
    .pVertexAttributeDescriptions = attributes.data(),
};