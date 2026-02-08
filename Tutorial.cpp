#include "Tutorial.hpp"

#include "VK.hpp"
// #include "refsol.hpp"

#include <GLFW/glfw3.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include <random>
#include <algorithm>
#include <functional>

struct Vec3 {
    float x, y, z;
};

Tutorial::Tutorial(RTG &rtg_, S72 &s72_) : rtg(rtg_), s72(s72_) {
	// refsol::Tutorial_constructor(rtg, &depth_format, &render_pass, &command_pool);

	{ // set camera mode based on input
		if (rtg.configuration.camera_mode == "scene") {
			camera_mode = CameraMode::Scene;
		} else if (rtg.configuration.camera_mode == "user") {
			camera_mode = CameraMode::User;
		} else if (rtg.configuration.camera_mode == "debug") {
			camera_mode = CameraMode::Debug;
		} else {
			throw std::runtime_error("Invalid camera mode '" + rtg.configuration.camera_mode + "'. Must be 'scene', 'user', or 'debug'.");
		}
	}

	// select a depth format:
	// at least one of these two must be supported, according to the spec; but neither are required
	depth_format = rtg.helpers.find_image_format(
		{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32 }, // one-component, 32-bit signed floating-point format that has 32 bits in the depth component;  a two-component, 32-bit format that has 24 unsigned normalized bits in the depth component and, optionally, 8 bits that are unused.
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT //  an image view can be used as a framebuffer depth/stencil attachment and as an input attachment.
	);

	{ // create render pass 
		// attachments
		std::array< VkAttachmentDescription, 2 > attachments{
			VkAttachmentDescription{ // color attachment:
				.format = rtg.surface_format.format,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, // Clear to background color at start; how to actually load the data (.loadOp) before rendering happens
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE, // Save results after rendering; how to write the data back after rendering (.storeOp)
				.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, // No stencil buffer
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, // Don't care about old contents // what layout (.initialLayout) the image will be transitioned to before the load
				.finalLayout = rtg.present_layout, // Prepare for display // what layout (.finalLayout) the image will be transitioned to after the store.
			},
			VkAttachmentDescription{ // depth attachment:
				.format = depth_format,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // Clear to max depth at start
				.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, // Discard after rendering
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			},
		};

		// subpass
		VkAttachmentReference color_attachment_ref{
			.attachment = 0,
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};

		VkAttachmentReference depth_attachment_ref{
			.attachment = 1,
			.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};

		VkSubpassDescription subpass{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.inputAttachmentCount = 0, 
			.pInputAttachments = nullptr,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment_ref,
			.pDepthStencilAttachment = &depth_attachment_ref,
		};

		// dependencies
		//this defers the image load actions for the attachments:
		std::array< VkSubpassDependency, 2 > dependencies {
			// finish all work in the color attachment output stage, then do the layout transition, then start work in the color attachment output stage again
			// Before this render pass writes to the color attachment (`dstAccessMask = WRITE`), wait for any previous color attachment output operations (`srcStageMask`) from external work to complete.
			VkSubpassDependency{
				.srcSubpass = VK_SUBPASS_EXTERNAL, // everthing before; "Previous frame" or pre-render pass work
				.dstSubpass = 0,  // Our subpass (the first one, index 0)
				.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask = 0, // We don't care *what* the previous frame did (reading, writing, presenting)—we just need to wait until it's done touching the color attachment. The `initialLayout = UNDEFINED` already told Vulkan we're overwriting everything anyway.
				.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			},
			
			// when all existing work finishes the late fragment tests stage (the last point in the pipeline that touches the depth buffer), 
			// then the layout transition for the depth image happens, 
			// before subpass zero of this render pass can do operations in its early fragment tests stage (the earliest stage that touches the depth buffer).
			// Before early fragment tests write to the depth attachment in this render pass, wait for any previous late fragment test writes from external work to finish.
			VkSubpassDependency{
				.srcSubpass = VK_SUBPASS_EXTERNAL,
				.dstSubpass = 0,

				// If the previous frame was doing depth testing, finish writing those depth values before we clear and start using the depth buffer.
				.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, // Happen *after* fragment shaders (for things like alpha testing)
				.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, // Happen *before* fragment shaders run (fast depth rejection)
				.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			}
		};

		VkRenderPassCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = uint32_t(attachments.size()),
			.pAttachments = attachments.data(),
			.subpassCount = 1,
			.pSubpasses = &subpass,
			.dependencyCount = uint32_t(dependencies.size()),
			.pDependencies = dependencies.data(),
		};

		VK( vkCreateRenderPass(rtg.device, &create_info, nullptr, &render_pass) );
	}

	{ //create command pool
		VkCommandPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, // Each command buffer can be reset on its own
			.queueFamilyIndex = rtg.graphics_queue_family.value(),
		};
		VK( vkCreateCommandPool(rtg.device, &create_info, nullptr, &command_pool) );
	}

	background_pipeline.create(rtg, render_pass, 0);
	lines_pipeline.create(rtg, render_pass, 0);
	objects_pipeline.create(rtg, render_pass, 0);

	{ // create descriptor tool:
		uint32_t per_workspace = uint32_t(rtg.workspaces.size()); // for easier-to-read counting

		std::array< VkDescriptorPoolSize, 2 > pool_sizes{
			VkDescriptorPoolSize{ // uniform buffer descriptors
				.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 2 * per_workspace, // 1 descriptor per set, 2 set per workspace (world, camera)
			},
			VkDescriptorPoolSize{ // uniform buffer descriptors
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1 * per_workspace, // one descriptor per set, one set per workspace
			},
		};

		VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0, // because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, can't free individual descriptors allocated from this pool
			.maxSets = 3 * per_workspace, // 3 sets per workspace (2 uniform buffer for world and camera and 1 storage buffer for transforms)
			.poolSizeCount = uint32_t(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(),
		};

		VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &descriptor_pool) );
	}

	workspaces.resize(rtg.workspaces.size());
	for (Workspace &workspace : workspaces) {
		// refsol::Tutorial_constructor_workspace(rtg, command_pool, &workspace.command_buffer);
		{ // allocate command buffer
			VkCommandBufferAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = command_pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, // it can be submitted directly to a queue
				.commandBufferCount = 1,
			};
			VK( vkAllocateCommandBuffers(rtg.device, &alloc_info, &workspace.command_buffer) );
		}

		workspace.Camera_src = rtg.helpers.create_buffer(
			sizeof(LinesPipeline::Camera),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // going to have GPU copy from this memory
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, // host-visible memory, coherent (no special sync needed)
			Helpers::Mapped // get a pointer to memory
		);
		workspace.Camera = rtg.helpers.create_buffer(
			sizeof(LinesPipeline::Camera),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as a uniform buffer, also going to have GPU copy into this memory
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // GPU-local memory
			Helpers::Unmapped // don't get a pointer to memory
		);

		// descriptor set:
		{ //allocate descriptor set for Camera descriptor
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &lines_pipeline.set0_Camera,
			};

			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.Camera_descriptors) );
		}

		workspace.World_src = rtg.helpers.create_buffer(
			sizeof(ObjectsPipeline::World),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // going to have GPU copy from this memory
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, // host-visible memory (The CPU can see this memory), coherent (no special sync needed, CPU writes are automatically visible to GPU)
			Helpers::Mapped // get a pointer to memory
		);
		workspace.World = rtg.helpers.create_buffer(
			sizeof(ObjectsPipeline::World),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as a uniform buffer, also going to have GPU copy into this memory
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // GPU-local memory
			Helpers::Unmapped // don't get a pointer to memory
		);

		{ //allocate descriptor set for World descriptor
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &objects_pipeline.set0_World,
			};

			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.World_descriptors) );
		}

		{ //allocate descriptor set for Transforms descriptor
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &objects_pipeline.set1_Transforms,
			};

			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.Transforms_descriptors) ); // NOTE: we will fill in this descriptor set in render when buffers are [re-]allocated
		}

		// descriptor write:
		{ //point descriptor to Camera buffer:
			VkDescriptorBufferInfo Camera_info{
				.buffer = workspace.Camera.handle,
				.offset = 0,
				.range = workspace.Camera.size,
			};

			VkDescriptorBufferInfo World_info{
				.buffer = workspace.World.handle,
				.offset = 0,
				.range = workspace.World.size,
			};

			std::array< VkWriteDescriptorSet, 2 > writes{
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = workspace.Camera_descriptors, // Which descriptor set to update  
					.dstBinding = 0, // Which binding slot (matches shader)
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.pBufferInfo = &Camera_info, // The actual buffer to bind 
				},
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = workspace.World_descriptors,
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.pBufferInfo = &World_info,
				},
			};

			vkUpdateDescriptorSets(
				rtg.device, //device
				uint32_t(writes.size()), //descriptorWriteCount
				writes.data(), //pDescriptorWrites
				0, //descriptorCopyCount
				nullptr //pDescriptorCopies
			);
		}
	}

	{ //create a vertex buffer for the S72 (previously create object vertices pool buffer)
		size_t bytes = s72.vertices.size() * sizeof(s72.vertices[0]);

		object_vertices = rtg.helpers.create_buffer(
			bytes,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as vertex buffer, also going to have GPU copy into this memory
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // GPU-local memory
			Helpers::Unmapped // don't get a pointer to memory
		);

		// copy data to buffer
		// notice: this uploads the data during initialization instead of during the per-frame rendering loop (our rendering function Tutorial::render())// foreshadow!
		rtg.helpers.transfer_to_buffer(s72.vertices.data(), bytes, object_vertices);
	}

	{ // make some textures for objects
		textures.reserve(2);

		{ // texture 0: white to light blue gradient
			uint32_t size = 128;
			std::vector<uint32_t> data;
			data.reserve(size * size);
			for (uint32_t y = 0; y < size; ++y)
			{
				float t = y / float(size - 1); // 0 at top, 1 at bottom
				// white (255,255,255) to light blue (180,210,255)
				uint8_t r = uint8_t(255.0f - t * 75.0f);
				uint8_t g = uint8_t(255.0f - t * 45.0f);
				uint8_t b = 255;
				uint8_t a = 255;
				uint32_t pixel = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
				for (uint32_t x = 0; x < size; ++x)
				{
					data.emplace_back(pixel);
				}
			}
			assert(data.size() == size * size);

			textures.emplace_back(rtg.helpers.create_image(
				VkExtent2D{.width = size, .height = size},
				VK_FORMAT_R8G8B8A8_UNORM,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				Helpers::Unmapped));

			rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), textures.back());
		}

		{ // texture 1: light blue to dark gray gradient
			uint32_t size = 256;
			std::vector<uint32_t> data;
			data.reserve(size * size);
			for (uint32_t y = 0; y < size; ++y)
			{
				float t = y / float(size - 1); // 0 at top, 1 at bottom
				// light blue (180,210,255) to dark gray (60,60,60)
				uint8_t r = uint8_t(180.0f - t * 120.0f);
				uint8_t g = uint8_t(210.0f - t * 150.0f);
				uint8_t b = uint8_t(255.0f - t * 195.0f);
				uint8_t a = 255;
				uint32_t pixel = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
				for (uint32_t x = 0; x < size; ++x)
				{
					data.emplace_back(pixel);
				}
			}
			assert(data.size() == size * size);

			textures.emplace_back(rtg.helpers.create_image(
				VkExtent2D{.width = size, .height = size},
				VK_FORMAT_R8G8B8A8_SRGB,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				Helpers::Unmapped));

			rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), textures.back());
		}
	}

	{ // make image views for each texture image
		for (Helpers::AllocatedImage const &image : textures) {
			// An image view describes how to access an image — Vulkan requires you to create a view before you can use an image in a shader or pipeline.
			VkImageViewCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.flags = 0,
				.image = image.handle, // The underlying VkImage handle this view refers to.
				.viewType = VK_IMAGE_VIEW_TYPE_2D, // Treat the image as a standard 2D texture.
				.format = image.format, // Use the same format the image was created with (e.g., VK_FORMAT_R8G8B8A8_SRGB).
				// .components sets swizzling and is fine when zero-initialied; Left zero-initialized, which means no channel swizzling — R maps to R, G to G, etc. (identity mapping). 
				.subresourceRange{ // Specifies which part of the image to view:
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // this is a color image (not depth/stencil).
					.baseMipLevel = 0, .levelCount = 1, // only the base mip level (no mipmaps).
					.baseArrayLayer = 0, .layerCount = 1, // single layer (not an array texture). 
				},
			};

			VkImageView image_view = VK_NULL_HANDLE;
			// creates the view and stores the handle in the local image_view variable:
			VK( vkCreateImageView(
				rtg.device,
				&create_info,
				nullptr,
				&image_view
			) );

			texture_views.emplace_back(image_view);
		}
		assert(texture_views.size() == textures.size());
	}

	{ // make a sampler for the textures
		VkSamplerCreateInfo create_info {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.flags = 0,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST, // When a texture is magnified or minified, use nearest-neighbor filtering (no interpolation, picks the closest texel). The alternative would be VK_FILTER_LINEAR for smoother blending.  
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, // When selecting between mipmap levels, snap to the nearest level rather than blending between two.
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, // When texture coordinates go outside [0, 1], the texture repeats (tiles). Other options include clamping or mirroring.
			.mipLodBias = 0.0f, // No bias applied when selecting mipmap levels.
			.anisotropyEnable = VK_FALSE, // Anisotropic filtering is disabled. This means maxAnisotropy is ignored.
			.maxAnisotropy = VK_FALSE, // doesn't matter if anisotropy isn't enabled
			.compareEnable = VK_FALSE, // Depth comparison is disabled (used for shadow mapping). So compareOp is ignored.
			.compareOp = VK_COMPARE_OP_ALWAYS, // doesn't matter if compare isn't enabled
			.minLod = 0.0f, 
			.maxLod = 0.0f, // Clamps the mipmap level to exactly 0, meaning only the base mip level is ever used. 
			.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
			.unnormalizedCoordinates = VK_FALSE, // Texture coordinates are in the standard [0, 1] range rather than pixel coordinates.
		};

		// creates the sampler object and stores the handle in texture_sampler:
		VK( vkCreateSampler(rtg.device, &create_info, nullptr, &texture_sampler) );
	}
		
	{ // create the texture descriptor pool
		uint32_t per_texture = uint32_t(textures.size()); // for easier-to-read counting

		std::array< VkDescriptorPoolSize, 1 > pool_sizes{ // tells Vulkan how much memory to reserve in the pool, categorized by type
			VkDescriptorPoolSize{ // total number of individual descriptors available, categorized by type 
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // matches with the descriptor type in descriptor set layout (set2_TEXTURE) 
				.descriptorCount = 1 * per_texture, // one descriptor per set, one set per workspace
			},
		};

		VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0, // because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, can't free individual descriptors allocated from this pool
			.maxSets = 1 * per_texture, // one set per texture; total number of descriptor sets you can allocate from this pool
			.poolSizeCount = uint32_t(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(), // total number of individual descriptors available, categorized by type   
		};

		VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &texture_descriptor_pool) );
	}

	{ // allocate and write the texture descriptor sets
		VkDescriptorSetAllocateInfo alloc_info {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = texture_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &objects_pipeline.set2_TEXTURE,
		};
		texture_descriptors.assign(textures.size(), VK_NULL_HANDLE);

		for (VkDescriptorSet &descriptor_set : texture_descriptors) {
			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &descriptor_set) );
		}

		// write descriptors for textures:
		std::vector< VkDescriptorImageInfo > infos(textures.size());
		std::vector< VkWriteDescriptorSet > writes(textures.size());

		for (Helpers::AllocatedImage const &image : textures) {
			size_t i = &image - &textures[0];

			infos[i] = VkDescriptorImageInfo{
				.sampler = texture_sampler, // how to sample (filtering, wrapping, etc.)    
				.imageView = texture_views[i], // which texture image to sample from
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // expected layout during shader access 
			};
			writes[i] = VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = texture_descriptors[i], // which descriptor set to update      
				.dstBinding = 0, // binding index within that set (matches layout)
				.dstArrayElement = 0, // starting array index (for arrayed bindings) 
				.descriptorCount = 1, // updating 1 descriptor  
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // matches with the descriptor type in descriptor set layout (set2_TEXTURE) 
				.pImageInfo = &infos[i],
			};
		}

		vkUpdateDescriptorSets(
			rtg.device,
			uint32_t(writes.size()), // descriptorWrites count
			writes.data(), // descriptorWrites; can I use &writes here //vv No. &writes references to the vector, writes.data() references to the first elem
			0, nullptr // descriptorCopies count, data - what are these //vv specifies that we are updating the descriptor sets by writing new data into it instead of copying one set to another 
		);
	}
}

Tutorial::~Tutorial() {
	//just in case rendering is still in flight, don't destroy resources:
	//(not using VK macro to avoid throw-ing in destructor)
	if (VkResult result = vkDeviceWaitIdle(rtg.device); result != VK_SUCCESS) {
		std::cerr << "Failed to vkDeviceWaitIdle in Tutorial::~Tutorial [" << string_VkResult(result) << "]; continuing anyway." << std::endl;
	}

	if (texture_descriptor_pool) {
		vkDestroyDescriptorPool(rtg.device, texture_descriptor_pool, nullptr);
		texture_descriptor_pool = nullptr;

		// this also frees the descriptor sets allocated from the pool:
		texture_descriptors.clear();
	}

	if (texture_sampler) {
		vkDestroySampler(rtg.device, texture_sampler, nullptr);
		texture_sampler = VK_NULL_HANDLE; // why do we still need to set it back to null? what happens if we don't //??
	}

	for (VkImageView &view : texture_views) {
		vkDestroyImageView(rtg.device, view, nullptr);
		view = VK_NULL_HANDLE;
	}
	texture_views.clear();

	for (auto &texture : textures) {
		rtg.helpers.destroy_image(std::move(texture));
	}
	textures.clear();

	rtg.helpers.destroy_buffer(std::move(object_vertices)); // why don't we need to check whether it != NULL before destroying it, like the other checks //vv the type is AllocatedBuffer, is a struct that wraps the handle; the destroy_buffer function can take care of checking whether the handle is null

	if (swapchain_depth_image.handle != VK_NULL_HANDLE) {
		destroy_framebuffers();
	}

	for (Workspace &workspace : workspaces) {
		// refsol::Tutorial_destructor_workspace(rtg, command_pool, &workspace.command_buffer);
		if (workspace.command_buffer != VK_NULL_HANDLE) {
			vkFreeCommandBuffers(rtg.device, command_pool, 1, &workspace.command_buffer);
			workspace.command_buffer = VK_NULL_HANDLE;
		}

		if (workspace.lines_vertices_src.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices_src));
		}
		if (workspace.lines_vertices.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices));
		}

		if (workspace.Camera_src.handle != VK_NULL_HANDLE)  {
			rtg.helpers.destroy_buffer(std::move(workspace.Camera_src));
		}
		if (workspace.Camera.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.Camera));
		}
		// Camera_descriptors freed when pool is destroyed

		if (workspace.World_src.handle != VK_NULL_HANDLE)  {
			rtg.helpers.destroy_buffer(std::move(workspace.World_src));
		}
		if (workspace.World.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.World));
		}
		// World_descriptors freed when pool is destroyed

		if (workspace.Transforms.handle != VK_NULL_HANDLE)  {
			rtg.helpers.destroy_buffer(std::move(workspace.Transforms));
		}
		if (workspace.Transforms_src.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.Transforms_src));
		}
		// Transforms_descriptors freed when pool is destroyed
	}
	workspaces.clear();

	if (descriptor_pool) {
		vkDestroyDescriptorPool(rtg.device, descriptor_pool, nullptr);
		descriptor_pool = nullptr;
		// (this also frees the descriptor sets allocated from the pool)
	}

	background_pipeline.destroy(rtg);
	lines_pipeline.destroy(rtg);
	objects_pipeline.destroy(rtg);

	// refsol::Tutorial_destructor(rtg, &render_pass, &command_pool);
	// destroy command pool:
	if (command_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(rtg.device, command_pool, nullptr);
		command_pool = VK_NULL_HANDLE;
	}

	// destroy render pass:
	if (render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(rtg.device, render_pass, nullptr);
		render_pass = VK_NULL_HANDLE;
	}
}

void Tutorial::on_swapchain(RTG &rtg_, RTG::SwapchainEvent const &swapchain) {
	//[re]create framebuffers:
	// refsol::Tutorial_on_swapchain(rtg, swapchain, depth_format, render_pass, &swapchain_depth_image, &swapchain_depth_image_view, &swapchain_framebuffers);
	// clean up existing framebuffers (and depth image):
	if (swapchain_depth_image.handle != VK_NULL_HANDLE) {
		destroy_framebuffers();
	}

	//allocate depth image for framebuffers to share:
	swapchain_depth_image = rtg.helpers.create_image(
		swapchain.extent,
		depth_format,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		Helpers::Unmapped
	);

	{ //create depth image view:
		// The depth image view references the entire depth image as a 2D texture with depth values
		VkImageViewCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = swapchain_depth_image.handle,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = depth_format,
			.subresourceRange{
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
		};

		VK( vkCreateImageView(rtg.device, &create_info, nullptr, &swapchain_depth_image_view) );
	}

	// create framebuffers pointing to each swapchain image view and the shared depth image view
	//Make framebuffers for each swapchain image:
	swapchain_framebuffers.assign(swapchain.image_views.size(), VK_NULL_HANDLE); // resizes the vector and fills in with the null handle
	for (size_t i = 0; i < swapchain.image_views.size(); ++i) {
		std::array< VkImageView, 2 > attachments{
			swapchain.image_views[i],
			swapchain_depth_image_view,
		};
		VkFramebufferCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = render_pass,
			.attachmentCount = uint32_t(attachments.size()),
			.pAttachments = attachments.data(),
			.width = swapchain.extent.width,
			.height = swapchain.extent.height,
			.layers = 1,
		};

		VK( vkCreateFramebuffer(rtg.device, &create_info, nullptr, &swapchain_framebuffers[i]) );
	}
}

void Tutorial::destroy_framebuffers() {
	// refsol::Tutorial_destroy_framebuffers(rtg, &swapchain_depth_image, &swapchain_depth_image_view, &swapchain_framebuffers);

	for (VkFramebuffer &framebuffer : swapchain_framebuffers) {
		assert(framebuffer != VK_NULL_HANDLE);
		vkDestroyFramebuffer(rtg.device, framebuffer, nullptr);
		framebuffer = VK_NULL_HANDLE;
	}
	swapchain_framebuffers.clear();

	assert(swapchain_depth_image_view != VK_NULL_HANDLE);
	vkDestroyImageView(rtg.device, swapchain_depth_image_view, nullptr);
	swapchain_depth_image_view = VK_NULL_HANDLE;

	rtg.helpers.destroy_image(std::move(swapchain_depth_image));
}


void Tutorial::render(RTG &rtg_, RTG::RenderParams const &render_params) {
	//assert that parameters are valid:
	assert(&rtg == &rtg_);
	assert(render_params.workspace_index < workspaces.size());
	assert(render_params.image_index < swapchain_framebuffers.size());

	//get more convenient names for the current workspace and target framebuffer:
	Workspace &workspace = workspaces[render_params.workspace_index]; // sets of data used for rendering an individual image; swapchain images are the places where rendering results eventually get stored
	[[maybe_unused]] VkFramebuffer framebuffer = swapchain_framebuffers[render_params.image_index];

	// // record (into `workspace.command_buffer`) commands that run a `render_pass` that just clears `framebuffer`:
	// refsol::Tutorial_render_record_blank_frame(rtg, render_pass, framebuffer, &workspace.command_buffer);

	VK( vkResetCommandBuffer(workspace.command_buffer, 0) ); // reset the command buffer (clear old commands)
	{ // begin recording
		VkCommandBufferBeginInfo begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, //will record again every submit
		};
		VK( vkBeginCommandBuffer(workspace.command_buffer, &begin_info));
	}

	if (!lines_vertices.empty()) { // upload lines vertices
		//[re-]allocate lines buffers if needed:
		size_t needed_bytes = lines_vertices.size() * sizeof(lines_vertices[0]);
		if (workspace.lines_vertices_src.handle == VK_NULL_HANDLE || workspace.lines_vertices_src.size < needed_bytes) { // if the source buffer is missing or too small
			//round to next multiple of 4k to avoid re-allocating continuously if vertex count grows slowly
			int i1 = workspace.lines_vertices_src.handle == VK_NULL_HANDLE;
			int i2 = workspace.lines_vertices_src.size < needed_bytes;
			std::cout << "workspace index: " << render_params.workspace_index << ", checks: " << i1 << ", " << i2 << std::endl;
			size_t new_bytes = ((needed_bytes + 4096) / 4096) * 4096; 
			if (workspace.lines_vertices_src.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices_src));
			}
			if (workspace.lines_vertices.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices));
			}

			workspace.lines_vertices_src = rtg.helpers.create_buffer(
				new_bytes, 
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // /going to have GPU copy from this memory
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, // host-visible memory (the memory can be mapped from the CPU side), coherent (no special sync needed) (the memory doesn't require special flush operations to make host writes available)
				Helpers::Mapped // get a pointer to the memory
			);
			workspace.lines_vertices = rtg.helpers.create_buffer(
				new_bytes,
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as vertex buffer, also going to have GPU into this memory i.e. the target if memory copy
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // GPU-local memory
				Helpers::Unmapped // don't get a pointer to memory
			);

			std::cout << "Re-allocated lines buffers to " << new_bytes << " bytes." << std::endl;
		}

		assert(workspace.lines_vertices_src.size == workspace.lines_vertices.size);
		assert(workspace.lines_vertices_src.size >= needed_bytes);

		// host-side copy into lines_vertices_src
		// use the CPU to copy from the lines_vertices vector to the workspace.lines_vertices_src staging buffer
		assert(workspace.lines_vertices_src.allocation.mapped);
		std::memcpy(workspace.lines_vertices_src.allocation.data(), lines_vertices.data(), needed_bytes);

		// device-side copy from lines_vertices_src -> lines_vertices:
		// record a command to have the GPU copy the data from the staging buffer to the workspace.lines_vertices buffer.
		VkBufferCopy copy_region{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = needed_bytes,
		};
		vkCmdCopyBuffer(workspace.command_buffer, workspace.lines_vertices_src.handle, workspace.lines_vertices.handle, 1, &copy_region);
	}

	{ // upload camera info:
		// CameraInstance = storage format kept in CPU; LinesPipeline::Camera = the GPU/shader format that gets uploaded
		// because The shader is written to read CLIP_FROM_WORLD at offset 0 //TODO: do we need the shader to read more?
		LinesPipeline::Camera camera{
			.CLIP_FROM_WORLD = CLIP_FROM_WORLD};

		// //TODO: later add conditions to check camera mode
		// CameraInstance camera = scene_camera_instances[active_scene_camera]; // TODO: check if this is correct. should use the current active scene camera
		assert(workspace.Camera_src.size == sizeof(camera));

		// host-side copy into Camera_src:
		memcpy(workspace.Camera_src.allocation.data(), &camera, sizeof(camera));

		// add device-side copy from Camera_src -> Camera:
		assert(workspace.Camera_src.size == workspace.Camera.size);
		VkBufferCopy copy_region{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = workspace.Camera_src.size,
		};
		vkCmdCopyBuffer(workspace.command_buffer, workspace.Camera_src.handle, workspace.Camera.handle, 1, &copy_region);
	}

	{ // upload world info:
		assert(workspace.World_src.size == sizeof(world)); // don't think this is correct //??

		// host-side copy into World_src:
		memcpy(workspace.World_src.allocation.data(), &world, sizeof(world));

		// add device-side copy from World_src -> World:
		assert(workspace.World_src.size == workspace.World.size);
		VkBufferCopy copy_region{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = workspace.World_src.size,
		};
		vkCmdCopyBuffer(workspace.command_buffer, workspace.World_src.handle, workspace.World.handle, 1, &copy_region);
	}

	if (!object_instances.empty()) { // upload object transforms:
		//[re-]allocate object buffers if needed:
		size_t needed_bytes = object_instances.size() * sizeof(object_instances[0]);
		if (workspace.Transforms_src.handle == VK_NULL_HANDLE || workspace.Transforms_src.size < needed_bytes) { // if the source buffer is missing or too small
			//round to next multiple of 4k to avoid re-allocating continuously if vertex count grows slowly
			size_t new_bytes = ((needed_bytes + 4096) / 4096) * 4096; 
			if (workspace.Transforms_src.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.Transforms_src));
			}
			if (workspace.Transforms.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.Transforms));
			}

			workspace.Transforms_src = rtg.helpers.create_buffer(
				new_bytes, 
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // /going to have GPU copy from this memory
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, // host-visible memory (the memory can be mapped from the CPU side), coherent (no special sync needed) (the memory doesn't require special flush operations to make host writes available)
				Helpers::Mapped // get a pointer to the memory
			);
			workspace.Transforms = rtg.helpers.create_buffer(
				new_bytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as vertex buffer, also going to have GPU into this memory i.e. the target if memory copy
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // GPU-local memory
				Helpers::Unmapped // don't get a pointer to memory
			);

			// update the descriptor set:
			// Tells Vulkan "the Transforms shader binding should point to this specific buffer."
			// It connects your GPU buffer to the shader's descriptor. 
			// Describe the buffer:
			VkDescriptorBufferInfo Transforms_info{ 
				.buffer = workspace.Transforms.handle, // which buffer
				.offset = 0, // start at beginning
				.range = workspace.Transforms.size, // use whole buffer
			};

			// describe the write operation:
			std::array< VkWriteDescriptorSet, 1 > writes{
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  
					.dstSet = workspace.Transforms_descriptors, // which descriptor set to update  
					.dstBinding = 0, // binding 0 in that set
					.dstArrayElement = 0, // first element (if it were an array)
					.descriptorCount = 1,  // updating 1 descriptor
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &Transforms_info, 
				},
			};

			// execute the update (update includes all operations above, like the object_instances, etc.)
			vkUpdateDescriptorSets( 
				rtg.device,
				uint32_t(writes.size()), writes.data(), // descriptorWrites count, data
				0, nullptr // descriptorCopies count, data
			);

			std::cout << "Re-allocated object buffers to " << new_bytes << " bytes." << std::endl;
		}

		assert(workspace.Transforms.size == workspace.Transforms.size);
		assert(workspace.Transforms.size >= needed_bytes);

		{ //copy transforms into Transforms_src: use the CPU to copy from the transforms to the workspace.Transforms_src staging buffer
			assert(workspace.Transforms_src.allocation.mapped);
			ObjectsPipeline::Transform *out = reinterpret_cast< ObjectsPipeline::Transform* >(workspace.Transforms_src.allocation.data()); // struct aliasing violation, but it doesn't matter
			for (ObjectInstance const &inst : object_instances) {
				*out = inst.transform;
				++out;
			}
		}
		// device-side copy from lines_vertices_src -> lines_vertices:
		// record a command to have the GPU copy the data from the staging buffer to the workspace.lines_vertices buffer.
		VkBufferCopy copy_region{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = needed_bytes,
		};
		vkCmdCopyBuffer(workspace.command_buffer, workspace.Transforms_src.handle, workspace.Transforms.handle, 1, &copy_region);
	}

	{ // memory barrier to make sure copies compelte before rendering happens
		VkMemoryBarrier memory_barrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
		};

		vkCmdPipelineBarrier(
			workspace.command_buffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,		// srcStageMask; ensures all transfer operations (like buffer copies) complete
			VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, // dstStageMask; wait for all transfer ops before vertex input reads the data
			0,									// dependencyFlags,
			1, &memory_barrier,					// memoryBarriers (count, data)
			0, nullptr,							// bufferMemoryBarriers (count, data)
			0, nullptr							// imageMemoryBarriers (count, data)
		);
	}

	// put GPU commands here
	//render pass:
	std::array< VkClearValue, 2 > clear_values{
		VkClearValue{ .color{ .float32{ 0.54, 0.35, 0.80, 1.0f } } },
		VkClearValue{ .depthStencil{ .depth = 1.0f, .stencil = 0 } },
	};

	VkRenderPassBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 
		.renderPass = render_pass,
		.framebuffer = framebuffer,
		.renderArea{
			.offset = {.x = 0, .y = 0},
			.extent = rtg.swapchain_extent,
		},
		.clearValueCount = uint32_t(clear_values.size()),
		.pClearValues = clear_values.data(),
	};
	
	vkCmdBeginRenderPass(workspace.command_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

	// run pipelines here:
	{ // set scissor rectangle:
		VkRect2D scissor{
			.offset = {.x = 0, .y = 0},
			.extent = rtg.swapchain_extent,
		};
		vkCmdSetScissor(workspace.command_buffer, 0, 1, &scissor);
	}
	{ // configure viewport transform:
		VkViewport viewport{
			.x = 0.0f,
			.y = 0.0f,
			.width = float(rtg.swapchain_extent.width),
			.height = float(rtg.swapchain_extent.height),
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		vkCmdSetViewport(workspace.command_buffer, 0, 1, &viewport);
	}

	{ // draw with the background pipeline:
		vkCmdBindPipeline(workspace.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, background_pipeline.handle);
		
		{ // push time:
			BackgroundPipeline::Push push{
				.time = time,
			};
			vkCmdPushConstants(workspace.command_buffer, background_pipeline.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		}
		
		vkCmdDraw(workspace.command_buffer, 3, 1, 0, 0);
	}

	{ // draw with the lines pipeline:
		vkCmdBindPipeline(
			workspace.command_buffer, 
			VK_PIPELINE_BIND_POINT_GRAPHICS, 
			lines_pipeline.handle
		);
		
		{ // use lines vertices (offset 0) as vertex buffer binding 0:
			std::array< VkBuffer, 1 > vertex_buffers{ workspace.lines_vertices.handle };
			std::array< VkDeviceSize, 1 > offsets{ 0 };
			vkCmdBindVertexBuffers(
				workspace.command_buffer, 
				0, 
				uint32_t(vertex_buffers.size()), 
				vertex_buffers.data(), 
				offsets.data()
			);
		}

		{ //bind Camera descriptor set:
			std::array< VkDescriptorSet, 1 > descriptor_sets{
				workspace.Camera_descriptors, //0: Camera
			};
			vkCmdBindDescriptorSets(
				workspace.command_buffer, //command buffer
				VK_PIPELINE_BIND_POINT_GRAPHICS, //pipeline bind point
				lines_pipeline.layout, //pipeline layout
				0, //first set
				uint32_t(descriptor_sets.size()), descriptor_sets.data(), //descriptor sets count, ptr
				0, nullptr //dynamic offsets count, ptr
			);
		}

		// draw lines vertices:
		vkCmdDraw(workspace.command_buffer, uint32_t(lines_vertices.size()), 1, 0, 0);
	}

	if (!object_instances.empty()) { // draw with the objects pipeline
		vkCmdBindPipeline(workspace.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, objects_pipeline.handle);

		{// use object vertices (offset 0) as vertex buffer binding 0: // what does offset 0. and vertex buffer binding mean //??
			std::array< VkBuffer, 1 > vertex_buffers{ object_vertices.handle };
			std::array< VkDeviceSize, 1 > offsets{ 0 };
			vkCmdBindVertexBuffers(
				workspace.command_buffer,
				0,
				uint32_t(vertex_buffers.size()),
				vertex_buffers.data(),
				offsets.data()
			);
		}

		{ // bind World and Transforms descriptor set:
			std::array< VkDescriptorSet, 2 > descriptor_sets{
				workspace.World_descriptors, // 0: World
				workspace.Transforms_descriptors, // 1: Transforms
			};
			vkCmdBindDescriptorSets(
				workspace.command_buffer, // command buffer
				VK_PIPELINE_BIND_POINT_GRAPHICS, // pipeline bind point
				objects_pipeline.layout, // pipeline layout
				0, // first set; note that before creating the world descriptor set, our descriptor set got bound as set 1, not set 0.
				uint32_t(descriptor_sets.size()), descriptor_sets.data(), // descriptor sets count, ptr
				0, nullptr // dynamic offsets count, ptr
			);
		}

		// camera descriptor set is still bound (!), but not used <- what does this mean //vv
		// we didn't need to re-bind the camera descriptor set -- we were able to leave it bound because set 0 for both the lines pipeline and the objects pipeline are compatible.
		// - You drew lines with the lines pipeline (camera was bound)
		// - Now you switch to the objects pipeline with vkCmdBindPipeline
		// - You don't need to rebind the camera descriptor set!

		// draw all instances:
		for (ObjectInstance const &inst : object_instances) {
			uint32_t index = uint32_t(&inst - &object_instances[0]);

			// bind texture descriptor set
			vkCmdBindDescriptorSets(
				workspace.command_buffer, // command buffer
				VK_PIPELINE_BIND_POINT_GRAPHICS, // pipeline bind point
				objects_pipeline.layout, // pipeline layout
				2, // set number (slot 2)   
				1, &texture_descriptors[inst.texture], // descriptor sets count, ptr (which descriptor set to put in slot 2)
				0, nullptr // dynamic offsets count, ptr
			);

			// vkCmdDraw(workspace.command_buffer, inst.vertices.count, 1, inst.vertices.first, index); // Prev for drawing objects
			vkCmdDraw(workspace.command_buffer, inst.mesh->count, 1, inst.mesh->first_vertex, index);
		}
	}

	vkCmdEndRenderPass(workspace.command_buffer);

	//end recording:
	VK( vkEndCommandBuffer(workspace.command_buffer ));
	
	{ //submit `workspace.command buffer` for the GPU to run:
		// refsol::Tutorial_render_submit(rtg, render_params, workspace.command_buffer)
		// Now, we've seen vkQueueSubmit before, but this one is a bit unique in that it needs to wait on and signal semaphores as well as signal a fence.
		std::array< VkSemaphore, 1 > wait_semaphores{ // what is semaphores //vv  used for synchronizing work between on-GPU workloads.
			render_params.image_available // swapchain signals this when an image is ready to render to  
		};
		std::array< VkPipelineStageFlags, 1 > wait_stages{
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		};
		static_assert(wait_semaphores.size() == wait_stages.size(), "every semaphore needs a stage");

		std::array< VkSemaphore, 1 > signal_semaphores{
			// The work that waits on this semaphore will be submitted by the window system interface layer after we finish the render call
			render_params.image_done // your render signals this  after the rendering work in this batch is done
		};
		VkSubmitInfo submit_info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = uint32_t(wait_semaphores.size()),
			.pWaitSemaphores = wait_semaphores.data(), // why use array.data() instead of &array here //vv wait_semaphores.data() is VkSemaphore* type; &wait_semaphores is std::array<VkSemaphore,1>* type 

			// tells Vulkan: Don't let any submitted work reach the color attachment output stage (where fragment shaders write to the framebuffer) until image_available is signaled.
			// Operations before the color attachment stage (like copies, vertex processing) can start running immediately, even while the image is still being presented! 
			// Only when GPU work reaches the point of actually writing colors does it have to wait for the semaphore:
			.pWaitDstStageMask = wait_stages.data(),
			.commandBufferCount = 1,
			.pCommandBuffers = &workspace.command_buffer,
			.signalSemaphoreCount = uint32_t(signal_semaphores.size()),
			.pSignalSemaphores = signal_semaphores.data()
		};

		VK( vkQueueSubmit(rtg.graphics_queue, 1, &submit_info, render_params.workspace_available) ); // what's a VkFence (render_params.workspace_available) //??
	}
}


void Tutorial::update(float dt) {
	time  = std::fmod(time + dt, 60.0f);

	{ // add each s72 mesh to object_instances (previously create some objects: sphere surrounded by rotating torus //TODO: understand this)
		// TODO: can we move this chunk outside of update? is it necessary to re-traverse the tree and re-create object instances every frame?
		object_instances.clear();

		// 1. traverse the scene graph from root; "roots" is an optional array of references to nodes at which to start drawing the scene.

		// 2. Building Local Transform from node's TRS (Translation, Rotation Scale: local = Translation × Rotation × Scale      
		// Where: Translation = mat4 with (tx, ty, tz) in last column; Rotation = quaternion (x,y,z,w) → 3x3 rotation matrix; Scale = diagonal mat4 with (sx, sy, sz, 1)  

		// understand these helpers //??
		// helper: make translation matrix from S72::vec3
		auto translate = [](S72::vec3 const &t) -> mat4 {
			return mat4{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				t.x,  t.y,  t.z,  1.0f,
			};
		};

		// helper: scale matrix
		auto scale = [](S72::vec3 const &s) -> mat4 {
			return mat4{
				s.x, 0.0f, 0.0f, 0.0f,
				0.0f, s.y, 0.0f, 0.0f,
				0.0f, 0.0f, s.z, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f,
			};
		};

		// helper: rotation matrix from quaternion (column-major)
		auto rotation_from_quat = [](S72::quat const &q) -> mat4 {
			float x = q.x, y = q.y, z = q.z, w = q.w;
			float xx = x * x, yy = y * y, zz = z * z;
			float xy = x * y, xz = x * z, yz = y * z;
			float wx = w * x, wy = w * y, wz = w * z;
			// 3x3 rotation
			float m00 = 1.0f - 2.0f * (yy + zz);
			float m01 = 2.0f * (xy - wz);
			float m02 = 2.0f * (xz + wy);

			float m10 = 2.0f * (xy + wz);
			float m11 = 1.0f - 2.0f * (xx + zz);
			float m12 = 2.0f * (yz - wx);

			float m20 = 2.0f * (xz - wy);
			float m21 = 2.0f * (yz + wx);
			float m22 = 1.0f - 2.0f * (xx + yy);

			return mat4{
				m00, m10, m20, 0.0f,
				m01, m11, m21, 0.0f,
				m02, m12, m22, 0.0f,
				0.0f,0.0f,0.0f,1.0f,
			};
		};

		// helper: transpose a mat4
		auto transpose = [](mat4 const &A) -> mat4 {
			mat4 R;
			for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) R[c*4 + r] = A[r*4 + c];
			return R;
		};

		// helper: inverse of an affine mat4 (bottom row = 0,0,0,1). If not invertible, returns identity.
		auto inverse_affine = [](mat4 const &M) -> mat4 {
			// Extract upper-left 3x3 (column-major)
			float a00 = M[0], a10 = M[1], a20 = M[2];
			float a01 = M[4], a11 = M[5], a21 = M[6];
			float a02 = M[8], a12 = M[9], a22 = M[10];

			// compute determinant
			float det = a00*(a11*a22 - a12*a21) - a01*(a10*a22 - a12*a20) + a02*(a10*a21 - a11*a20);
			if (std::fabs(det) < 1e-12f) return mat4_identity;
			float invdet = 1.0f / det;

			// inverse 3x3 = adjugate / det
			float b00 =  (a11*a22 - a12*a21) * invdet;
			float b01 = -(a01*a22 - a02*a21) * invdet;
			float b02 =  (a01*a12 - a02*a11) * invdet;

			float b10 = -(a10*a22 - a12*a20) * invdet;
			float b11 =  (a00*a22 - a02*a20) * invdet;
			float b12 = -(a00*a12 - a02*a10) * invdet;

			float b20 =  (a10*a21 - a11*a20) * invdet;
			float b21 = -(a00*a21 - a01*a20) * invdet;
			float b22 =  (a00*a11 - a01*a10) * invdet;

			// translation vector
			float tx = M[12], ty = M[13], tz = M[14];

			// invT = -invM * t
			float itx = -(b00*tx + b01*ty + b02*tz);
			float ity = -(b10*tx + b11*ty + b12*tz);
			float itz = -(b20*tx + b21*ty + b22*tz);

			mat4 R;
			// column 0
			R[0] = b00; R[1] = b10; R[2] = b20; R[3] = 0.0f;
			// column 1
			R[4] = b01; R[5] = b11; R[6] = b21; R[7] = 0.0f;
			// column 2
			R[8] = b02; R[9] = b12; R[10] = b22; R[11] = 0.0f;
			// column 3 (translation)
			R[12] = itx; R[13] = ity; R[14] = itz; R[15] = 1.0f;
			return R;
		};

		// recursive traversal
		std::function< void(S72::Node*, mat4 const &) > traverse = [&](S72::Node* node, mat4 const &parent_world) {
			// build local TRS = Translation * Rotation * Scale
			mat4 local = translate(node->translation) * rotation_from_quat(node->rotation) * scale(node->scale);
			mat4 world = parent_world * local; // child's world = parent_world × local

			if (node->mesh != nullptr) {
				ObjectsPipeline::Transform tf;
				tf.WORLD_FROM_LOCAL = world;
				tf.CLIP_FROM_LOCAL = CLIP_FROM_WORLD * world;
				tf.WORLD_FROM_LOCAL_NORMAL = transpose(inverse_affine(world));

				object_instances.emplace_back(ObjectInstance{
					.mesh = node->mesh,
					.transform = tf,
					.texture = 0,
				});
			}

			if (node->camera != nullptr) {
				scene_camera_instances.emplace_back(CameraInstance{
					.camera = node->camera,
					.WORLD_FROM_LOCAL = world,
				});
			}

			for (S72::Node* child : node->children) {
				traverse(child, world);
			}
		};

		// start traversal from roots using identity as parent
		for (S72::Node* root : s72.scene.roots) {
			if (root) traverse(root, mat4_identity);
		}
		
		// { // previous: sphere at center
		// 	mat4 WORLD_FROM_LOCAL{ // sphere at origin
		// 		1.0f, 0.0f, 0.0f, 0.0f,
		// 		0.0f, 1.0f, 0.0f, 0.0f,
		// 		0.0f, 0.0f, 1.0f, 0.0f,
		// 		0.0f, 0.0f, 0.0f, 1.0f,
		// 	};

		// 	object_instances.emplace_back(ObjectInstance{
		// 		.vertices = sphere_vertices, // TODO: understand this
		// 		.transform{
		// 			.CLIP_FROM_LOCAL = CLIP_FROM_WORLD * WORLD_FROM_LOCAL,
		// 			.WORLD_FROM_LOCAL = WORLD_FROM_LOCAL,
		// 			.WORLD_FROM_LOCAL_NORMAL = WORLD_FROM_LOCAL,
		// 		},
		// 		.texture = 0,
		// 	});
		// }
	}

	if (camera_mode == CameraMode::Scene) {
		//TODO: what do we need here? the rendering happens through one of the cameras in the scene graph and the user cannot change the camera transformation
		if (scene_camera_instances.empty()) {
			camera_mode = CameraMode::User; // switch to user camera if no cameras in scene
		} else {
			CameraInstance const &camera = scene_camera_instances[active_scene_camera];
			S72::Camera::Perspective& projection = std::get<S72::Camera::Perspective>(camera.camera->projection); //vv need to use this instead of camera.camera->projection; because the original type is a variant
			mat4 perpective_projection_matrix = perspective(
				projection.vfov,
				projection.aspect,
				projection.near,
				projection.far
			);
			// View = inverse of camera's world transform //??
			mat4 view = inverse(camera.WORLD_FROM_LOCAL);
			CLIP_FROM_WORLD = perpective_projection_matrix * view; //??
		}
	} else if (camera_mode == CameraMode::User) { // understand this //??
		CLIP_FROM_WORLD = perspective(
			free_camera.fov,
			rtg.swapchain_extent.width / float(rtg.swapchain_extent.height), //aspect
			free_camera.near,
			free_camera.far
		) * orbit(
			free_camera.target_x, free_camera.target_y, free_camera.target_z,
			free_camera.azimuth, free_camera.elevation, free_camera.radius
		);
	} else if (camera_mode == CameraMode::Debug) {
		// TODO: the rendering happens through a second user-controlled camera, but the culling happens for the previously-active camera (this is very useful for debugging culling). When using the debug  camera, your renderer should display object bounding boxes and camera frustums using lines.
	} else {
		assert(0 && "only two camera modes");
	}

	{ // static sun and sky
		// Direction: (0, 0, 1) — pointing straight up along the Z-axis 
		world.SKY_DIRECTION.x = 0.0f;
		world.SKY_DIRECTION.y = 0.0f;
		world.SKY_DIRECTION.z = 1.0f;

		// Energy: (0.1, 0.1, 0.2) — a dim, slightly blue tint (the blue channel is twice as strong as red/green)    
		world.SKY_ENERGY.r = 0.1f;
		world.SKY_ENERGY.g = 0.1f;
		world.SKY_ENERGY.b = 0.2f;

		// Direction: (6/23, 13/23, 18/23) ≈ (0.26, 0.57, 0.78) — a normalized vector pointing roughly up and to the side
		world.SUN_DIRECTION.x = 6.0f / 23.0f;
		world.SUN_DIRECTION.y = 13.0f / 23.0f;
		world.SUN_DIRECTION.z = 18.0f / 23.0f;

		// Energy: (1.0, 1.0, 0.9) — bright white with a slight warm/yellow tint (red and green at full, blue slightly reduced)  
		world.SUN_ENERGY.r = 1.0f;
		world.SUN_ENERGY.g = 1.0f;
		world.SUN_ENERGY.b = 0.9f;
	}

	{ // 4 triangular pyramids (wireframe tetrahedra) using your Vec3; rotation stays whatever you already do in your transform
		lines_vertices.clear();

		constexpr uint32_t pyramids = 4;
		constexpr size_t edges_per_pyramid = 6;
		constexpr size_t count = pyramids * edges_per_pyramid * 2; // 6 edges * 2 verts per edge * 4 pyramids
		lines_vertices.reserve(count);

		auto vadd = [](Vec3 a, Vec3 b) -> Vec3 { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; };
		auto vscale = [](Vec3 a, float s) -> Vec3 { return Vec3{a.x * s, a.y * s, a.z * s}; };

		auto push_edge = [&](Vec3 a, Vec3 b,
							uint8_t ar, uint8_t ag, uint8_t ab, uint8_t aa,
							uint8_t br, uint8_t bg, uint8_t bb, uint8_t ba) {
			lines_vertices.emplace_back(PosColVertex{
				.Position{ .x = a.x, .y = a.y, .z = a.z },
				.Color{ .r = ar, .g = ag, .b = ab, .a = aa },
			});
			lines_vertices.emplace_back(PosColVertex{
				.Position{ .x = b.x, .y = b.y, .z = b.z },
				.Color{ .r = br, .g = bg, .b = bb, .a = ba },
			});
		};

		auto push_tetra = [&](Vec3 center, float s,
							// color per vertex (apex, base1, base2, base3)
							uint8_t c0r, uint8_t c0g, uint8_t c0b, uint8_t c0a,
							uint8_t c1r, uint8_t c1g, uint8_t c1b, uint8_t c1a,
							uint8_t c2r, uint8_t c2g, uint8_t c2b, uint8_t c2a,
							uint8_t c3r, uint8_t c3g, uint8_t c3b, uint8_t c3a) {

			// Local tetra vertices (triangular pyramid), then translate by center:
			Vec3 A = vadd(center, vscale(Vec3{ 0.0f,  0.75f,  0.0f}, s)); // apex
			Vec3 B = vadd(center, vscale(Vec3{-0.65f, -0.375f, -0.45f}, s)); // base v1
			Vec3 C = vadd(center, vscale(Vec3{ 0.65f, -0.375f, -0.45f}, s)); // base v2
			Vec3 D = vadd(center, vscale(Vec3{ 0.0f, -0.375f,  0.75f}, s)); // base v3

			// 6 edges:
			push_edge(A, B, c0r,c0g,c0b,c0a, c1r,c1g,c1b,c1a);
			push_edge(A, C, c0r,c0g,c0b,c0a, c2r,c2g,c2b,c2a);
			push_edge(A, D, c0r,c0g,c0b,c0a, c3r,c3g,c3b,c3a);

			push_edge(B, C, c1r,c1g,c1b,c1a, c2r,c2g,c2b,c2a);
			push_edge(C, D, c2r,c2g,c2b,c2a, c3r,c3g,c3b,c3a);
			push_edge(D, B, c3r,c3g,c3b,c3a, c1r,c1g,c1b,c1a);
		};

		const float s = 0.35f; // pyramid size

		// Arrange 4 pyramids in a 2x2 layout with varying z for depth parallax.
		push_tetra(Vec3{-0.45f,  0.35f, 0.25f}, s,
				0xff,0x44,0x44,0xff,  0xff,0xff,0x00,0xff,  0x00,0xff,0x88,0xff,  0x44,0x88,0xff,0xff);

		push_tetra(Vec3{ 0.45f,  0.35f, 0.55f}, s,
				0xff,0x88,0x00,0xff,  0xff,0xff,0xff,0xff,  0x88,0x00,0xff,0xff,  0x00,0xaa,0xff,0xff);

		push_tetra(Vec3{-0.45f, -0.35f, 0.45f}, s,
				0x00,0xff,0xff,0xff,  0xff,0x00,0xaa,0xff,  0xaa,0xff,0x00,0xff,  0xff,0xaa,0x00,0xff);

		push_tetra(Vec3{ 0.45f, -0.35f, 0.15f}, s,
				0x88,0xff,0x88,0xff,  0x00,0x00,0xff,0xff,  0xff,0x00,0x00,0xff,  0x88,0x88,0x88,0xff);

		assert(lines_vertices.size() == count);
	}
}


void Tutorial::on_input(InputEvent const &evt) { // review/understand this //??
	//if there is a current action, it gets input priority:
	if (action) { // where is this action function defined //??
		action(evt);
		return;
	}

	// general controls:
	if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_TAB) { // tab key
		// switch cameras modes
		camera_mode = CameraMode((int(camera_mode) + 1) % 3); // 3 camera modes: Scene, User, Debug
		std::cout << "Camera mode: " << (camera_mode == CameraMode::Scene ? "Scene" : camera_mode == CameraMode::User ? "User" : "Debug") << std::endl;
		if (camera_mode == CameraMode::Scene && !scene_camera_instances.empty()) {
			std::cout << "Active scene camera: " << scene_camera_instances[active_scene_camera].camera->name << std::endl;
		}
		return; // returns since we don't want any later event handling code to be allowed to respond to the tab key
	}

	// scene camera controls:
	if (camera_mode == CameraMode::Scene) {
		if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_SPACE) {
			active_scene_camera = (int(active_scene_camera) + 1) % scene_camera_instances.size(); // change between scene cameras
			std::cout << "Active scene camera: " << scene_camera_instances[active_scene_camera].camera->name << std::endl;
			return;
		}
	}

	// user (previously called "free") camera controls:
	if (camera_mode == CameraMode::User) {
		if (evt.type == InputEvent::MouseWheel) {
			// change distance by 10% every scroll click:
			free_camera.radius *= std::exp(std::log(1.1f) * -evt.wheel.y);
			// make sure camera isn't too close or too far from target:
			free_camera.radius = std::max(free_camera.radius, 0.5f * free_camera.near); // it's kinda like setting the min and max spirng arm length in UE?
			free_camera.radius = std::min(free_camera.radius, 2.0f * free_camera.far);
			return;
		}

		if (evt.type == InputEvent::MouseButtonDown && evt.button.button == GLFW_MOUSE_BUTTON_LEFT && (evt.button.mods & GLFW_MOD_SHIFT)) {
			//start panning
			float init_x = evt.button.x;
			float init_y = evt.button.y;
			OrbitCamera init_camera = free_camera;

			action = [this,init_x,init_y,init_camera](InputEvent const &evt) {
				if (evt.type == InputEvent::MouseButtonUp && evt.button.button == GLFW_MOUSE_BUTTON_LEFT) {
					//cancel upon button lifted:
					action = nullptr;
					return;
				}
				if (evt.type == InputEvent::MouseMotion) {
					// handle motion:
					//image height at plane of target point:
					float height = 2.0f * std::tan(free_camera.fov * 0.5f) * free_camera.radius;

					//motion, therefore, at target point:
					float dx = (evt.motion.x - init_x) / rtg.swapchain_extent.height * height;
					float dy =-(evt.motion.y - init_y) / rtg.swapchain_extent.height * height; //note: negated because glfw uses y-down coordinate system

					//compute camera transform to extract right (first row) and up (second row):
					mat4 camera_from_world = orbit(
						init_camera.target_x, init_camera.target_y, init_camera.target_z,
						init_camera.azimuth, init_camera.elevation, init_camera.radius
					);

					//move the desired distance:
					free_camera.target_x = init_camera.target_x - dx * camera_from_world[0] - dy * camera_from_world[1];
					free_camera.target_y = init_camera.target_y - dx * camera_from_world[4] - dy * camera_from_world[5];
					free_camera.target_z = init_camera.target_z - dx * camera_from_world[8] - dy * camera_from_world[9];
					return;
				}
			};

			return;
		}

		if (evt.type == InputEvent::MouseButtonDown && evt.button.button == GLFW_MOUSE_BUTTON_LEFT) {
			//start tumbling // what is tumbling //??

			// std::cout << "Tumble started." << std::endl;
			float init_x = evt.button.x;
			float init_y = evt.button.y;
			OrbitCamera init_camera = free_camera;
			
			action = [this,init_x,init_y,init_camera](InputEvent const &evt) {
				if (evt.type == InputEvent::MouseButtonUp && evt.button.button == GLFW_MOUSE_BUTTON_LEFT) {
					//cancel upon button lifted:
					action = nullptr;
					// std::cout << "Tumble ended." << std::endl;
					return;
				}
				if (evt.type == InputEvent::MouseMotion) {
					// handle motion, normalized so 1.0 is window height:
					float dx = (evt.motion.x - init_x) / rtg.swapchain_extent.height;
					float dy =-(evt.motion.y - init_y) / rtg.swapchain_extent.height; //note: negated because glfw uses y-down coordinate system

					//rotate camera based on motion:
					float speed = float(M_PI); //how much rotation happens at one full window height
					float flip_x = (std::abs(init_camera.elevation) > 0.5f * float(M_PI) ? -1.0f : 1.0f); //switch azimuth rotation when camera is upside-down
					free_camera.azimuth = init_camera.azimuth - dx * speed * flip_x;
					free_camera.elevation = init_camera.elevation - dy * speed;

					//reduce azimuth and elevation to [-pi,pi] range:
					const float twopi = 2.0f * float(M_PI);
					free_camera.azimuth -= std::round(free_camera.azimuth / twopi) * twopi;
					free_camera.elevation -= std::round(free_camera.elevation / twopi) * twopi;
					return;
				}
			};

			return;
		}
	}
}
