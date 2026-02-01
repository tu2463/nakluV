#include "Helpers.hpp"

#include "RTG.hpp"
#include "VK.hpp"

#include <vulkan/utility/vk_format_utils.h> // useful for byte counting

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

Helpers::Allocation Helpers::allocate(VkDeviceSize size, VkDeviceSize alignment, uint32_t memory_type_index, MapFlag map) {
	Helpers::Allocation allocation;

	VkMemoryAllocateInfo alloc_info{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = size,
		.memoryTypeIndex = memory_type_index,
	};

	VK( vkAllocateMemory( rtg.device, &alloc_info, nullptr, &allocation.handle) );

	allocation.size = size;
	allocation.offset = 0;

	if (map == Mapped) { // mapping the memory into the host address space if requested:
		VK( vkMapMemory(rtg.device, allocation.handle, 0, allocation.size, 0, &allocation.mapped) );
	}

	return allocation;
}

// This version of our allocate function passes the work of allocating the memory to the other overload of the function, 
// and the work of finding a memory type in the memoryTypeBits bit set that also has the memory properties in properties to a function called find_memory_type
// The conveneince overload unpacks the Vulkan structs and calls the low-level overload
Helpers::Allocation Helpers::allocate(VkMemoryRequirements const &req, VkMemoryPropertyFlags properties, MapFlag map) {
	return allocate(req.size, req.alignment, find_memory_type(req.memoryTypeBits, properties), map);
}

void Helpers::free(Helpers::Allocation &&allocation) {
	if (allocation.mapped != nullptr) {
		vkUnmapMemory(rtg.device, allocation.handle);
		allocation.mapped = nullptr;
	}

	vkFreeMemory(rtg.device, allocation.handle, nullptr);

	allocation.handle = VK_NULL_HANDLE;
	allocation.offset = 0;
	allocation.size = 0;
}

//----------------------------

Helpers::AllocatedBuffer Helpers::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, MapFlag map) {
	AllocatedBuffer buffer;
	// refsol::Helpers_create_buffer(rtg, size, usage, properties, (map == Mapped), &buffer);
	VkBufferCreateInfo create_info{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE, //  the buffer/image is owned by one queue family at a time.
	};
	VK( vkCreateBuffer(rtg.device, &create_info, nullptr, &buffer.handle) );
	buffer.size = size;

	// determine memory requirements
	VkMemoryRequirements req;
	// The members of the memory structure are:
	// a size (in bytes), an alignment (in bytes) -- both of which are what you expect -- 
	// and a memoryTypeBits, which is a bitfield of which memory types from the physical device are supported for the backing memory.
	vkGetBufferMemoryRequirements(rtg.device, buffer.handle, &req);

	// allocate memory:
	buffer.allocation = allocate(req, properties, map);

	// bind memory:
	VK( vkBindBufferMemory(rtg.device, buffer.handle, buffer.allocation.handle, buffer.allocation.offset) );
	return buffer;
}

void Helpers::destroy_buffer(AllocatedBuffer &&buffer) {
	// refsol::Helpers_destroy_buffer(rtg, &buffer);
	vkDestroyBuffer(rtg.device, buffer.handle, nullptr); // how to understand the use of & here //??
	buffer.handle = VK_NULL_HANDLE;
	buffer.size = 0;

	this->free(std::move(buffer.allocation)); // pass the memory allocation to our free function to take care of releasing it
}


Helpers::AllocatedImage Helpers::create_image(VkExtent2D const &extent, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, MapFlag map) {
	// 1. create the VkImage
	AllocatedImage image;
	// refsol::Helpers_create_image(rtg, extent, format, tiling, usage, properties, (map == Mapped), &image);
	image.extent = extent;
	image.format = format;

	VkImageCreateInfo create_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent{
			.width = extent.width,
			.height = extent.height,
			.depth = 1
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, // No multisampling
		.tiling = tiling,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE, //  the buffer/image is owned by one queue family at a time.
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, // if you wanted to specify images directly by writing data to mapped memory (instead of copying from a buffer) you'd instead set this to VK_IMAGE_LAYOUT_PREINITIALIZED and the tiling to VK_IMAGE_TILING_LINEAR, which together would guarantee a known image layout.
	};

	VK( vkCreateImage(rtg.device, &create_info, nullptr, &image.handle) );

	// 2. ask how much memory it needs
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(rtg.device, image.handle, &req); // Strangely enough, vkGetBufferMemoryRequirements is one of the very rare Vulkan functions that cannot return an error. So we don't wrap it with VK()

	// 3. create the memory
	image.allocation = allocate(req, properties, map);

	// 4. bind the memory
	VK( vkBindImageMemory(rtg.device, image.handle, image.allocation.handle, image.allocation.offset) );
	return image;
}

void Helpers::destroy_image(AllocatedImage &&image) {
	// refsol::Helpers_destroy_image(rtg, &image);
	vkDestroyImage(rtg.device, image.handle, nullptr);

	image.handle = VK_NULL_HANDLE;
	image.extent = VkExtent2D{.width = 0, .height = 0};
	image.format = VK_FORMAT_UNDEFINED;

	this->free(std::move(image.allocation));
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

	assert(target.handle != VK_NULL_HANDLE); // target imgage should be allocated already

	// check data is the right size [new]
	size_t bytes_per_block = vkuFormatTexelBlockSize(target.format);
	size_t texels_per_block = vkuFormatTexelsPerBlock(target.format);
	assert(size == target.extent.width * target.extent.height * bytes_per_block / texels_per_block);

	// create a host-coherent source buffer
	AllocatedBuffer transfer_src = create_buffer(
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // This buffer will be the source of a copy operatio
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, // he CPU can see this memory | CPU writes are automatically visible to GPU
		Mapped
	);

	// copy image data into the source buffer
	// Use *data*, not *&data*, because The function signature shows data is already a pointer (void const *data). The bug is using &data which takes the address of the pointer variable itself (on the stack) instead of the data it points to.  
	std::memcpy(transfer_src.allocation.data(), data, size);

	// begin recording a command buffer
	VK( vkResetCommandBuffer(transfer_command_buffer, 0) );

	VkCommandBufferBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // will record again every submit
	};

	VK( vkBeginCommandBuffer(transfer_command_buffer, &begin_info) );

	VkImageSubresourceRange whole_image{
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,// Color data (not depth/stencil) 
		.baseMipLevel = 0, // Start at mip 0 (full resolution)     
		.levelCount = 1, // Only 1 mip level                                       
		.baseArrayLayer = 0, // Start at layer 0   
		.layerCount = 1, // Only 1 layer 
	};

	{ // put the receiving image in destination-optimal layout [new]
		// To tell the GPU to put the image in a specific layout, we use a pipeline barrier command with a VkImageMemoryBarrier structure. 
		// This is a synchronization primitive that requires that every command before the barrier (in a certain pipeline stage, doing a certain memory operation) 
		// must happen before the layout transition, 
		// and that every command after the barrier (in a certain pipeline stage, doing a certain memory operation) 
		// must happen after the layout transition.

		//   What This Achieves:                                                                                                                                                              
		//   [Image: UNDEFINED layout, unknown contents]                                                                                                                                       
		//                       │                                                                                                                                                             
		//                       ▼ barrier                                                                                                                                                     
		//   [Image: TRANSFER_DST_OPTIMAL layout, ready for vkCmdCopyBufferToImage]                                                                                                            
		//                       │                                                                                                                                                             
		//                       ▼ (next step: copy data into image)   

		VkImageMemoryBarrier barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0, // doesn't place any conditions on earlier command; No prior access to wait for； Nothing was accessing this image before
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, //the transition must complete before any transfers write data to the image
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // throw away old image; Discard existing contents (faster than preserving
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // whatever layout is best for receiving data
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,  //  Not transferring ownership between queues
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = target.handle, // The image to transition 
			.subresourceRange = whole_image, // Affect the whole image 
		};

		vkCmdPipelineBarrier(
			transfer_command_buffer, // commandBuffer
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // srcStageMask: after nothing (start of pipeline) 
			VK_PIPELINE_STAGE_TRANSFER_BIT, // dstStageMask: before transfer operations 
			0, // no dependencyFlags
			0, nullptr, // memory barrier count, pointer (no memory barriers)
			0, nullptr, // buffer memory barrier count, pointer (no buffer barriers)
			1, &barrier // image memory barrier count, pointer (1 iamge barrier)
		);
	}

	{ // copy the source buffer to the image [new]
		// describe what part of the image to copy;
		// parameters indicate buffer and image to copy between and the current format of the image:
		VkBufferImageCopy region{
			.bufferOffset = 0,
			.bufferRowLength = target.extent.width,
			.bufferImageHeight = target.extent.height,
			.imageSubresource{ // Frustratingly, the imageSubresource field of VkBufferImageCopy is a VkImageSubresourceLayers not a VkImageSubresourceRange, otherwise we could have used our convenient whole_image structure from above.
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
			1, &region // region count, region ptr
		);
		// NOTE: if image had mip levels, would need to copy as additional regions here
	}

	{ // transition the image memory to shader-read-only-optimal layout [new]
		VkImageMemoryBarrier barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, // waits until all transfer writes are complete, then transitions the image
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT, // wailts until image transition finishes, then allows fragment shader reads to proceed
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,  //  Not transferring ownership between queues
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = target.handle, // The image to transition 
			.subresourceRange = whole_image, // Affect the whole image 
		};

		vkCmdPipelineBarrier(
			transfer_command_buffer, // commandBuffer
			VK_PIPELINE_STAGE_TRANSFER_BIT, // srcStageMask
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // dstStageMask
			0, // no dependencyFlags
			0, nullptr, // memory barrier count, pointer (no memory barriers)
			0, nullptr, // buffer memory barrier count, pointer (no buffer barriers)
			1, &barrier // image memory barrier count, pointer (1 iamge barrier)
		);
	}

	// end and submit the command buffer:
	VK( vkEndCommandBuffer(transfer_command_buffer) );

	VkSubmitInfo submit_info{ // what does this format mean//??
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &transfer_command_buffer
	};

	VK( vkQueueSubmit(rtg.graphics_queue, 1, &submit_info, VK_NULL_HANDLE) );

	// wait for command buffer to finish executing:
	VK( vkQueueWaitIdle(rtg.graphics_queue) );

	// destroy the source buffer:
	destroy_buffer(std::move(transfer_src)); // what does move() mean //vv transfer ownership to the destroy function
}

//----------------------------

uint32_t Helpers::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags flags) const { // what does this const mean //??
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
		VkMemoryType const &type = memory_properties.memoryTypes[i]; // walks through each memory type
		// from the property flags, 
		// attempt to find one that both appears in the type_filter bit-set and 
		// supports all the requested flags.
		if ((type_filter & (1 << i)) != 0 && (type.propertyFlags & flags) == flags) {
			return i;
		}
	}
	throw std::runtime_error("No suitable memory type found.");
}

// we'll need this when we get to creating swapchain-related resources.
// The caller asks for features they want (as a logical or of VkFormatFeatureFlagBits), and 
// the function looks for formats (among the candidates the caller requests) that support those features.
// This code finds a supported image format from a list of candidates. Not all GPUs support all formats for all uses, so you must check.   
VkFormat Helpers::find_image_format(std::vector< VkFormat > const &candidates, VkImageTiling tiling, VkFormatFeatureFlags features) const {
	// return refsol::Helpers_find_image_format(rtg, candidates, tiling, features);

	for (VkFormat format : candidates) { // Loop through your preferred formats
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(rtg.physical_device, format, &props);
		// by now, The props struct contains:                                                                               
		// - linearTilingFeatures — what's supported for linear tiling                                              
		// - optimalTilingFeatures — what's supported for optimal tiling 
		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
			// If using linear tiling, check if all requested features are supported.
			return format;
		} else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
			// If using optimal tiling, check optimal features instead.
			return format;
		}
	}
	throw std::runtime_error("No supported format matches request");
}

VkShaderModule Helpers::create_shader_module(uint32_t const *code, size_t bytes) const {
	// create a shader module from some SPIR-V bytecode - what is SPIR_V//vv a binary intermediate language for representing graphics shaders and compute kernels
	// Instead of sending raw GLSL/HLSL shader source code to the GPU driver, you compile your shaders to SPIR-V bytecode first. This bytecode is then consumed by the Vulkan driver. 
	VkShaderModule shader_module = VK_NULL_HANDLE;
	// refsol::Helpers_create_shader_module(rtg, code, bytes, &shader_module);
	VkShaderModuleCreateInfo create_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = bytes,
		.pCode = code
	};
	VK( vkCreateShaderModule(rtg.device, &create_info, nullptr, &shader_module) );
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

	vkGetPhysicalDeviceMemoryProperties(rtg.physical_device, &memory_properties);

	if (rtg.configuration.debug) {
		std::cout << "Memory types:\n";
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
			VkMemoryType const &type = memory_properties.memoryTypes[i];
			std::cout << " [" << i << "] heap " << type.heapIndex << ", flags: " << string_VkMemoryPropertyFlags(type.propertyFlags) << '\n';
		}
		std::cout << "Memory heaps:\n";
		for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i) {
			VkMemoryHeap const &heap = memory_properties.memoryHeaps[i];
			std::cout << " [" << i << "] " << heap.size << " bytes, flags: " << string_VkMemoryHeapFlags(heap.flags) << '\n';
		}
		std::cout.flush(); //?? what is flush?
	}
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
