#include "Tutorial.hpp"

#include "Helpers.hpp"
#include "VK.hpp"

#include <array>

static uint32_t light_grid_code[] =
#include "spv/light_grid.comp.inl"
;

/* Credit: p30 from https://d3d3g8mu99pzk9.cloudfront.net/AndrewSchneider/Nubis%20Cubed.pdf
"Transmittance is a measure of the amount of light at a given depth in an optical medium. 
The graph indicates how the intensity of light decreases as more of it is absorbed over depth in the cloud. 
*In order to gather this depth, you must conduct a second (and I will add very expensive) light ray march toward the light*."

To avoid the expensive light ray march, we will precompute a "light grid" and lookup from it during rendering.
This approach is inspired by https://github.com/YueZhang1027/CIS5650-Final-Project-Frostnova
*/

void Tutorial::LightGridPipeline::create(RTG &rtg) {
	VkShaderModule module = rtg.helpers.create_shader_module(light_grid_code);

	{ // set0_LightGridImage: binding 0 = light grid storage image
		std::array<VkDescriptorSetLayoutBinding, 1> bindings{
			VkDescriptorSetLayoutBinding{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		VkDescriptorSetLayoutCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = uint32_t(bindings.size()),
			.pBindings = bindings.data(),
		};
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set0_LightGridImage));
	}

	{ // set1_CloudNVDF: bindings 0, 1, 2 = Nubis cloud VDB channels (dimensional_profile, detail_type, density_scale)
		std::array<VkDescriptorSetLayoutBinding, 3> bindings{
			VkDescriptorSetLayoutBinding{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
			},
			VkDescriptorSetLayoutBinding{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
			},
			VkDescriptorSetLayoutBinding{
				.binding = 2,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		VkDescriptorSetLayoutCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = uint32_t(bindings.size()),
			.pBindings = bindings.data(),
		};
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set1_CloudNVDF));
	}

	{ // set2_Params: binding 0 = CloudParams
		std::array<VkDescriptorSetLayoutBinding, 1> bindings{
			VkDescriptorSetLayoutBinding{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		VkDescriptorSetLayoutCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = uint32_t(bindings.size()),
			.pBindings = bindings.data(),
		};
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set2_Params));
	}

	{ // pipeline layout
		std::array<VkDescriptorSetLayout, 3> layouts{
			set0_LightGridImage,
			set1_CloudNVDF,
			set2_Params,
		};
		VkPipelineLayoutCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = uint32_t(layouts.size()),
			.pSetLayouts = layouts.data(),
		};
		VK(vkCreatePipelineLayout(rtg.device, &create_info, nullptr, &layout));
	}

	{ // create compute pipeline
		VkComputePipelineCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = VkPipelineShaderStageCreateInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = module,
				.pName = "main",
			},
			.layout = layout,
		};
		VK(vkCreateComputePipelines(rtg.device, VK_NULL_HANDLE, 1, &create_info, nullptr, &handle));
	}

    // modules no longer needed now that pipeline is created:
	vkDestroyShaderModule(rtg.device, module, nullptr);
}

void Tutorial::LightGridPipeline::destroy(RTG &rtg) {
	if (handle != VK_NULL_HANDLE) {
		vkDestroyPipeline(rtg.device, handle, nullptr);
		handle = VK_NULL_HANDLE;
	}
	if (layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(rtg.device, layout, nullptr);
		layout = VK_NULL_HANDLE;
	}
	if (set2_Params != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set2_Params, nullptr);
		set2_Params = VK_NULL_HANDLE;
	}
	if (set1_CloudNVDF != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set1_CloudNVDF, nullptr);
		set1_CloudNVDF = VK_NULL_HANDLE;
	}
	if (set0_LightGridImage != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set0_LightGridImage, nullptr);
		set0_LightGridImage = VK_NULL_HANDLE;
	}
}
