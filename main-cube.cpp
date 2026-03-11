#include "RTG.hpp"
#include "VK.hpp"
#include "CubePipeline.hpp"

#include <glm/glm.hpp>

#include <iostream>

// Credit: adapted from Zulip discussion https://15-472-s26.zulipchat.com/#narrow/channel/570157-A2/topic/Adding.20Cube.20Utility.20to.20Maekfile/with/575174040

// Wraps one cubemap face on the GPU: an R32G32B32A32_SFLOAT storage image + a uniform buffer with the WORLD_FROM_PX transform matrix, plus descriptor set binding both
struct GPUFace {
    Helpers::AllocatedImage image;
    Helpers::AllocatedBuffer buffer;
    VkImageView view;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    void create(RTG& rtg, VkDescriptorPool descriptor_pool, CubePipeline const &cube_pipeline, uint32_t const sz, glm::vec3 * const data) {
        // add a 4th component to the data vector
        std::vector< glm::vec4 > data_padded; 
        data_padded.reserve(sz*sz);
        for (uint32_t i = 0; i < sz*sz; ++i) {
            data_padded.emplace_back(glm::vec4(data[i], 0.0f));
        }
        
        // create image
        image = rtg.helpers.create_image(
            VkExtent2D{.width = sz, .height = sz},
            VK_FORMAT_R32G32B32A32_SFLOAT, // why use this format for cube face image//vv I think it's because we need 4-component to support storage
            VK_IMAGE_TILING_OPTIMAL,

            /*
            VK_IMAGE_USAGE_STORAGE_BIT: Compute shader can read/write pixels directly
            VK_IMAGE_USAGE_TRANSFER_DST_BIT: Can be the destination of a transfer — needed so transfer_to_image can upload the initial data to it
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT: Can be the source of a transfer — needed to copy results back out
            */
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, // allocate on GPU, not visible on CPU
            Helpers::Unmapped // CPU won't map this memory
        );

        // upload the image
        rtg.helpers.transfer_to_image(data_padded.data(), sizeof(data_padded[0]) * sz * sz, image, 1, VK_IMAGE_LAYOUT_GENERAL);

        // -- buffer --
        { // buffer
            buffer = rtg.helpers.create_buffer(
                sizeof(CubePipeline::Face),

                /*
                Uniform vs Storage for this:
                - Uniform buffers have a size limit (~16KB typically) but are faster — drivers can cache them in dedicated constant memory
                - Storage buffers are larger and writable but have more overhead
                - Small read-only config data like face dimensions → uniform is the right choice
                */
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // use uniform buffer bit because Face holds per-face data that can be read as constants
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                Helpers::Unmapped
            );

            CubePipeline::Face face_info{};

            // Each cubemap face is defined by three orthogonal vectors: (refer to 2/25 my lec note)
            // looking toward +X, horizontal axis points −Z, vertical axis points −Y
            // the "right" axis across the face in world space.
            glm::vec3 s = glm::vec3(0.0f, 0.0f, -1.0f); // s = (0, 0, -1), meaning increasing pixel column moves in the −Z direction.
            // the "down" axis across the face in world space.
            glm::vec3 t = glm::vec3(0.0f, -1.0f, -0.0f); // t = (0, -1, 0), meaning increasing pixel row moves in the −Y direction.
            // the direction the face "looks at" (the face normal); direction pointing straight out of the cube face 
            glm::vec3 center = glm::vec3(1.0f, 0.0f, 0.0f); // For the +X face here, center = (1, 0, 0).
            
            /* 
            So every pixel on the face corresponds to: dir(u,v)=center+s⋅x+t⋅y
            Pixel coordinates go: 0 ... sz-1
            But the cube face coordinate must go: -1 ... +1 for 3D direction in world
                Range size: 2
            So 1 pixel step corresponds to: 1 * (2 / sz)​ <- pixel width
            */

            float pixel_width = 2.0f / float(sz);
            face_info.WORLD_FROM_PX.m0 = pixel_width * s.x;
            face_info.WORLD_FROM_PX.m1 = pixel_width * s.y;
            face_info.WORLD_FROM_PX.m2 = pixel_width * s.z;

            face_info.WORLD_FROM_PX.m3 = pixel_width * t.x;
            face_info.WORLD_FROM_PX.m4 = pixel_width * t.y;
            face_info.WORLD_FROM_PX.m5 = pixel_width * t.z;

            // computes the direction of pixel (0,0)
            // first center = 1 - pixel_width / 2 = 1 - (2 / sz) / 2
            float corner = 1.0f - pixel_width * 0.5f; //??-A2-diffuse understand this better

            face_info.WORLD_FROM_PX.m6 = center.x - corner * s.x - corner * t.x;
            face_info.WORLD_FROM_PX.m7 = center.y - corner * s.y - corner * t.y;
            face_info.WORLD_FROM_PX.m8 = center.z - corner * s.z - corner * t.z;

            /*
            dir(u,v)= center + s * (2u/sz ​− corner) + t * (2v/sz ​− corner)
            dir(u,v)= center + s * (u * pixel_width ​− corner) + t * (v * pixel_width ​− corner)

            full computation:
                The matrix columns are:
                    - Column 0: pixel_width * s (m0,m1,m2)
                    - Column 1: pixel_width * t (m3,m4,m5)
                    - Column 2: center - corner*s - corner*t (m6,m7,m8)

                multiply with (u, v, 1):
                    result = col0*u + col1*v + col2*1
                        = pixel_width*s*u  +  pixel_width*t*v  +  (center - corner*s - corner*t)
                        = center  +  s*(pixel_width*u - corner)  +  t*(pixel_width*v - corner)

            Example: sz=4, then pixel_width=2/sz = 0.5, corner = 1-0.5*0.5 = 0.75
            pixel(0,0) = center + s*(0-0.75) + t*(0-0.75) = center - 0.75s - 0.75t
            pixel(3,3) = center + s*(1.5-0.75) + t*(1.5-0.75) = center + 0.75s + 0.75t
            So the face spans directions from −0.75 to +0.75 along the face axes.
            */
            rtg.helpers.transfer_to_buffer( &face_info, sizeof(face_info), buffer );
        }

        { // image view
            VkImageViewCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .flags = 0, // normal image view; some other options are settings for density map
                .format = image.format,
                .image = image.handle,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // Color data (not depth, not stencil) 
                    .baseArrayLayer = 0, // Start at layer 0 
                    .layerCount = 1, // Only 1 layer (not a cubemap or array texture)
                    .baseMipLevel = 0, // Start at mip level 0 (full resolution) 
                    .levelCount = 1, // Only 1 mip level (no mipmaps) 
                },
                .viewType = VK_IMAGE_VIEW_TYPE_2D, // some other options are: 1D texture for gradient, volumetric data, cubemap, array of textures/cubemaps
            };
            VK( vkCreateImageView(rtg.device, &create_info, nullptr, &view));
        }

        { // descriptor set with world_from_px and storage image
            { // allocate
                VkDescriptorSetAllocateInfo alloc_info{
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool = descriptor_pool,
                    .descriptorSetCount = 1, // the .create fn is called once by in_face and once by out_face, creating one descriptor set for each GPUFace
                    .pSetLayouts = &cube_pipeline.set01_face,
                };
                VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &descriptors ) );
            }
            { // write
                VkDescriptorBufferInfo buffer_info{ // write buffer info into descriptor set
                    .buffer = buffer.handle, // buffer was populated by transfer_to_buffer; it stores a CubePipeline::Face that holds the WORLD_FROM_PX
                    .offset = 0,
                    .range = buffer.size,
                };
                VkDescriptorImageInfo image_info{
                    // this is a storage image (has VK_IMAGE_USAGE_STORAGE_BIT), which typically uses the GENERAL layout
                    // Reference: https://docs.vulkan.org/guide/latest/storage_image_and_texel_buffers.html#:~:text=swift%20Copied!-,Image%20Formats%20for%20Storage%20Images,for%20both%20reading%20and%20writing.
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .imageView = view,
                };

                std::array< VkWriteDescriptorSet, 2> writes{
                    VkWriteDescriptorSet{
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .dstArrayElement = 0, // Starting array index within the binding — 0 since this isn't an array descriptor
                        .dstBinding = 0, // corresponds to the binding = N number delcared in shader AND in the VkDescriptorSetLayoutBinding in CubePipeline.cpp
                        .dstSet = descriptors, // Which descriptor set to write into 
                        .pBufferInfo = &buffer_info, // Points to a VkDescriptorBufferInfo specifying the actual buffer handle, offset, and range
                    },
                    VkWriteDescriptorSet{
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .dstArrayElement = 0,
                        .dstBinding = 1,
                        .dstSet = descriptors,
                        .pImageInfo = &image_info,
                    }
                };

                vkUpdateDescriptorSets(
                    rtg.device,
                    uint32_t(writes.size()), // count of descriptor writes
                    writes.data(),
                    0, // descriptorCopyCount
                    nullptr // pDescriptorCopies
                );
            }
        }
    }
    void destroy(RTG &rtg) {
        vkDestroyImageView(rtg.device, view, nullptr);
        view = VK_NULL_HANDLE;
        rtg.helpers.destroy_buffer(std::move(buffer));
        rtg.helpers.destroy_image(std::move(image));
    }
};

/*
### main
- configure application (RTG) in headless mode
- create cube pipeline
- create descriptor pool
- create command pool
- allocate command buffer

### run pipeline
- Bind compute pipeline
- Bind in/out descriptor sets
- vkCmdDispatchBase dispatches sz×sz×1 workgroups — one thread per output texel
*/
int main (int argc, char **argv) {
    try {
        // init RTG headless
        // configure application:
		RTG::Configuration configuration;

		configuration.application_info = VkApplicationInfo{
			.pApplicationName = "Cube Utility",
			.applicationVersion = VK_MAKE_VERSION(0,0,0),
			.pEngineName = "Unknown",
			.engineVersion = VK_MAKE_VERSION(0,0,0),
			.apiVersion = VK_API_VERSION_1_3
		};

        bool print_usage = false;

		try {
			configuration.parse(argc, argv);
		} catch (std::runtime_error &e) {
			std::cerr << "Failed to parse arguments:\n" << e.what() << std::endl;
			print_usage = true;
		}

		if (print_usage) {
			std::cerr << "Usage:" << std::endl;
			RTG::Configuration::usage( [](const char *arg, const char *desc){ 
				std::cerr << "    " << arg << "\n        " << desc << std::endl;
			});
			return 1;
		}

        configuration.headless = true;

		// loads vulkan library, creates surface, initializes helpers:
		RTG rtg(configuration); // Creates an RTG object named rtg; Passes configuration as a parameter to the constructor

        // TODO: create CubePipeline
        CubePipeline cube_pipeline;
        cube_pipeline.create(rtg);

        // TODO: load images / create descriptors
        // The pool was sized for 13 + 12 anticipating the full 6×2 face pipeline (all 12 faces + params), but the current code only creates one in_face and one out_face — it's a work in progress. The comment at line 186
        
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        { // create descriptor pool
            std::array< VkDescriptorPoolSize, 2> pool_sizes{
                VkDescriptorPoolSize{
                    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, // for the WORLD_FROM_PIXEL transform buffers
                    .descriptorCount = 6 * 2 + 1, // one for each input and output cube face, plus one for params//??A2-diffuse
                },
                VkDescriptorPoolSize{
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, // for the face images
                    .descriptorCount = 6 * 2, // one for each input and output cube face
                }
            };

            VkDescriptorPoolCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,

                /* three options for flags:
                - 0: Default — sets can only be freed by resetting/destroying the whole pool
                - VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT: Allows freeing individual descriptor sets with vkFreeDescriptorSets
                - VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT: Lets you update descriptors after binding them to a command buffer (needed for bindless) */
                .flags = 0, // never need to free individual sets, therefore use 0 instead of CREATE_FREE_DESCRIPTOR_SET_BIT, thus CANNOT free individual descriptors allocated from this pool
                .maxSets = 12 + 1, // (13 sets: 6 for in cube face + 1 for out cube face + 1 for params)
                .poolSizeCount = uint32_t(pool_sizes.size()),
                .pPoolSizes = pool_sizes.data(),
            };

            VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &descriptor_pool));
        }

        VkCommandPool command_pool = VK_NULL_HANDLE;
        { // create command pool
            VkCommandPoolCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = rtg.graphics_queue_family.value(), // safely retrieve the value stored in the std::optional object 
            };
            VK( vkCreateCommandPool(rtg.device, &create_info, nullptr, &command_pool));
        }

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        { // allocate command buffer from command pool
            VkCommandBufferAllocateInfo alloc_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = command_pool,
                .commandBufferCount = 1,

                /*level controls how the command buffer is submitted
                - VK_COMMAND_BUFFER_LEVEL_PRIMARY: Submitted directly to a queue via vkQueueSubmit
                - VK_COMMAND_BUFFER_LEVEL_SECONDARY: Cannot be submitted to a queue. Must be called from a primary buffer via vkCmdExecuteCommands. Used for multi-threading. */
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            };
            VK( vkAllocateCommandBuffers(rtg.device, &alloc_info, &command_buffer));
        }

        size_t sz = 128; // set to 128*128 pixels
        std::vector< glm::vec3 > data(sz*sz, glm::vec3(1.0f, 1.0f, 1.0f));

        // in_face and out_face each holds a descriptor set
        GPUFace in_face;
        in_face.create(rtg, descriptor_pool, cube_pipeline, sz, data.data());
        GPUFace out_face;
        out_face.create(rtg, descriptor_pool, cube_pipeline, sz, data.data());

        { // run pipeline
            /*
            write a sequence of commands into a CPU-side buffer:
            1. vkBeginCommandBuffer — puts the buffer into recording state. The driver prepares to accept commands.                                                                                                                                                             
            2. vkCmd* calls — each call writes a command into the buffer (draw, bind pipeline, copy image, etc.). Nothing runs on the GPU yet.                                                                                                                                  
            3. vkEndCommandBuffer — finalizes the buffer. No more commands can be added.                                                                                                                                                                                      
            4. vkQueueSubmit — sends the finished buffer to the GPU queue. Only now does the GPU execute the recorded commands.                                                                                                                                               
            */
            // Bind compute pipeline
            VK( vkResetCommandBuffer(command_buffer, 0) ); // reset the command buffer (clear old commands)
            { // begin recording
                VkCommandBufferBeginInfo begin_info{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, //will record again every submit
                };
                VK( vkBeginCommandBuffer(command_buffer, &begin_info));
            }

            // use the cube pipeline
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cube_pipeline.handle);

            { // Bind in/out descriptor sets
                std::array<VkDescriptorSet, 2> descriptor_sets{
                    in_face.descriptors,
                    out_face.descriptors,
                    /*params_descriptors, */ // TODO - when will we need it?
                };
                vkCmdBindDescriptorSets(
					command_buffer, //command buffer
					VK_PIPELINE_BIND_POINT_COMPUTE, //pipeline bind point
					cube_pipeline.layout, //pipeline layout
					0, //first set
					uint32_t(descriptor_sets.size()), descriptor_sets.data(), //descriptor sets count, ptr
                    0, nullptr // dynamic offsets count, ptr
                );
            }

            // vkCmdDispatchBase, a compute dispatch command, dispatches a sz×sz×1 workgroup grid — one thread per output texel; actually run the thing
            vkCmdDispatchBase(command_buffer, 0, 0, 1, sz, sz, 1); // Dispatch a grid of workgroups, IDs start at (0, 0, 1)

            // done recording
            VK( vkEndCommandBuffer(command_buffer) );

            { // submit command buffer
                VkSubmitInfo submit_info{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &command_buffer,
                };
                VK( vkQueueSubmit(rtg.graphics_queue, 1, &submit_info, nullptr) );
            }

            VK( vkDeviceWaitIdle(rtg.device) ); // blocks the CPU until all operations on all queues of the device have finished.
        }

        std::cout << "Computing: done." << std::endl;

        // TODO: get back results
        // TOOD: destroy all the things
        in_face.destroy(rtg);
        out_face.destroy(rtg);

        vkDestroyDescriptorPool(rtg.device, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;

        vkDestroyCommandPool(rtg.device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;

        cube_pipeline.destroy(rtg);

    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}