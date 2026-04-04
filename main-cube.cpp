#include "RTG.hpp"
#include "VK.hpp"
#include "CubePipeline.hpp"
#include "RGBE.hpp"

#include <glm/glm.hpp>

// we need this #define because stb_image also something like "#ifdef STB_IMAGE_IMPLEMENTATION ..."
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
// suppress the 'sprintf' is deprecated warning
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "stb_image_write.h"
#pragma clang diagnostic pop

#include <iostream>

// Credit: adapted from Zulip discussion https://15-472-s26.zulipchat.com/#narrow/channel/570157-A2/topic/Adding.20Cube.20Utility.20to.20Maekfile/with/575174040

// Wraps one cubemap face on the GPU: an R32G32B32A32_SFLOAT storage image + a uniform buffer with the WORLD_FROM_PX transform matrix, plus descriptor set binding both
struct GPUFace {
    Helpers::AllocatedImage image;
    Helpers::AllocatedBuffer buffer;
    VkImageView view;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    void create(RTG& rtg, VkDescriptorPool descriptor_pool, CubePipeline const &cube_pipeline, uint32_t const sz, glm::vec3 * const data, int face_i, bool is_input = false, uint32_t mip_levels = 1) {
        // add a 4th component to the data vector
        std::vector< glm::vec4 > data_padded;
        data_padded.reserve(sz*sz);
        for (uint32_t i = 0; i < sz*sz; ++i) {
            data_padded.emplace_back(glm::vec4(data[i], 0.0f));
        }

        // create image
        image = rtg.helpers.create_image(
            VkExtent2D{.width = sz, .height = sz},
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_TILING_OPTIMAL,
            is_input
                ? (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) // sampled for textureLod; TRANSFER_DST for mip 0 upload; TRANSFER_SRC for blitting down to lower mips
                : (VK_IMAGE_USAGE_STORAGE_BIT  | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT), // storage for compute write; TRANSFER_SRC for readback
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            Helpers::Unmapped,
            0,          // flags
            1,          // arrayLayers
            mip_levels  // mip levels: 1 for output faces; floor(log2(sz))+1 for input faces (GGX mode)
        );

        // upload mip 0; transfer_to_image will leave it in SHADER_READ_ONLY_OPTIMAL (input) or GENERAL (output)
        rtg.helpers.transfer_to_image(data_padded.data(), sizeof(data_padded[0]) * sz * sz, image, 1,
            is_input ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);

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
            // https://github.com/15-472/s72/blob/main/examples/env-cube.png
            static const glm::vec3 face_s[6] = { // what does static mean? why use it? // right across the face
               {0, 0, -1}, // looking toward +x, horizontal axis points −Z, vertical axis points −Y; s = (0, 0, -1), meaning increasing pixel column moves in the −Z direction.
               {0, 0, 1}, // toward -x
               {1, 0, 0}, // +y
               {1, 0, 0}, // -y
               {1, 0, 0}, // +z
               {-1, 0, 0}, // -z
            };

            static const glm::vec3 face_t[6] = { // the "down" axis across the face in world space.
                {0, -1, 0}, // +x face; t = (0, -1, 0), meaning increasing pixel row moves in the −Y direction.
                {0, -1, 0},
                {0, 0, 1},
                {0, 0, -1},
                {0, -1, 0},
                {0, -1, 0},
            };

            // the direction the face "looks at" (the face normal); direction pointing straight out of the cube face
            static const glm::vec3 face_center[6] = {
                {1, 0, 0}, // +x face
                {-1, 0, 0},
                {0, 1, 0},
                {0, -1, 0},
                {0, 0, 1},
                {0, 0, -1},
            };

            glm::vec3 s = face_s[face_i];
            glm::vec3 t = face_t[face_i];
            glm::vec3 center = face_center[face_i];

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
                .flags = 0,
                .format = image.format,
                .image = image.handle,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                    .baseMipLevel = 0,
                    .levelCount = mip_levels, // expose full mip chain for input faces so textureLod can access all levels
                },
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
            };
            VK( vkCreateImageView(rtg.device, &create_info, nullptr, &view));
        }

        { // descriptor set with world_from_px and image binding
            VkDescriptorSetLayout layout = is_input ? cube_pipeline.set0_in_face : cube_pipeline.set1_out_face;
            { // allocate
                VkDescriptorSetAllocateInfo alloc_info{
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool = descriptor_pool,
                    .descriptorSetCount = 1,
                    .pSetLayouts = &layout,
                };
                VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &descriptors ) );
            }
            { // write
                VkDescriptorBufferInfo buffer_info{
                    .buffer = buffer.handle,
                    .offset = 0,
                    .range = buffer.size,
                };
                VkDescriptorImageInfo image_info{
                    .sampler     = is_input ? cube_pipeline.in_sampler : VK_NULL_HANDLE,
                    .imageView   = view,
                    .imageLayout = is_input ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                };

                std::array< VkWriteDescriptorSet, 2> writes{
                    VkWriteDescriptorSet{
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .dstArrayElement = 0,
                        .dstBinding = 0,
                        .dstSet = descriptors,
                        .pBufferInfo = &buffer_info,
                    },
                    VkWriteDescriptorSet{
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .descriptorCount = 1,
                        .descriptorType = is_input ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .dstArrayElement = 0,
                        .dstBinding = 1,
                        .dstSet = descriptors,
                        .pImageInfo = &image_info,
                    }
                };

                vkUpdateDescriptorSets(rtg.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
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
- vkCmdDispatchBase dispatches workgroups — one thread per output texel
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

        // create CubePipeline — mode is GGX if --ggx was passed, otherwise Lambertian
        CubePipeline::Mode mode = !configuration.ggx_out_file.empty() ? CubePipeline::Mode::GGX : CubePipeline::Mode::Lambertian;
        CubePipeline cube_pipeline;
        cube_pipeline.create(rtg, mode);

        // Load image using stb_image
        if (configuration.in_file.empty())
            throw std::runtime_error("No input file given. Usage: cube in.png --lambertian out.png OR cube in.png --ggx out.png");
        if (configuration.lambertian_out_file.empty() && configuration.ggx_out_file.empty())
            throw std::runtime_error("No output file given. Usage: cube in.png --lambertian out.png OR cube in.png --ggx out.png");

        int in_w, in_h, channels;
        /* c_str returns a const char* pointer to the underlying null-terminated C string of a std::string
        std::string s = "hello";
        const char* p = s.c_str();  // p points to "hello\0"  */
        unsigned char* in_data = stbi_load(configuration.in_file.c_str(), &in_w, &in_h, &channels, 4);

        if (!in_data) {
            throw std::runtime_error("WARNING: Failed to load cubemap from \"" + configuration.in_file + "\": " + stbi_failure_reason());
        }

        if (in_h != in_w * 6) {  // check if this is 6 face stacked vertically
            stbi_image_free(in_data);
            throw std::runtime_error("WARNING: Invalid cubemap from \"" + configuration.in_file + "\". Expected height == 6 * width, got: in_h=" + std::to_string(in_h) + ", in_w=" + std::to_string(in_w));
        }

        std::cout << "Loaded " << configuration.in_file
                  << " (" << in_w << "x" << in_h
                  << "px per face)" << std::endl;

        // size_t sz = 128; // set to 128*128 pixels
        size_t sz = in_w;
        constexpr int LAMBERTIAN_OUT_SIZE = 16; // writeup: having an edge length of 16 pixels is reasonable. TODO: size cannot be fixed for different mip levels

        // For GGX: first mip level after the base level; lowest non-zero roughness; half the side length of the input cube
        // number of mip levels N = floor(log2(sz)). -> 2^N = sz.
        //      sz    1 2 4
        // level(N)   0 1 2
        // Mip m has out_size = sz >> m = sz / (2^m) and roughness = m / N.
        // level(m)   0    1      2       N
        //   out_sz   sz   sz/2   sz/4    sz/(2^N)
        //    rough   0    1/N    2/N     1
        // Mip 1 (lowest non-zero roughness, half res) through mip N (roughness=1, 1x1).                                                                                                          
        int ggx_mip_count = 0;                                                                                                                                                                    
        if (mode == CubePipeline::Mode::GGX) {                                                                                                                                                    
            int sz_step = sz;                                                                                                                                                                           
            while (sz_step > 1) { sz_step >>= 1; ggx_mip_count++; }                                                                                                                                           
            if (ggx_mip_count == 0) {                                                                                                                                                             
                stbi_image_free(in_data);                                                                                                                                                         
                throw std::runtime_error("Input cubemap too small for GGX mip generation. Need at least 2x2 per face).");                                                                         
            }                                                                                                                                                                                     
            std::cout << "GGX: will generate " << ggx_mip_count << " mip levels." << std::endl;                                                                                              
        }

        // mip levels for input faces: full chain for GGX (textureLod needs it), just 1 for Lambertian (texelFetch at lod=0)
        uint32_t in_mip_levels = 1;
        if (mode == CubePipeline::Mode::GGX) {
            uint32_t s = sz;
            while (s > 1) { s >>= 1; in_mip_levels++; }
        }

        // create descriptors
        VkDescriptorPool in_descriptor_pool = VK_NULL_HANDLE; // pool for input faces
        { // create descriptor pool for input faces
            std::array< VkDescriptorPoolSize, 2> pool_sizes{
                VkDescriptorPoolSize{
                    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .descriptorCount = 6,
                },
                VkDescriptorPoolSize{
                    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // sampler2D: supports textureLod for mip-level bias fix
                    .descriptorCount = 6,
                }
            };

            VkDescriptorPoolCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,

                /* three options for flags:
                - 0: Default — sets can only be freed by resetting/destroying the whole pool
                - VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT: Allows freeing individual descriptor sets with vkFreeDescriptorSets
                - VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT: Lets you update descriptors after binding them to a command buffer (needed for bindless) */
                .flags = 0, // never need to free individual sets, therefore use 0 instead of CREATE_FREE_DESCRIPTOR_SET_BIT, thus CANNOT free individual descriptors allocated from this pool
                .maxSets = 6,
                .poolSizeCount = uint32_t(pool_sizes.size()),
                .pPoolSizes = pool_sizes.data(),
            };

            VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &in_descriptor_pool));
        }

        /* A2-pbr
        Create pool for output faces + params. 
        Originally in faces, out faces, and params are all in one pool. I split it to handle mip map.
        mip loop need to reset the descriptor to reuse the same descriptor set slots for output faces for each mip level.
        if everything were in one pool, resetting the descriptor pool will lose the input face descriptor sets.
        Now in_descriptor_pool never resets. out_descriptor_pool resets at the start of each mip level.
        */
        VkDescriptorPool out_descriptor_pool = VK_NULL_HANDLE;
        { // create descriptor pool for output faces + params
            std::array< VkDescriptorPoolSize, 2> pool_sizes{
                VkDescriptorPoolSize{
                    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, // for the WORLD_FROM_PIXEL transform buffers
                    .descriptorCount = 6 + 1, // 6 out face UBOs (WORLD_FROM_OUT_PX)+ 1 param UBO (roughness)
                },
                VkDescriptorPoolSize{
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, // for the face images
                    .descriptorCount = 6, // 6 storage images
                }
            };

            VkDescriptorPoolCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,

                /* three options for flags:
                - 0: Default — sets can only be freed by resetting/destroying the whole pool
                - VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT: Allows freeing individual descriptor sets with vkFreeDescriptorSets
                - VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT: Lets you update descriptors after binding them to a command buffer (needed for bindless) */
                .flags = 0, // never need to free individual sets, therefore use 0 instead of CREATE_FREE_DESCRIPTOR_SET_BIT, thus CANNOT free individual descriptors allocated from this pool
                .maxSets = 6 + 1,
                .poolSizeCount = uint32_t(pool_sizes.size()),
                .pPoolSizes = pool_sizes.data(),
            };

            VK( vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &out_descriptor_pool));
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

        /*
        Originally all input and output faces were created at once during A2-diffuse
        I split it for A2-pbr because input faces always have the same size and image data, but output faces change for every mip level
        */
        std::array<GPUFace, 6> in_faces; // create input faces
        std::array<GPUFace, 6> out_faces; // create output faces
        for (int f = 0; f < 6; f++) {
            std::vector< glm::vec3 > data(sz * sz);
            
            int row_start = f * in_w;
            int row_end = (f + 1) * in_w;
            for (int r = row_start; r < row_end; r++) {
                for (int c = 0; c < in_w; c++) {
                    /*
                    stbi_load(..., 4) loads the image as a flat array of bytes, row by row, 4 bytes per pixel (RGBA):                                                                                                                                                                                               
                    [ R G B A | R G B A | R G B A | ... ]                                                                                                                                                                                                                                                           
                      pixel 0   pixel 1   pixel 2 
                    RGBA values in range[0, 255]
                    */
                    unsigned char* px = in_data + (r * in_w + c) * 4; // points to the first byte of one pixel; 4 is the offset in byte
                    // data[(r - row_start) * in_w + c] = glm::vec3( // BUG: input files are RGBE-encoded, need to use rgbe_to_float
                    //     px[0] / 255.0f, 
                    //     px[1] / 255.0f,
                    //     px[2] / 255.0f
                    // );
                    data[(r - row_start) * in_w + c] = rgbe_to_float(glm::u8vec4(px[0], px[1], px[2], px[3])); // then read the next consecutive bytes (ignore A) & convert RGB from [0, 255] to [0, 1]
                }
            }

            in_faces[f].create(rtg, in_descriptor_pool, cube_pipeline, sz, data.data(), f, /*is_input=*/true, in_mip_levels);
        }
        stbi_image_free(in_data);
        std::cout << "Created " << in_faces.size() << " input faces (" << in_mip_levels << " mip levels)" << std::endl;

        // Generate mipmaps for input faces (GL_generateMipmap equivalent)
        // Only needed for GGX: Lambertian uses texelFetch at lod=0 so mip chain isn't needed
        if (in_mip_levels > 1) {
            VK( vkResetCommandBuffer(command_buffer, 0) );
            VkCommandBufferBeginInfo begin_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            VK( vkBeginCommandBuffer(command_buffer, &begin_info) );

            for (int f = 0; f < 6; f++) {
                VkImage img = in_faces[f].image.handle;

                // Mip 0 was left in SHADER_READ_ONLY_OPTIMAL by transfer_to_image; transition it to TRANSFER_SRC to use as blit source
                VkImageMemoryBarrier to_src{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = img,
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                };
                vkCmdPipelineBarrier(command_buffer,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &to_src);

                for (uint32_t mip = 1; mip < in_mip_levels; mip++) {
                    int32_t src_w = (int32_t)std::max(1u, (uint32_t)sz >> (mip - 1));
                    int32_t dst_w = (int32_t)std::max(1u, (uint32_t)sz >> mip);

                    // Transition destination mip from UNDEFINED → TRANSFER_DST
                    VkImageMemoryBarrier to_dst{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = 0,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = img,
                        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 },
                    };
                    vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &to_dst);

                    // Blit mip-1 → mip (half resolution, linear filter)
                    VkImageBlit blit{
                        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1 },
                        .srcOffsets = { {0, 0, 0}, {src_w, src_w, 1} },
                        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 },
                        .dstOffsets = { {0, 0, 0}, {dst_w, dst_w, 1} },
                    };
                    vkCmdBlitImage(command_buffer,
                        img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit, VK_FILTER_LINEAR);

                    // Transition mip-1 from TRANSFER_SRC → SHADER_READ_ONLY (done with it as blit source)
                    VkImageMemoryBarrier src_done{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = img,
                        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, 1 },
                    };
                    vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &src_done);

                    // Transition mip from TRANSFER_DST → TRANSFER_SRC for next iteration
                    VkImageMemoryBarrier dst_to_src{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = img,
                        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 },
                    };
                    vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &dst_to_src);
                }

                // Transition last mip from TRANSFER_SRC → SHADER_READ_ONLY
                VkImageMemoryBarrier last_done{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = img,
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, in_mip_levels - 1, 1, 0, 1 },
                };
                vkCmdPipelineBarrier(command_buffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &last_done);
            }

            VK( vkEndCommandBuffer(command_buffer) );
            VkSubmitInfo si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command_buffer };
            VK( vkQueueSubmit(rtg.graphics_queue, 1, &si, VK_NULL_HANDLE) );
            VK( vkQueueWaitIdle(rtg.graphics_queue) );
            std::cout << "Generated mipmaps for all 6 input faces." << std::endl;
        }

        Helpers::AllocatedBuffer params_buffer;
        VkDescriptorSet params_descriptors = VK_NULL_HANDLE;
        // params buffer and descriptors
            if (mode == CubePipeline::Mode::GGX) {
                params_buffer = rtg.helpers.create_buffer(
                    sizeof(CubePipeline::Params),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    Helpers::Unmapped
                );
            }

            std::string out_base = (mode == CubePipeline::Mode::GGX) ? configuration.ggx_out_file : configuration.lambertian_out_file;
            int loop_start = (mode == CubePipeline::Mode::GGX) ? 1 : 0;
            int loop_end = (mode == CubePipeline::Mode::GGX) ? ggx_mip_count : 0;

            for (int mip = loop_start; mip <= loop_end; mip++) {
                // calc output size and roughness for every iteration
                int out_size;
                float roughness;
                if (mode == CubePipeline::Mode::GGX) {
                    out_size = sz >> mip;
                    roughness = float(mip) / (float)ggx_mip_count;
                } else {
                    out_size = LAMBERTIAN_OUT_SIZE;
                    roughness = 0.f;
                }

                // reset descriptor pool so that its slots can be reused for every iteration's out faces and params
                VK( vkResetDescriptorPool(rtg.device, out_descriptor_pool, 0));

                for (int f = 0; f < 6; f++) {
                    std::vector<glm::vec3> zero(out_size * out_size, glm::vec3(0.0f));
                    out_faces[f].create(rtg, out_descriptor_pool, cube_pipeline, out_size, zero.data(), f);
                }

                if (mode == CubePipeline::Mode::GGX) { // descriptor set with the params uniform
                    CubePipeline::Params params{ .roughness = roughness };
                    rtg.helpers.transfer_to_buffer(&params, sizeof(params), params_buffer);

                    { // allocate
                        VkDescriptorSetAllocateInfo alloc_info{
                            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                            .descriptorPool = out_descriptor_pool,
                            .descriptorSetCount = 1,
                            .pSetLayouts = &cube_pipeline.set2_params,
                        };
                        VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &params_descriptors) );
                    }
                    { //write               
                        VkDescriptorBufferInfo buffer_info{
                            .buffer = params_buffer.handle,
                            .offset = 0,
                            .range = params_buffer.size,
                        };
                        VkWriteDescriptorSet write{
                            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .descriptorCount = 1,
                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                            .dstArrayElement = 0, // Starting array index within the binding — 0 since this isn't an array descriptor
                            .dstBinding = 0, // corresponds to the binding = N number delcared in shader AND in the VkDescriptorSetLayoutBinding in CubePipeline.cpp
                            .dstSet = params_descriptors, // write to this descriptor set
                            .pBufferInfo = &buffer_info,
                        };
                        vkUpdateDescriptorSets(rtg.device, 1, &write, 0, nullptr);
                    }
                }

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

            // use the cube pipeline; tells GPU which shader to use
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cube_pipeline.handle);

            { // Bind in/out descriptor sets
                for (int out_f = 0; out_f < 6; out_f++) {
                    for (int in_f = 0; in_f < 6; in_f++) { // each out_face integrates over all incoming directions above the surface, thus need to sample ALL 6 in_faces
                        // Lambertian needs 2 sets (in_face, out_face).
                        // GGX needs 3 sets (in_face, out_face, params with roughness).
                        std::vector<VkDescriptorSet> descriptor_sets = {
                            in_faces[in_f].descriptors,
                            out_faces[out_f].descriptors,
                        };
                        if (mode == CubePipeline::Mode::GGX) {
                            descriptor_sets.push_back(params_descriptors);
                        }

                        vkCmdBindDescriptorSets( // tells GPU which images/buffers the shader reads/writes
                            command_buffer,                                           // command buffer
                            VK_PIPELINE_BIND_POINT_COMPUTE,                           // pipeline bind point
                            cube_pipeline.layout,                                     // pipeline layout
                            0,                                                        // first set
                            uint32_t(descriptor_sets.size()), descriptor_sets.data(), // descriptor sets count, ptr
                            0, nullptr                                                // dynamic offsets count, ptr
                        );

                        /* a compute dispatch command, dispatches a sz×sz×1 workgroup grid — one thread per output texel; actually run the thing

                        in compute shader: layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in; // defining the size of the local invocations per dimension in the compute shader
                        total invocations = dispatch dimensions × local_size dimensions                                                                                                                                                                                        
                            = (sz × sz × 1)  ×  (1 × 1 × 1)                                                                                                                                                                                                      
                            = sz × sz
                        
                        One invocation = one run of void main() in the shader, responsible for one output pixel.
                        
                        in our settings:
                        - vkCmdDispatch(sz, sz, 1) → launches sz × sz workgroups
                        - local_size = (1, 1, 1) → each workgroup has 1 thread
                        - Total threads = sz × sz × 1 = one thread per output pixel

                        When dispatch sz × sz workgroups, the GPU runs void main() sz × sz times *in parallel*, each time with a different gl_GlobalInvocationID. That ID is what tells each invocation which pixel it owns.
                        */
                        vkCmdDispatchBase(command_buffer, 0, 0, 1, out_size, out_size, 1); // Dispatch out_size * out_size workgroups, IDs start at (0, 0, 1);

                        // barrier: ensure write from this dispatch is visible to next dispatch
                        // needed because shader reads accumulated values from all prev faces
                        VkMemoryBarrier barrier{
                            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        };
                        vkCmdPipelineBarrier(
                            command_buffer,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // srcStageMask: wait until this dispatch's compute shader stage has finished writing
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // dstStageMask: before the next dispatch's compute shader stage is allowed to start reading
                            0, // dependencyFlags,
                            1, &barrier, // memoryBarriers (count, data)
                            0, nullptr, // bufferMemoryBarriers (count, data)
                            0, nullptr // imageMemoryBarriers (count, data)
                        );
                    }
                }
            }

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

        if (mode == CubePipeline::Mode::GGX) {
            std::cout << "Mip " << mip << "/" << ggx_mip_count << " (roughness=" << roughness << ", size=" << out_size << "): computing done." << std::endl;
        } else {
            std::cout << "Computing: done." << std::endl;
        }

        // VkDeviceSize face_size = sz * sz * sizeof(glm::vec4); // bug: out_face are out_size*out_size; sz*sz is in_face size
        VkDeviceSize face_size = (VkDeviceSize)out_size * out_size * sizeof(glm::vec4);
        Helpers::AllocatedBuffer out_pixels_buffer = rtg.helpers.create_buffer(
            face_size * 6,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            Helpers::Mapped
        );
        { // get back results
            // Bind compute pipeline
            VK( vkResetCommandBuffer(command_buffer, 0) ); // reset the command buffer (clear old commands)
            { // begin recording
                VkCommandBufferBeginInfo begin_info{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, //will record again every submit
                };
                VK( vkBeginCommandBuffer(command_buffer, &begin_info));
            }

            for (int f = 0; f < 6; f++) {
                VkBufferImageCopy region{
                    .bufferOffset = f * face_size,
                    .bufferRowLength = 0, // means tightly packed
                    .bufferImageHeight = 0,
                    .imageSubresource{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                    .imageOffset{.x = 0, .y = 0, .z = 0},
                    .imageExtent{
                        .width = (uint32_t)out_size,
                        .height = (uint32_t)out_size,
                        .depth = 1},
                };
                vkCmdCopyImageToBuffer(
                    command_buffer, // buffer
                    out_faces[f].image.handle, // src
                    VK_IMAGE_LAYOUT_GENERAL, 
                    out_pixels_buffer.handle, 
                    1, &region
                );
            }

            // Submit command buffer
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

            { // convert float dat to RGBE and write PNG
                // out_pixels_buffer.allocation.data() was written by vkCmdCopyImageToBuffer, which copies out_face images into this buffer.
                // it holds 6 faces × out_size × out_size pixels, each pixel as glm::vec4
                glm::vec4* out_pixels_floats = reinterpret_cast<glm::vec4*>(out_pixels_buffer.allocation.data());
                int out_w = out_size;
                int out_h = out_size * 6;
                std::vector<glm::u8vec4> rgbe_out(out_w * out_h);
                for (int i = 0; i < (int)rgbe_out.size(); i++) {
                    glm::vec4 p = out_pixels_floats[i];
                    glm::vec3 color;
                    if (mode == CubePipeline::Mode::GGX) {
                        // rgb holds prefiltered_color = sum(Li * NdotL) across all 6 face passes.
                        // alpha holds total_weight = sum(NdotL) across all 6 face passes.
                        // do prefiltered_color/total_weight to normalize.
                        color = (p.a > 0.0f) ? glm::vec3(p) / p.a : glm::vec3(0.0f);
                    } else {
                        color = glm::vec3(p);
                    }
                    rgbe_out[i] = float_to_rgbe(color);
                }

                std::string out_file;
                // out_base will look like "filename.png"
                if (mode == CubePipeline::Mode::GGX) { // need to convert to "filename.N.png"
                    size_t dot_pos = out_base.rfind('.'); // what is .rfind()?
                    out_file = out_base.substr(0, dot_pos) + "." + std::to_string(mip) + out_base.substr(dot_pos);
                } else {
                    out_file = out_base;
                }

                int write_ok = stbi_write_png(
                    out_file.c_str(), out_w, out_h,
                    4, // channel (RGBA)
                    rgbe_out.data(),
                    out_w * 4 // stride = # of bytes/row = (# of pixels/row) * (# of bytes/pixel) = out_w * 4
                );
                if (!write_ok) throw std::runtime_error("stbi_write_png failed for '" + out_file + "'.");
            }
        }
        rtg.helpers.destroy_buffer(std::move(out_pixels_buffer));
        for (auto &f : out_faces) f.destroy(rtg);
        // descriptor sets for out_faces and params_descriptors were allocated from out_descriptor_pool
        // and will be freed when the pool is reset at the start of the next iteration (or destroyed at cleanup)
        }

        // destroy all the things
        for (auto &in_face : in_faces) in_face.destroy(rtg);

        if (params_buffer.handle != VK_NULL_HANDLE) {
            rtg.helpers.destroy_buffer(std::move(params_buffer));
        }

        vkDestroyDescriptorPool(rtg.device, in_descriptor_pool, nullptr);
        vkDestroyDescriptorPool(rtg.device, out_descriptor_pool, nullptr);
        in_descriptor_pool = VK_NULL_HANDLE;
        out_descriptor_pool = VK_NULL_HANDLE;

        vkDestroyCommandPool(rtg.device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;

        cube_pipeline.destroy(rtg);

    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}