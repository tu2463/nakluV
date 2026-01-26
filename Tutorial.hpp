#pragma once

#include "PosColVertex.hpp"
#include "PosNorTexVertex.hpp"
#include "mat4.hpp"

#include "RTG.hpp"

struct Tutorial : RTG::Application {

	Tutorial(RTG &);
	Tutorial(Tutorial const &) = delete; //you shouldn't be copying this object
	~Tutorial();

	//kept for use in destructor:
	RTG &rtg;

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
		// VkDescriptorSetLayout set0_Camera = VK_NULL_HANDLE; //we'll get back to set0
		VkDescriptorSetLayout set1_Transforms;

		// types for descriptors:
		struct Transform {
			mat4 CLIP_FROM_LOCAL; // from object's local space to clip space, for gl_Position
			mat4 WORLD_FROM_LOCAL; // from local positions to world space, for positions (lighting calculations)
			mat4 WORLD_FROM_LOCAL_NORMAL; // for normals = transpose(inverse(WORLD_FROM_LOCAL))
		};
		static_assert(sizeof(Transform) == 16*4 + 16*4 + 16*4, "Transform is the expected size.");

		// no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;
		
		// vertex bindings:
		using Vertex = PosNorTexVertex;

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
	struct ObjectVertices {
		uint32_t first = 0;
		uint32_t count = 0;
	};
	ObjectVertices plane_vertices;
	ObjectVertices torus_vertices;

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

	float time = 0.0f;

	mat4 CLIP_FROM_WORLD;

	std::vector< LinesPipeline::Vertex > lines_vertices;

	struct ObjectInstance {
		ObjectVertices vertices;
		ObjectsPipeline::Transform transform;
	};
	std::vector< ObjectInstance > object_instances;

	//--------------------------------------------------------------------
	//Rendering function, uses all the resources above to queue work to draw a frame:

	virtual void render(RTG &, RTG::RenderParams const &) override;
};
