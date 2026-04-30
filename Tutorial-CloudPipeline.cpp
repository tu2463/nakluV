#include "Tutorial.hpp"

#include "Helpers.hpp"
#include "VK.hpp"

#include <array>
#include <vector>

static uint32_t cloud_code[] =
#include "spv/cloud.comp.inl"
;

void Tutorial::CloudPipeline::create(RTG &rtg) {
	VkShaderModule module = rtg.helpers.create_shader_module(cloud_code);

	{ // set0_OutputImage: binding 0 = cloud output image (storage image, compute shader)
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
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set0_OutputImage));
	}

	{ // set1_Camera: binding 0 = Camera (WORLD_FROM_CLIP + camera pos, compute)
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
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set1_Camera));
	}

	{ // set2_CloudTextures: bindings 0-3 = dimensional_profile, detail_type, density_scale, noise (sampler3D, compute)
		std::array<VkDescriptorSetLayoutBinding, 4> bindings{
			VkDescriptorSetLayoutBinding{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
			VkDescriptorSetLayoutBinding{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
			VkDescriptorSetLayoutBinding{
				.binding = 2,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
			VkDescriptorSetLayoutBinding{
				.binding = 3,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		VkDescriptorSetLayoutCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = uint32_t(bindings.size()),
			.pBindings = bindings.data(),
		};
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set2_CloudTextures));
	}

	{ // set3_Time: binding 0 = Time (deltaTime, totalTime, sun XYZ, compute)
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
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set3_Time));
	}

	{ // set4_LightGrid: binding 0 = precomputed light grid, used for approximating ambient scattering (and possibly secondary marching later) (sampler3D, compute)
		std::array<VkDescriptorSetLayoutBinding, 1> bindings{
			VkDescriptorSetLayoutBinding{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		VkDescriptorSetLayoutCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = uint32_t(bindings.size()),
			.pBindings = bindings.data(),
		};
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set4_LightGrid));
	}

	{ // set5_Params: binding 0 = CloudParams (compute)
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
		VK(vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set5_Params));
	}

	{ // create pipeline layout
		std::array<VkDescriptorSetLayout, 6> layouts{
			set0_OutputImage,
			set1_Camera,
			set2_CloudTextures,
			set3_Time,
			set4_LightGrid,
			set5_Params,
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

	vkDestroyShaderModule(rtg.device, module, nullptr);
}

void Tutorial::CloudPipeline::destroy(RTG &rtg) {
	if (set0_OutputImage != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set0_OutputImage, nullptr);
		set0_OutputImage = VK_NULL_HANDLE;
	}
	if (set1_Camera != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set1_Camera, nullptr);
		set1_Camera = VK_NULL_HANDLE;
	}
	if (set2_CloudTextures != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set2_CloudTextures, nullptr);
		set2_CloudTextures = VK_NULL_HANDLE;
	}
	if (set3_Time != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set3_Time, nullptr);
		set3_Time = VK_NULL_HANDLE;
	}
	if (set4_LightGrid != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set4_LightGrid, nullptr);
		set4_LightGrid = VK_NULL_HANDLE;
	}
	if (set5_Params != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(rtg.device, set5_Params, nullptr);
		set5_Params = VK_NULL_HANDLE;
	}
	if (layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(rtg.device, layout, nullptr);
		layout = VK_NULL_HANDLE;
	}
	if (handle != VK_NULL_HANDLE) {
		vkDestroyPipeline(rtg.device, handle, nullptr);
		handle = VK_NULL_HANDLE;
	}
}
