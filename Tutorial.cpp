#include "Tutorial.hpp"

#include "VK.hpp"
// #include "refsol.hpp"

#include "RGBE.hpp"
#include "stb_image.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

#include <random>
#include <algorithm>
#include <functional>

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

	{ // set culling mode based on input
		if (rtg.configuration.culling_mode == "none") {
			culling_mode = CullingMode::None;
		} else if (rtg.configuration.culling_mode == "frustum") {
			culling_mode = CullingMode::Frustum;
		} else {
			throw std::runtime_error("Invalid culling mode '" + rtg.configuration.culling_mode + "'.");
		}
	}

	{ // set culling mode based on input
		if (rtg.configuration.tone_map == "linear") {
			objects_pipeline.tone_map = ObjectsPipeline::ToneMap::Linear;
		} else if (rtg.configuration.tone_map == "aces") {
			objects_pipeline.tone_map = ObjectsPipeline::ToneMap::ACES;
		} else {
			throw std::runtime_error("Invalid tone map '" + rtg.configuration.tone_map + "'.");
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

		std::array< VkDescriptorPoolSize, 3 > pool_sizes{
			VkDescriptorPoolSize{ // uniform buffer descriptors
				.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 2 * per_workspace, // 1 descriptor per set, 2 set per workspace (world, camera)
			},
			VkDescriptorPoolSize{ // storage buffer descriptors
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 2 * per_workspace, // one descriptor per set, two set (Transforms + Lights) per workspace
			},
			VkDescriptorPoolSize{ // combined image sampler for shadow maps (set1 binding 2, one per workspace)
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1 * per_workspace,
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

	{ // A3-shadows: count shadow-casting spot lights and create comparison sampler
		for (auto &[name, light] : s72.lights) {
			if (std::holds_alternative<S72::Light::Spot>(light.source) && light.shadow > 0) {
				shadow_light_index_map[&light] = shadow_count; // shadow_count = layer index = insertion order
				shadow_count++;
				shadow_resolution = std::max(shadow_resolution, light.shadow);
			}
		}
		// if no shadow lights, use a minimal 1x1 image so descriptors are always valid
		if (shadow_count == 0) shadow_resolution = 1;

		VkSamplerCreateInfo sampler_info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR, // Use linear filtering for magnification & minification (smoother)
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,

			/* A3-shadows: MoltenVK (macOS) does not support, so need to set to VK_FALSE. Depth comparison is done manually in the fragment shader
			E: vkUpdateDescriptorSets(): pDescriptorWrites[0] (portability error): sampler comparison not available.
			The Vulkan spec states: If the VK_KHR_portability_subset extension is enabled, and VkPhysicalDevicePortabilitySubsetFeaturesKHR::mutableComparisonSamplers is VK_FALSE, then sampler must have been created with VkSamplerCreateInfo::compareEnable set to VK_FALSE (https://vulkan.lunarg.com/doc/view/1.4.335.1/mac/antora/spec/latest/chapters/descriptorsets.html#VUID-VkDescriptorImageInfo-mutableComparisonSamplers-04450)
			*/
			.compareEnable = VK_FALSE,
			.minLod = 0.0f,
			.maxLod = 0.0f, // Clamps the mipmap level to exactly 0, meaning only the base mip level is ever used.

			// Border color 是当 addressMode = CLAMP_TO_BORDER 时，UV 超出 [0,1] 范围返回的固定颜色值。  对 shadow sampler，这个值被当作深度值（取 R = 1.0 或 0.0）参与比较，而不是颜色。
			.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, // A3-shadows-?? outside light frustum = lit (no shadow)
		};
		VK( vkCreateSampler(rtg.device, &sampler_info, nullptr, &shadow_sampler) );
	}

	{ // A3-shadows: create depth-only shadow render pass
		VkAttachmentDescription depth_attachment{
			.format = depth_format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,        // clear to max depth (1.0) at the start of each shadow pass
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,      // keep depth in memory after the pass; the main pass samples it
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,   // no stencil
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // no stencil
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,                    // don't care what was there before; we'll clear it anyway
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, // auto-transition on pass end: ready for texture sampling in main pass
		};

		VkAttachmentReference depth_attachment_ref{
			.attachment = 0,                                              // index into the attachments array above
			.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,  // layout Vulkan uses while the subpass is executing depth writes
		};
		VkSubpassDescription subpass{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 0,                           // depth-only: no color attachments
			.pDepthStencilAttachment = &depth_attachment_ref,
		};

		std::array<VkSubpassDependency, 2> dependencies{
			// Dependency 0: previous frame's main pass may still be sampling the shadow map, we must wait for that read to finish before overwriting the depth values for the new frame.
			VkSubpassDependency{
				.srcSubpass = VK_SUBPASS_EXTERNAL, // work outside this render pass (previous frame)
				.dstSubpass = 0, // this shadow subpass
				.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,  // previous frame: shadow was sampled here
				.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, // this pass: depth writes begin here
				.srcAccessMask = VK_ACCESS_SHADER_READ_BIT, // previous frame read the shadow map
				.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, // this pass writes new depth values
				.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
			},

			// Dependency 1: this shadow pass finishes writing depth (LATE_FRAGMENT_TESTS WRITE), 
			// need to wait for that before the main pass's fragment shader samples it (FRAGMENT_SHADER READ).
			VkSubpassDependency{
				.srcSubpass = 0, // this shadow subpass
				.dstSubpass = VK_SUBPASS_EXTERNAL, // work outside (the main render pass)
				.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, // shadow depth writes finish here
				.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,  // main pass samples the shadow map here
				.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, // shadow depth writes
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,  // main pass reads via texture()
				.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
			},
		};
		VkRenderPassCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &depth_attachment,
			.subpassCount = 1,
			.pSubpasses = &subpass,
			.dependencyCount = uint32_t(dependencies.size()),
			.pDependencies = dependencies.data(),
		};
		VK( vkCreateRenderPass(rtg.device, &create_info, nullptr, &shadow_render_pass) );
	}
	shadow_pipeline.create(rtg, shadow_render_pass);

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

			// use uniform buffer because Camera data (CLIP_FROM_WORLD matrix) is a small block of constants that every vertex shader invocation reads - same value for all vertices in a draw call
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
				.pSetLayouts = &objects_pipeline.set1_TransformsLightsShadows,
			};

			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.TransformsLights_descriptors) ); // NOTE: we will fill in this descriptor set in render when buffers are [re-]allocated
		}

		{ // A3-shadows: create per-workspace 2D array shadow image and initialize binding 2
			uint32_t layers = std::max(1u, shadow_count); // at least 1 layer so the image/view are always valid
			workspace.shadow_image = rtg.helpers.create_image(
				VkExtent2D{ .width = shadow_resolution, .height = shadow_resolution },
				depth_format,
				VK_IMAGE_TILING_OPTIMAL,

				// depth stencil attachment = 用途-写入：作为 framebuffer 的深度附件，Shadow pipeline 渲染时，GPU 把深度值写进这张图；
				// sampled = 用途-读取：作为 shader 里的纹理采样，Objects pipeline 渲染时，fragment shader 用 texture(shadowMaps, ...) 读这张图
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				Helpers::Unmapped,
				0, // no special image create flags
				layers // arrayLayers
			);

			{ // 2d array view for sampling in objects.frag
				VkImageViewCreateInfo create_info{
					.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					.image = workspace.shadow_image.handle,
					.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
					.format = depth_format,
					.subresourceRange{ // Specifies which part of the image to view:
						.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, // this is depth/stencil image, not color
						.baseMipLevel = 0, .levelCount = 1, // only the base mip level, no mipmaps
						.baseArrayLayer = 0, .layerCount = layers,
					},
				};
				VK( vkCreateImageView(rtg.device, &create_info, nullptr, &workspace.shadow_image_view) );
			}

			{ // transition all layers: UNDEFINED -> DEPTH_STENCIL_READ_ONLY_OPTIMAL
				rtg.helpers.transfer_to_image(
					nullptr, 0, // no pixel data to upload
					workspace.shadow_image,
					layers,
					VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
					0, // mip level
					VK_IMAGE_ASPECT_DEPTH_BIT
				);
			}

			// bind the array image to set1 binding 2
			VkDescriptorImageInfo shadow_image_info{
				.sampler = shadow_sampler,
				.imageView = workspace.shadow_image_view,
				.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
			};
			VkWriteDescriptorSet shadow_write{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = workspace.TransformsLights_descriptors,
				.dstBinding = 2,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &shadow_image_info,
			};
			vkUpdateDescriptorSets(rtg.device, 1, &shadow_write, 0, nullptr);
		}

		{ // A3-shadows: create single-layer views for each light and framebuffers for the shadow render pass
			workspace.shadow_views.resize(shadow_count);
			workspace.shadow_framebuffers.resize(shadow_count);
			for (uint32_t shadow_i = 0; shadow_i < shadow_count; shadow_i++) {
				VkImageViewCreateInfo view_info{
					.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					.image = workspace.shadow_image.handle,
					.viewType = VK_IMAGE_VIEW_TYPE_2D,
					.format = depth_format,
					.subresourceRange{
						.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
						.baseMipLevel = 0, .levelCount = 1,
						.baseArrayLayer = shadow_i, .layerCount = 1,
					},
				};
				VK( vkCreateImageView(rtg.device, &view_info, nullptr, &workspace.shadow_views[shadow_i]) );

				VkFramebufferCreateInfo create_info{
					.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
					.renderPass = shadow_render_pass,
					.attachmentCount = 1,
					.pAttachments = &workspace.shadow_views[shadow_i],
					.width = shadow_resolution,
					.height = shadow_resolution,
					.layers = 1,
				};
				VK( vkCreateFramebuffer(rtg.device, &create_info, nullptr, &workspace.shadow_framebuffers[shadow_i]) );
			}
		}

		// descriptor write for World and Camera:
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

	uint32_t lambertian_texture_index = UINT32_MAX; // track texture index holds the prefiltered lambertian cubemap

	{ // make textures for objects from S72 scene textures
		// First, create a default white texture (index 0) for materials without textures
		{
			uint32_t size = 1;
			std::vector<uint32_t> data = {0xFFFFFFFF}; // white pixel (RGBA)

			textures.emplace_back(rtg.helpers.create_image(
				VkExtent2D{.width = size, .height = size},
				VK_FORMAT_R8G8B8A8_UNORM,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				Helpers::Unmapped));

			rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), textures.back());
		}

		// Now load textures from S72
		for (auto &[key, s72_texture] : s72.textures) {
			// Skip textures that failed to load (empty pixels)
			if (s72_texture.pixels.empty()) {
				std::cerr << "WARNING: Skipping texture with empty pixels: " << s72_texture.src << std::endl;
				continue;
			}

			// Record the texture index for this S72 texture
			uint32_t texture_index = static_cast<uint32_t>(textures.size());
			texture_index_map[&s72_texture] = texture_index;

			if (s72.env_lambertian_texture && &s72_texture == s72.env_lambertian_texture) {
				lambertian_texture_index = texture_index;
			}

			// Choose format, VkImageCreateFlags, arrayLayers, layerCount based on the texture's specification
			const void *data = s72_texture.pixels.data();
			size_t data_size = s72_texture.pixels.size(); // pixels is uint8_t, so .size() == byte count
			VkFormat format;
			VkImageCreateFlags flags = 0;
			uint32_t arrayLayers = 1;
			uint32_t layerCount = 1;
			uint32_t img_width = static_cast<uint32_t>(s72_texture.width);
			uint32_t img_height = static_cast<uint32_t>(s72_texture.height);
			switch (s72_texture.format) {
				case S72::Texture::Format::srgb:
					format = VK_FORMAT_R8G8B8A8_SRGB;
					break;
				case S72::Texture::Format::linear:
					/*
					VK_FORMAT_R8G8B8A8_UNORM:                                                                                                                                                  
					"Unsigned Normalized" — the GPU hands the shader the raw value linearly scaled to [0,1]:                                                                                    
					stored byte 128  →  shader reads  128/255 ≈ 0.502                                                                                                                           
					stored byte 255  →  shader reads  1.0                                                                                                                                       
					stored byte 0    →  shader reads  0.0                                                                                                                                       
					
					VK_FORMAT_R8G8B8A8_SRGB:                                                                                                                                                  
					The GPU applies sRGB gamma decode before the shader sees the value:                                                                                                         
					stored byte 128  →  shader reads  ~0.216   (not 0.502!)                                                                                                                   
					stored byte 255  →  shader reads  1.0                                                                                                                                       
					stored byte 0    →  shader reads  0.0                                                                                                                                       
					The curve is nonlinear — dark values get darkened even more.
					*/
					format = VK_FORMAT_R8G8B8A8_UNORM; // A2-env-DONE check: what format should this be //vv
					break;
				case S72::Texture::Format::rgbe:
					data = s72_texture.RGBE_floats.data();
					data_size = s72_texture.RGBE_floats.size() * sizeof(float); // in bytes
					format = cubemap_format;
					// cubmap has 6 faces stacked vertically in the source image, so per-face height = total height / 6
					img_height = img_height / 6;
					if (img_height == 0 || img_width == 0 || s72_texture.RGBE_floats.empty()) {
						std::cerr << "WARNING: Skipping invalid RGBE texture: " << s72_texture.src << std::endl;
						continue;
					}
					flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
					arrayLayers = 6;
					layerCount = 6;
					break;
				default:
					format = VK_FORMAT_R8G8B8A8_UNORM;
					break;
			}

			textures.emplace_back(rtg.helpers.create_image(
				VkExtent2D{.width = img_width, .height = img_height},
				format,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				Helpers::Unmapped,
				flags,
				arrayLayers));

			rtg.helpers.transfer_to_image(data, data_size, textures.back(), layerCount);
			std::cout << "Created GPU texture for: " << s72_texture.src << " at index " << texture_index << std::endl;
		}

		std::cout << "Created " << textures.size() << " GPU textures (including default white)." << std::endl;
	}

	{ // create texture indices for materials,  textures for color albedos

		{ // create a dummy normal map
			uint32_t size = 1;

			// R=128, G=128, B=255, A=255.
			// Decodes as: (128/255)*2-1 ≈ 0, (128/255)*2-1 ≈ 0, (255/255)*2-1 = 1 => (0,0,1)
			std::vector<uint32_t> data = {0xFFFF8080};

			normal_maps.emplace_back(rtg.helpers.create_image(
				VkExtent2D{.width = size, .height = size},
				normal_map_format,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				Helpers::Unmapped));
			rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), normal_maps.back());
		}

		for (auto &[name, mat] : s72.materials) {
			uint32_t tex_index = 0; // default white

			if (auto* pbr = std::get_if<S72::Material::PBR>(&mat.brdf)) {
				if (auto* tex = std::get_if<S72::Texture*>(&pbr->albedo)) {
					auto it = texture_index_map.find(*tex);
					if (it != texture_index_map.end()) {
						tex_index = it->second;
					}
				} else if (auto* col = std::get_if<S72::color>(&pbr->albedo)) {
					// Create 1x1 texture from color
					uint8_t r = static_cast<uint8_t>(std::clamp(col->r, 0.0f, 1.0f) * 255.0f);
					uint8_t g = static_cast<uint8_t>(std::clamp(col->g, 0.0f, 1.0f) * 255.0f);
					uint8_t b = static_cast<uint8_t>(std::clamp(col->b, 0.0f, 1.0f) * 255.0f);
					uint32_t pixel = (r) | (g << 8) | (b << 16) | (0xFF << 24); // RGBA little-endian
					std::vector<uint32_t> data = {pixel};

					tex_index = static_cast<uint32_t>(textures.size());
					textures.emplace_back(rtg.helpers.create_image(
						VkExtent2D{.width = 1, .height = 1},
						VK_FORMAT_R8G8B8A8_UNORM,
						VK_IMAGE_TILING_OPTIMAL,
						VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
						VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
						Helpers::Unmapped));
					rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), textures.back());
				}
			} else if (auto* lambertian = std::get_if<S72::Material::Lambertian>(&mat.brdf)) {
				if (auto* tex = std::get_if<S72::Texture*>(&lambertian->albedo)) {
					auto it = texture_index_map.find(*tex);
					if (it != texture_index_map.end()) {
						tex_index = it->second;
					}
				} else if (auto* col = std::get_if<S72::color>(&lambertian->albedo)) {
					// Create 1x1 texture from color
					uint8_t r = static_cast<uint8_t>(std::clamp(col->r, 0.0f, 1.0f) * 255.0f);
					uint8_t g = static_cast<uint8_t>(std::clamp(col->g, 0.0f, 1.0f) * 255.0f);
					uint8_t b = static_cast<uint8_t>(std::clamp(col->b, 0.0f, 1.0f) * 255.0f);
					uint32_t pixel = (r) | (g << 8) | (b << 16) | (0xFF << 24); // RGBA little-endian
					std::vector<uint32_t> data = {pixel};

					tex_index = static_cast<uint32_t>(textures.size());
					textures.emplace_back(rtg.helpers.create_image(
						VkExtent2D{.width = 1, .height = 1},
						VK_FORMAT_R8G8B8A8_UNORM,
						VK_IMAGE_TILING_OPTIMAL,
						VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
						VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
						Helpers::Unmapped));
					rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), textures.back());
				}
			}
			// Mirror and Environment materials use default white (tex_index = 0)
			material_albedo_map[&mat] = tex_index;

			if (mat.normal_map != nullptr) { // make image view for normal map
				if (normal_index_map.contains(mat.normal_map)) continue;

				S72::Texture &normal_map = *mat.normal_map;
				if (normal_map.pixels.empty() || normal_map.width == 0 || normal_map.height == 0) { 
					std::cerr << "WARNING: Skipping normal map with empty pixels: " << normal_map.src << std::endl;
					continue;
				}
				
				uint32_t nm_index = static_cast<uint32_t>(normal_maps.size());

				normal_maps.emplace_back(rtg.helpers.create_image(
					VkExtent2D{.width = static_cast<uint32_t>(normal_map.width), .height = static_cast<uint32_t>(normal_map.height)},
					normal_map_format, // VK_FORMAT_R8G8B8A8_UNORM
					VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					Helpers::Unmapped));

				rtg.helpers.transfer_to_image(normal_map.pixels.data(), normal_map.pixels.size(), normal_maps.back());
				normal_index_map[mat.normal_map] = nm_index;
			}

		}
		std::cout << "Mapped " << material_albedo_map.size() << " materials to texture indices." << std::endl;
	}

	{ // create 1x1 black cubemap if no environment cubemap exists in the scene
		bool has_cubemap = std::any_of(textures.begin(), textures.end(), [this](Helpers::AllocatedImage const &img) { // A2-env-TODO: def needs to be optmized...
			return img.format == cubemap_format;
		});
		if (!has_cubemap) {
			std::vector<float> black(6 * 1 * 1 * 4, 0.0f); // 6 faces x 1x1 pixel x 4 floats
			uint32_t size = 1;
			
			textures.emplace_back(rtg.helpers.create_image(
				VkExtent2D{.width = size, .height = size},
				cubemap_format,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				Helpers::Unmapped,
				VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
				6
			));
			rtg.helpers.transfer_to_image(black.data(), black.size() * sizeof(float), textures.back(), 6);
		}
	}

	{ // make image views for each texture image
		for (int i = 0; i < textures.size(); i++) {
			Helpers::AllocatedImage const &image = textures[i];
			// An image view describes how to access an image — Vulkan requires you to create a view before you can use an image in a shader or pipeline.
			bool is_cubemap = (image.format == cubemap_format);

			VkImageViewCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.flags = 0,
				.image = image.handle, // The underlying VkImage handle this view refers to.
				.viewType = is_cubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, // if not cube map, treat the image as a standard 2D texture.
				.format = image.format, // Use the same format the image was created with (e.g., VK_FORMAT_R8G8B8A8_SRGB).
				// .components sets swizzling and is fine when zero-initialied; Left zero-initialized, which means no channel swizzling — R maps to R, G to G, etc. (identity mapping).
				.subresourceRange{ // Specifies which part of the image to view:
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // this is a color image (not depth/stencil).
					.baseMipLevel = 0, .levelCount = 1, // only the base mip level (no mipmaps).
					.baseArrayLayer = 0, .layerCount = is_cubemap ? 6u : 1u, // 6 layers for cubemap, 1 for non-array texture
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

			if (!is_cubemap) {
				texture_views.emplace_back(image_view);
			} else if (i == lambertian_texture_index) {
				lambertian_cubemap_view = image_view;
			} else {
				assert(cubemap_view == VK_NULL_HANDLE); // A2 write-up: "There is no more than one instance of an Environment object in every scene."
				cubemap_view = image_view;
			}
		}
		assert(texture_views.size()
			+ (cubemap_view != VK_NULL_HANDLE ? 1 : 0)
			+ (lambertian_cubemap_view != VK_NULL_HANDLE ? 1 : 0)
			== textures.size());
	}

	{ // make image views for each normal map image
		for (int i = 0; i < normal_maps.size(); i++) {
			Helpers::AllocatedImage const &image = normal_maps[i];
			VkImageViewCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.flags = 0,
				.image = image.handle,		   // The underlying VkImage handle this view refers to.
				.viewType = VK_IMAGE_VIEW_TYPE_2D, // treat the image as a standard 2D texture.
				.format = image.format,	   // Use the same format the image was created with
				.subresourceRange{
					// Specifies which part of the image to view:
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // this is a color image (not depth/stencil).
					.baseMipLevel = 0,
					.levelCount = 1, // only the base mip level (no mipmaps).
					.baseArrayLayer = 0,
					.layerCount = 1u, // 1 layer for non-array texture
				},
			};

			VkImageView image_view = VK_NULL_HANDLE;
			// creates the view and stores the handle in the local image_view variable:
			VK(vkCreateImageView(
				rtg.device,
				&create_info,
				nullptr,
				&image_view));

			normal_map_views.emplace_back(image_view);
		}
		assert(normal_map_views.size() == normal_maps.size());
	}

	{ // make a sampler for the textures
		VkSamplerCreateInfo create_info {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.flags = 0,
			.magFilter = VK_FILTER_LINEAR, // Use linear filtering for magnification (smoother)
			.minFilter = VK_FILTER_LINEAR, // Use linear filtering for minification (smoother)
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
		uint32_t per_normal_map = uint32_t(normal_maps.size()); // A2-normal

		std::array< VkDescriptorPoolSize, 1 > pool_sizes{ // tells Vulkan how much memory to reserve in the pool, categorized by type
			VkDescriptorPoolSize{ // total number of individual descriptors available, categorized by type 
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // matches with the descriptor type in descriptor set layout (set2_TEXTURE) 
				.descriptorCount = per_texture + 4 + per_normal_map, // 1 per texture + 1 radiance cubemap + 1 lambertian cubemap + 1 per normal map + 1 ggx + 1 brdf lut
			},
		};

		VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0, // because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, can't free individual descriptors allocated from this pool
			.maxSets = per_texture + 4 + per_normal_map, // 1 per texture + 1 radiance cubemap + 1 lambertian cubemap + 1 per normal map + 1 ggx + 1 brdf lut
			.poolSizeCount = uint32_t(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(), // total number of individual descriptors available, categorized by type   
		};

		VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &texture_descriptor_pool) );
	}

	{ // allocate and write the texture descriptor sets
		VkDescriptorSetAllocateInfo alloc_info {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = texture_descriptor_pool,
			.descriptorSetCount = 1, // one per texture
			.pSetLayouts = &objects_pipeline.set2_TEXTURE,
		};
		texture_descriptors.assign(textures.size(), VK_NULL_HANDLE);

		for (size_t i = 0; i < textures.size(); i++) {
			if (textures[i].format == cubemap_format) continue; // no set2 descriptor for the cubemap
			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &texture_descriptors[i]) );
		}

		// write descriptors for textures:
		std::vector< VkDescriptorImageInfo > infos(texture_views.size());
		std::vector< VkWriteDescriptorSet > writes(texture_views.size());

		size_t view_i = 0;
		for (size_t tex_i = 0; tex_i < textures.size(); tex_i++) {
			if (textures[tex_i].format == cubemap_format) continue; // skip cubemap; //A2-env-TODO: maybe I should not save the cubemap material at the same place as the other texture_views... it's causing too much problem

			infos[view_i] = VkDescriptorImageInfo{
				.sampler = texture_sampler,// how to sample (filtering, wrapping, etc.) 
				.imageView = texture_views[view_i], // which texture image to sample from; view_i tracks position in texture_views
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // expected layout during shader access
			};
			writes[view_i] = VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = texture_descriptors[tex_i], // which descriptor set to update; tex_i is the textures index (matches inst.texture)
				.dstBinding = 0,// binding index within that set (matches layout)
				.dstArrayElement = 0,// starting array index (for arrayed bindings) 
				.descriptorCount = 1,// updating 1 descriptor
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // matches with the descriptor type in descriptor set layout (set2_TEXTURE) 
				.pImageInfo = &infos[view_i],
			};
			view_i++;
		}

		vkUpdateDescriptorSets(
			rtg.device,
			uint32_t(writes.size()), // descriptorWrites count
			writes.data(), // descriptorWrites; can I use &writes here //vv No. &writes references to the vector, writes.data() references to the first elem
			0, nullptr // descriptorCopies count, data - what are these //vv specifies that we are updating the descriptor sets by writing new data into it instead of copying one set to another 
		);
	}

	if (lambertian_cubemap_view != VK_NULL_HANDLE) { // allocate and write the lambertian cubemap descriptor set (set=4) 
		VkDescriptorSetAllocateInfo alloc_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = texture_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &objects_pipeline.set4_LambertianCubeMap,
		};
		VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &lambertian_cubemap_descriptors) );

		VkDescriptorImageInfo image_info{
			.sampler = texture_sampler,
			.imageView = lambertian_cubemap_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		std::array<VkWriteDescriptorSet, 1> writes{
			VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = lambertian_cubemap_descriptors,
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_info,
			},
		};
		vkUpdateDescriptorSets(rtg.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
	}

	if (cubemap_view != VK_NULL_HANDLE) { // allocate and write the cubemap descriptor sets
		VkDescriptorSetAllocateInfo alloc_info {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = texture_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &objects_pipeline.set3_CubeMap,
		};
		VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &cubemap_descriptors) );

		// write descriptors for cube map:
		VkDescriptorImageInfo cubemap_image_info{
			.sampler = texture_sampler, //which sampler to use//??								 // how to sample (filtering, wrapping, etc.)
			.imageView = cubemap_view,							 // which texture image to sample from
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // expected layout during shader access
		};

		// describe the write operation:
		std::array<VkWriteDescriptorSet, 1> writes{
			VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = cubemap_descriptors, // which descriptor set to update      
				.dstBinding = 0, // binding index within that set (matches layout)
				.dstArrayElement = 0, // starting array index (for arrayed bindings) 
				.descriptorCount = 1, // updating 1 descriptor  
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // matches with the descriptor type in descriptor set layout (set3_CubeMap) 
				.pImageInfo = &cubemap_image_info,
			},
		};

		vkUpdateDescriptorSets(
			rtg.device,
			uint32_t(writes.size()), // descriptorWrites count
			writes.data(), // descriptorWrites	
			0, nullptr // descriptorCopies count, data - what are these //vv specifies that we are updating the descriptor sets by writing new data into it instead of copying one set to another 
		);
	}

	{ // A2-normal: allocate and write the normal map descriptor set
		VkDescriptorSetAllocateInfo alloc_info {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = texture_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &objects_pipeline.set5_NormalMap,
		};
		normal_map_descriptors.assign(normal_maps.size(), VK_NULL_HANDLE); // .resize() preserves the existing elements; .assign() discards all existign elements
		for (size_t i = 0; i < normal_maps.size(); i++) {
			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &normal_map_descriptors[i]) );
		}

		// write descriptors:
		std::vector< VkDescriptorImageInfo > infos(normal_map_views.size());
		std::vector< VkWriteDescriptorSet > writes(normal_map_views.size());

		size_t view_i = 0;
		for (size_t tex_i = 0; tex_i < normal_maps.size(); tex_i++) {
			infos[view_i] = VkDescriptorImageInfo{
				.sampler = texture_sampler,// how to sample (filtering, wrapping, etc.) 
				.imageView = normal_map_views[view_i], // which normal_map image to sample from; view_i tracks position in normal_map_views
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // expected layout during shader access
			};
			writes[view_i] = VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = normal_map_descriptors[tex_i], // which descriptor set to update; tex_i is the normal_maps index (matches inst.normal_map)
				.dstBinding = 0,// binding index within that set (matches layout)
				.dstArrayElement = 0,// starting array index (for arrayed bindings) 
				.descriptorCount = 1,// updating 1 descriptor
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // matches with the descriptor type in descriptor set layout (set2_TEXTURE) 
				.pImageInfo = &infos[view_i],
			};
			view_i++;
		}

		vkUpdateDescriptorSets(
			rtg.device,
			uint32_t(writes.size()), writes.data(), // descriptorWrites count, descriptorWrites
			0, nullptr // descriptorCopies count, data - what are these //vv specifies that we are updating the descriptor sets by writing new data into it instead of copying one set to another 
		);
	}

	{ // Final project: upload Nubis VDB cloud channels as sampled 3D textures.
		if (!s72.clouds.empty()) {
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

			VkDescriptorSetLayoutCreateInfo layout_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = uint32_t(bindings.size()),
				.pBindings = bindings.data(),
			};
			VK(vkCreateDescriptorSetLayout(rtg.device, &layout_info, nullptr, &cloud_descriptor_set_layout));

			VkSamplerCreateInfo sampler_info{
				.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.magFilter = VK_FILTER_LINEAR,
				.minFilter = VK_FILTER_LINEAR,
				.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
				.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
				.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
				.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
				.mipLodBias = 0.0f,
				.anisotropyEnable = VK_FALSE,
				.maxAnisotropy = 1.0f,
				.compareEnable = VK_FALSE,
				.compareOp = VK_COMPARE_OP_ALWAYS,
				.minLod = 0.0f,
				.maxLod = 0.0f,
				.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
				.unnormalizedCoordinates = VK_FALSE,
			};
			VK(vkCreateSampler(rtg.device, &sampler_info, nullptr, &cloud_sampler));

			VkDescriptorPoolSize pool_size{
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = uint32_t(s72.clouds.size() * 3),
			};
			VkDescriptorPoolCreateInfo pool_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = uint32_t(s72.clouds.size()),
				.poolSizeCount = 1,
				.pPoolSizes = &pool_size,
			};
			VK(vkCreateDescriptorPool(rtg.device, &pool_info, nullptr, &cloud_descriptor_pool));

			auto upload_cloud_channel = [&](S72::Cloud const &cloud, char const *channel_name, S72::Cloud::GridData const &grid) {
				if (grid.nx <= 0 || grid.ny <= 0 || grid.nz <= 0 || grid.values.empty()) {
					throw std::runtime_error("Cloud \"" + cloud.name + "\" channel \"" + channel_name + "\" has no loaded voxel data.");
				}

				CloudChannelTexture texture;
				texture.image = rtg.helpers.create_image_3d(
					VkExtent3D{
						.width = uint32_t(grid.nx),
						.height = uint32_t(grid.ny),
						.depth = uint32_t(grid.nz),
					},
					VK_FORMAT_R32_SFLOAT,
					VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					Helpers::Unmapped
				);
				rtg.helpers.transfer_to_image(
					grid.values.data(),
					grid.values.size() * sizeof(grid.values[0]),
					texture.image
				);

				VkImageViewCreateInfo view_info{
					.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					.image = texture.image.handle,
					.viewType = VK_IMAGE_VIEW_TYPE_3D,
					.format = texture.image.format,
					.subresourceRange{
						.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
						.baseMipLevel = 0,
						.levelCount = 1,
						.baseArrayLayer = 0,
						.layerCount = 1,
					},
				};
				VK(vkCreateImageView(rtg.device, &view_info, nullptr, &texture.view));
				return texture;
			};

			cloud_textures.reserve(s72.clouds.size());
			for (auto &[name, cloud] : s72.clouds) {
				uint32_t cloud_index = uint32_t(cloud_textures.size());
				cloud_index_map[&cloud] = cloud_index;

				CloudData gpu_cloud;
				gpu_cloud.dimensional_profile = upload_cloud_channel(cloud, "dimensional_profile", cloud.dimensional_profile);
				gpu_cloud.detail_type = upload_cloud_channel(cloud, "detail_type", cloud.detail_type);
				gpu_cloud.density_scale = upload_cloud_channel(cloud, "density_scale", cloud.density_scale);

				VkDescriptorSetAllocateInfo alloc_info{
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
					.descriptorPool = cloud_descriptor_pool,
					.descriptorSetCount = 1,
					.pSetLayouts = &cloud_descriptor_set_layout,
				};
				VK(vkAllocateDescriptorSets(rtg.device, &alloc_info, &gpu_cloud.descriptors));

				std::array<VkDescriptorImageInfo, 3> image_infos{
					VkDescriptorImageInfo{
						.sampler = cloud_sampler,
						.imageView = gpu_cloud.dimensional_profile.view,
						.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					},
					VkDescriptorImageInfo{
						.sampler = cloud_sampler,
						.imageView = gpu_cloud.detail_type.view,
						.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					},
					VkDescriptorImageInfo{
						.sampler = cloud_sampler,
						.imageView = gpu_cloud.density_scale.view,
						.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					},
				};
				std::array<VkWriteDescriptorSet, 3> writes{};
				for (uint32_t binding = 0; binding < uint32_t(writes.size()); ++binding) {
					writes[binding] = VkWriteDescriptorSet{
						.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						.dstSet = gpu_cloud.descriptors,
						.dstBinding = binding,
						.dstArrayElement = 0,
						.descriptorCount = 1,
						.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
						.pImageInfo = &image_infos[binding],
					};
				}
				vkUpdateDescriptorSets(rtg.device, uint32_t(writes.size()), writes.data(), 0, nullptr);

				cloud_textures.emplace_back(std::move(gpu_cloud));
				std::cout << "Created GPU cloud textures for: " << name << std::endl;
			}
		}
	}

	{ // A2-pbr: load GGX specular prefiltered mip chain, and create image/view/descriptor
		if (s72.env_radiance_texture != nullptr && !s72.env_radiance_texture->path.empty()) {
			size_t dot_pos = s72.env_radiance_texture->path.rfind('.'); // rfind searches from right to left. returns pos of the last dot
			std::vector<ObjectsPipeline::MipData> mip_data;

			// load mip pngs e.g. X.ggx.N.png until file not found
			for (int mip = 1; ; mip++) {
				std::string ggx_file = s72.env_radiance_texture->path.substr(0, dot_pos) + ".ggx." + std::to_string(mip) + ".png";

				int ggx_w, ggx_h, ggx_channels;
				unsigned char* data = stbi_load(ggx_file.c_str(), &ggx_w, &ggx_h, &ggx_channels, 4);
				if (!data) {
					std::cout << "GGX mip " << mip << "not found at " << ggx_file << ". Stopping at mip=" << (mip - 1) << std::endl;
					break;
				}

				ObjectsPipeline::MipData cur_mip_data;
				int face_h = ggx_h / 6;
				cur_mip_data.face_width = ggx_w;
				cur_mip_data.face_height = face_h;
				cur_mip_data.floats.resize(ggx_w * ggx_h * 4); // 4 channels
				for (int i = 0; i < ggx_w * ggx_h; i++) {
					glm::vec3 rgb = rgbe_to_float(glm::u8vec4(data[i*4], data[i*4+1], data[i*4+2], data[i*4+3]));
					cur_mip_data.floats[i*4+0] = rgb.r;
					cur_mip_data.floats[i*4+1] = rgb.g;
					cur_mip_data.floats[i*4+2] = rgb.b;
					cur_mip_data.floats[i*4+3] = 1.0f;
				}

				stbi_image_free(data);
				mip_data.push_back(cur_mip_data);
				constexpr int MAX_REFLECTION_LOD = 4; // I define it to be 4 in frag shader
				std::cout << "Loaded GGX mip=" << mip << ": " << ggx_w << "x" << face_h << ", roughness=" << (float(mip - 1) / MAX_REFLECTION_LOD) << std::endl;
			}

			ggx_mip_count = static_cast<uint32_t>(mip_data.size());

			if (ggx_mip_count > 0) {
				uint32_t face_w = mip_data[0].face_width;
				uint32_t face_h = mip_data[0].face_height;

				ggx_image = rtg.helpers.create_image(
					VkExtent2D{.width = face_w, .height = face_h},
					cubemap_format,
					VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					Helpers::Unmapped,
					VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
					6,
					ggx_mip_count
				);
				// upload each mip level's 6 faces
				for (uint32_t mip = 0; mip < ggx_mip_count; mip++) {
					ObjectsPipeline::MipData const &cur_mip_data = mip_data[mip];
					rtg.helpers.transfer_to_image(
						cur_mip_data.floats.data(),
						cur_mip_data.floats.size() * sizeof(float),
						ggx_image,
						6,
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						mip
					);
				}

				{ // create cubemap image view for all mip levels
					VkImageViewCreateInfo create_info{
						.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
						.flags = 0,
						.image = ggx_image.handle, // The underlying VkImage handle this view refers to.
						.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
						.format = ggx_image.format, // Use the same format the image was created with (e.g., VK_FORMAT_R8G8B8A8_SRGB).
						// .components sets swizzling and is fine when zero-initialied; Left zero-initialized, which means no channel swizzling — R maps to R, G to G, etc. (identity mapping).
						.subresourceRange{ // Specifies which part of the image to view:
							.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // this is a color image (not depth/stencil).
							.baseMipLevel = 0, .levelCount = ggx_mip_count,
							.baseArrayLayer = 0, .layerCount = 6u, // 6 layers for cubemap
						},
					};
					VK( vkCreateImageView(rtg.device, &create_info, nullptr, &ggx_view) );
				}

				{ // create sampler for ggx
					// explain why create info was set up like this //?? how is it differ from the texture sampler? and why different?
					VkSamplerCreateInfo create_info {
						.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
						.flags = 0,
						.magFilter = VK_FILTER_LINEAR, // Use linear filtering for magnification (smoother)
						.minFilter = VK_FILTER_LINEAR, // Use linear filtering for minification (smoother)
						.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, // ggx needs smooth interpolation, filter_linear results in smoother interp compared to filter_nearest
						.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // should not repeat. //??-A2-pbr
						.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
						.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
						.mipLodBias = 0.0f, // No bias applied when selecting mipmap levels.
						.anisotropyEnable = VK_FALSE, // Anisotropic filtering is disabled. This means maxAnisotropy is ignored.
						.maxAnisotropy = VK_FALSE, // doesn't matter if anisotropy isn't enabled
						.compareEnable = VK_FALSE, // Depth comparison is disabled (used for shadow mapping). So compareOp is ignored.
						.compareOp = VK_COMPARE_OP_ALWAYS, // doesn't matter if compare isn't enabled
						.minLod = 0.0f,
						.maxLod = VK_LOD_CLAMP_NONE,
						.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
						.unnormalizedCoordinates = VK_FALSE, // Texture coordinates are in the standard [0, 1] range rather than pixel coordinates.
					};
					VK( vkCreateSampler(rtg.device, &create_info, nullptr, &ggx_sampler) );
				}

				{ // allocate and write the ggx descriptor sets
					VkDescriptorSetAllocateInfo alloc_info {
						.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
						.descriptorPool = texture_descriptor_pool,
						.descriptorSetCount = 1,
						.pSetLayouts = &objects_pipeline.set6_GGXPrefilteredEnvMap,
					};
					VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &ggx_descriptors) );

					// write descriptors for ggx:
					VkDescriptorImageInfo image_info{
						.sampler = ggx_sampler, // use ggx sampler (mip interpolation, clamp-to-edge)
						.imageView = ggx_view, // which texture image to sample from
						.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // expected layout during shader access
					};

					// describe the write operation:
					std::array<VkWriteDescriptorSet, 1> writes{
						VkWriteDescriptorSet{
							.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
							.dstSet = ggx_descriptors, // which descriptor set to update      
							.dstBinding = 0, // binding index within that set (matches layout)
							.dstArrayElement = 0, // starting array index (for arrayed bindings) 
							.descriptorCount = 1, // updating 1 descriptor  
							.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // matches with the descriptor type in descriptor set layout (set3_CubeMap) 
							.pImageInfo = &image_info,
						},
					};

					vkUpdateDescriptorSets(
						rtg.device,
						uint32_t(writes.size()), // descriptorWrites count
						writes.data(), // descriptorWrites	
						0, nullptr // descriptorCopies count, data; specifies that we are updating the descriptor sets by writing new data into it instead of copying one set to another 
					);
				}

				std::cout << "Loaded GGX specular mip chain: " << ggx_mip_count << " levels, base " << face_w << "x" << face_h << std::endl;
			}
		}
	}

	{ // A2-pbr: load BRDF split-sum LUT from brdf_lut.bin (generate with: ./bin/brdf)
		constexpr uint32_t BRDF_LUT_SIZE = 512; // same as LUT_SIZE in main-brdf.cpp
		constexpr char BRDF_LUT_PATH[] = "brdf_lut.bin";

		std::vector<float> brdf_pixels(BRDF_LUT_SIZE * BRDF_LUT_SIZE * 2);

		{ // load precomputed BRDF LUT from disk — generate with: ./bin/brdf [output.bin]
			std::ifstream fin(BRDF_LUT_PATH, std::ios::binary);
			if (fin.good()) {
				fin.read(reinterpret_cast<char *>(brdf_pixels.data()), brdf_pixels.size() * sizeof(float));
				std::cout << "Loaded BRDF LUT from " << BRDF_LUT_PATH << std::endl;
			} else {
				throw std::runtime_error(std::string(BRDF_LUT_PATH) + " not found. Run './bin/brdf' to precompute it.");
			}
		}

		brdf_lut_image = rtg.helpers.create_image(
			VkExtent2D{.width = BRDF_LUT_SIZE, .height = BRDF_LUT_SIZE},
			VK_FORMAT_R32G32_SFLOAT,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
		);
		rtg.helpers.transfer_to_image(brdf_pixels.data(), brdf_pixels.size() * sizeof(float), brdf_lut_image);

		{ // image view
			VkImageViewCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = brdf_lut_image.handle,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = VK_FORMAT_R32G32_SFLOAT,
				.subresourceRange{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0, .levelCount = 1,
					.baseArrayLayer = 0, .layerCount = 1,
				},
			};
			VK( vkCreateImageView(rtg.device, &create_info, nullptr, &brdf_lut_view) );
		}

		{ // sampler: clamp to edge, no mipmaps (UV range [0,1] encodes NdotV and roughness)
			VkSamplerCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.magFilter = VK_FILTER_LINEAR,
				.minFilter = VK_FILTER_LINEAR,
				.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
				.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.minLod = 0.0f,
				.maxLod = 0.0f,
			};
			VK( vkCreateSampler(rtg.device, &create_info, nullptr, &brdf_lut_sampler) );
		}

		{ // allocate and write descriptor set for BRDF LUT (set=7)
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = texture_descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &objects_pipeline.set7_BRDFLookup,
			};
			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &brdf_lut_descriptors) );

			VkDescriptorImageInfo image_info{
				.sampler = brdf_lut_sampler,
				.imageView = brdf_lut_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			std::array<VkWriteDescriptorSet, 1> writes{
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = brdf_lut_descriptors,
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &image_info,
				},
			};
			vkUpdateDescriptorSets(rtg.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
		}
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
		normal_map_descriptors.clear();
		ggx_descriptors = VK_NULL_HANDLE;
		brdf_lut_descriptors = VK_NULL_HANDLE;
	}

	if (cloud_descriptor_pool) {
		vkDestroyDescriptorPool(rtg.device, cloud_descriptor_pool, nullptr);
		cloud_descriptor_pool = VK_NULL_HANDLE;
		for (CloudData &cloud : cloud_textures) {
			cloud.descriptors = VK_NULL_HANDLE;
		}
	}

	if (cloud_descriptor_set_layout) {
		vkDestroyDescriptorSetLayout(rtg.device, cloud_descriptor_set_layout, nullptr);
		cloud_descriptor_set_layout = VK_NULL_HANDLE;
	}

	if (cloud_sampler) {
		vkDestroySampler(rtg.device, cloud_sampler, nullptr);
		cloud_sampler = VK_NULL_HANDLE;
	}

	auto destroy_cloud_channel = [&](CloudChannelTexture &channel) {
		if (channel.view != VK_NULL_HANDLE) {
			vkDestroyImageView(rtg.device, channel.view, nullptr);
			channel.view = VK_NULL_HANDLE;
		}
		rtg.helpers.destroy_image(std::move(channel.image));
	};

	for (CloudData &cloud : cloud_textures) {
		destroy_cloud_channel(cloud.dimensional_profile);
		destroy_cloud_channel(cloud.detail_type);
		destroy_cloud_channel(cloud.density_scale);
	}
	cloud_textures.clear();
	cloud_index_map.clear();

	if (texture_sampler) {
		vkDestroySampler(rtg.device, texture_sampler, nullptr);
		texture_sampler = VK_NULL_HANDLE; // why do we still need to set it back to null? what happens if we don't //??
	}

	for (VkImageView &view : texture_views) {
		vkDestroyImageView(rtg.device, view, nullptr);
		view = VK_NULL_HANDLE;
	}
	texture_views.clear();

	for (VkImageView &view : normal_map_views) {
      vkDestroyImageView(rtg.device, view, nullptr);
      view = VK_NULL_HANDLE;
  	}
  	normal_map_views.clear();

	if (cubemap_view != VK_NULL_HANDLE) {
		vkDestroyImageView(rtg.device, cubemap_view, nullptr);
		cubemap_view = VK_NULL_HANDLE;
	}

	if (lambertian_cubemap_view != VK_NULL_HANDLE) {
		vkDestroyImageView(rtg.device, lambertian_cubemap_view, nullptr);
		lambertian_cubemap_view = VK_NULL_HANDLE;
	}

	if (ggx_sampler) {
		vkDestroySampler(rtg.device, ggx_sampler, nullptr);
		ggx_sampler = VK_NULL_HANDLE;
	}

	if (ggx_view != VK_NULL_HANDLE) {
		vkDestroyImageView(rtg.device, ggx_view, nullptr);
		ggx_view = VK_NULL_HANDLE;
	}

    rtg.helpers.destroy_image(std::move(ggx_image));

	if (brdf_lut_sampler) {
		vkDestroySampler(rtg.device, brdf_lut_sampler, nullptr);
		brdf_lut_sampler = VK_NULL_HANDLE;
	}

	if (brdf_lut_view != VK_NULL_HANDLE) {
		vkDestroyImageView(rtg.device, brdf_lut_view, nullptr);
		brdf_lut_view = VK_NULL_HANDLE;
	}

	rtg.helpers.destroy_image(std::move(brdf_lut_image));

	if (shadow_sampler != VK_NULL_HANDLE) {
		vkDestroySampler(rtg.device, shadow_sampler, nullptr);
		shadow_sampler = VK_NULL_HANDLE;
	}

	for (auto &texture : textures) {
		rtg.helpers.destroy_image(std::move(texture));
	}
	textures.clear();

	for (auto &image : normal_maps) {
      rtg.helpers.destroy_image(std::move(image));
  	}
  	normal_maps.clear();

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
		if (workspace.Lights.handle != VK_NULL_HANDLE)  {
			rtg.helpers.destroy_buffer(std::move(workspace.Lights));
		}
		if (workspace.Lights_src.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.Lights_src));
		}
		// TransformsLights_descriptors freed when pool is destroyed

		if (workspace.shadow_image_view != VK_NULL_HANDLE) {
			vkDestroyImageView(rtg.device, workspace.shadow_image_view, nullptr);
			workspace.shadow_image_view = VK_NULL_HANDLE;
		}
		rtg.helpers.destroy_image(std::move(workspace.shadow_image));

		for (VkFramebuffer &framebuffer : workspace.shadow_framebuffers) {
			assert(framebuffer != VK_NULL_HANDLE);
			vkDestroyFramebuffer(rtg.device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
		workspace.shadow_framebuffers.clear();

		for (VkImageView &view : workspace.shadow_views) {
			assert(view != VK_NULL_HANDLE);
			vkDestroyImageView(rtg.device, view, nullptr);
			view = VK_NULL_HANDLE;
		}
		workspace.shadow_views.clear();
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
	shadow_pipeline.destroy(rtg);

	if (shadow_render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(rtg.device, shadow_render_pass, nullptr);
		shadow_render_pass = VK_NULL_HANDLE;
	}

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


// Credit: adapted from More (Robust) Frustum Culling by Bruno Opsenica
static bool SAT_visibility_test(const Tutorial::CullingFrustum& frustum, const mat4& VIEW_FROM_LOCAL, const S72::vec3& bmin, const S72::vec3& bmax)
{
    // Near, far
    float z_near = frustum.near_plane;
    float z_far = frustum.far_plane;
    // half width, half height
    float x_near = frustum.near_right;
    float y_near = frustum.near_top;

    // Consider four adjacent corners of the bounding box
	vec3 corners[] = {
		{bmin.x, bmin.y, bmin.z},
		{bmax.x, bmin.y, bmin.z},
		{bmin.x, bmax.y, bmin.z},
		{bmin.x, bmin.y, bmax.z},
	};

	// Transform corners
    // Note: I think this approach is only sufficient if our transform is non-shearing (affine)
    for (size_t corner_idx = 0; corner_idx < 4; corner_idx++) {
        corners[corner_idx] = VIEW_FROM_LOCAL * corners[corner_idx]; // transfer to camera space (view space)
    }

	// Compute the 3 edge vectors (axes)
    // Use transformed corners to calculate center, axes and extents
    Tutorial::OBB obb = {
        .axes = {
            corners[1] - corners[0], // edge along x-axis of the box
            corners[2] - corners[0], // edge along y-axis of the box
            corners[3] - corners[0] // edge along z-axis of the box
        },
    };
    obb.center = corners[0] + 0.5f * (obb.axes[0] + obb.axes[1] + obb.axes[2]); // center of the box is the first corner plus half of the sum of the edge vectors
    obb.extents = vec3{ length(obb.axes[0]), length(obb.axes[1]), length(obb.axes[2]) };
    obb.axes[0] = obb.axes[0] / obb.extents[0]; // normalize length = 1, so the axes represent direction only; the extents represent the length along that direction
    obb.axes[1] = obb.axes[1] / obb.extents[1];
    obb.axes[2] = obb.axes[2] / obb.extents[2];
    obb.extents *= 0.5f;

	// start testing for each of the 26 separating axes
	{	
		// "M" is the separating axis (a unit vector) being tested
		vec3 M = { 0.0f, 0.0f, 1.0f }; // the axis along which we're projecting the OBB; the +z of camera space (note that the frustum is looking down -z)
		[[maybe_unused]] float MoX = 0.0f; // | m . x | (abs of M dot x-axis of the box, i.e. how much the box extends along the M axis on its x-axis; projection of the box's x-axis onto M)
		[[maybe_unused]] float MoY = 0.0f; // | m . y |
		[[maybe_unused]] float MoZ = M[2]; // m . z (not abs!)

		// Projected center of our OBB
		float MoC = obb.center[2]; // M dot center; Since M is (0, 0, 1), the projection of the center onto M is just the z component of the center in camera space
		// Projected size of OBB
		float radius = 0.0f;
		for (size_t i = 0; i < 3; i++) { // loop through the three axes of the box
			// dot(M, axes[i]) == axes[i].z;
			// computes the half-width of the OBB's projection onto the Z-axis.
			radius += fabsf(obb.axes[i][2]) * obb.extents[i]; // radius += |axis_i * M| * extent_i; (extent_i is the half-length along that axis, and |axis_i * M| is how much that axis contributes to the projection onto M. Use abs value because projection might be negative if the axis is pointed away from M)
		}
		float obb_min = MoC - radius;
		float obb_max = MoC + radius;
		// We can skip calculating the projection here, it's known
		float m0 = z_far; // Since the frustum's direction is negative z, far is smaller than near
		float m1 = z_near;

		if (obb_min > m1 || obb_max < m0) {
			return false;
		}
	}

	{ // Frustum normals
		const vec3 M[] = { // normals//??
			{ 0.0, -z_near, y_near }, // Top plane
			{ 0.0, z_near, y_near }, // Bottom plane
			{ -z_near, 0.0f, x_near }, // Right plane
			{ z_near, 0.0f, x_near }, // Left Plane
		};
		for (size_t m = 0; m < 4; m++) { // loop through the 4 frustum normals
			float MoX = fabsf(M[m][0]);
			float MoY = fabsf(M[m][1]);
			float MoZ = M[m][2];
			float MoC = dot(M[m], obb.center); // projection of the box center onto the normal

			float obb_radius = 0.0f;
			for (size_t i = 0; i < 3; i++) {
				obb_radius += fabsf(dot(M[m], obb.axes[i])) * obb.extents[i];
			}
			float obb_min = MoC - obb_radius;
			float obb_max = MoC + obb_radius;

			// compute the frustum interval along this axis //??
			float p = x_near * MoX + y_near * MoY;

			float tau_0 = z_near * MoZ - p;
			float tau_1 = z_near * MoZ + p;

			if (tau_0 < 0.0f) {
				tau_0 *= z_far / z_near;
			}
			if (tau_1 > 0.0f) {
				tau_1 *= z_far / z_near;
			}

			if (obb_min > tau_1 || obb_max < tau_0) {
				return false;
			}
		}
	}
	return true;
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
		if (needed_bytes > 0) {
			vkCmdCopyBuffer(workspace.command_buffer, workspace.lines_vertices_src.handle, workspace.lines_vertices.handle, 1, &copy_region);
		}
	}

	{ // upload camera info:
		// SceneCamera = storage format kept in CPU; LinesPipeline::Camera = the GPU/shader format that gets uploaded
		// because The shader is written to read CLIP_FROM_WORLD at offset 0 //TODO: do we need the shader to read more?
		LinesPipeline::Camera camera{
			.CLIP_FROM_WORLD = CLIP_FROM_WORLD};

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

	// object_instances.clear(); // used for CPU bottleneck testing;
	if (!object_instances.empty()) {
		{ // upload object transforms
			//[re-]allocate object buffers if needed:
			size_t needed_bytes = object_instances.size() * sizeof(object_instances[0]);
			if (workspace.Transforms_src.handle == VK_NULL_HANDLE || workspace.Transforms_src.size < needed_bytes) { // if the source buffer is missing or too small
				size_t new_bytes = ((needed_bytes + 4096) / 4096) * 4096; //round to next multiple of 4k to avoid re-allocating continuously if vertex count grows slowly
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
						.dstSet = workspace.TransformsLights_descriptors, // which descriptor set to update  
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

		assert(workspace.Transforms_src.size == workspace.Transforms.size);
		assert(workspace.Transforms_src.size >= needed_bytes);

			{ //copy transforms into Transforms_src: use the CPU to copy from the transforms to the workspace.Transforms_src staging buffer
				assert(workspace.Transforms_src.allocation.mapped);
				ObjectsPipeline::Transform *out = reinterpret_cast< ObjectsPipeline::Transform* >(workspace.Transforms_src.allocation.data()); // struct aliasing violation, but it doesn't matter
				for (ObjectInstance const &inst : object_instances) {
					*out = inst.transform;
					++out;
				}
			}
			// device-side copy from Transforms_src -> Transforms:
			// record a command to have the GPU copy the data from the staging buffer to the workspace.lines_vertices buffer.
			VkBufferCopy copy_region{
				.srcOffset = 0,
				.dstOffset = 0,
				.size = needed_bytes,
			};
			if (needed_bytes > 0) {
				vkCmdCopyBuffer(workspace.command_buffer, workspace.Transforms_src.handle, workspace.Transforms.handle, 1, &copy_region);
			}
		}

		{ // upload lights
			//[re-]allocate buffers if needed:
			size_t needed_bytes = light_instances.size() * sizeof(ObjectsPipeline::LightData);
			if (workspace.Lights_src.handle == VK_NULL_HANDLE || workspace.Lights_src.size < needed_bytes) { // if the source buffer is missing or too small
				size_t new_bytes = ((needed_bytes + 4096) / 4096) * 4096; //round to next multiple of 4k to avoid re-allocating continuously if vertex count grows slowly
				if (workspace.Lights_src.handle) {
					rtg.helpers.destroy_buffer(std::move(workspace.Lights_src));
				}
				if (workspace.Lights.handle) {
					rtg.helpers.destroy_buffer(std::move(workspace.Lights));
				}

				workspace.Lights_src = rtg.helpers.create_buffer(
					new_bytes, 
					VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // going to have GPU copy from this memory
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, // host-visible memory (the memory can be mapped from the CPU side), coherent (no special sync needed) (the memory doesn't require special flush operations to make host writes available)
					Helpers::Mapped // get a pointer to the memory
				);
				workspace.Lights = rtg.helpers.create_buffer(
					new_bytes,
					VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as vertex buffer, also going to have GPU into this memory i.e. the target if memory copy
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // GPU-local memory
					Helpers::Unmapped // don't get a pointer to memory
				);

				// update the descriptor set: Tells Vulkan "the Lights shader binding should point to this specific buffer."
				// It connects your GPU buffer to the shader's descriptor. 
				// Describe the buffer:
				VkDescriptorBufferInfo Lights_info{ 
					.buffer = workspace.Lights.handle, // which buffer
					.offset = 0, // start at beginning
					.range = workspace.Lights.size, // use whole buffer
				};

				// describe the write operation:
				std::array< VkWriteDescriptorSet, 1 > writes{
					VkWriteDescriptorSet{
						.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  
						.dstSet = workspace.TransformsLights_descriptors, // which descriptor set to update  
						.dstBinding = 1, // binding 1 in TransformsLights_descriptors, 0 is object transforms
						.dstArrayElement = 0, // first element (if it were an array)
						.descriptorCount = 1,  // updating 1 descriptor
						.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
						.pBufferInfo = &Lights_info, 
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

			assert(workspace.Lights_src.size == workspace.Lights.size);
			assert(workspace.Lights_src.size >= needed_bytes);

			{ // copy Lights into Lights_src: use the CPU to copy from the Lights to the workspace.Lights_src staging buffer
				// Populate Lights_src (CPU side)
				assert(workspace.Lights_src.allocation.mapped); // make sure that we have a CPU pointer to the buffer memory already
				ObjectsPipeline::LightData *out = reinterpret_cast< ObjectsPipeline::LightData* >(workspace.Lights_src.allocation.data()); // convert the memory address to Transform*, so that we can write into it using the Transform's size; struct aliasing violation, but it doesn't matter
				for (Light const &inst : light_instances) {
					// world-space position: 
					/* mat4 is column-major
					             col0  col1  col2  col3                                                                                                                          
						row 0  [ [0]   [4]   [8]   [12] ]                                                                                                                        
						row 1  [ [1]   [5]   [9]   [13] ]                                                                                                                        
						row 2  [ [2]   [6]   [10]  [14] ]                                                                                                                        
						row 3  [ [3]   [7]   [11]  [15] ]  
					so mat[col=3][row=0] -> mat[3*4+0]
					*/
					out->position[0] = inst.WORLD_FROM_LOCAL[3 * 4 + 0]; //A3-materials-TODO review math
					out->position[1] = inst.WORLD_FROM_LOCAL[3 * 4 + 1];
					out->position[2] = inst.WORLD_FROM_LOCAL[3 * 4 + 2];

					// world-space forward direction: //A3-materials-TODO review math
					// lights are "pointing along the local $-z$ axis" (by S72 spec), so forward = (0,0,-1)
					out->direction[0] = -inst.WORLD_FROM_LOCAL[2 * 4 + 0];
					out->direction[1] = -inst.WORLD_FROM_LOCAL[2 * 4 + 1];
					out->direction[2] = -inst.WORLD_FROM_LOCAL[2 * 4 + 2];
					
					out->tint[0] = inst.light->tint.r;
					out->tint[1] = inst.light->tint.g;
					out->tint[2] = inst.light->tint.b;
					// out->shadow = inst.light->shadow; // A3-materials-TODO: need to add shadow to struct and here

					if (std::holds_alternative<S72::Light::Sun>(inst.light->source)) { // returns true/false checks if a std::variant currently holds a specific alternative type
						out->type = ObjectsPipeline::LightType::Sun;
						auto const &sun = std::get<S72::Light::Sun>(inst.light->source); // get_if returns a pointer to the value if it holds the type T, or nullptr if not
						out->angle = sun.angle;
						out->strength = sun.strength;
					} else if (std::holds_alternative<S72::Light::Sphere>(inst.light->source)) {
						out->type = ObjectsPipeline::LightType::Sphere;
						auto const &sphere = std::get<S72::Light::Sphere>(inst.light->source); // get_if returns a pointer to the value if it holds the type T, or nullptr if not
						out->radius = sphere.radius;
						out->power = sphere.power;
						out->limit = sphere.limit;
					} else if (std::holds_alternative<S72::Light::Spot>(inst.light->source)) {
						out->type = ObjectsPipeline::LightType::Spot;
						auto const &spot = std::get<S72::Light::Spot>(inst.light->source); // get_if returns a pointer to the value if it holds the type T, or nullptr if not
						out->radius = spot.radius;
						out->power = spot.power;
						out->limit = spot.limit;
						out->fov = spot.fov;
						out->blend = spot.blend;
						auto shadow_it = shadow_light_index_map.find(inst.light); // A3-shadows
						if (shadow_it != shadow_light_index_map.end()) {
							out->shadow_i = int32_t(shadow_it->second);
							out->shadow_map_size = float(shadow_resolution);
							mat4 LOCAL_FROM_WORLD = inverse(inst.WORLD_FROM_LOCAL);
							float far = (std::isinf(spot.limit) || spot.limit <= 0.0f) ? SHADOW_DEFAULT_FAR_LIMIT : spot.limit;
							mat4 CLIP_FROM_WORLD = perspective(spot.fov, 1.0f, 0.1f, far) * LOCAL_FROM_WORLD;
							for (int i = 0; i < 16; i++) out->CLIP_FROM_WORLD[i] = CLIP_FROM_WORLD[i];
						}
					}

					++out; // move the pointer to the next ObjectsPipeline::LightData-sized chunk of memory
				}
			}

			// device-side copy from Lights_src (CPU)-> Lights (GPU)
			// record a command to have the GPU copy the data from the staging buffer to the workspace.lines_vertices buffer.
			VkBufferCopy copy_region{
				.srcOffset = 0,
				.dstOffset = 0,
				.size = needed_bytes,
			};
			if (needed_bytes > 0) {
				vkCmdCopyBuffer(workspace.command_buffer, workspace.Lights_src.handle, workspace.Lights.handle, 1, &copy_region);
			}
		}
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

	// A3-shadows, render shadow maps for each spot light with shadows
	if (shadow_count > 0 && !object_instances.empty()) { // draw with the shadow pipeline
		vkCmdBindPipeline(workspace.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline.handle);

		{ // bind object vertex buffer
			std::array<VkBuffer, 1> vertex_buffers{ object_vertices.handle };
			std::array<VkDeviceSize, 1> offsets{ 0 };
			vkCmdBindVertexBuffers(
				workspace.command_buffer, 
				0, // first binding; this corresponds to the "binding = 0" in the vertex shader's input definitions (VkVertexInputAttributeDescription from PosNorTexVertex.cpp)
				1, // binding count
				vertex_buffers.data(), 
				offsets.data()
			);
		}

		for (Light const &inst : light_instances) {
			if (!std::holds_alternative<S72::Light::Spot>(inst.light->source)) continue;
			auto const &spot = std::get<S72::Light::Spot>(inst.light->source);
			auto shadow_it = shadow_light_index_map.find(inst.light);
			if (shadow_it == shadow_light_index_map.end()) continue;
			uint32_t shadow_i = shadow_it->second;

			// put GPU commands here
			// render pass:
			VkClearValue clear_values{ .depthStencil{ .depth = 1.0f, .stencil = 0 } };
			VkRenderPassBeginInfo begin_info{
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass = shadow_render_pass,
				.framebuffer = workspace.shadow_framebuffers[shadow_i],
				.renderArea{ .offset = { 0, 0 }, .extent = { shadow_resolution, shadow_resolution } },
				.clearValueCount = 1,
				.pClearValues = &clear_values,
			};
			vkCmdBeginRenderPass(workspace.command_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

			// run pipelines here:
			{ // set scissor rectangle:
				VkRect2D shadow_scissor{ .offset = { 0, 0 }, .extent = { shadow_resolution, shadow_resolution } };
				vkCmdSetScissor(workspace.command_buffer, 0, 1, &shadow_scissor);
			}

			{ // configure viewport transform: 
				VkViewport shadow_viewport{
					.x = 0.0f, .y = 0.0f,
					.width = float(shadow_resolution), .height = float(shadow_resolution),
					.minDepth = 0.0f, .maxDepth = 1.0f,
				};
				vkCmdSetViewport(workspace.command_buffer, 0, 1, &shadow_viewport);
			}

			// CLIP_FROM_WORLD for this spot light: perspective * inv(WORLD_FROM_LOCAL)
			mat4 LOCAL_FROM_WORLD = inverse(inst.WORLD_FROM_LOCAL);
			float far = (std::isinf(spot.limit) || spot.limit <= 0.0f) ? SHADOW_DEFAULT_FAR_LIMIT : spot.limit;
			mat4 CLIP_FROM_WORLD = perspective(spot.fov, 1.0f, 0.1f, far) * LOCAL_FROM_WORLD;

			for (ObjectInstance const &obj : object_instances) {
				ShadowPipeline::Push push{
					.CLIP_FROM_LOCAL = CLIP_FROM_WORLD * obj.transform.WORLD_FROM_LOCAL,
				};
				vkCmdPushConstants(workspace.command_buffer, shadow_pipeline.layout,
					VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
				vkCmdDraw(workspace.command_buffer, obj.mesh->count, 1, obj.mesh->first_vertex, 0);
			}

			vkCmdEndRenderPass(workspace.command_buffer);
		}
	}

	// put GPU commands here
	//render pass:
	std::array< VkClearValue, 2 > clear_values{
		// VkClearValue{ .color{ .float32{ 0.54, 0.35, 0.80, 1.0f } } }, // light purple; 
		VkClearValue{ .color{ .float32{ 0,0,0, 1.0f } } },
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
	// Calculate viewport dimensions, handling letterbox/pillarbox for scene cameras
	float viewport_x = 0.0f;
	float viewport_y = 0.0f;
	float viewport_width = float(rtg.swapchain_extent.width);
	float viewport_height = float(rtg.swapchain_extent.height);

	if (camera_mode == CameraMode::Scene && !scene_camera_instances.empty()) {
		SceneCamera const &cam = scene_camera_instances[active_scene_camera];
		S72::Camera::Perspective& projection = std::get<S72::Camera::Perspective>(cam.camera->projection);

		float camera_aspect = projection.aspect;
		float window_aspect = float(rtg.swapchain_extent.width) / float(rtg.swapchain_extent.height);

		if (window_aspect > camera_aspect) {
			// Window is too wide -> pillarbox (black bars on left/right)
			viewport_width = viewport_height * camera_aspect;
			viewport_x = (float(rtg.swapchain_extent.width) - viewport_width) * 0.5f;
		} else if (window_aspect < camera_aspect) {
			// Window is too narrow -> letterbox (black bars on top/bottom)
			viewport_height = viewport_width / camera_aspect;
			viewport_y = (float(rtg.swapchain_extent.height) - viewport_height) * 0.5f;
		}
		// If aspects match exactly, no adjustment needed
	}

	{ // set scissor rectangle:
		VkRect2D scissor{
			.offset = {.x = int32_t(viewport_x), .y = int32_t(viewport_y)},
			.extent = {.width = uint32_t(viewport_width), .height = uint32_t(viewport_height)},
		};
		vkCmdSetScissor(workspace.command_buffer, 0, 1, &scissor);
	}
	{ // configure viewport transform:
		VkViewport viewport{
			.x = viewport_x,
			.y = viewport_y,
			.width = viewport_width,
			.height = viewport_height,
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

	if (!lines_vertices.empty()) { // draw with the lines pipeline:
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
		
		{// use object vertices (offset 0) as vertex buffer binding 0: // what does offset 0. and vertex buffer binding mean //vv The shader expects vertex data at binding 0. When you call vkCmdBindVertexBuffers(..., 0, ...), you're saying "attach this buffer to binding 0." 
			std::array< VkBuffer, 1 > vertex_buffers{ object_vertices.handle };
			std::array< VkDeviceSize, 1 > offsets{ 0 }; // tells Where in that buffer the data starts 
			vkCmdBindVertexBuffers(
				workspace.command_buffer,
				0, // first binding; this corresponds to the "binding = 0" in the vertex shader's input definitions (VkVertexInputAttributeDescription from PosNorTexVertex.cpp)
				uint32_t(vertex_buffers.size()),
				vertex_buffers.data(),
				offsets.data()
			);
		}

		{ // bind World and Transforms descriptor set:
			std::array< VkDescriptorSet, 2 > descriptor_sets{
				workspace.World_descriptors, // 0: World
				workspace.TransformsLights_descriptors, // 1: Transforms & Lights
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

		// bind cubemap descriptor set. cubemap_descriptors is always valid because
		// the fallback 1x1 black cubemap ensures it's created even with no scene environment
		vkCmdBindDescriptorSets(
			workspace.command_buffer, // command buffer
			VK_PIPELINE_BIND_POINT_GRAPHICS, // pipeline bind point
			objects_pipeline.layout, // pipeline layout
			3, // set=3: radiance cubemap (mirror / environment material)
			1, &cubemap_descriptors, // descriptor sets count, ptr (which descriptor set to put in slot 3)
			0, nullptr // dynamic offsets count, ptr
		);
		// bind lambertian cubemap at slot 4; BUG: only bound when view != NULL. but frag shader always expects lambertianCubeMap
		// if (lambertian_cubemap_view != VK_NULL_HANDLE) {
		{ // FIX: always bind set 4, fallback to cubemap_descriptors if no lambertian exists (i.e. either no environment obj at all, or the env obj only has a radiance map, no lambertian)
			VkDescriptorSet final_lambertian_cubemap_descriptors = (lambertian_cubemap_view != VK_NULL_HANDLE)
												  ? lambertian_cubemap_descriptors
												  : cubemap_descriptors;
			vkCmdBindDescriptorSets(
				workspace.command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				objects_pipeline.layout,
				4, // set=4: prefiltered irradiance cubemap (lambertian material)
				1, &final_lambertian_cubemap_descriptors,
				0, nullptr // dynamic offsets count, ptr
			);
		}

		{ // bind ggx mip map at set=6
			VkDescriptorSet final_ggx_descriptors = (ggx_descriptors != VK_NULL_HANDLE)
												  ? ggx_descriptors
												  : cubemap_descriptors;
			vkCmdBindDescriptorSets(
				workspace.command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				objects_pipeline.layout,
				6, // set=6: ggx prefiltered mip map
				1, &final_ggx_descriptors,
				0, nullptr // dynamic offsets count, ptr
			);
		}

		if (brdf_lut_descriptors != VK_NULL_HANDLE) {
			vkCmdBindDescriptorSets(
				workspace.command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				objects_pipeline.layout,
				7, // set=7: BRDF split-sum LUT
				1, &brdf_lut_descriptors,
				0, nullptr
			);
		}

		// draw all instances:
		for (ObjectInstance const &inst : object_instances) {
			{ // push material_type:
				ObjectsPipeline::Push push{
					.material_type = inst.material_type,
					.exposure = rtg.configuration.exposure,
					.tone_map_push = objects_pipeline.tone_map,
					.roughness = inst.roughness,
					.metalness = inst.metalness,
					.lights_count = int(light_instances.size()),
				};
				vkCmdPushConstants(workspace.command_buffer, objects_pipeline.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			}

			if (culling_mode == CullingMode::Frustum){
				// Get local-space bounding box corners
				S72::vec3 const &bmin = inst.mesh->bbox_min;
				S72::vec3 const &bmax = inst.mesh->bbox_max;

				/* takes in:
				1. the view matrix of the camera; Transforms points from world space into camera (view) space.
				2. the model/world transform of object; Converts points from model space into world space.
				*/
        		mat4 VIEW_FROM_LOCAL = CAMERA_FROM_WORLD * inst.transform.WORLD_FROM_LOCAL;

				if (!SAT_visibility_test(frustum, VIEW_FROM_LOCAL, bmin, bmax)) {
					continue; // skip this instance if it's not visible
				}
			}

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

			// bind normal map at slot 5
			S72::Material *material = inst.mesh->material;
			VkDescriptorSet final_normal_map_descriptors = normal_map_descriptors[0];
			if (material != nullptr && material->normal_map != nullptr) {
				auto normal_it = normal_index_map.find(material->normal_map);
				if (normal_it != normal_index_map.end()) {
					final_normal_map_descriptors = normal_map_descriptors[normal_it->second];
				}
			}

			vkCmdBindDescriptorSets(
				workspace.command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				objects_pipeline.layout,
				5, // set=5
				1, &final_normal_map_descriptors,
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

// evaluate driver's state at animation_time and write into driver.node.translation/rotation/scale based on driver.values
void Tutorial::evaluate_driver(S72::Driver& driver, float time) {
	if (driver.times.empty()) return;

	// find keyframe interval
	if (time <= driver.times[0]) {
		// before first keyframe, set to first keyframe value
		// set the animated value to driver.values start at 0
		// The values in the values array are grouped into 1D-4D vectors depending on the channel type and interpolation scheme. 
		// For example, a 3D channel with n times will have 3n values, which should be considered as n 3-vectors.
		size_t i = 0;
		if (driver.channel == S72::Driver::Channel::translation) {
			driver.node.translation = S72::vec3{
				.x = driver.values[i + 0],
				.y = driver.values[i + 1],
				.z = driver.values[i + 2]};
		}
		else if (driver.channel == S72::Driver::Channel::rotation) {
			driver.node.rotation = S72::quat{
				.x = driver.values[i + 0],
				.y = driver.values[i + 1],
				.z = driver.values[i + 2],
				.w = driver.values[i + 3]};
		}
		else if (driver.channel == S72::Driver::Channel::scale) {
			driver.node.scale = S72::vec3{
				.x = driver.values[i + 0],
				.y = driver.values[i + 1],
				.z = driver.values[i + 2]};
		}
	} else if (time > driver.times.back()) {
		// after last keyframe, set to last keyframe value
		// set the animated value to driver.values.back()
		size_t stride = (driver.channel == S72::Driver::Channel::rotation) ? 4 : 3;                                                                                                                           
  		size_t i = driver.values.size() - stride;    
		if (driver.channel == S72::Driver::Channel::translation) {
			driver.node.translation = S72::vec3{
				.x = driver.values[i + 0],
				.y = driver.values[i + 1],
				.z = driver.values[i + 2]     
			};
		} else if (driver.channel == S72::Driver::Channel::rotation) {
			driver.node.rotation = S72::quat{   
				.x = driver.values[i + 0],
				.y = driver.values[i + 1],
				.z = driver.values[i + 2],
				.w = driver.values[i + 3] 
			}; 
		} else if (driver.channel == S72::Driver::Channel::scale) {
			driver.node.scale = S72::vec3{
				.x = driver.values[i + 0],
				.y = driver.values[i + 1],
				.z = driver.values[i + 2]   
			};
		}
	} else {
		// time is between keyframes, find the interval [times[i], times[i+1]] that contains animation_time
		size_t i = 0;
		while (i < driver.times.size() && driver.times[i] <= time) {
			i++;
		}
		assert(0 < i && i < driver.times.size()); // because time > driver.times[0] and time < driver.times.back()

		if (driver.interpolation == S72::Driver::Interpolation::STEP) {
			// the output value in the middle of a time interval is the value at the beginning of that interval.
			size_t start_i = (i - 1) * (driver.channel == S72::Driver::Channel::rotation ? 4 : 3);
			if (driver.channel == S72::Driver::Channel::translation) {
				driver.node.translation = S72::vec3{
					.x = driver.values[start_i + 0],
					.y = driver.values[start_i + 1],
					.z = driver.values[start_i + 2]     
				};
			} else if (driver.channel == S72::Driver::Channel::rotation) {
				driver.node.rotation = S72::quat{   
					.x = driver.values[start_i + 0],
					.y = driver.values[start_i + 1],
					.z = driver.values[start_i + 2],
					.w = driver.values[start_i + 3] 
				};
			} else if (driver.channel == S72::Driver::Channel::scale) {
				driver.node.scale = S72::vec3{
					.x = driver.values[start_i + 0],
					.y = driver.values[start_i + 1],
					.z = driver.values[start_i + 2]   
				};
			}
		} else if (driver.interpolation == S72::Driver::Interpolation::LINEAR) {
			// the output value in the middle of a time interval is a linear mix of the starting and ending values.
			size_t start_i = (i - 1) * (driver.channel == S72::Driver::Channel::rotation ? 4 : 3);
			size_t end_i = i * (driver.channel == S72::Driver::Channel::rotation ? 4 : 3);

			float t = (time - driver.times[i-1]) / (driver.times[i] - driver.times[i-1]); // normalized time in [0, 1]
			if (driver.channel == S72::Driver::Channel::translation) {
				driver.node.translation = S72::vec3{
					.x = (1.0f - t) * driver.values[start_i + 0] + t * driver.values[end_i + 0],
					.y = (1.0f - t) * driver.values[start_i + 1] + t * driver.values[end_i + 1],
					.z = (1.0f - t) * driver.values[start_i + 2] + t * driver.values[end_i + 2],    
				};
			} else if (driver.channel == S72::Driver::Channel::rotation) {
				driver.node.rotation = S72::quat{   
					.x = (1.0f - t) * driver.values[start_i + 0] + t * driver.values[end_i + 0],
					.y = (1.0f - t) * driver.values[start_i + 1] + t * driver.values[end_i + 1],
					.z = (1.0f - t) * driver.values[start_i + 2] + t * driver.values[end_i + 2],
					.w = (1.0f - t) * driver.values[start_i + 3] + t * driver.values[end_i + 3], 
				}; // TODO: check - would this linear mix work for rotation?
			} else if (driver.channel == S72::Driver::Channel::scale) {
				driver.node.scale = S72::vec3{
					.x = (1.0f - t) * driver.values[start_i + 0] + t * driver.values[end_i + 0],
					.y = (1.0f - t) * driver.values[start_i + 1] + t * driver.values[end_i + 1],
					.z = (1.0f - t) * driver.values[start_i + 2] + t * driver.values[end_i + 2],   
				};
			}
		} else if (driver.interpolation == S72::Driver::Interpolation::SLERP) {
			// TODO: the output value in the middle of a time interval is a "spherical linear interpolation" between the starting and ending values. 
			// (Doesn't make sense for 1D signals or non-normalized signals.)
			size_t start_i = (i - 1) * (driver.channel == S72::Driver::Channel::rotation ? 4 : 3);
			size_t end_i = i * (driver.channel == S72::Driver::Channel::rotation ? 4 : 3);

			float t = (time - driver.times[i-1]) / (driver.times[i] - driver.times[i-1]); // normalized time in [0, 1]
			if (driver.channel == S72::Driver::Channel::translation) {
				driver.node.translation = S72::vec3{
					.x = (1.0f - t) * driver.values[start_i + 0] + t * driver.values[end_i + 0],
					.y = (1.0f - t) * driver.values[start_i + 1] + t * driver.values[end_i + 1],
					.z = (1.0f - t) * driver.values[start_i + 2] + t * driver.values[end_i + 2],    
				};
			} else if (driver.channel == S72::Driver::Channel::rotation) {
				glm::quat q_start{ driver.values[start_i + 0], driver.values[start_i + 1], driver.values[start_i + 2], driver.values[start_i + 3] };
				glm::quat q_end{ driver.values[end_i + 0], driver.values[end_i + 1], driver.values[end_i + 2], driver.values[end_i + 3] };
				glm::quat q_interp = glm::slerp(q_start, q_end, t);
				driver.node.rotation = S72::quat{   
					.x = q_interp.x,
					.y = q_interp.y,
					.z = q_interp.z,
					.w = q_interp.w, 
				};
			} else if (driver.channel == S72::Driver::Channel::scale) {
				driver.node.scale = S72::vec3{
					.x = (1.0f - t) * driver.values[start_i + 0] + t * driver.values[end_i + 0],
					.y = (1.0f - t) * driver.values[start_i + 1] + t * driver.values[end_i + 1],
					.z = (1.0f - t) * driver.values[start_i + 2] + t * driver.values[end_i + 2],   
				};
			}
		}
	}
}

void Tutorial::update(float dt) {
	// FPS tracking //A1-test-TODO: enable them when specified --debug
	// fps_accumulator += dt;
	// fps_frame_count++;
	// if (fps_accumulator >= 1.0f) {
	// 	float fps = fps_frame_count / fps_accumulator;
	// 	float ms_per_frame = (fps_accumulator / fps_frame_count) * 1000.0f;
	// 	std::cout << "FPS: " << fps << " (" << ms_per_frame << " ms/frame)" << std::endl;
	// 	fps_accumulator = 0.0f;
	// 	fps_frame_count = 0;
	// }

	time  = std::fmod(time + dt, 60.0f);
	if (animation_playing) {
		if (!rtg.configuration.headless) {
			animation_time += dt; // measure the elapsed time from the first frame
		} else {
			// TODO: in headless mode, using the times from the AVAILABLE events.
		}

		for (S72::Driver& driver : s72.drivers) {
			evaluate_driver(driver, animation_time);
		}
	}

	auto push_edge = [&](vec3 a, vec3 b,
						 uint8_t ar, uint8_t ag, uint8_t ab, uint8_t aa,
						 uint8_t br, uint8_t bg, uint8_t bb, uint8_t ba) {
		lines_vertices.emplace_back(PosColVertex{
			.Position{.x = a[0], .y = a[1], .z = a[2]},
			.Color{.r = ar, .g = ag, .b = ab, .a = aa},
		});
		lines_vertices.emplace_back(PosColVertex{
			.Position{.x = b[0], .y = b[1], .z = b[2]},
			.Color{.r = br, .g = bg, .b = bb, .a = ba},
		});
	};

	{ // add each s72 mesh to object_instances (previously create some objects: sphere surrounded by rotating torus)
		// TODO: think about - can we move this chunk outside of update? is it necessary to re-traverse the tree and re-create object instances every frame?
		object_instances.clear();
		scene_camera_instances.clear();
		light_instances.clear();

		// 1. traverse the scene graph from root; "roots" is an optional array of references to nodes at which to start drawing the scene.

		// 2. Building Local Transform from node's TRS (Translation, Rotation Scale: local = Translation × Rotation × Scale      
		// Where: Translation = mat4 with (tx, ty, tz) in last column; Rotation = quaternion (x,y,z,w) → 3x3 rotation matrix; Scale = diagonal mat4 with (sx, sy, sz, 1)  

		// TODO: clean up
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

				// Determine texture index from material
				uint32_t tex_index = 0; // default white texture
				if (node->mesh->material != nullptr) {
					auto it = material_albedo_map.find(node->mesh->material);
					if (it != material_albedo_map.end()) {
						tex_index = it->second;
					}
				}

				ObjectsPipeline::MaterialType material_type = ObjectsPipeline::MaterialType::Lambertian;
				float inst_roughness = 0.5f, inst_metalness = 0.0f;
				if (node->mesh->material != nullptr) {
					if (std::holds_alternative<S72::Material::PBR>(node->mesh->material->brdf)) { // returns true/false checks if a std::variant currently holds a specific alternative type
						material_type = ObjectsPipeline::MaterialType::PBR;
						auto const &pbr = std::get<S72::Material::PBR>(node->mesh->material->brdf); // get_if returns a pointer to the value if it holds the type T, or nullptr if not
						if (auto *roughness = std::get_if<float>(&pbr.roughness)) inst_roughness = *roughness;
						if (auto *metalness = std::get_if<float>(&pbr.metalness)) inst_metalness = *metalness;
					} else if (std::holds_alternative<S72::Material::Lambertian>(node->mesh->material->brdf)) {
						material_type = ObjectsPipeline::MaterialType::Lambertian;
					} else if (std::holds_alternative<S72::Material::Mirror>(node->mesh->material->brdf)) {
						material_type = ObjectsPipeline::MaterialType::Mirror;
					} else if (std::holds_alternative<S72::Material::Environment>(node->mesh->material->brdf)) {
						material_type = ObjectsPipeline::MaterialType::Environment;
					}
				}

				object_instances.emplace_back(ObjectInstance{
					.mesh = node->mesh,
					.transform = tf,
					.texture = tex_index,
					.material_type = material_type,
					.roughness = inst_roughness,
					.metalness = inst_metalness,
				});
			}

			if (node->camera != nullptr) {
				scene_camera_instances.emplace_back(SceneCamera{
					.camera = node->camera,
					.WORLD_FROM_LOCAL = world,
				});
			}

			if (node->light != nullptr) {
				light_instances.emplace_back(Light{
					.light = node->light,
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
	}

	lines_vertices.clear();
	if (camera_mode == CameraMode::Scene) {
		// the rendering happens through one of the cameras in the scene graph and the user cannot change the camera transformation
		if (scene_camera_instances.empty()) {
			camera_mode = CameraMode::User; // switch to user camera if no cameras in scene
		} else {
			SceneCamera const &camera = scene_camera_instances[active_scene_camera];
			S72::Camera::Perspective& projection = std::get<S72::Camera::Perspective>(camera.camera->projection); //vv need to use this instead of camera.camera->projection; because the original type is a variant

			// CLIP_FROM_WORLD = perpective_projection_matrix * view;
			// View = inverse of camera's world transform //??
			CLIP_FROM_WORLD = perspective(
				projection.vfov,
				projection.aspect,
				projection.near,
				projection.far
			) * inverse(
				camera.WORLD_FROM_LOCAL
			);

			float tan_fov = tan(projection.vfov * 0.5f); //??
			frustum = {
				.near_right = projection.aspect * projection.near * tan_fov,
				.near_top = projection.near * tan_fov,
				.near_plane = -projection.near,
				.far_plane = -projection.far,
			};

			// p_world = WORLD_FROM_LOCAL * p_camera
			// p_camera = CAMERA_FROM_WORLD * p_world
			CAMERA_FROM_WORLD = inverse(camera.WORLD_FROM_LOCAL);
			CLIP_FROM_WORLD_CULLING = CLIP_FROM_WORLD;

			world.EYE.x = camera.WORLD_FROM_LOCAL[12]; // we only need the translation col for EYE //??
			world.EYE.y = camera.WORLD_FROM_LOCAL[13];
			world.EYE.z = camera.WORLD_FROM_LOCAL[14];
		}
	} else if (camera_mode == CameraMode::User) { //??
		CAMERA_FROM_WORLD = orbit(
			free_camera.target_x, free_camera.target_y, free_camera.target_z,
			free_camera.azimuth, free_camera.elevation, free_camera.radius
		);

		float aspect = rtg.swapchain_extent.width / float(rtg.swapchain_extent.height);
		CLIP_FROM_WORLD = perspective(
			free_camera.fov,
			aspect,
			free_camera.near,
			free_camera.far
		) * CAMERA_FROM_WORLD;

		float tan_fov = tan(free_camera.fov * 0.5f);
		frustum = {
			.near_right = aspect * free_camera.near * tan_fov,
			.near_top = free_camera.near * tan_fov,
			.near_plane = -free_camera.near,
			.far_plane = -free_camera.far,
		};

		CLIP_FROM_WORLD_CULLING = CLIP_FROM_WORLD;

		mat4 WORLD_FROM_LOCAL = inverse(CAMERA_FROM_WORLD);
		world.EYE.x = WORLD_FROM_LOCAL[12];
		world.EYE.y = WORLD_FROM_LOCAL[13];
		world.EYE.z = WORLD_FROM_LOCAL[14];
	} else if (camera_mode == CameraMode::Debug)
	{
		// the rendering happens through a second user-controlled camera
		CAMERA_FROM_WORLD = orbit(
			debug_camera.target_x, debug_camera.target_y, debug_camera.target_z,
			debug_camera.azimuth, debug_camera.elevation, debug_camera.radius
		);

		CLIP_FROM_WORLD = perspective(
			debug_camera.fov,
			rtg.swapchain_extent.width / float(rtg.swapchain_extent.height), //aspect
			debug_camera.near,
			debug_camera.far
		) * CAMERA_FROM_WORLD;

		mat4 WORLD_FROM_LOCAL = inverse(CAMERA_FROM_WORLD);
		world.EYE.x = WORLD_FROM_LOCAL[12];
		world.EYE.y = WORLD_FROM_LOCAL[13];
		world.EYE.z = WORLD_FROM_LOCAL[14];

		// the culling happens for the previously-active camera (this is very useful for debugging culling). When using the debug  camera, your renderer should display object bounding boxes and camera frustums using lines.
		// Draw frustum for the culling camera (previously-active camera)
		mat4 WORLD_FROM_CLIP = inverse(CLIP_FROM_WORLD_CULLING);

		auto clip_to_world = [&](float cx, float cy, float cz) -> vec3 {
			vec4 clip = {cx, cy, cz, 1.0f};
			vec4 world = WORLD_FROM_CLIP * clip;
			return vec3{world[0]/world[3], world[1]/world[3], world[2]/world[3]}; // perspective divide
		};

		// 8 corners in clip space (z from 0 to 1)
		vec3 nbl = clip_to_world(-1, -1, 0); // near bottom-left
		vec3 nbr = clip_to_world(+1, -1, 0); // near bottom-right
		vec3 ntr = clip_to_world(+1, +1, 0); // near top-right
		vec3 ntl = clip_to_world(-1, +1, 0); // near top-left
		vec3 fbl = clip_to_world(-1, -1, 1); // far bottom-left
		vec3 fbr = clip_to_world(+1, -1, 1); // far bottom-right
		vec3 ftr = clip_to_world(+1, +1, 1); // far top-right
		vec3 ftl = clip_to_world(-1, +1, 1); // far top-left

		// Near plane (4 edges)
		push_edge(nbl, nbr, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(nbr, ntr, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(ntr, ntl, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(ntl, nbl, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		// Far plane (4 edges)
		push_edge(fbl, fbr, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(fbr, ftr, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(ftr, ftl, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(ftl, fbl, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		// Connecting edges (4 edges)
		push_edge(nbl, fbl, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(nbr, fbr, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(ntr, ftr, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);
		push_edge(ntl, ftl, 0xff,0xff,0x00,0xff, 0xff,0xff,0x00,0xff);

		// Draw bounding boxes for each object instance
		for (ObjectInstance const &inst : object_instances) {
			if (inst.mesh == nullptr) continue;

			// Get local-space bounding box corners
			S72::vec3 const &bmin = inst.mesh->bbox_min;
			S72::vec3 const &bmax = inst.mesh->bbox_max;

			auto local_to_world = [&](float lx, float ly, float lz) -> vec3 {
				vec4 local = {lx, ly, lz, 1.0f};
				vec4 world_pt = inst.transform.WORLD_FROM_LOCAL * local;
				return vec3{world_pt[0], world_pt[1], world_pt[2]};
			};

			// 8 corners of the bounding box in world space
			vec3 c000 = local_to_world(bmin.x, bmin.y, bmin.z);
			vec3 c001 = local_to_world(bmin.x, bmin.y, bmax.z);
			vec3 c010 = local_to_world(bmin.x, bmax.y, bmin.z);
			vec3 c011 = local_to_world(bmin.x, bmax.y, bmax.z);
			vec3 c100 = local_to_world(bmax.x, bmin.y, bmin.z);
			vec3 c101 = local_to_world(bmax.x, bmin.y, bmax.z);
			vec3 c110 = local_to_world(bmax.x, bmax.y, bmin.z);
			vec3 c111 = local_to_world(bmax.x, bmax.y, bmax.z);

			// Draw 12 edges of the bounding box (cyan color)
			// Bottom face (z = min)
			push_edge(c000, c100, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c100, c110, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c110, c010, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c010, c000, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			// Top face (z = max)
			push_edge(c001, c101, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c101, c111, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c111, c011, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c011, c001, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			// Vertical edges connecting bottom and top faces
			push_edge(c000, c001, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c100, c101, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c110, c111, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
			push_edge(c010, c011, 0x00,0xff,0xff,0xff, 0x00,0xff,0xff,0xff);
		}
	}
	else
	{
		assert(0 && "only three camera modes");
	}

	{ // static sun and sky
		// pointing straight up along the Z-axis
		world.SKY_DIRECTION.x = 0.0f;
		world.SKY_DIRECTION.y = 0.0f;
		world.SKY_DIRECTION.z = 1.0f;

		// dim, slightly blue
		world.SKY_ENERGY.r = 0.1f;
		world.SKY_ENERGY.g = 0.1f;
		world.SKY_ENERGY.b = 0.2f;

		// Direction: (6/23, 13/23, 18/23) ≈ (0.26, 0.57, 0.78) - roughly up and to the side
		world.SUN_DIRECTION.x = 6.0f / 23.0f;
		world.SUN_DIRECTION.y = 13.0f / 23.0f;
		world.SUN_DIRECTION.z = 18.0f / 23.0f;

		// almost bright white
		world.SUN_ENERGY.r = 1.0f;
		world.SUN_ENERGY.g = 1.0f;
		world.SUN_ENERGY.b = 0.9f;
	}
}


void Tutorial::on_input(InputEvent const &evt) {
	if (action) { //vv //if there is a current action, it gets input priority
		action(evt);
		return;
	}

	// general controls:
	// switch camera modes by number keys
	if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_1 && camera_mode != CameraMode::Scene) {
		// camera_mode = CameraMode((int(camera_mode) + 1) % 3); // 3 camera modes: Scene, User, Debug
		camera_mode = CameraMode::Scene;
		std::cout << "Camera mode: " << (camera_mode == CameraMode::Scene ? "Scene" : camera_mode == CameraMode::User ? "User" : "Debug") << std::endl;
		if (camera_mode == CameraMode::Scene && !scene_camera_instances.empty()) {
			std::cout << "Active scene camera(" << int(active_scene_camera) << "): " << scene_camera_instances[active_scene_camera].camera->name << std::endl;
		}
		return; // returns since we don't want any later event handling code to be allowed to respond to the tab key
	}
	if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_2 && camera_mode != CameraMode::User) {
		camera_mode = CameraMode::User;
		std::cout << "Camera mode: " << (camera_mode == CameraMode::Scene ? "Scene" : camera_mode == CameraMode::User ? "User" : "Debug") << std::endl;
		return;
	}
	if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_3 && camera_mode != CameraMode::Debug) {
		camera_mode = CameraMode::Debug;
		std::cout << "Camera mode: " << (camera_mode == CameraMode::Scene ? "Scene" : camera_mode == CameraMode::User ? "User" : "Debug") << std::endl;
		return;
	}

	// animation controls:
	if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_P) {
		animation_playing = !animation_playing;
		std::cout << "Animation " << (animation_playing ? "playing" : "paused") << std::endl;
		return;
	}
	if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_R) {
		animation_time = 0.0f;
		std::cout << "Animation restarted" << std::endl;
		return;
	}

	// scene camera controls:
	if (camera_mode == CameraMode::Scene) {
		if (evt.type == InputEvent::KeyDown && evt.key.key == GLFW_KEY_SPACE) {
			active_scene_camera = (int(active_scene_camera) + 1) % scene_camera_instances.size(); // change between scene cameras
			std::cout << "Active scene camera(" << int(active_scene_camera) << "): " << scene_camera_instances[active_scene_camera].camera->name << std::endl;
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

			// capture variables [this,init_x,init_y,init_camera] from outer scope to inside this lambda function
			action = [this,init_x,init_y,init_camera](InputEvent const &evt) {
				if (evt.type == InputEvent::MouseButtonUp && evt.button.button == GLFW_MOUSE_BUTTON_LEFT) {
					//cancel upon button lifted:
					action = nullptr;
					return;
				}
				if (evt.type == InputEvent::MouseMotion) {
					// handle motion: // review/understand: //??
					// computing the camera's left and up directions and offsetting based on the mouse's distance travelled

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

			return; // if not return, will continue to the next statement (tumbling)
		}

		if (evt.type == InputEvent::MouseButtonDown && evt.button.button == GLFW_MOUSE_BUTTON_LEFT) {
			//start tumbling // what is tumbling //vv it's just rotating the camera around the target point

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

	if (camera_mode == CameraMode::Debug) {
		if (evt.type == InputEvent::MouseWheel) {
			// change distance by 10% every scroll click:
			debug_camera.radius *= std::exp(std::log(1.1f) * -evt.wheel.y);
			// make sure camera isn't too close or too far from target:
			debug_camera.radius = std::max(debug_camera.radius, 0.5f * debug_camera.near); // it's kinda like setting the min and max spirng arm length in UE?
			debug_camera.radius = std::min(debug_camera.radius, 2.0f * debug_camera.far);
			return;
		}

		if (evt.type == InputEvent::MouseButtonDown && evt.button.button == GLFW_MOUSE_BUTTON_LEFT && (evt.button.mods & GLFW_MOD_SHIFT)) {
			//start panning
			float init_x = evt.button.x;
			float init_y = evt.button.y;
			OrbitCamera init_camera = debug_camera;

			// capture variables [this,init_x,init_y,init_camera] from outer scope to inside this lambda function
			action = [this,init_x,init_y,init_camera](InputEvent const &evt) {
				if (evt.type == InputEvent::MouseButtonUp && evt.button.button == GLFW_MOUSE_BUTTON_LEFT) {
					//cancel upon button lifted:
					action = nullptr;
					return;
				}
				if (evt.type == InputEvent::MouseMotion) {
					// handle motion: // review/understand: //??
					// computing the camera's left and up directions and offsetting based on the mouse's distance travelled

					//image height at plane of target point:
					float height = 2.0f * std::tan(debug_camera.fov * 0.5f) * debug_camera.radius;

					//motion, therefore, at target point:
					float dx = (evt.motion.x - init_x) / rtg.swapchain_extent.height * height;
					float dy =-(evt.motion.y - init_y) / rtg.swapchain_extent.height * height; //note: negated because glfw uses y-down coordinate system

					//compute camera transform to extract right (first row) and up (second row):
					mat4 camera_from_world = orbit(
						init_camera.target_x, init_camera.target_y, init_camera.target_z,
						init_camera.azimuth, init_camera.elevation, init_camera.radius
					);

					//move the desired distance:
					debug_camera.target_x = init_camera.target_x - dx * camera_from_world[0] - dy * camera_from_world[1];
					debug_camera.target_y = init_camera.target_y - dx * camera_from_world[4] - dy * camera_from_world[5];
					debug_camera.target_z = init_camera.target_z - dx * camera_from_world[8] - dy * camera_from_world[9];
					return;
				}
			};

			return; // if not return, will continue to the next statement (tumbling)
		}

		if (evt.type == InputEvent::MouseButtonDown && evt.button.button == GLFW_MOUSE_BUTTON_LEFT) {
			//start tumbling // what is tumbling //vv it's just rotating the camera around the target point

			// std::cout << "Tumble started." << std::endl;
			float init_x = evt.button.x;
			float init_y = evt.button.y;
			OrbitCamera init_camera = debug_camera;
			
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
					debug_camera.azimuth = init_camera.azimuth - dx * speed * flip_x;
					debug_camera.elevation = init_camera.elevation - dy * speed;

					//reduce azimuth and elevation to [-pi,pi] range:
					const float twopi = 2.0f * float(M_PI);
					debug_camera.azimuth -= std::round(debug_camera.azimuth / twopi) * twopi;
					debug_camera.elevation -= std::round(debug_camera.elevation / twopi) * twopi;
					return;
				}
			};

			return;
		}
	}
}
