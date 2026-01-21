#include "Tutorial.hpp"

#include "VK.hpp"
#include "refsol.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

Tutorial::Tutorial(RTG &rtg_) : rtg(rtg_) {
	refsol::Tutorial_constructor(rtg, &depth_format, &render_pass, &command_pool);

	background_pipeline.create(rtg, render_pass, 0);
	lines_pipeline.create(rtg, render_pass, 0);

	{ // create descriptor tool:
		uint32_t per_workspace = uint32_t(rtg.workspaces.size()); // for easier-to-read counting

		std::array< VkDescriptorPoolSize, 1 > pool_sizes{
			// we only need uniform buffer descriptors for the moment:
			VkDescriptorPoolSize{
				.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 1 * per_workspace, // one descriptor per set, one set per workspace
			},
		};

		VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0, // because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, can't free individual descriptors allocated from this pool
			.maxSets = 1 * per_workspace, // one set per workspace
			.poolSizeCount = uint32_t(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(),
		};

		VK( vkCreateDescriptorPool(rtg.device, create_info, nullptr, &descriptor_pool) );
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
		workspace.Camera_src = rtg.helpers.create_buffer(
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

		// descriptor write:
		{ //point descriptor to Camera buffer:
			VkDescriptorBufferInfo Camera_info{
				.buffer = workspace.Camera.handle,
				.offset = 0,
				.range = workspace.Camera.size,
			};

			std::array< VkWriteDescriptorSet, 1 > writes{
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = workspace.Camera_descriptors,
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.pBufferInfo = &Camera_info,
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
}

Tutorial::~Tutorial() {
	//just in case rendering is still in flight, don't destroy resources:
	//(not using VK macro to avoid throw-ing in destructor)
	if (VkResult result = vkDeviceWaitIdle(rtg.device); result != VK_SUCCESS) {
		std::cerr << "Failed to vkDeviceWaitIdle in Tutorial::~Tutorial [" << string_VkResult(result) << "]; continuing anyway." << std::endl;
	}

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
	}
	workspaces.clear();

	if (descriptor_pool) {
		vkDestroyDescriptorPool(rtg.device, descriptor_pool, nullptr);
		descriptor_pool = nullptr;
		// (this also frees the descriptor sets allocated from the pool)
	}

	background_pipeline.destroy(rtg);
	lines_pipeline.destroy(rtg);

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
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // going to use as vertex buffer, also going to have GPU into this memory i.e. tge target if memory copy
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

		{ // memory barrier to make sure copies compelte before rendering happens
			VkMemoryBarrier memory_barrier{
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			};

			vkCmdPipelineBarrier(
				workspace.command_buffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT, // srcStageMask; ensures all transfer operations (like buffer copies) complete
				VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, // dstStageMask; wait for all transfer ops before vertex input reads the data
				0, // dependencyFlags,
				1, &memory_barrier, // memoryBarriers (count, data)
				0, nullptr, // bufferMemoryBarriers (count, data)
				0, nullptr // imageMemoryBarriers (count, data)
			);
		}
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

	// TODO: run pipelines here
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

		// draw lines vertices:
		vkCmdDraw(workspace.command_buffer, uint32_t(lines_vertices.size()), 1, 0, 0);
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

	{ // make some crossing lines at different depths:
		lines_vertices.clear();
		constexpr size_t count = 2 * 30 + 2 * 30; //?? what is constexpr?
		lines_vertices.reserve(count);
		// horizontal lines at z = 0.5f:
		for (uint32_t i = 0; i < 30; ++i) {
			float y = (i + 0.5f) / 30.0f * 2.0f - 1.0f;
			lines_vertices.emplace_back(PosColVertex{
				.Position{ .x = -1.0f, .y = y - 0.05f, .z = 0.5f },
				.Color{ .r = 0x00, .g = 0x00, .b = 0x00, .a = 0xff },
			});
			lines_vertices.emplace_back(PosColVertex{
				.Position{ .x = 1.0f, .y = y + 0.05f, .z = 0.5f},
				.Color{ .r = 0xff, .g = 0xff, .b = 0x00, .a = 0xff},
			});
		}
		// vertical lines at z = 0.0f (near) through 1.0f (far)
		for (uint32_t i = 0; i < 30; ++i) {
			float x = (i + 0.5f) / 30.0f * 2.0f - 1.0f;
			float z = (i + 0.5f) / 30.0f;
			lines_vertices.emplace_back(PosColVertex{
				.Position{.x = x + 0.05f, .y = -1.0f, .z = z},
				.Color{ .r = 0x00, .g = 0x00, .b = 0xff, .a = 0xff},
			});
			lines_vertices.emplace_back(PosColVertex{
				.Position{.x = x - 0.05f, .y = 1.0f, .z = z},
				.Color{ .r = 0x44, .g = 0x00, .b = 0xff, .a = 0xff},
			});
		}
		assert(lines_vertices.size() == count);
	}

	// // HACK: transform vertices on the CPU(!)
	// for (PosColVertex &v : lines_vertices) {
	// 	vec4 res = CLIP_FROM_WORLD * vec4{v.Position.x, v.Position.y, v.Position.z, 1.0f};
	// 	v.Position.x = res[0] / res[3];
	// 	v.Position.y = res[1] / res[3];
	// 	v.Position.z = res[2] / res[3];
	// }
}


void Tutorial::on_input(InputEvent const &) {
}
