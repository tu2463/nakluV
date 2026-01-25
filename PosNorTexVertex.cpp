#include "PosColTexVertex.hpp"

#include <array>

static std::array< VkVertexInputBindingDescription, 1 > bindings{
    VkVertexInputBindingDescription{
        .binding = 0,
        .stride = sizeof(PosColTexVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    }
};

static std::array< VkVertexInputAttributeDescription, 3 > attributes{
    // Notice that we're putting Position at location 0, Normal at location 1, and TexCoord at location 2. We'll need to remember those for when we update our shader.
    
    VkVertexInputAttributeDescription{
        .location = 0, // shader input location for this attribute
        .binding = 0, // binding number which this attribute takes its data from
        .format = VK_FORMAT_R32G32B32_SFLOAT, // Reads 12 bytes as 3 × 32-bit floats (x, y, z). Shader receives: vec3
        .offset = offsetof(PosColTexVertex, Position), // offsetof() macro: Automatically calculates the byte offset of a member in a struct. This is safer than hardcoding numbers.
    },
    VkVertexInputAttributeDescription{
        .location = 1, 
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT, // Reads 12 bytes as 3 × 32-bit floats (x, y, z). Shader receives: vec3
        .offset = offsetof(PosColTexVertex, Normal), // offsetof() macro: Automatically calculates the byte offset of a member in a struct. This is safer than hardcoding numbers.
    },
    VkVertexInputAttributeDescription{
        .location = 2, 
        .binding = 0,
        .format = VK_FORMAT_R32G32_SFLOAT, // Reads 8 bytes as 2 × 32-bit floats (x, y). Shader receives vec2s
        .offset = offsetof(PosColTexVertex, TexCoord), // offsetof() macro: Automatically calculates the byte offset of a member in a struct. This is safer than hardcoding numbers.
    },
};

const VkPipelineVertexInputStateCreateInfo PosColTexVertex::array_input_state{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = uint32_t(bindings.size()),
    .pVertexBindingDescriptions = bindings.data(),
    .vertexAttributeDescriptionCount = uint32_t(attributes.size()),
    .pVertexAttributeDescriptions = attributes.data(),
};