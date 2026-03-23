#include "RTG.hpp"

// Credit: adapted from Zulip discussion https://15-472-s26.zulipchat.com/#narrow/channel/570157-A2/topic/Adding.20Cube.20Utility.20to.20Maekfile/with/575174040
struct CubePipeline {
    // descriptor set layouts:
    /*. set01_face is the layout shared by two descriptor sets (one for in_face, one for out_face), 
    because input and output faces have identical structure
    
    Layout:                                                                                                                                                                                                              
    binding 0 → VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER   (WORLD_FROM_PX matrix)                                                                                                                                               
    binding 1 → VK_DESCRIPTOR_TYPE_STORAGE_IMAGE    (face pixel data)     
    
    When creating the pipeline layout: std::array< VkDescriptorSetLayout, 3 > layouts{
            set01_face,
            set01_face,
            set2_params,
        };
    */
    enum class Mode { Lambertian, GGX };

    VkDescriptorSetLayout set01_face = VK_NULL_HANDLE;
	VkDescriptorSetLayout set2_params = VK_NULL_HANDLE; //used for ggx only

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

    struct Params {
		float roughness;
	};
	static_assert(sizeof(Params) == 4, "Params descriptor is the expected size.");

    VkPipelineLayout layout = VK_NULL_HANDLE;

    VkPipeline handle = VK_NULL_HANDLE;

    void create(RTG &, Mode mode = Mode::Lambertian);
    void destroy(RTG &);
};