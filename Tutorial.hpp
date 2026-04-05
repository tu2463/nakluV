#pragma once

#include "PosColVertex.hpp"
#include "PosNorTexVertex.hpp"
#include "mat4.hpp"

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
	} lines_pipeline; // what does the last line in this struct syntax mean//vv just means lines_pipeline's type is this struct

	struct ObjectsPipeline {
		// descriptor set layouts:
		VkDescriptorSetLayout set0_World = VK_NULL_HANDLE;
		VkDescriptorSetLayout set1_Transforms;
		VkDescriptorSetLayout set2_TEXTURE;
		VkDescriptorSetLayout set3_CubeMap;
		VkDescriptorSetLayout set4_LambertianCubeMap = VK_NULL_HANDLE;
		VkDescriptorSetLayout set5_NormalMap = VK_NULL_HANDLE; // A2-normal
		VkDescriptorSetLayout set6_GGXPrefilteredEnvMap = VK_NULL_HANDLE; // A2-pbr prefiltered GGX specular mipmap
		VkDescriptorSetLayout set7_BRDFLookup = VK_NULL_HANDLE; // BRDF split-sum LUT (NdotV, roughness) -> (scale, bias)
		VkDescriptorSetLayout set8_Lights = VK_NULL_HANDLE; // A3-materials

		// types for descriptors:
		struct World {
			struct { float x, y, z, padding_; } SKY_DIRECTION; // padding is require by the std140 layout, which aligns vec3s on 4-element boundaries; what is std140 //??
			struct { float r, g, b, padding_; } SKY_ENERGY;
			struct { float x, y, z, padding_; } SUN_DIRECTION;
			struct { float r, g, b, padding_; } SUN_ENERGY;
			struct { float x, y, z, padding_; } EYE; // camera pos in world space
		};
		static_assert(sizeof(World) == 4*4 + 4*4 + 4*4 + 4*4 + 4*4, "World is the expected size.");

		struct Transform {
			mat4 CLIP_FROM_LOCAL; // from object's local space to clip space, for gl_Position
			mat4 WORLD_FROM_LOCAL; // from local positions to world space, for positions (lighting calculations); Where the object IS in the world (position + orientation)
			mat4 WORLD_FROM_LOCAL_NORMAL; // for normals = transpose(inverse(WORLD_FROM_LOCAL))
		};
		static_assert(sizeof(Transform) == 16*4 + 16*4 + 16*4, "Transform is the expected size.");

		enum class MaterialType {
			PBR = 0,
			Lambertian = 1,
			Mirror = 2,
			Environment = 3,
		};

		enum class ToneMap {
			Linear = 0,
			ACES = 1,
		};

		struct MipData {
			std::vector<float> floats;
			int face_width, face_height;
		};

		struct LightData {
			// a vec3 has a base alignment of 4*4bytes; The aligned byte offset of a variable must be equal to a multiple of its base alignment - Credit: https://learnopengl.com/Advanced-OpenGL/Advanced-GLSL
			
			// the first 16 bytes:
			float position[3]; // 3 bytes
			int32_t type; // 1 byte; 0 = sun, 1 = sphere, 2 = spot //vv use int32_t, not int, because we want exact size for GPU buffer, but the size of int depends on platform implementation
;
			// 2nd 16 bytes:
			float tint[3];
			float fov; // spot

			// 3rd 16 bytes:
			float direction[3];
			float blend; // spot

			// 4th 16 bytes:
			// sun:
			float angle;
			float strength;

			// sphere & spot:
			float radius;
			float power;

			// 5th 16 bytes:
			float limit = std::numeric_limits<float>::infinity(); // optional, will be infinity if not specified
			float pad[3];
		};
		static_assert(sizeof(LightData) == 16*5 );

		// push constants
		struct Push {
			MaterialType material_type = MaterialType::Lambertian;
			int exposure = 0;
			ToneMap tone_map_push = ToneMap::Linear;
			float roughness = 0.5f; // A2-pbr
			float metalness = 0.0f;
		};
		ToneMap tone_map = ToneMap::Linear;

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

		// Light may change per frame, so these buffers need to be in workspace.
		Helpers::AllocatedBuffer Lights_src; // host coherent; mapped
		Helpers::AllocatedBuffer Lights; // device-local
		VkDescriptorSet Lights_descriptors; // references Lights, the descriptor set
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


	VkSampler texture_sampler = VK_NULL_HANDLE; // gives the sampler state (wrapping, interpolation, etc)
	std::vector< Helpers::AllocatedImage > textures; // holds actual image data
	std::vector< VkImageView > texture_views;
	VkDescriptorPool texture_descriptor_pool = VK_NULL_HANDLE; // from which we allocate texture descriptor sets
	std::vector< VkDescriptorSet > texture_descriptors; // allocated from texture_descriptor_pool; includes a descriptor for each of our textures.
	std::unordered_map< S72::Texture*, uint32_t > texture_index_map; // maps S72 texture pointers to texture indices
	std::unordered_map< S72::Material*, uint32_t > material_albedo_map; // maps materials to their albedo texture index

	VkImageView cubemap_view = VK_NULL_HANDLE; // A2-diffuse
	VkDescriptorSet cubemap_descriptors = VK_NULL_HANDLE; // allocated from texture_descriptor_pool; include a descriptor for the cube map

	VkImageView lambertian_cubemap_view = VK_NULL_HANDLE; // A2-diffuse
	VkDescriptorSet lambertian_cubemap_descriptors = VK_NULL_HANDLE;

	std::vector< Helpers::AllocatedImage > normal_maps; // holds actual image data
	std::vector< VkImageView > normal_map_views; // A2-normal
	std::vector< VkDescriptorSet>  normal_map_descriptors;
	std::unordered_map< S72::Texture*, uint32_t > normal_index_map; // maps S72 normal map pointers to normal map indices

	// A2-pbr: BRDF split-sum LUT (precomputed 2D texture, stored as brdf_lut.bin)
	Helpers::AllocatedImage brdf_lut_image;
	VkImageView brdf_lut_view = VK_NULL_HANDLE;
	VkSampler brdf_lut_sampler = VK_NULL_HANDLE;
	VkDescriptorSet brdf_lut_descriptors = VK_NULL_HANDLE;

	// A2-pbr: GGX (one cubemap imag with multiple mip levels)
	Helpers::AllocatedImage ggx_image;
	/* we need a separate sampler for GGX because:
	- maxLod must be set to the number of mip levels (e.g. 5.0) — otherwise textureLod() can't access the roughness mip levels. A sampler configured for a regular texture might clamp maxLod too low.
	- minFilter = LINEAR + mipmapMode = LINEAR for smooth interpolation between mip levels (between roughness values).
	- addressMode needs to be appropriate for a cubemap (typically CLAMP_TO_EDGE to avoid seams at cube edges).
	*/
	VkSampler ggx_sampler;
	VkImageView ggx_view = VK_NULL_HANDLE;
	VkDescriptorSet ggx_descriptors = VK_NULL_HANDLE; // will be allocated from texture_descriptor_pool
	uint32_t ggx_mip_count = 0;
	
	/* A2-env
	You must use a pixel format for environment images which can support high dynamic range.
	An easy, but wasteful, way to do this is by decoding the images, on-CPU, to VK_FORMAT_R32G32B32A32_SFLOAT (yes, the alpha channel is needed to guarantee GPU support, acc'd to the required format support table).
	A more memory-efficient method is to transcode to VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 shared-exponent format or the VK_FORMAT_R16G16B16A16_SFLOAT half-float format;
	though you will need to be careful to correctly clamp under- and over-flowing values when translating.
	*/
	VkFormat cubemap_format = VK_FORMAT_R32G32B32A32_SFLOAT; // TODO: see if we can use less wasteful format; if changed, also update process_textures

	/* A2-normal
	Normal maps store direction vectors, not colors. Each RGB texel encodes an XYZ direction as:
	stored byte → shader value
			0   → 0/255 ≈ 0.0
			128 → 128/255 ≈ 0.502
			255 → 255/255 = 1.0
	Then the shader expands it: direction = rgb * 2.0 - 1.0 → [-1, 1]. A flat normal (0, 0, 1) is stored as (128, 128, 255).
	Why not SRGB? because _SRGB apply gamma decode before the shader reads the value, making the value non-linear, resulting in incorrect normal direction. 
	SRGB is only correct for color textures because those were painted by artists in the sRGB color space and need gamma decoding to get back to linear light values.
	*/
	VkFormat normal_map_format = VK_FORMAT_R8G8B8A8_UNORM;

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

	// -- movement -- 
	float time = 0.0f;

	// A1-move
	float animation_time = 0.0f; // current playback position (seconds)
	bool animation_playing = true;

	// FPS tracking
	float fps_accumulator = 0.0f;
	int fps_frame_count = 0;

	void evaluate_driver(S72::Driver& driver, float t);

	// -- camera & culling --
	enum class CullingMode {
		None = 0,
		Frustum = 1,
	} culling_mode = CullingMode::None; 

	// Credit: adapted from More (Robust) Frustum Culling by Bruno Opsenica
	struct CullingFrustum {
		float near_right;
		float near_top;
		float near_plane;
		float far_plane;
	} frustum;

	struct OBB {
		vec3 center = {};
		vec3 extents = {};
		// Orthonormal basis
		vec3 axes[3] = {};
	};

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
		float target_x = 0.0f, target_y = 0.0f, target_z = 0.0f; // where the camera is looking + orbiting
		float radius = 2.0f; // distance from camera to target
		float azimuth = 0.0f; // counterclockwise angle around z axis between x axis and camera direction (radians)
		float elevation = 0.25f * float(M_PI); // angle up from xy plane to camera direction (radians) //??

		float fov = 60.0f / 180.0f * float(M_PI); // vertical field of view (radians) //??
		float near = 0.1f; // near clipping plane
		float far = 1000.0f; // far clipping plane
	} free_camera;

	OrbitCamera debug_camera; // TODO: increase usefulness by setting the debug camera to a position that can see the whole scene
	// std::variant< SceneCamera, OrbitCamera > culling_camera = free_camera; // previously active camera // FIXED-BUG: can't save a pointer to 2 types, so save the prev mode and mode and CLIP_FROM_WORLD to compute fructum
  	
	//computed from the current camera (as set by camera_mode) during update():
	mat4 CLIP_FROM_WORLD;
	mat4 CLIP_FROM_WORLD_CULLING;  // the culling frustum matrix  
  	mat4 CAMERA_FROM_WORLD; // for transforming object positions into camera space for culling

	// -- lights -- 
	struct Light {
		S72::Light *light; // reference to the light data for this object, which includes shadow, type, tint
		mat4 WORLD_FROM_LOCAL;
	};
	std::vector < Light > light_instances; // at every frame, will transfer Light to LightData and pass to frag shader when creating SSBO

	// -- objects --
	std::vector< LinesPipeline::Vertex > lines_vertices;

	ObjectsPipeline::World world;

	struct ObjectInstance {
		// ObjectVertices vertices; // previously used for tutorial code before S72 loader; now we can just reference the mesh's vertices in the pooled buffer
		S72::Mesh *mesh; // reference to the mesh data for this object, which includes the vertex count and first vertex index into the pooled buffer
		ObjectsPipeline::Transform transform;
		uint32_t texture = 0;
		ObjectsPipeline::MaterialType material_type = ObjectsPipeline::MaterialType::Lambertian;
		float roughness = 0.5f; // A2-pbr
		float metalness = 0.0f;
	};
	
	std::vector< ObjectInstance > object_instances;

	std::vector< S72::Mesh > s72_meshes;

	//--------------------------------------------------------------------
	//Rendering function, uses all the resources above to queue work to draw a frame:

	virtual void render(RTG &, RTG::RenderParams const &) override;
};
