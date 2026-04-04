#include "RTG.hpp"
#include "VK.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

static uint32_t brdf_comp_code[] =
#include "spv/brdf.comp.inl"
;

/*
BRDF LUT precomputation utility
Runs brdf.comp on the GPU to integrate the split-sum BRDF (scale, bias) for all (NdotV, roughness) pairs and writes the result to a raw RG32F binary file brdf_lut.bin).

Usage: /bin/brdf

The output is a 512*512 grid of (scale, bias) float pairs.
*/
int main(int argc, char **argv) {
    std::string OUT_PATH = "brdf_lut.bin";
    constexpr uint32_t LUT_SIZE = 512; // Credit: https://learnopengl.com/PBR/IBL/Specular-IBL uses 512

    try {
        // init RTG headless;  configure application:
        RTG::Configuration configuration;
        configuration.application_info = VkApplicationInfo{
            .pApplicationName = "BRDF Utility",
            .applicationVersion = VK_MAKE_VERSION(0,0,0),
            .pEngineName = "Unknown",
            .engineVersion = VK_MAKE_VERSION(0,0,0),
            .apiVersion = VK_API_VERSION_1_3,
        };
        configuration.headless = true;
        RTG rtg(configuration);

        std::cout << "Precomputing BRDF LUT (" << LUT_SIZE << "x" << LUT_SIZE << " RG32F)..." << std::endl;

        // --- storage image: brdf.comp writes (scale, bias) per texel ---
        Helpers::AllocatedImage image = rtg.helpers.create_image(
            VkExtent2D{.width = LUT_SIZE, .height = LUT_SIZE},
            VK_FORMAT_R32G32_SFLOAT, // need 4-component to support storage
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // Compute shader can read/write pixels directly | Can be the source of a transfer — needed to copy results back out
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );

        VkImageView view = VK_NULL_HANDLE;
        { // image view
            VkImageViewCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D, // some other options are: 1D texture for gradient, volumetric data, cubemap, array of textures/cubemaps
                .format = VK_FORMAT_R32G32_SFLOAT,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            VK( vkCreateImageView(rtg.device, &create_info, nullptr, &view) );
        }

        VkDescriptorSetLayout set0 = VK_NULL_HANDLE;
        { // descriptor set layout: set=0, binding=0, storage image
            VkDescriptorSetLayoutBinding binding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            };
            VkDescriptorSetLayoutCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 1,
                .pBindings = &binding,
            };
            VK( vkCreateDescriptorSetLayout(rtg.device, &create_info, nullptr, &set0) );
        }

        VkCommandPool command_pool = VK_NULL_HANDLE;
        {
            VkCommandPoolCreateInfo create_info{
                .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = rtg.graphics_queue_family.value(),
            };
            VK( vkCreateCommandPool(rtg.device, &create_info, nullptr, &command_pool) );
        }

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        {
            VkCommandBufferAllocateInfo alloc_info{
                .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool        = command_pool,
                .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VK( vkAllocateCommandBuffers(rtg.device, &alloc_info, &command_buffer) );
        }

        VkPipelineLayout layout = VK_NULL_HANDLE;
        { // pipeline layout
            VkPipelineLayoutCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &set0,
            };
            VK( vkCreatePipelineLayout(rtg.device, &create_info, nullptr, &layout) );
        }

        VkShaderModule module = rtg.helpers.create_shader_module(brdf_comp_code);
        VkPipeline pipeline = VK_NULL_HANDLE;
        { // compute pipeline from brdf.comp SPIR-V (binary bytecode format that Vulkan uses to execute shaders on the GPU)
            /*
            brdf.comp          →   glslc compiler   →   brdf.comp.spv   →   brdf.comp.inl                                                                                                                                                                                    
            (GLSL source you      (runs at build time)   (binary SPIR-V)     (same bytes, formatted                                                                                                                                                                          
            write and read)                                                   as a C array literal                                                                                                                                                                          
                                                                                so you can #include it) 
            */
            VkComputePipelineCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {
                    .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName  = "main",
                },
                .layout = layout,
            };
            VK( vkCreateComputePipelines(rtg.device, VK_NULL_HANDLE, 1, &create_info, nullptr, &pipeline) );
        }
        vkDestroyShaderModule(rtg.device, module, nullptr); // no longer needed after pipeline creation

        // -- descriptor pool + set --
        VkDescriptorPool pool = VK_NULL_HANDLE;
        { // create descriptor pool for input faces
            VkDescriptorPoolSize pool_size{.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1};
            VkDescriptorPoolCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1,
                .poolSizeCount = 1,
                .pPoolSizes = &pool_size,
            };
            VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &pool) );
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        { // alloc + write
            VkDescriptorSetAllocateInfo alloc_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &set0,
            };
            VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &set) );

            // bind storage image at set=0, binding=0 
            VkDescriptorImageInfo image_info{.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &image_info,
            };
            vkUpdateDescriptorSets(rtg.device, 1, &write, 0, nullptr);
        }

        // CPU-visible readback buffer
        size_t readback_size = LUT_SIZE * LUT_SIZE * 2 * sizeof(float); // 2 floats (scale, bias) per texel
        Helpers::AllocatedBuffer readback_buffer = rtg.helpers.create_buffer(
            readback_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            Helpers::Mapped
        );

        { // run pipeline: record UNDEFINED→GENERAL, dispatch, GENERAL→TRANSFER_SRC
            VK( vkResetCommandBuffer(command_buffer, 0) );
            VkCommandBufferBeginInfo begin_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            VK( vkBeginCommandBuffer(command_buffer, &begin_info) );

            VkImageSubresourceRange whole{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };

            { // UNDEFINED -> GENERAL (storage images must be in GENERAL layout)
                VkImageMemoryBarrier b{
                    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask       = 0,
                    .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image               = image.handle, .subresourceRange = whole,
                };
                vkCmdPipelineBarrier(command_buffer,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &b);
            }

            // dispatch one thread per output texel; brdf.comp local_size is (1,1,1)
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command_buffer, LUT_SIZE, LUT_SIZE, 1);

            { // GENERAL -> TRANSFER_SRC_OPTIMAL for readback copy
                VkImageMemoryBarrier b{
                    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
                    .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image               = image.handle, .subresourceRange = whole,
                };
                vkCmdPipelineBarrier(command_buffer,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &b);
            }

            VkBufferImageCopy copy_region{
                .bufferOffset      = 0,
                .bufferRowLength   = LUT_SIZE,
                .bufferImageHeight = LUT_SIZE,
                .imageSubresource{
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = 0,
                    .baseArrayLayer = 0, .layerCount = 1,
                },
                .imageOffset = {0, 0, 0},
                .imageExtent = {LUT_SIZE, LUT_SIZE, 1},
            };
            vkCmdCopyImageToBuffer(command_buffer,
                image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                readback_buffer.handle, 1, &copy_region);

            VK( vkEndCommandBuffer(command_buffer) );
            VkSubmitInfo si{
                .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1, .pCommandBuffers = &command_buffer,
            };
            VK( vkQueueSubmit(rtg.graphics_queue, 1, &si, VK_NULL_HANDLE) );
            VK( vkQueueWaitIdle(rtg.graphics_queue) );
        }

        // --- write to disk ---
        std::ofstream fout(OUT_PATH, std::ios::binary);
        if (!fout.good()) throw std::runtime_error("Failed to open output file: " + OUT_PATH);
        fout.write(static_cast<char const *>(readback_buffer.allocation.data()), static_cast<std::streamsize>(readback_size));
        std::cout << "Saved BRDF LUT to " << OUT_PATH << std::endl;

        // --- cleanup ---
        vkDestroyCommandPool(rtg.device, command_pool, nullptr); // frees command_buffer implicitly
        rtg.helpers.destroy_buffer(std::move(readback_buffer));
        vkDestroyPipeline(rtg.device, pipeline, nullptr);
        vkDestroyPipelineLayout(rtg.device, layout, nullptr);
        vkDestroyDescriptorPool(rtg.device, pool, nullptr);
        vkDestroyDescriptorSetLayout(rtg.device, set0, nullptr);
        vkDestroyImageView(rtg.device, view, nullptr);
        rtg.helpers.destroy_image(std::move(image));

        return 0;
    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
}
