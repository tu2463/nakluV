#include "Tutorial.hpp"

#include "Helpers.hpp"
#include "refsol.hpp"

void Tutorial::BackgroundPipeline::create(RTG &rtg, VkRenderPass render_pass, uint32_t subpass) {
	VkShaderModule vert_module = VK_NULL_HANDLE;
	VkShaderModule frag_module = VK_NULL_HANDLE;

	refsol::BackgroundPipeline_create(rtg, render_pass, subpass, vert_module, frag_module, &layout, &handle);
}

void Tutorial::BackgroundPipeline::destroy(RTG &rtg) {
	refsol::BackgroundPipeline_destroy(rtg, &layout, &handle);
}