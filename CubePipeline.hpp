#include "RTG.hpp"

struct CubePipeline {
    // descriptor set layouts:
    VkDescriptorSetLayout set01_face = VK_NULL_HANDLE;

    // types for descriptors:
    struct Face
    {   
        /*
        Each cubemap face is a square image of sz × sz pixels. 
        For every pixel (px, py), you need a 3D world-space direction vector to know what part of the environment that pixel captures. 
        world_dir = WORLD_FROM_PX * vec3(px, py, 1)
        */
        struct {
            float m0, m1, m2, padding0_; // per-pixel
            float m3, m4, m5, padding1_; // per-pixel
			float m6, m7, m8, padding2_; // translation/origin
        } WORLD_FROM_PX;
    };
    static_assert(sizeof(Face) == (3*4)*4, "Face descriptor structure is the expected size");

    // no push constants

    VkPipelineLayout layout = VK_NULL_HANDLE;

    VkPipeline handle = VK_NULL_HANDLE;

    void create(RTG &);
    void destroy(RTG &);
};