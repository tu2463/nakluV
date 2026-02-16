#pragma once

#include "PosColVertex.hpp"
#include "PosNorTexVertex.hpp"
#include "mat4.hpp"
#include "vec3.hpp"

#include "RTG.hpp"
#include "S72.hpp"

struct Tutorial : RTG::Application {

	Tutorial(RTG &, S72 &);
	Tutorial(Tutorial const &) = delete; //you shouldn't be copying this object
	~Tutorial();

	//kept for use in destructor:
	RTG &rtg;
	S72 &s72;

	//--------------------------------------------------------------------
	//Resources that last the lifetime of the application:

	//chosen format for depth buffer:
	VkFormat depth_format{};
	//Render passes describe how pipelines write to images:
	VkRenderPass render_pass = VK_NULL_HANDLE;

	//Pipelines:
	struct BackgroundPipeline {
		// no descriptor set layouts

		// push constants
		struct Push {
			float time;
		};

		VkPipelineLayout layout = VK_NULL_HANDLE;
		 // no vertex bindings

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} background_pipeline;

	struct LinesPipeline {
		// descriptor set layouts:
		VkDescriptorSetLayout set0_Camera = VK_NULL_HANDLE;

		// types for descriptors:
		struct Camera {
			mat4 CLIP_FROM_WORLD;
		};
		static_assert(sizeof(Camera) == 16 * 4, "Camera buffer structure is packed");

		// no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;
		
		// vertex bindings:
		using Vertex = PosColVertex;

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} lines_pipeline; //?? what does the last line in this struct syntax mean?

	struct ObjectsPipeline {
		// descriptor set layouts:
		VkDescriptorSetLayout set0_World = VK_NULL_HANDLE;
		VkDescriptorSetLayout set1_Transforms;
		VkDescriptorSetLayout set2_TEXTURE;

		// types for descriptors:
		struct World {
			struct { float x, y, z, padding_; } SKY_DIRECTION; // padding is require by the std140 layout, which aligns vec3s on 4-element boundaries; what is std140 //??
			struct { float r, g, b, padding_; } SKY_ENERGY;
			struct { float x, y, z, padding_; } SUN_DIRECTION;
			struct { float r, g, b, padding_; } SUN_ENERGY;
		};
		static_assert(sizeof(World) == 4*4 + 4*4 + 4*4 + 4*4, "World is the expected size.");

		struct Transform {
			mat4 CLIP_FROM_LOCAL; // from object's local space to clip space, for gl_Position
			mat4 WORLD_FROM_LOCAL; // from local positions to world space, for positions (lighting calculations); Where the object IS in the world (position + orientation)
			mat4 WORLD_FROM_LOCAL_NORMAL; // for normals = transpose(inverse(WORLD_FROM_LOCAL))
		};
		static_assert(sizeof(Transform) == 16*4 + 16*4 + 16*4, "Transform is the expected size.");

		// no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;
		
		// vertex bindings:
		// using Vertex = PosNorTexVertex; // used for tutorial code before S72 loader
		using Vertex = PosNorTexTanVertex;

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} objects_pipeline;

	//pools from which per-workspace things are allocated:
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

	//workspaces hold per-render resources:
	struct Workspace {
		VkCommandBuffer command_buffer = VK_NULL_HANDLE; //from the command pool above; reset at the start of every render.

		// location for lines data: (streamed to GPU per-frame)
		Helpers::AllocatedBuffer lines_vertices_src; // hose corehent; mapped
		Helpers::AllocatedBuffer lines_vertices; //device-local
		
		// location for LinesPipeline::Camera data: (streamed to GPU per-frame)
		Helpers::AllocatedBuffer Camera_src; // host coherent; mapped
		Helpers::AllocatedBuffer Camera; // device-local
		VkDescriptorSet Camera_descriptors; // references Camera

		// location for ObjectsPipeline::World data: (streamed to GPU per-frame)
		Helpers::AllocatedBuffer World_src; // host coherent; mapped
		Helpers::AllocatedBuffer World; // device-local
		VkDescriptorSet World_descriptors; // the descriptor set, references World
		
		// we'll need a descriptor set and a buffer to point it at.
		// We'll stream the transformations per-frame, so we'll define them per workspace
		// location for ObjectsPipeline::Transforms data: (streamed to GPU per-frame)
		Helpers::AllocatedBuffer Transforms_src; // host coherent; mapped
		Helpers::AllocatedBuffer Transforms; // device-local
		VkDescriptorSet Transforms_descriptors; // references Transforms, the descriptor set
	};
	std::vector< Workspace > workspaces;

	//-------------------------------------------------------------------
	//static scene resources:

	Helpers::AllocatedBuffer object_vertices; // why don't we want this to be per workspace? why are lines_vertices per workspace //vv because objects are static, can share among workspaces
	// struct ObjectVertices {
	// 	uint32_t first = 0;
	// 	uint32_t count = 0;
	// };
	// ObjectVertices sphere_vertices;
	// ObjectVertices torus_vertices;

	std::vector< Helpers::AllocatedImage > textures; // holds actual image data
	std::vector< VkImageView > texture_views;
	VkSampler texture_sampler = VK_NULL_HANDLE; // gives the sampler state (wrapping, interpolation, etc)
	VkDescriptorPool texture_descriptor_pool = VK_NULL_HANDLE; // from which we allocate texture descriptor sets
	std::vector< VkDescriptorSet > texture_descriptors; // allocated from texture_descriptor_pool; includes a descriptor for each of our textures.
	std::unordered_map< S72::Texture*, uint32_t > texture_index_map; // maps S72 texture pointers to texture indices
	std::unordered_map< S72::Material*, uint32_t > material_albedo_map; // maps materials to their albedo texture index

	//--------------------------------------------------------------------
	//Resources that change when the swapchain is resized:

	virtual void on_swapchain(RTG &, RTG::SwapchainEvent const &) override;

	Helpers::AllocatedImage swapchain_depth_image;
	VkImageView swapchain_depth_image_view = VK_NULL_HANDLE;
	std::vector< VkFramebuffer > swapchain_framebuffers;
	//used from on_swapchain and the destructor: (framebuffers are created in on_swapchain)
	void destroy_framebuffers();

	//--------------------------------------------------------------------
	//Resources that change when time passes or the user interacts:

	virtual void update(float dt) override;
	virtual void on_input(InputEvent const &) override;

	//modal action, intercepts inputs:
	// an event handling function that gets all input until cancelled
	std::function< void(InputEvent const &) > action;

	float time = 0.0f;

	//for selecting between cameras:
	enum class CameraMode {
		Scene = 0,
		User = 1, // previously called "Free" in tutorial
		Debug = 2,
	} camera_mode = CameraMode::User;

	struct SceneCamera {
		S72::Camera *camera; // reference to the camera data for this object, which includes projection (vfov, aspect, near, far)   
		mat4 WORLD_FROM_LOCAL; // is this optional //?? for scene camera's world position/orientation 
	};
	std::vector< SceneCamera > scene_camera_instances;
	uint8_t active_scene_camera = 0; // index into scene_camera_instances of the currently active camera, used when camera_mode == CameraMode::Scene

	struct OrbitCamera {
		float target_x = 0.0f, target_y = 0.0f, target_z = 0.0f; //where the camera is looking + orbiting
		float radius = 2.0f; //distance from camera to target
		float azimuth = 0.0f; //counterclockwise angle around z axis between x axis and camera direction (radians)
		float elevation = 0.25f * float(M_PI); //angle up from xy plane to camera direction (radians)

		float fov = 60.0f / 180.0f * float(M_PI); //vertical field of view (radians)
		float near = 0.1f; //near clipping plane
		float far = 1000.0f; //far clipping plane
	} free_camera;

	OrbitCamera debug_camera; // TODO: increase usefulness by etting the debug camera to a position that can see the whole scene
	// std::variant< SceneCamera, OrbitCamera > culling_camera = free_camera; // previously active camera // FIXED-BUG: can't save a pointer to 2 types, so save the prev mode and mode and CLIP_FROM_WORLD to compute fructum
  	mat4 CLIP_FROM_WORLD_CULLING;  // the culling frustum matrix  
	
	//computed from the current camera (as set by camera_mode) during update():
	mat4 CLIP_FROM_WORLD;

	std::vector< LinesPipeline::Vertex > lines_vertices;

	ObjectsPipeline::World world;

	struct ObjectInstance {
		// ObjectVertices vertices; // previously used for tutorial code before S72 loader; now we can just reference the mesh's vertices in the pooled buffer
		S72::Mesh *mesh; // reference to the mesh data for this object, which includes the vertex count and first vertex index into the pooled buffer
		ObjectsPipeline::Transform transform;
		uint32_t texture = 0;
	};
	std::vector< ObjectInstance > object_instances;

	std::vector< S72::Mesh > s72_meshes;

	//--------------------------------------------------------------------
	//Rendering function, uses all the resources above to queue work to draw a frame:

	virtual void render(RTG &, RTG::RenderParams const &) override;
};
