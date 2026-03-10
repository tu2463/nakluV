#include "CubePipeline.hpp"
#include "VK.hpp"

void CubePipeline::create(RTG &rtg) {
    // Section 2 — create(): shader module

    // void CubePipeline::create(RTG &rtg) {
    //     VkShaderModule module = rtg.helpers.create_shader_module(cube_code);
    // Wraps the SPIR-V bytes into a VkShaderModule. Must be destroyed after the pipeline is created (section 2e).

    // ---
    // Section 3 — create(): descriptor set layout for faces (set01_face)

    //     { // set0 and set1 share this layout: UBO at binding=0, storage image at binding=1
    //         std::array<VkDescriptorSetLayoutBinding, 2> bindings{ ... };
    //         VK( vkCreateDescriptorSetLayout(..., &set01_face) );
    //     }
    // Both the input face (set 0) and output face (set 1) use the same layout — that's why it's called set01_face. The layout has:
    // - binding = 0 → VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER (the Face / WORLD_FROM_PX matrix)
    // - binding = 1 → VK_DESCRIPTOR_TYPE_STORAGE_IMAGE (the pixel data)

    // ---
    // Section 4 — create(): descriptor set layout for params (set2_params)

    //     { // set2: roughness UBO at binding=0
    //         std::array<VkDescriptorSetLayoutBinding, 1> bindings{ ... };
    //         VK( vkCreateDescriptorSetLayout(..., &set2_params) );
    //     }
    // Just one binding: binding = 0 → VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER (the Params / roughness value).

    // ---
    // Section 5 — create(): pipeline layout

    //     { // pipeline layout uses 3 descriptor sets
    //         std::array<VkDescriptorSetLayout, 3> layouts{
    //             set01_face,   // set 0 = input face
    //             set01_face,   // set 1 = output face (reuses same layout)
    //             set2_params,  // set 2 = roughness params
    //         };
    //         VK( vkCreatePipelineLayout(..., &layout) );
    //     }
    // Three sets, two of which reuse set01_face because input and output faces have identical structure.

    // ---
    // Section 6 — create(): compute pipeline + cleanup

    //     { // create the compute pipeline
    //         VkComputePipelineCreateInfo create_info{
    //             .flags = VK_PIPELINE_CREATE_DISPATCH_BASE,  // needed for vkCmdDispatchBase
    //             .stage = { .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
    //             .layout = layout,
    //         };
    //         VK( vkCreateComputePipelines(..., &handle) );
    //     }

    //     vkDestroyShaderModule(rtg.device, module, nullptr); // SPIR-V no longer needed
    // }
    // VK_PIPELINE_CREATE_DISPATCH_BASE is required because main-cube.cpp uses vkCmdDispatchBase (not the plain vkCmdDispatch).
}

void CubePipeline::destroy(RTG &rtg) {
    if (handle != VK_NULL_HANDLE) {
        vkDestroyPipeline(rtg.device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(rtg.device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
    if (set01_face != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(rtg.device, set01_face, nullptr);
        set01_face = VK_NULL_HANDLE;
    }
}