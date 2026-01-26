#include "Tutorial.hpp"

#include "VK.hpp"
#include "refsol.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include <random>
#include <algorithm>

struct Vec3 {
    float x, y, z;
};

Tutorial::Tutorial(RTG &rtg_) : rtg(rtg_) {
	refsol::Tutorial_constructor(rtg, &depth_format, &render_pass, &command_pool);

	background_pipeline.create(rtg, render_pass, 0);
	lines_pipeline.create(rtg, render_pass, 0);
	objects_pipeline.create(rtg, render_pass, 0);

	{ // create descriptor tool:
		uint32_t per_workspace = uint32_t(rtg.workspaces.size()); // for easier-to-read counting

		std::array< VkDescriptorPoolSize, 2 > pool_sizes{
			VkDescriptorPoolSize{ // uniform buffer descriptors
				.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 2 * per_workspace, // one descriptor per set, two set per workspace
			},
			VkDescriptorPoolSize{ // uniform buffer descriptors
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1 * per_workspace, // one descriptor per set, one set per workspace
			},
		};

		VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0, // because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, can't free individual descriptors allocated from this pool
			.maxSets = 2 * per_workspace, // two sets per workspace (for uniform buffer and storage buffer)
			.poolSizeCount = uint32_t(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(),
		};

		VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &descriptor_pool) );
	}

	workspaces.resize(rtg.workspaces.size());
	for (Workspace &workspace : workspaces) {
		refsol::Tutorial_constructor_workspace(rtg, command_pool, &workspace.command_buffer);

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
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			Helpers::Mapped
		);
		workspace.World = rtg.helpers.create_buffer(
			sizeof(ObjectsPipeline::World),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			Helpers::Unmapped
		);

		{ //allocate descriptor set for World descriptor
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &objects_pipeline.set0_World,
			};

			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.World_descriptors) );
			//NOTE: will actually fill in this descriptor set just a bit lower
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
					.dstSet = workspace.Camera_descriptors,
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.pBufferInfo = &Camera_info,
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

	{ //create object vertices
		std::vector< PosNorTexVertex > vertices;

		{ // //A [-1,1]x[-1,1]x{0} quadrilateral:
			plane_vertices.first = uint32_t(vertices.size());
			vertices.emplace_back(PosNorTexVertex{
				.Position{ .x = -1.0f, .y = -1.0f, .z = 0.0f },
				.Normal{ .x = 0.0f, .y = 0.0f, .z = 1.0f },
				.TexCoord{ .s = 0.0f, .t = 0.0f },
			});
			vertices.emplace_back(PosNorTexVertex{
				.Position{ .x = 1.0f, .y = -1.0f, .z = 0.0f },
				.Normal{ .x = 0.0f, .y = 0.0f, .z = 1.0f},
				.TexCoord{ .s = 1.0f, .t = 0.0f },
			});
			vertices.emplace_back(PosNorTexVertex{
				.Position{ .x = -1.0f, .y = 1.0f, .z = 0.0f },
				.Normal{ .x = 0.0f, .y = 0.0f, .z = 1.0f},
				.TexCoord{ .s = 0.0f, .t = 1.0f },
			});
			vertices.emplace_back(PosNorTexVertex{
				.Position{ .x = 1.0f, .y = 1.0f, .z = 0.0f },
				.Normal{ .x = 0.0f, .y = 0.0f, .z = 1.0f },
				.TexCoord{ .s = 1.0f, .t = 1.0f },
			});
			vertices.emplace_back(PosNorTexVertex{
				.Position{ .x = -1.0f, .y = 1.0f, .z = 0.0f },
				.Normal{ .x = 0.0f, .y = 0.0f, .z = 1.0f},
				.TexCoord{ .s = 0.0f, .t = 1.0f },
			});
			vertices.emplace_back(PosNorTexVertex{
				.Position{ .x = 1.0f, .y = -1.0f, .z = 0.0f },
				.Normal{ .x = 0.0f, .y = 0.0f, .z = 1.0f},
				.TexCoord{ .s = 1.0f, .t = 0.0f },
			});

			plane_vertices.count = uint32_t(vertices.size() - plane_vertices.first);
		}

		{ // A torus:
			torus_vertices.first = uint32_t(vertices.size());

			// will parameterize with (u, v) where:
			// - u is angle around main axis (+z)
			// -v is angle around the tube

			constexpr float R1 = 0.75f; // main radius
			constexpr float R2 = 0.15f; // tube radius

			constexpr uint32_t U_STEPS = 20;
			constexpr uint32_t V_STEPS = 16;

			// texture repeats around the torus:
			constexpr float V_REPEATS = 2.0f;
			constexpr float U_REPEATS = int(V_REPEATS / R2 * R1 + 0.999f); // approximately square, rounded up

			auto emplace_vertex = [&](uint32_t ui, uint32_t vi)
			{
				// convert steps to angles:
				//  (doing the mod since trig on 2 M_PI may not exactly match 0)
				float ua = (ui % U_STEPS) / float(U_STEPS) * 2.0f * float(M_PI);
				float va = (vi % V_STEPS) / float(V_STEPS) * 2.0f * float(M_PI);

				vertices.emplace_back(PosNorTexVertex{
					.Position{
						.x = (R1 + R2 * std::cos(va)) * std::cos(ua),
						.y = (R1 + R2 * std::cos(va)) * std::sin(ua),
						.z = R2 * std::sin(va),
					},
					.Normal{
						.x = std::cos(va) * std::cos(ua),
						.y = std::cos(va) * std::sin(ua),
						.z = std::sin(va),
					},
					.TexCoord{
						.s = ui / float(U_STEPS) * U_REPEATS,
						.t = vi / float(V_STEPS) * V_REPEATS,
					},
				});
			};

			for (uint32_t ui = 0; ui < U_STEPS; ++ui)
			{
				for (uint32_t vi = 0; vi < V_STEPS; ++vi)
				{
					emplace_vertex(ui, vi);
					emplace_vertex(ui + 1, vi);
					emplace_vertex(ui, vi + 1);

					emplace_vertex(ui, vi + 1);
					emplace_vertex(ui + 1, vi);
					emplace_vertex(ui + 1, vi + 1);
				}
			}

			torus_vertices.count = uint32_t(vertices.size() - torus_vertices.first);
		}

		size_t bytes = vertices.size() * sizeof(vertices[0]);

		object_vertices = rtg.helpers.create_buffer(
			bytes,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as vertex buffer, also going to have GPU copy into this memory
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // GPU-local memory
			Helpers::Unmapped // don't get a pointer to memory
		);

		// copy data to buffer
		// notice: this uploads the data during initialization instead of during the per-frame rendering loop (our rendering function Tutorial::render())// foreshadow!
		rtg.helpers.transfer_to_buffer(vertices.data(), bytes, object_vertices);

		{ //make some textures
			textures.reserve(2);

			{ //texture 0 will be a dark grey / light grey checkerboard with a red square at the origin.
				//actually make the texture:
				uint32_t size = 128;
				std::vector< uint32_t > data;
				data.reserve(size * size);
				for (uint32_t y = 0; y < size; ++y) {
					float fy = (y + 0.5f) / float(size);
					for (uint32_t x = 0; x < size; ++x) {
						float fx = (x + 0.5f) / float(size);
						//highlight the origin:
						if      (fx < 0.05f && fy < 0.05f) data.emplace_back(0xff0000ff); //red
						else if ( (fx < 0.5f) == (fy < 0.5f)) data.emplace_back(0xff444444); //dark grey
						else data.emplace_back(0xffbbbbbb); //light grey
					}
				}
				assert(data.size() == size*size);

				//make a place for the texture to live on the GPU:
				textures.emplace_back(rtg.helpers.create_image(
					VkExtent2D{ .width = size , .height = size }, //size of image
					VK_FORMAT_R8G8B8A8_UNORM, //how to interpret image data (in this case, linearly-encoded 8-bit RGBA)
					VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, //will sample and upload
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //should be device-local
					Helpers::Unmapped
				));

				//transfer data:
				rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), textures.back());
			}

			{ //texture 1 will be a classic 'xor' texture:
				//actually make the texture:
				uint32_t size = 256;
				std::vector< uint32_t > data;
				data.reserve(size * size);
				for (uint32_t y = 0; y < size; ++y) {
					for (uint32_t x = 0; x < size; ++x) {
						uint8_t r = uint8_t(x) ^ uint8_t(y);
						uint8_t g = uint8_t(x + 128) ^ uint8_t(y);
						uint8_t b = uint8_t(x) ^ uint8_t(y + 27);
						uint8_t a = 0xff;
						data.emplace_back( uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24) );
					}
				}
				assert(data.size() == size*size);

				//make a place for the texture to live on the GPU:
				textures.emplace_back(rtg.helpers.create_image(
					VkExtent2D{ .width = size , .height = size }, //size of image
					VK_FORMAT_R8G8B8A8_SRGB, //how to interpret image data (in this case, SRGB-encoded 8-bit RGBA)
					VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, //will sample and upload
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //should be device-local
					Helpers::Unmapped
				));

				//transfer data:
				rtg.helpers.transfer_to_image(data.data(), sizeof(data[0]) * data.size(), textures.back());
			}
		}

		{ //make image views for the textures
			texture_views.reserve(textures.size());
			for (Helpers::AllocatedImage const &image : textures) {
				VkImageViewCreateInfo create_info{
					.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					.flags = 0,
					.image = image.handle,
					.viewType = VK_IMAGE_VIEW_TYPE_2D,
					.format = image.format,
					// .components sets swizzling and is fine when zero-initialized
					.subresourceRange{
						.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
						.baseMipLevel = 0,
						.levelCount = 1,
						.baseArrayLayer = 0,
						.layerCount = 1,
					},
				};

				VkImageView image_view = VK_NULL_HANDLE;
				VK( vkCreateImageView(rtg.device, &create_info, nullptr, &image_view) );

				texture_views.emplace_back(image_view);
			}
			assert(texture_views.size() == textures.size());
		}

		{ // make a sampler for the textures
			VkSamplerCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.flags = 0,
				.magFilter = VK_FILTER_NEAREST,
				.minFilter = VK_FILTER_NEAREST,
				.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
				.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
				.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
				.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
				.mipLodBias = 0.0f,
				.anisotropyEnable = VK_FALSE,
				.maxAnisotropy = 0.0f, //doesn't matter if anisotropy isn't enabled
				.compareEnable = VK_FALSE,
				.compareOp = VK_COMPARE_OP_ALWAYS, //doesn't matter if compare isn't enabled
				.minLod = 0.0f,
				.maxLod = 0.0f,
				.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
				.unnormalizedCoordinates = VK_FALSE,
			};
			VK( vkCreateSampler(rtg.device, &create_info, nullptr, &texture_sampler) );
		}
			
		{ // create the texture descriptor pool
			uint32_t per_texture = uint32_t(textures.size()); //for easier-to-read counting

			std::array< VkDescriptorPoolSize, 1> pool_sizes{
				VkDescriptorPoolSize{
					.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1 * 1 * per_texture, //one descriptor per set, one set per texture
				},
			};
			
			VkDescriptorPoolCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.flags = 0, //because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, *can't* free individual descriptors allocated from this pool
				.maxSets = 1 * per_texture, //one set per texture
				.poolSizeCount = uint32_t(pool_sizes.size()),
				.pPoolSizes = pool_sizes.data(),
			};

			VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &texture_descriptor_pool) );
		}

		{ //allocate and write the texture descriptor sets

			//allocate the descriptors (using the same alloc_info):
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = texture_descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &objects_pipeline.set2_TEXTURE,
			};
			texture_descriptors.assign(textures.size(), VK_NULL_HANDLE);
			for (VkDescriptorSet &descriptor_set : texture_descriptors) {
				VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &descriptor_set) );
			}

			//write descriptors for textures:
			std::vector< VkDescriptorImageInfo > infos(textures.size());
			std::vector< VkWriteDescriptorSet > writes(textures.size());

			for (Helpers::AllocatedImage const &image : textures) {
				size_t i = &image - &textures[0];
				
				infos[i] = VkDescriptorImageInfo{
					.sampler = texture_sampler,
					.imageView = texture_views[i],
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
				writes[i] = VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = texture_descriptors[i],
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &infos[i],
				};
			}

			vkUpdateDescriptorSets( rtg.device, uint32_t(writes.size()), writes.data(), 0, nullptr );
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

		//this also frees the descriptor sets allocated from the pool:
		texture_descriptors.clear();
	}

	if (texture_sampler) {
		vkDestroySampler(rtg.device, texture_sampler, nullptr);
		texture_sampler = VK_NULL_HANDLE;
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

	rtg.helpers.destroy_buffer(std::move(object_vertices)); // why don't we need to check whether it != NULL before destroying it //??

	if (swapchain_depth_image.handle != VK_NULL_HANDLE) {
		destroy_framebuffers();
	}

	for (Workspace &workspace : workspaces) {
		refsol::Tutorial_destructor_workspace(rtg, command_pool, &workspace.command_buffer);

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
		//Camera_descriptors freed when pool is destroyed.

		if (workspace.World_src.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.World_src));
		}
		if (workspace.World.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.World));
		}
		//World_descriptors freed when pool is destroyed.

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

	refsol::Tutorial_destructor(rtg, &render_pass, &command_pool);
}

void Tutorial::on_swapchain(RTG &rtg_, RTG::SwapchainEvent const &swapchain) {
	//[re]create framebuffers:
	refsol::Tutorial_on_swapchain(rtg, swapchain, depth_format, render_pass, &swapchain_depth_image, &swapchain_depth_image_view, &swapchain_framebuffers);
}

void Tutorial::destroy_framebuffers() {
	refsol::Tutorial_destroy_framebuffers(rtg, &swapchain_depth_image, &swapchain_depth_image_view, &swapchain_framebuffers);
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

	if (!object_instances.empty()) { // upload lines vertices
		//[re-]allocate lines buffers if needed:
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
			VkDescriptorBufferInfo Transforms_info{ // Describe the buffer
				.buffer = workspace.Transforms.handle, // which buffer
				.offset = 0, // start at beginning
				.range = workspace.Transforms.size, // use whole buffer
			};

			std::array< VkWriteDescriptorSet, 1 > writes{
				VkWriteDescriptorSet{ // describe the write operation
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  
					.dstSet = workspace.Transforms_descriptors, // which descriptor set to update  
					.dstBinding = 0, // binding 0 in that set
					.dstArrayElement = 0, // first element (if it were an array)
					.descriptorCount = 1,  // updating 1 descriptor
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &Transforms_info, 
				},
			};

			vkUpdateDescriptorSets( // execute the update (update includes all operations above, like the object_instances, etc.)
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

	{ // draw with the objects pipeline
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

		{ //bind Transforms descriptor set:
			std::array< VkDescriptorSet, 1 > descriptor_sets{
				workspace.Transforms_descriptors, //1: Transforms
			};
			vkCmdBindDescriptorSets(
				workspace.command_buffer, //command buffer
				VK_PIPELINE_BIND_POINT_GRAPHICS, //pipeline bind point
				objects_pipeline.layout, //pipeline layout
				1, //first set
				uint32_t(descriptor_sets.size()), descriptor_sets.data(), //descriptor sets count, ptr
				0, nullptr //dynamic offsets count, ptr
			);
		}

		// camera descriptor set is still bound (!), but not used <- what does this mean //??
		// we didn't need to re-bind the camera descriptor set -- we were able to leave it bound because set 0 for both the lines pipeline and the objects pipeline are compatible.
		// - You drew lines with the lines pipeline (camera was bound)
		// - Now you switch to the objects pipeline with vkCmdBindPipeline
		// - You don't need to rebind the camera descriptor set!

		// draw all instances:
		for (ObjectInstance const &inst : object_instances) {
			uint32_t index = uint32_t(&inst - &object_instances[0]);

			//bind texture descriptor set:
			vkCmdBindDescriptorSets(
				workspace.command_buffer, //command buffer
				VK_PIPELINE_BIND_POINT_GRAPHICS, //pipeline bind point
				objects_pipeline.layout, //pipeline layout
				2, //second set
				1, &texture_descriptors[inst.texture], //descriptor sets count, ptr
				0, nullptr //dynamic offsets count, ptr
			);
			
			vkCmdDraw(workspace.command_buffer, inst.vertices.count, 1, inst.vertices.first, index);
		}
	}

	vkCmdEndRenderPass(workspace.command_buffer);

	//end recording:
	VK( vkEndCommandBuffer(workspace.command_buffer ));
	
	//submit `workspace.command buffer` for the GPU to run:
	refsol::Tutorial_render_submit(rtg, render_params, workspace.command_buffer);
}


void Tutorial::update(float dt) {
	time  = std::fmod(time + dt, 60.0f);

	{ // camera orbiting the origin:
		float ang = float(M_PI) * 2.0f * 10.0f * (time / 60.0f);
		CLIP_FROM_WORLD = perspective( // TODO: understand this
			60.0f * float(M_PI) / 180.0f, //vfov
			rtg.swapchain_extent.width / float(rtg.swapchain_extent.height), //aspect
			0.1f, //near
			1000.0f //far
		) * look_at(
			3.0f * std::cos(ang), 3.0f * std::sin(ang), 1.0f, //eye
			0.0f, 0.0f, 0.5f, //target
			0.0f, 0.0f, 1.0f //up
		);
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

	{ // make some objects
		object_instances.clear();

		{ //plane translated +x by one unit:
			mat4 WORLD_FROM_LOCAL{ //TODO: understand this
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				1.0f, 0.0f, 0.0f, 1.0f,
			};

			object_instances.emplace_back(ObjectInstance{
				.vertices = plane_vertices,
				.transform{ // TODO: understand this
					.CLIP_FROM_LOCAL = CLIP_FROM_WORLD * WORLD_FROM_LOCAL,
					.WORLD_FROM_LOCAL = WORLD_FROM_LOCAL, 
					.WORLD_FROM_LOCAL_NORMAL = WORLD_FROM_LOCAL, // this is the normals? //??
				},
				.texture = 1,
			});
		}
		{ //torus translated -x by one unit and rotated CCW around +y: TODO: understand this
			float ang = time / 60.0f * 2.0f * float(M_PI) * 10.0f;
			float ca = std::cos(ang);
			float sa = std::sin(ang);
			mat4 WORLD_FROM_LOCAL{
				  ca, 0.0f,  -sa, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				  sa, 0.0f,   ca, 0.0f,
				-1.0f,0.0f, 0.0f, 1.0f,
			};

			object_instances.emplace_back(ObjectInstance{
				.vertices = torus_vertices,
				.transform{
					.CLIP_FROM_LOCAL = CLIP_FROM_WORLD * WORLD_FROM_LOCAL,
					.WORLD_FROM_LOCAL = WORLD_FROM_LOCAL,
					.WORLD_FROM_LOCAL_NORMAL = WORLD_FROM_LOCAL,
				},
			});
		}
	}
}


void Tutorial::on_input(InputEvent const &) {
}
