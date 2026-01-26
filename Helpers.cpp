#include "Helpers.hpp"

#include "RTG.hpp"
#include "VK.hpp"
#include "refsol.hpp"

#include <vulkan/utility/vk_format_utils.h> //useful for byte counting

#include <utility>
#include <cassert>
#include <cstring>
#include <iostream>

Helpers::Allocation::Allocation(Allocation &&from) {
	assert(handle == VK_NULL_HANDLE && offset == 0 && size == 0 && mapped == nullptr);

	std::swap(handle, from.handle);
	std::swap(size, from.size);
	std::swap(offset, from.offset);
	std::swap(mapped, from.mapped);
}

Helpers::Allocation &Helpers::Allocation::operator=(Allocation &&from) {
	if (!(handle == VK_NULL_HANDLE && offset == 0 && size == 0 && mapped == nullptr)) {
		//not fatal, just sloppy, so complain but don't throw:
		std::cerr << "Replacing a non-empty allocation; device memory will leak." << std::endl;
	}

	std::swap(handle, from.handle);
	std::swap(size, from.size);
	std::swap(offset, from.offset);
	std::swap(mapped, from.mapped);

	return *this;
}

Helpers::Allocation::~Allocation() {
	if (!(handle == VK_NULL_HANDLE && offset == 0 && size == 0 && mapped == nullptr)) {
		std::cerr << "Destructing a non-empty Allocation; device memory will leak." << std::endl;
	}
}

//----------------------------

Helpers::AllocatedBuffer Helpers::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, MapFlag map) {
	AllocatedBuffer buffer;
	refsol::Helpers_create_buffer(rtg, size, usage, properties, (map == Mapped), &buffer);
	return buffer;
}

void Helpers::destroy_buffer(AllocatedBuffer &&buffer) {
	refsol::Helpers_destroy_buffer(rtg, &buffer);
}


Helpers::AllocatedImage Helpers::create_image(VkExtent2D const &extent, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, MapFlag map) {
	AllocatedImage image;
	refsol::Helpers_create_image(rtg, extent, format, tiling, usage, properties, (map == Mapped), &image);
	return image;
}

void Helpers::destroy_image(AllocatedImage &&image) {
	refsol::Helpers_destroy_image(rtg, &image);
}

//----------------------------

void Helpers::transfer_to_buffer(void const *data, size_t size, AllocatedBuffer &target) {
	// refsol::Helpers_transfer_to_buffer(rtg, data, size, &target);

	// NOTE: could let this stick around and use it for all uploads, but this function isn't for performant transfers anyway:
	// Create a CPU-visible "staging" buffer:
	AllocatedBuffer transfer_src = create_buffer(
		size, 
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		Mapped
	);

	// copy data to transfer buffer: // Copy data from CPU → staging buffer using memcpy
	std::memcpy(transfer_src.allocation.data(), data, size);

	{ //record command buffer that does CPU->GPU transfer: // Use GPU to copy staging buffer → GPU-local buffer
		// what's the difference between this transfer_command_buffer and the workspace.command_buffer used in Tutorial.cpp //??
		VK( vkResetCommandBuffer(transfer_command_buffer, 0)); // reset the command buffer (clear old commands)
		
		VkCommandBufferBeginInfo begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // will record again every submit // what does this mean //??
		};

		VK( vkBeginCommandBuffer(transfer_command_buffer, &begin_info));

		VkBufferCopy copy_region{ //   Defines what part of each buffer to copy: 
			.srcOffset = 0,
			.dstOffset = 0,
			.size = size,
		};
		vkCmdCopyBuffer(transfer_command_buffer, transfer_src.handle, target.handle, 1, &copy_region);

		VK( vkEndCommandBuffer(transfer_command_buffer) );
	}

	{// run command buffer
		VkSubmitInfo submit_info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &transfer_command_buffer
		};

		// Submits a sequence of semaphores or command buffers to a queue
		VK( vkQueueSubmit(rtg.graphics_queue, 1, &submit_info, VK_NULL_HANDLE) );
	}

	// wait for command buffer to finish:
	VK( vkQueueWaitIdle(rtg.graphics_queue) );
	
	// don't leak buffer memory:
	destroy_buffer(std::move(transfer_src)); // what does std::move do //??
}

void Helpers::transfer_to_image(void const *data, size_t size, AllocatedImage &target) {
	// refsol::Helpers_transfer_to_image(rtg, data, size, &target);

	assert(target.handle != VK_NULL_HANDLE); //target image should be allocated already

	//check data is the right size: [new]
	size_t bytes_per_block = vkuFormatTexelBlockSize(target.format);
	size_t texels_per_block = vkuFormatTexelsPerBlock(target.format);
	assert(size == target.extent.width * target.extent.height * bytes_per_block / texels_per_block);

	//create a host-coherent source buffer
	AllocatedBuffer transfer_src = create_buffer(
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		Mapped
	);

	//copy image data into the source buffer
	std::memcpy(transfer_src.allocation.data(), data, size);

	//begin recording a command buffer
	VK( vkResetCommandBuffer(transfer_command_buffer, 0) );

	VkCommandBufferBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, //will record again every submit
	};

	VK( vkBeginCommandBuffer(transfer_command_buffer, &begin_info) );

	VkImageSubresourceRange whole_image{
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};

	{ //put the receiving image in destination-optimal layout [new]
		VkImageMemoryBarrier barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, //throw away old image
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = target.handle,
			.subresourceRange = whole_image,
		};

		vkCmdPipelineBarrier(
			transfer_command_buffer, //commandBuffer
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, //srcStageMask
			VK_PIPELINE_STAGE_TRANSFER_BIT, //dstStageMask
			0, //dependencyFlags
			0, nullptr, //memory barrier count, pointer
			0, nullptr, //buffer memory barrier count, pointer
			1, &barrier //image memory barrier count, pointer
		);
	}

	{ // copy the source buffer to the image [new]
		VkBufferImageCopy region{
			.bufferOffset = 0,
			.bufferRowLength = target.extent.width,
			.bufferImageHeight = target.extent.height,
			.imageSubresource{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
			.imageOffset{ .x = 0, .y = 0, .z = 0 },
			.imageExtent{
				.width = target.extent.width,
				.height = target.extent.height,
				.depth = 1
			},
		};

		vkCmdCopyBufferToImage(
			transfer_command_buffer,
			transfer_src.handle,
			target.handle,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &region
		);

		//NOTE: if image had mip levels, would need to copy as additional regions here.
	}

	{ // transition the image memory to shader-read-only-optimal layout: [new]
		VkImageMemoryBarrier barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = target.handle,
			.subresourceRange = whole_image,
		};

		vkCmdPipelineBarrier(
			transfer_command_buffer, //commandBuffer
			VK_PIPELINE_STAGE_TRANSFER_BIT, //srcStageMask
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, //dstStageMask
			0, //dependencyFlags
			0, nullptr, //memory barrier count, pointer
			0, nullptr, //buffer memory barrier count, pointer
			1, &barrier //image memory barrier count, pointer
		);
	}

	//end and submit the command buffer
	VK( vkEndCommandBuffer(transfer_command_buffer) );

	VkSubmitInfo submit_info{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &transfer_command_buffer
	};

	VK( vkQueueSubmit(rtg.graphics_queue, 1, &submit_info, VK_NULL_HANDLE) );

	//wait for command buffer to finish executing
	VK( vkQueueWaitIdle(rtg.graphics_queue) );

	//destroy the source buffer
	destroy_buffer(std::move(transfer_src));
}

//----------------------------

VkFormat Helpers::find_image_format(std::vector< VkFormat > const &candidates, VkImageTiling tiling, VkFormatFeatureFlags features) const {
	return refsol::Helpers_find_image_format(rtg, candidates, tiling, features);
}

VkShaderModule Helpers::create_shader_module(uint32_t const *code, size_t bytes) const {
	VkShaderModule shader_module = VK_NULL_HANDLE;
	refsol::Helpers_create_shader_module(rtg, code, bytes, &shader_module);
	return shader_module;
}

//----------------------------

Helpers::Helpers(RTG const &rtg_) : rtg(rtg_) {
}

Helpers::~Helpers() {
}

void Helpers::create() {
	VkCommandPoolCreateInfo create_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, // allows individual command buffers to be reset and reused
		.queueFamilyIndex = rtg.graphics_queue_family.value(),
	};
	VK( vkCreateCommandPool(rtg.device, &create_info, nullptr, &transfer_command_pool) );

	VkCommandBufferAllocateInfo alloc_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = transfer_command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, // it can be submitted directly to a queue
		.commandBufferCount = 1,
	};
	VK( vkAllocateCommandBuffers(rtg.device, &alloc_info, &transfer_command_buffer) );
}

void Helpers::destroy() {
	// technically not needed since freeing the pool will free all contained buffers:
	if (transfer_command_buffer != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(rtg.device, transfer_command_pool, 1, &transfer_command_buffer);
		transfer_command_buffer = VK_NULL_HANDLE;
	}
	if (transfer_command_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(rtg.device, transfer_command_pool, nullptr);
		transfer_command_pool = VK_NULL_HANDLE;
	}
}
