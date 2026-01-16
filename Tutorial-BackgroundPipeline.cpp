#include "Tutorial.hpp"

#include "Helpers.hpp"
#include "refsol.hpp"

static uint32_t vert_code[] =
#include "spv/background.vert.inl"
;

static uint32_t frag_code[] =
#include "spv/background.frag.inl"
;

void Tutorial::BackgroundPipeline::create(RTG &rtg, VkRenderPass render_pass, uint32_t subpass) {
	VkShaderModule vert_module = rtg.helpers.create_shader_module(vert_code);
	VkShaderModule frag_module = rtg.helpers.create_shader_module(frag_code);

	refsol::BackgroundPipeline_create(rtg, render_pass, subpass, vert_module, frag_module, &layout, &handle);
}

void Tutorial::BackgroundPipeline::destroy(RTG &rtg) {
	refsol::BackgroundPipeline_destroy(rtg, &layout, &handle);
}