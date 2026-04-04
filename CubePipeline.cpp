#include "CubePipeline.hpp"

#include "Helpers.hpp"
#include "VK.hpp"

static uint32_t cube_lambertian_code[] =
#include "spv/cube.comp.lambertian.inl"
;

/*
  GGX (Trowbridge-Reitz)
  - Models specular/glossy surfaces with microfacet theory
  - Reflection depends on view angle and surface roughness
  - Has a characteristic long tail — highlights fade gradually rather than sharply cutting off
  - Controlled by a roughness parameter (0 = mirror, 1 = very rough)
  - Examples: metals, plastics, polished surfaces
  - Used in physically-based rendering (PBR) pipelines

  - cube.comp.lambertian — prefilters the environment map for diffuse irradiance (used for the diffuse lighting term)
  - cube.comp.ggx — prefilters for specular reflections at various roughness levels (used for the specular term in split-sum approximation)
*/
static uint32_t cube_ggx_code[] =
#include "spv/cube.comp.ggx.inl"
;

// Credit: adapted from Zulip discussion https://15-472-s26.zulipchat.com/#narrow/channel/570157-A2/topic/Adding.20Cube.20Utility.20to.20Maekfile/with/575174040
void CubePipeline::create(RTG &rtg, Mode mode) {
    VkShaderModule module;
    if (mode == Mode::Lambertian) {
        module = rtg.helpers.create_shader_module(cube_lambertian_code);
    } else {
        module = rtg.helpers.create_shader_module(cube_ggx_code);
    }
   
    { // set0_in_face: input face — binding0=UBO (WORLD_FROM_PX), binding1=COMBINED_IMAGE_SAMPLER for textureLod
        std::array< VkDescriptorSetLayoutBinding, 2 > bindings{
            VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };
        VkDescriptorSetLayoutCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = uint32_t(bindings.size()),
            .pBindings = bindings.data(),
        };
        VK( vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set0_in_face) );
    }

    { // set1_out_face: output face — binding0=UBO (WORLD_FROM_PX), binding1=STORAGE_IMAGE)
        std::array< VkDescriptorSetLayoutBinding, 2 > bindings{
            VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };

        VkDescriptorSetLayoutCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = uint32_t(bindings.size()),
            .pBindings = bindings.data(),
        };
        VK( vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set1_out_face) );
    }

    { // in_sampler: trilinear。 Credit: Bright dots in the pre-filter convolution from https://learnopengl.com/PBR/IBL/Specular-IBL
        VkSamplerCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR, // bilinear within a mip level
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, // bilinear between two mip
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .minLod = 0.0f,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        VK( vkCreateSampler(rtg.device, &create_info, nullptr, &in_sampler) );
    }

    { // the set2 layout holds roughness info (and maybe more brdf params in the future):
        std::array< VkDescriptorSetLayoutBinding, 1 > bindings{
            VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };

        VkDescriptorSetLayoutCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = uint32_t(bindings.size()),
            .pBindings = bindings.data(),
        };
        VK( vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set2_params) );
    }

    { // create pipeline layout
        std::array< VkDescriptorSetLayout, 3> layouts = {
           set0_in_face, // set 0: input face (COMBINED_IMAGE_SAMPLER)
           set1_out_face,   // set 1: output face (STORAGE_IMAGE)
           set2_params,  // set 2: roughness params
        };
        VkPipelineLayoutCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .flags = 0,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges = nullptr,
            .pSetLayouts = layouts.data(),
            .setLayoutCount = uint32_t(layouts.size()),
        };
        VK( vkCreatePipelineLayout(rtg.device, &create_info, nullptr, &layout) );
    }
    
    { // create compute pipeline
        VkComputePipelineCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .flags = VK_PIPELINE_CREATE_DISPATCH_BASE,
            .layout = layout,
            .stage = VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, // this is a compute shader (not vertex/fragment)
                .module = module, // VkShaderModule
                .pName = "main", // tells Vulkan which function to call as the starting point when the shader run
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            }
        };
        VK( vkCreateComputePipelines(rtg.device, VK_NULL_HANDLE, 1, &create_info, nullptr, &handle) );
    }
    // modules no longer needed now that pipeline is created:
    vkDestroyShaderModule(rtg.device, module, nullptr);
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

    if (in_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(rtg.device, in_sampler, nullptr);
        in_sampler = VK_NULL_HANDLE;
    }
    if (set2_params != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(rtg.device, set2_params, nullptr);
        set2_params = VK_NULL_HANDLE;
    }
    if (set1_out_face != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(rtg.device, set1_out_face, nullptr);
        set1_out_face = VK_NULL_HANDLE;
    }
    if (set0_in_face != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(rtg.device, set0_in_face, nullptr);
        set0_in_face = VK_NULL_HANDLE;
    }
}