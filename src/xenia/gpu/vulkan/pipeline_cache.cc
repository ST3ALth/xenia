/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/pipeline_cache.h"

#include "third_party/xxhash/xxhash.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/vulkan/vulkan_gpu_flags.h"

namespace xe {
namespace gpu {
namespace vulkan {

using xe::ui::vulkan::CheckResult;

// Generated with `xenia-build genspirv`.
#include "xenia/gpu/vulkan/shaders/bin/line_quad_list_geom.h"
#include "xenia/gpu/vulkan/shaders/bin/point_list_geom.h"
#include "xenia/gpu/vulkan/shaders/bin/quad_list_geom.h"
#include "xenia/gpu/vulkan/shaders/bin/rect_list_geom.h"

PipelineCache::PipelineCache(
    RegisterFile* register_file, ui::vulkan::VulkanDevice* device,
    VkDescriptorSetLayout uniform_descriptor_set_layout,
    VkDescriptorSetLayout texture_descriptor_set_layout)
    : register_file_(register_file), device_(*device) {
  // Initialize the shared driver pipeline cache.
  // We'll likely want to serialize this and reuse it, if that proves to be
  // useful. If the shaders are expensive and this helps we could do it per
  // game, otherwise a single shared cache for render state/etc.
  VkPipelineCacheCreateInfo pipeline_cache_info;
  pipeline_cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  pipeline_cache_info.pNext = nullptr;
  pipeline_cache_info.flags = 0;
  pipeline_cache_info.initialDataSize = 0;
  pipeline_cache_info.pInitialData = nullptr;
  auto err = vkCreatePipelineCache(device_, &pipeline_cache_info, nullptr,
                                   &pipeline_cache_);
  CheckResult(err, "vkCreatePipelineCache");

  // Descriptors used by the pipelines.
  // These are the only ones we can ever bind.
  VkDescriptorSetLayout set_layouts[] = {
      // Per-draw constant register uniforms.
      uniform_descriptor_set_layout,
      // All texture bindings.
      texture_descriptor_set_layout,
  };

  // Push constants used for draw parameters.
  // We need to keep these under 128b across all stages.
  VkPushConstantRange push_constant_ranges[2];
  push_constant_ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push_constant_ranges[0].offset = 0;
  push_constant_ranges[0].size = sizeof(float) * 16;
  push_constant_ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push_constant_ranges[1].offset = sizeof(float) * 16;
  push_constant_ranges[1].size = sizeof(int);

  // Shared pipeline layout.
  VkPipelineLayoutCreateInfo pipeline_layout_info;
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.pNext = nullptr;
  pipeline_layout_info.flags = 0;
  pipeline_layout_info.setLayoutCount =
      static_cast<uint32_t>(xe::countof(set_layouts));
  pipeline_layout_info.pSetLayouts = set_layouts;
  pipeline_layout_info.pushConstantRangeCount =
      static_cast<uint32_t>(xe::countof(push_constant_ranges));
  pipeline_layout_info.pPushConstantRanges = push_constant_ranges;
  err = vkCreatePipelineLayout(*device, &pipeline_layout_info, nullptr,
                               &pipeline_layout_);
  CheckResult(err, "vkCreatePipelineLayout");

  // Initialize our shared geometry shaders.
  // These will be used as needed to emulate primitive types Vulkan doesn't
  // support.
  VkShaderModuleCreateInfo shader_module_info;
  shader_module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_info.pNext = nullptr;
  shader_module_info.flags = 0;
  shader_module_info.codeSize =
      static_cast<uint32_t>(sizeof(line_quad_list_geom));
  shader_module_info.pCode =
      reinterpret_cast<const uint32_t*>(line_quad_list_geom);
  err = vkCreateShaderModule(device_, &shader_module_info, nullptr,
                             &geometry_shaders_.line_quad_list);
  CheckResult(err, "vkCreateShaderModule");
  shader_module_info.codeSize = static_cast<uint32_t>(sizeof(point_list_geom));
  shader_module_info.pCode = reinterpret_cast<const uint32_t*>(point_list_geom);
  err = vkCreateShaderModule(device_, &shader_module_info, nullptr,
                             &geometry_shaders_.point_list);
  CheckResult(err, "vkCreateShaderModule");
  shader_module_info.codeSize = static_cast<uint32_t>(sizeof(quad_list_geom));
  shader_module_info.pCode = reinterpret_cast<const uint32_t*>(quad_list_geom);
  err = vkCreateShaderModule(device_, &shader_module_info, nullptr,
                             &geometry_shaders_.quad_list);
  CheckResult(err, "vkCreateShaderModule");
  shader_module_info.codeSize = static_cast<uint32_t>(sizeof(rect_list_geom));
  shader_module_info.pCode = reinterpret_cast<const uint32_t*>(rect_list_geom);
  err = vkCreateShaderModule(device_, &shader_module_info, nullptr,
                             &geometry_shaders_.rect_list);
  CheckResult(err, "vkCreateShaderModule");
}

PipelineCache::~PipelineCache() {
  // Destroy all pipelines.
  for (auto it : cached_pipelines_) {
    vkDestroyPipeline(device_, it.second, nullptr);
  }
  cached_pipelines_.clear();

  // Destroy geometry shaders.
  vkDestroyShaderModule(device_, geometry_shaders_.line_quad_list, nullptr);
  vkDestroyShaderModule(device_, geometry_shaders_.point_list, nullptr);
  vkDestroyShaderModule(device_, geometry_shaders_.quad_list, nullptr);
  vkDestroyShaderModule(device_, geometry_shaders_.rect_list, nullptr);

  vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);

  // Destroy all shaders.
  for (auto it : shader_map_) {
    delete it.second;
  }
  shader_map_.clear();
}

VulkanShader* PipelineCache::LoadShader(ShaderType shader_type,
                                        uint32_t guest_address,
                                        const uint32_t* host_address,
                                        uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  uint64_t data_hash = XXH64(host_address, dword_count * sizeof(uint32_t), 0);
  auto it = shader_map_.find(data_hash);
  if (it != shader_map_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }

  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  VulkanShader* shader = new VulkanShader(device_, shader_type, data_hash,
                                          host_address, dword_count);
  shader_map_.insert({data_hash, shader});

  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!shader_translator_.Translate(shader)) {
    XELOGE("Shader translation failed; marking shader as ignored");
    return shader;
  }

  // Prepare the shader for use (creates our VkShaderModule).
  // It could still fail at this point.
  if (!shader->Prepare()) {
    XELOGE("Shader preparation failed; marking shader as ignored");
    return shader;
  }

  if (shader->is_valid()) {
    XELOGGPU("Generated %s shader at 0x%.8X (%db):\n%s",
             shader_type == ShaderType::kVertex ? "vertex" : "pixel",
             guest_address, dword_count * 4,
             shader->ucode_disassembly().c_str());
  }

  // Dump shader files if desired.
  if (!FLAGS_dump_shaders.empty()) {
    shader->Dump(FLAGS_dump_shaders, "vk");
  }

  return shader;
}

bool PipelineCache::ConfigurePipeline(VkCommandBuffer command_buffer,
                                      const RenderState* render_state,
                                      VulkanShader* vertex_shader,
                                      VulkanShader* pixel_shader,
                                      PrimitiveType primitive_type) {
  // Perform a pass over all registers and state updating our cached structures.
  // This will tell us if anything has changed that requires us to either build
  // a new pipeline or use an existing one.
  VkPipeline pipeline = nullptr;
  auto update_status = UpdateState(vertex_shader, pixel_shader, primitive_type);
  switch (update_status) {
    case UpdateStatus::kCompatible:
      // Requested pipeline is compatible with our previous one, so use that.
      // Note that there still may be dynamic state that needs updating.
      pipeline = current_pipeline_;
      break;
    case UpdateStatus::kMismatch:
      // Pipeline state has changed. We need to either create a new one or find
      // an old one that matches.
      current_pipeline_ = nullptr;
      break;
    case UpdateStatus::kError:
      // Error updating state - bail out.
      // We are in an indeterminate state, so reset things for the next attempt.
      current_pipeline_ = nullptr;
      return false;
  }
  if (!pipeline) {
    // Should have a hash key produced by the UpdateState pass.
    uint64_t hash_key = XXH64_digest(&hash_state_);
    pipeline = GetPipeline(render_state, hash_key);
    current_pipeline_ = pipeline;
    if (!pipeline) {
      // Unable to create pipeline.
      return false;
    }
  }

  // Bind the pipeline.
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  // Issue all changed dynamic state information commands.
  // TODO(benvanik): dynamic state is kept in the command buffer, so if we
  // have issued it before (regardless of pipeline) we don't need to do it now.
  // TODO(benvanik): track whether we have issued on the given command buffer.
  bool full_dynamic_state = true;
  if (!SetDynamicState(command_buffer, full_dynamic_state)) {
    // Failed to update state.
    return false;
  }

  return true;
}

void PipelineCache::ClearCache() {
  // TODO(benvanik): caching.
}

VkPipeline PipelineCache::GetPipeline(const RenderState* render_state,
                                      uint64_t hash_key) {
  // Lookup the pipeline in the cache.
  auto it = cached_pipelines_.find(hash_key);
  if (it != cached_pipelines_.end()) {
    // Found existing pipeline.
    return it->second;
  }

  VkPipelineDynamicStateCreateInfo dynamic_state_info;
  dynamic_state_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state_info.pNext = nullptr;
  dynamic_state_info.flags = 0;
  VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_LINE_WIDTH,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_DEPTH_BOUNDS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  dynamic_state_info.dynamicStateCount =
      static_cast<uint32_t>(xe::countof(dynamic_states));
  dynamic_state_info.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo pipeline_info;
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.pNext = nullptr;
  pipeline_info.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
  pipeline_info.stageCount = update_shader_stages_stage_count_;
  pipeline_info.pStages = update_shader_stages_info_;
  pipeline_info.pVertexInputState = &update_vertex_input_state_info_;
  pipeline_info.pInputAssemblyState = &update_input_assembly_state_info_;
  pipeline_info.pTessellationState = nullptr;
  pipeline_info.pViewportState = &update_viewport_state_info_;
  pipeline_info.pRasterizationState = &update_rasterization_state_info_;
  pipeline_info.pMultisampleState = &update_multisample_state_info_;
  pipeline_info.pDepthStencilState = &update_depth_stencil_state_info_;
  pipeline_info.pColorBlendState = &update_color_blend_state_info_;
  pipeline_info.pDynamicState = &dynamic_state_info;
  pipeline_info.layout = pipeline_layout_;
  pipeline_info.renderPass = render_state->render_pass_handle;
  pipeline_info.subpass = 0;
  pipeline_info.basePipelineHandle = nullptr;
  pipeline_info.basePipelineIndex = 0;
  VkPipeline pipeline = nullptr;
  auto err = vkCreateGraphicsPipelines(device_, nullptr, 1, &pipeline_info,
                                       nullptr, &pipeline);
  CheckResult(err, "vkCreateGraphicsPipelines");

  // Add to cache with the hash key for reuse.
  cached_pipelines_.insert({hash_key, pipeline});

  return pipeline;
}

VkShaderModule PipelineCache::GetGeometryShader(PrimitiveType primitive_type,
                                                bool is_line_mode) {
  switch (primitive_type) {
    case PrimitiveType::kLineList:
    case PrimitiveType::kLineLoop:
    case PrimitiveType::kLineStrip:
    case PrimitiveType::kTriangleList:
    case PrimitiveType::kTriangleFan:
    case PrimitiveType::kTriangleStrip:
      // Supported directly - no need to emulate.
      return nullptr;
    case PrimitiveType::kPointList:
      return geometry_shaders_.point_list;
    case PrimitiveType::kUnknown0x07:
      assert_always("Unknown geometry type");
      return nullptr;
    case PrimitiveType::kRectangleList:
      return geometry_shaders_.rect_list;
    case PrimitiveType::kQuadList:
      return is_line_mode ? geometry_shaders_.line_quad_list
                          : geometry_shaders_.quad_list;
    case PrimitiveType::kQuadStrip:
      // TODO(benvanik): quad strip geometry shader.
      assert_always("Quad strips not implemented");
      return nullptr;
    default:
      assert_unhandled_case(primitive_type);
      return nullptr;
  }
}

bool PipelineCache::SetDynamicState(VkCommandBuffer command_buffer,
                                    bool full_update) {
  auto& regs = set_dynamic_state_registers_;

  bool window_offset_dirty = SetShadowRegister(&regs.pa_sc_window_offset,
                                               XE_GPU_REG_PA_SC_WINDOW_OFFSET);

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  int16_t window_offset_x = 0;
  int16_t window_offset_y = 0;
  if ((regs.pa_su_sc_mode_cntl >> 16) & 1) {
    window_offset_x = regs.pa_sc_window_offset & 0x7FFF;
    window_offset_y = (regs.pa_sc_window_offset >> 16) & 0x7FFF;
    if (window_offset_x & 0x4000) {
      window_offset_x |= 0x8000;
    }
    if (window_offset_y & 0x4000) {
      window_offset_y |= 0x8000;
    }
  }

  // VK_DYNAMIC_STATE_SCISSOR
  bool scissor_state_dirty = full_update || window_offset_dirty;
  scissor_state_dirty |= SetShadowRegister(&regs.pa_sc_window_scissor_tl,
                                           XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
  scissor_state_dirty |= SetShadowRegister(&regs.pa_sc_window_scissor_br,
                                           XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
  if (scissor_state_dirty) {
    int32_t ws_x = regs.pa_sc_window_scissor_tl & 0x7FFF;
    int32_t ws_y = (regs.pa_sc_window_scissor_tl >> 16) & 0x7FFF;
    uint32_t ws_w = (regs.pa_sc_window_scissor_br & 0x7FFF) - ws_x;
    uint32_t ws_h = ((regs.pa_sc_window_scissor_br >> 16) & 0x7FFF) - ws_y;
    ws_x += window_offset_x;
    ws_y += window_offset_y;

    VkRect2D scissor_rect;
    scissor_rect.offset.x = ws_x;
    scissor_rect.offset.y = ws_y;
    scissor_rect.extent.width = ws_w;
    scissor_rect.extent.height = ws_h;
    vkCmdSetScissor(command_buffer, 0, 1, &scissor_rect);
  }

  // VK_DYNAMIC_STATE_VIEWPORT
  bool viewport_state_dirty = full_update || window_offset_dirty;
  viewport_state_dirty |=
      SetShadowRegister(&regs.rb_surface_info, XE_GPU_REG_RB_SURFACE_INFO);
  viewport_state_dirty |=
      SetShadowRegister(&regs.pa_cl_vte_cntl, XE_GPU_REG_PA_CL_VTE_CNTL);
  viewport_state_dirty |= SetShadowRegister(&regs.pa_cl_vport_xoffset,
                                            XE_GPU_REG_PA_CL_VPORT_XOFFSET);
  viewport_state_dirty |= SetShadowRegister(&regs.pa_cl_vport_yoffset,
                                            XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  viewport_state_dirty |= SetShadowRegister(&regs.pa_cl_vport_zoffset,
                                            XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
  viewport_state_dirty |= SetShadowRegister(&regs.pa_cl_vport_xscale,
                                            XE_GPU_REG_PA_CL_VPORT_XSCALE);
  viewport_state_dirty |= SetShadowRegister(&regs.pa_cl_vport_yscale,
                                            XE_GPU_REG_PA_CL_VPORT_YSCALE);
  viewport_state_dirty |= SetShadowRegister(&regs.pa_cl_vport_zscale,
                                            XE_GPU_REG_PA_CL_VPORT_ZSCALE);
  if (viewport_state_dirty) {
    // HACK: no clue where to get these values.
    // RB_SURFACE_INFO
    auto surface_msaa =
        static_cast<MsaaSamples>((regs.rb_surface_info >> 16) & 0x3);
    // TODO(benvanik): ??
    float window_width_scalar = 1;
    float window_height_scalar = 1;
    switch (surface_msaa) {
      case MsaaSamples::k1X:
        break;
      case MsaaSamples::k2X:
        window_width_scalar = 2;
        break;
      case MsaaSamples::k4X:
        window_width_scalar = 2;
        window_height_scalar = 2;
        break;
    }

    // Whether each of the viewport settings are enabled.
    // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
    bool vport_xscale_enable = (regs.pa_cl_vte_cntl & (1 << 0)) > 0;
    bool vport_xoffset_enable = (regs.pa_cl_vte_cntl & (1 << 1)) > 0;
    bool vport_yscale_enable = (regs.pa_cl_vte_cntl & (1 << 2)) > 0;
    bool vport_yoffset_enable = (regs.pa_cl_vte_cntl & (1 << 3)) > 0;
    bool vport_zscale_enable = (regs.pa_cl_vte_cntl & (1 << 4)) > 0;
    bool vport_zoffset_enable = (regs.pa_cl_vte_cntl & (1 << 5)) > 0;
    assert_true(vport_xscale_enable == vport_yscale_enable ==
                vport_zscale_enable == vport_xoffset_enable ==
                vport_yoffset_enable == vport_zoffset_enable);

    VkViewport viewport_rect;
    viewport_rect.x = 0;
    viewport_rect.y = 0;
    viewport_rect.width = 100;
    viewport_rect.height = 100;
    viewport_rect.minDepth = 0;
    viewport_rect.maxDepth = 1;

    if (vport_xscale_enable) {
      float texel_offset_x = 0.0f;
      float texel_offset_y = 0.0f;
      float vox = vport_xoffset_enable ? regs.pa_cl_vport_xoffset : 0;
      float voy = vport_yoffset_enable ? regs.pa_cl_vport_yoffset : 0;
      float vsx = vport_xscale_enable ? regs.pa_cl_vport_xscale : 1;
      float vsy = vport_yscale_enable ? regs.pa_cl_vport_yscale : 1;
      window_width_scalar = window_height_scalar = 1;
      float vpw = 2 * window_width_scalar * vsx;
      float vph = -2 * window_height_scalar * vsy;
      float vpx = window_width_scalar * vox - vpw / 2 + window_offset_x;
      float vpy = window_height_scalar * voy - vph / 2 + window_offset_y;
      viewport_rect.x = vpx + texel_offset_x;
      viewport_rect.y = vpy + texel_offset_y;
      viewport_rect.width = vpw;
      viewport_rect.height = vph;

      // TODO(benvanik): depth range adjustment?
      // float voz = vport_zoffset_enable ? regs.pa_cl_vport_zoffset : 0;
      // float vsz = vport_zscale_enable ? regs.pa_cl_vport_zscale : 1;
    } else {
      float texel_offset_x = 0.0f;
      float texel_offset_y = 0.0f;
      float vpw = 2 * 2560.0f * window_width_scalar;
      float vph = 2 * 2560.0f * window_height_scalar;
      float vpx = -2560.0f * window_width_scalar + window_offset_x;
      float vpy = -2560.0f * window_height_scalar + window_offset_y;
      viewport_rect.x = vpx + texel_offset_x;
      viewport_rect.y = vpy + texel_offset_y;
      viewport_rect.width = vpw;
      viewport_rect.height = vph;
    }
    float voz = vport_zoffset_enable ? regs.pa_cl_vport_zoffset : 0;
    float vsz = vport_zscale_enable ? regs.pa_cl_vport_zscale : 1;
    viewport_rect.minDepth = voz;
    viewport_rect.maxDepth = voz + vsz;

    vkCmdSetViewport(command_buffer, 0, 1, &viewport_rect);
  }

  // VK_DYNAMIC_STATE_BLEND_CONSTANTS
  bool blend_constant_state_dirty = full_update;
  blend_constant_state_dirty |=
      SetShadowRegister(&regs.rb_blend_rgba[0], XE_GPU_REG_RB_BLEND_RED);
  blend_constant_state_dirty |=
      SetShadowRegister(&regs.rb_blend_rgba[1], XE_GPU_REG_RB_BLEND_GREEN);
  blend_constant_state_dirty |=
      SetShadowRegister(&regs.rb_blend_rgba[2], XE_GPU_REG_RB_BLEND_BLUE);
  blend_constant_state_dirty |=
      SetShadowRegister(&regs.rb_blend_rgba[3], XE_GPU_REG_RB_BLEND_ALPHA);
  if (blend_constant_state_dirty) {
    vkCmdSetBlendConstants(command_buffer, regs.rb_blend_rgba);
  }

  // VK_DYNAMIC_STATE_LINE_WIDTH
  vkCmdSetLineWidth(command_buffer, 1.0f);

  // VK_DYNAMIC_STATE_DEPTH_BIAS
  vkCmdSetDepthBias(command_buffer, 0.0f, 0.0f, 0.0f);

  // VK_DYNAMIC_STATE_DEPTH_BOUNDS
  vkCmdSetDepthBounds(command_buffer, 0.0f, 1.0f);

  // VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK
  vkCmdSetStencilCompareMask(command_buffer, VK_STENCIL_FRONT_AND_BACK, 0);

  // VK_DYNAMIC_STATE_STENCIL_REFERENCE
  vkCmdSetStencilReference(command_buffer, VK_STENCIL_FRONT_AND_BACK, 0);

  // VK_DYNAMIC_STATE_STENCIL_WRITE_MASK
  vkCmdSetStencilWriteMask(command_buffer, VK_STENCIL_FRONT_AND_BACK, 0);

  // TODO(benvanik): push constants.

  bool push_constants_dirty = full_update;
  push_constants_dirty |=
      SetShadowRegister(&regs.sq_program_cntl, XE_GPU_REG_SQ_PROGRAM_CNTL);
  push_constants_dirty |=
      SetShadowRegister(&regs.sq_context_misc, XE_GPU_REG_SQ_CONTEXT_MISC);

  xenos::xe_gpu_program_cntl_t program_cntl;
  program_cntl.dword_0 = regs.sq_program_cntl;

  // Populate a register in the pixel shader with frag coord.
  int ps_param_gen = (regs.sq_context_misc >> 8) & 0xFF;
  // draw_batcher_.set_ps_param_gen(program_cntl.param_gen ? ps_param_gen : -1);

  // Normal vertex shaders only, for now.
  // TODO(benvanik): transform feedback/memexport.
  // https://github.com/freedreno/freedreno/blob/master/includes/a2xx.xml.h
  // 0 = normal
  // 2 = point size
  assert_true(program_cntl.vs_export_mode == 0 ||
              program_cntl.vs_export_mode == 2);

  return true;
}

bool PipelineCache::SetShadowRegister(uint32_t* dest, uint32_t register_name) {
  uint32_t value = register_file_->values[register_name].u32;
  if (*dest == value) {
    return false;
  }
  *dest = value;
  return true;
}

bool PipelineCache::SetShadowRegister(float* dest, uint32_t register_name) {
  float value = register_file_->values[register_name].f32;
  if (*dest == value) {
    return false;
  }
  *dest = value;
  return true;
}

PipelineCache::UpdateStatus PipelineCache::UpdateState(
    VulkanShader* vertex_shader, VulkanShader* pixel_shader,
    PrimitiveType primitive_type) {
  bool mismatch = false;

  // Reset hash so we can build it up.
  XXH64_reset(&hash_state_, 0);

#define CHECK_UPDATE_STATUS(status, mismatch, error_message) \
  {                                                          \
    if (status == UpdateStatus::kError) {                    \
      XELOGE(error_message);                                 \
      return status;                                         \
    } else if (status == UpdateStatus::kMismatch) {          \
      mismatch = true;                                       \
    }                                                        \
  }

  UpdateStatus status;
  status = UpdateShaderStages(vertex_shader, pixel_shader, primitive_type);
  CHECK_UPDATE_STATUS(status, mismatch, "Unable to update shader stages");
  status = UpdateVertexInputState(vertex_shader);
  CHECK_UPDATE_STATUS(status, mismatch, "Unable to update vertex input state");
  status = UpdateInputAssemblyState(primitive_type);
  CHECK_UPDATE_STATUS(status, mismatch,
                      "Unable to update input assembly state");
  status = UpdateViewportState();
  CHECK_UPDATE_STATUS(status, mismatch, "Unable to update viewport state");
  status = UpdateRasterizationState(primitive_type);
  CHECK_UPDATE_STATUS(status, mismatch, "Unable to update rasterization state");
  status = UpdateMultisampleState();
  CHECK_UPDATE_STATUS(status, mismatch, "Unable to update multisample state");
  status = UpdateDepthStencilState();
  CHECK_UPDATE_STATUS(status, mismatch, "Unable to update depth/stencil state");
  status = UpdateColorBlendState();
  CHECK_UPDATE_STATUS(status, mismatch, "Unable to update color blend state");

  return mismatch ? UpdateStatus::kMismatch : UpdateStatus::kCompatible;
}

PipelineCache::UpdateStatus PipelineCache::UpdateShaderStages(
    VulkanShader* vertex_shader, VulkanShader* pixel_shader,
    PrimitiveType primitive_type) {
  auto& regs = update_shader_stages_regs_;

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ.
  assert_true(register_file_->values[XE_GPU_REG_SQ_VS_CONST].u32 ==
                  0x000FF000 ||
              register_file_->values[XE_GPU_REG_SQ_VS_CONST].u32 == 0x00000000);
  assert_true(register_file_->values[XE_GPU_REG_SQ_PS_CONST].u32 ==
                  0x000FF100 ||
              register_file_->values[XE_GPU_REG_SQ_PS_CONST].u32 == 0x00000000);

  bool dirty = false;
  dirty |= SetShadowRegister(&regs.pa_su_sc_mode_cntl,
                             XE_GPU_REG_PA_SU_SC_MODE_CNTL);
  dirty |= regs.vertex_shader != vertex_shader;
  dirty |= regs.pixel_shader != pixel_shader;
  dirty |= regs.primitive_type != primitive_type;
  XXH64_update(&hash_state_, &regs, sizeof(regs));
  if (!dirty) {
    return UpdateStatus::kCompatible;
  }
  regs.vertex_shader = vertex_shader;
  regs.pixel_shader = pixel_shader;
  regs.primitive_type = primitive_type;

  update_shader_stages_stage_count_ = 0;

  auto& vertex_pipeline_stage =
      update_shader_stages_info_[update_shader_stages_stage_count_++];
  vertex_pipeline_stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertex_pipeline_stage.pNext = nullptr;
  vertex_pipeline_stage.flags = 0;
  vertex_pipeline_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertex_pipeline_stage.module = vertex_shader->shader_module();
  vertex_pipeline_stage.pName = "main";
  vertex_pipeline_stage.pSpecializationInfo = nullptr;

  bool is_line_mode = false;
  if (((regs.pa_su_sc_mode_cntl >> 3) & 0x3) != 0) {
    uint32_t front_poly_mode = (regs.pa_su_sc_mode_cntl >> 5) & 0x7;
    if (front_poly_mode == 1) {
      is_line_mode = true;
    }
  }
  auto geometry_shader = GetGeometryShader(primitive_type, is_line_mode);
  if (geometry_shader) {
    auto& geometry_pipeline_stage =
        update_shader_stages_info_[update_shader_stages_stage_count_++];
    geometry_pipeline_stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    geometry_pipeline_stage.pNext = nullptr;
    geometry_pipeline_stage.flags = 0;
    geometry_pipeline_stage.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    geometry_pipeline_stage.module = geometry_shader;
    geometry_pipeline_stage.pName = "main";
    geometry_pipeline_stage.pSpecializationInfo = nullptr;
  }

  auto& pixel_pipeline_stage =
      update_shader_stages_info_[update_shader_stages_stage_count_++];
  pixel_pipeline_stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pixel_pipeline_stage.pNext = nullptr;
  pixel_pipeline_stage.flags = 0;
  pixel_pipeline_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  pixel_pipeline_stage.module = pixel_shader->shader_module();
  pixel_pipeline_stage.pName = "main";
  pixel_pipeline_stage.pSpecializationInfo = nullptr;

  return UpdateStatus::kMismatch;
}

PipelineCache::UpdateStatus PipelineCache::UpdateVertexInputState(
    VulkanShader* vertex_shader) {
  auto& regs = update_vertex_input_state_regs_;
  auto& state_info = update_vertex_input_state_info_;

  bool dirty = false;
  dirty |= vertex_shader != regs.vertex_shader;
  XXH64_update(&hash_state_, &regs, sizeof(regs));
  if (!dirty) {
    return UpdateStatus::kCompatible;
  }
  regs.vertex_shader = vertex_shader;

  state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  state_info.pNext = nullptr;
  state_info.flags = 0;

  auto& vertex_binding_descrs = update_vertex_input_state_binding_descrs_;
  auto& vertex_attrib_descrs = update_vertex_input_state_attrib_descrs_;
  uint32_t vertex_binding_count = 0;
  uint32_t vertex_attrib_count = 0;
  for (const auto& vertex_binding : vertex_shader->vertex_bindings()) {
    assert_true(vertex_binding_count < xe::countof(vertex_binding_descrs));
    auto& vertex_binding_descr = vertex_binding_descrs[vertex_binding_count++];
    vertex_binding_descr.binding = vertex_binding.binding_index;
    vertex_binding_descr.stride = vertex_binding.stride_words * 4;
    vertex_binding_descr.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    for (const auto& attrib : vertex_binding.attributes) {
      assert_true(vertex_attrib_count < xe::countof(vertex_attrib_descrs));
      auto& vertex_attrib_descr = vertex_attrib_descrs[vertex_attrib_count++];
      vertex_attrib_descr.location = attrib.attrib_index;
      vertex_attrib_descr.binding = vertex_binding.binding_index;
      vertex_attrib_descr.format = VK_FORMAT_UNDEFINED;
      vertex_attrib_descr.offset = attrib.fetch_instr.attributes.offset * 4;

      bool is_signed = attrib.fetch_instr.attributes.is_signed;
      bool is_integer = attrib.fetch_instr.attributes.is_integer;
      switch (attrib.fetch_instr.attributes.data_format) {
        case VertexFormat::k_8_8_8_8:
          vertex_attrib_descr.format =
              is_signed ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
          break;
        case VertexFormat::k_2_10_10_10:
          vertex_attrib_descr.format = is_signed
                                           ? VK_FORMAT_A2R10G10B10_SNORM_PACK32
                                           : VK_FORMAT_A2R10G10B10_UNORM_PACK32;
          break;
        case VertexFormat::k_10_11_11:
          assert_always("unsupported?");
          vertex_attrib_descr.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
          break;
        case VertexFormat::k_11_11_10:
          assert_true(is_signed);
          vertex_attrib_descr.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
          break;
        case VertexFormat::k_16_16:
          vertex_attrib_descr.format =
              is_signed ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
          break;
        case VertexFormat::k_16_16_FLOAT:
          vertex_attrib_descr.format =
              is_signed ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED;
          break;
        case VertexFormat::k_16_16_16_16:
          vertex_attrib_descr.format = is_signed ? VK_FORMAT_R16G16B16A16_SNORM
                                                 : VK_FORMAT_R16G16B16A16_UNORM;
          break;
        case VertexFormat::k_16_16_16_16_FLOAT:
          vertex_attrib_descr.format = is_signed
                                           ? VK_FORMAT_R16G16B16A16_SSCALED
                                           : VK_FORMAT_R16G16B16A16_USCALED;
          break;
        case VertexFormat::k_32:
          vertex_attrib_descr.format =
              is_signed ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
          break;
        case VertexFormat::k_32_32:
          vertex_attrib_descr.format =
              is_signed ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
          break;
        case VertexFormat::k_32_32_32_32:
          vertex_attrib_descr.format =
              is_signed ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32_UINT;
          break;
        case VertexFormat::k_32_FLOAT:
          assert_true(is_signed);
          vertex_attrib_descr.format = VK_FORMAT_R32_SFLOAT;
          break;
        case VertexFormat::k_32_32_FLOAT:
          assert_true(is_signed);
          vertex_attrib_descr.format = VK_FORMAT_R32G32_SFLOAT;
          break;
        case VertexFormat::k_32_32_32_FLOAT:
          assert_true(is_signed);
          vertex_attrib_descr.format = VK_FORMAT_R32G32B32_SFLOAT;
          break;
        case VertexFormat::k_32_32_32_32_FLOAT:
          assert_true(is_signed);
          vertex_attrib_descr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
          break;
        default:
          assert_unhandled_case(attrib.fetch_instr.attributes.data_format);
          break;
      }
    }
  }

  state_info.vertexBindingDescriptionCount = vertex_binding_count;
  state_info.pVertexBindingDescriptions = vertex_binding_descrs;
  state_info.vertexAttributeDescriptionCount = vertex_attrib_count;
  state_info.pVertexAttributeDescriptions = vertex_attrib_descrs;

  return UpdateStatus::kMismatch;
}

PipelineCache::UpdateStatus PipelineCache::UpdateInputAssemblyState(
    PrimitiveType primitive_type) {
  auto& regs = update_input_assembly_state_regs_;
  auto& state_info = update_input_assembly_state_info_;

  bool dirty = false;
  dirty |= primitive_type != regs.primitive_type;
  dirty |= SetShadowRegister(&regs.pa_su_sc_mode_cntl,
                             XE_GPU_REG_PA_SU_SC_MODE_CNTL);
  dirty |= SetShadowRegister(&regs.multi_prim_ib_reset_index,
                             XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX);
  XXH64_update(&hash_state_, &regs, sizeof(regs));
  if (!dirty) {
    return UpdateStatus::kCompatible;
  }
  regs.primitive_type = primitive_type;

  state_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  state_info.pNext = nullptr;
  state_info.flags = 0;

  switch (primitive_type) {
    case PrimitiveType::kPointList:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      break;
    case PrimitiveType::kLineList:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case PrimitiveType::kLineStrip:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PrimitiveType::kLineLoop:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PrimitiveType::kTriangleList:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case PrimitiveType::kTriangleStrip:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PrimitiveType::kTriangleFan:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case PrimitiveType::kRectangleList:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case PrimitiveType::kQuadList:
      state_info.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      break;
    default:
    case PrimitiveType::kUnknown0x07:
      XELOGE("unsupported primitive type %d", primitive_type);
      assert_unhandled_case(primitive_type);
      return UpdateStatus::kError;
  }

  // TODO(benvanik): anything we can do about this? Vulkan seems to only support
  // first.
  assert_zero(regs.pa_su_sc_mode_cntl & (1 << 19));
  // if (regs.pa_su_sc_mode_cntl & (1 << 19)) {
  //   glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
  // } else {
  //   glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
  // }

  if (regs.pa_su_sc_mode_cntl & (1 << 21)) {
    state_info.primitiveRestartEnable = VK_TRUE;
  } else {
    state_info.primitiveRestartEnable = VK_FALSE;
  }
  // TODO(benvanik): no way to specify in Vulkan?
  assert_true(regs.multi_prim_ib_reset_index == 0xFFFF ||
              regs.multi_prim_ib_reset_index == 0xFFFFFFFF);
  // glPrimitiveRestartIndex(regs.multi_prim_ib_reset_index);

  return UpdateStatus::kMismatch;
}

PipelineCache::UpdateStatus PipelineCache::UpdateViewportState() {
  auto& state_info = update_viewport_state_info_;

  state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  state_info.pNext = nullptr;
  state_info.flags = 0;

  state_info.viewportCount = 1;
  state_info.scissorCount = 1;

  // Ignored; set dynamically.
  state_info.pViewports = nullptr;
  state_info.pScissors = nullptr;

  return UpdateStatus::kCompatible;
}

PipelineCache::UpdateStatus PipelineCache::UpdateRasterizationState(
    PrimitiveType primitive_type) {
  auto& regs = update_rasterization_state_regs_;
  auto& state_info = update_rasterization_state_info_;

  bool dirty = false;
  dirty |= SetShadowRegister(&regs.pa_su_sc_mode_cntl,
                             XE_GPU_REG_PA_SU_SC_MODE_CNTL);
  dirty |= SetShadowRegister(&regs.pa_sc_screen_scissor_tl,
                             XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
  dirty |= SetShadowRegister(&regs.pa_sc_screen_scissor_br,
                             XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
  dirty |= SetShadowRegister(&regs.multi_prim_ib_reset_index,
                             XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX);
  XXH64_update(&hash_state_, &regs, sizeof(regs));
  if (!dirty) {
    return UpdateStatus::kCompatible;
  }

  state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  state_info.pNext = nullptr;
  state_info.flags = 0;

  // TODO(benvanik): right setting?
  state_info.depthClampEnable = VK_FALSE;

  // TODO(benvanik): use in depth-only mode?
  state_info.rasterizerDiscardEnable = VK_FALSE;

  bool poly_mode = ((regs.pa_su_sc_mode_cntl >> 3) & 0x3) != 0;
  if (poly_mode) {
    uint32_t front_poly_mode = (regs.pa_su_sc_mode_cntl >> 5) & 0x7;
    uint32_t back_poly_mode = (regs.pa_su_sc_mode_cntl >> 8) & 0x7;
    // Vulkan only supports both matching.
    assert_true(front_poly_mode == back_poly_mode);
    static const VkPolygonMode kFillModes[3] = {
        VK_POLYGON_MODE_POINT, VK_POLYGON_MODE_LINE, VK_POLYGON_MODE_FILL,
    };
    state_info.polygonMode = kFillModes[front_poly_mode];
  } else {
    state_info.polygonMode = VK_POLYGON_MODE_FILL;
  }

  switch (regs.pa_su_sc_mode_cntl & 0x3) {
    case 0:
      state_info.cullMode = VK_CULL_MODE_NONE;
      break;
    case 1:
      state_info.cullMode = VK_CULL_MODE_FRONT_BIT;
      break;
    case 2:
      state_info.cullMode = VK_CULL_MODE_BACK_BIT;
      break;
  }
  if (regs.pa_su_sc_mode_cntl & 0x4) {
    state_info.frontFace = VK_FRONT_FACE_CLOCKWISE;
  } else {
    state_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  }
  if (primitive_type == PrimitiveType::kRectangleList) {
    // Rectangle lists aren't culled. There may be other things they skip too.
    state_info.cullMode = VK_CULL_MODE_NONE;
  }

  state_info.depthBiasEnable = VK_FALSE;

  // Ignored; set dynamically:
  state_info.depthBiasConstantFactor = 0;
  state_info.depthBiasClamp = 0;
  state_info.depthBiasSlopeFactor = 0;
  state_info.lineWidth = 1.0f;

  return UpdateStatus::kMismatch;
}

PipelineCache::UpdateStatus PipelineCache::UpdateMultisampleState() {
  auto& regs = update_multisample_state_regs_;
  auto& state_info = update_multisample_state_info_;

  state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  state_info.pNext = nullptr;
  state_info.flags = 0;

  state_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  state_info.sampleShadingEnable = VK_FALSE;
  state_info.minSampleShading = 0;
  state_info.pSampleMask = nullptr;
  state_info.alphaToCoverageEnable = VK_FALSE;
  state_info.alphaToOneEnable = VK_FALSE;

  return UpdateStatus::kCompatible;
}

PipelineCache::UpdateStatus PipelineCache::UpdateDepthStencilState() {
  auto& regs = update_depth_stencil_state_regs_;
  auto& state_info = update_depth_stencil_state_info_;

  bool dirty = false;
  dirty |= SetShadowRegister(&regs.rb_depthcontrol, XE_GPU_REG_RB_DEPTHCONTROL);
  dirty |=
      SetShadowRegister(&regs.rb_stencilrefmask, XE_GPU_REG_RB_STENCILREFMASK);
  XXH64_update(&hash_state_, &regs, sizeof(regs));
  if (!dirty) {
    return UpdateStatus::kCompatible;
  }

  state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  state_info.pNext = nullptr;
  state_info.flags = 0;

  state_info.depthTestEnable = VK_FALSE;
  state_info.depthWriteEnable = VK_FALSE;
  state_info.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  state_info.depthBoundsTestEnable = VK_FALSE;
  state_info.stencilTestEnable = VK_FALSE;
  state_info.front.failOp = VK_STENCIL_OP_KEEP;
  state_info.front.passOp = VK_STENCIL_OP_KEEP;
  state_info.front.depthFailOp = VK_STENCIL_OP_KEEP;
  state_info.front.compareOp = VK_COMPARE_OP_ALWAYS;
  state_info.back.failOp = VK_STENCIL_OP_KEEP;
  state_info.back.passOp = VK_STENCIL_OP_KEEP;
  state_info.back.depthFailOp = VK_STENCIL_OP_KEEP;
  state_info.back.compareOp = VK_COMPARE_OP_ALWAYS;

  // Ignored; set dynamically.
  state_info.minDepthBounds = 0;
  state_info.maxDepthBounds = 0;
  state_info.front.compareMask = 0;
  state_info.front.writeMask = 0;
  state_info.front.reference = 0;
  state_info.back.compareMask = 0;
  state_info.back.writeMask = 0;
  state_info.back.reference = 0;

  return UpdateStatus::kMismatch;
}

PipelineCache::UpdateStatus PipelineCache::UpdateColorBlendState() {
  auto& regs = update_color_blend_state_regs_;
  auto& state_info = update_color_blend_state_info_;

  // Alpha testing -- ALPHAREF, ALPHAFUNC, ALPHATESTENABLE
  // Deprecated in GL, implemented in shader.
  // if(ALPHATESTENABLE && frag_out.a [<=/ALPHAFUNC] ALPHAREF) discard;
  // uint32_t color_control = reg_file[XE_GPU_REG_RB_COLORCONTROL].u32;
  // draw_batcher_.set_alpha_test((color_control & 0x4) != 0,  //
  // ALPAHTESTENABLE
  //                             color_control & 0x7,         // ALPHAFUNC
  //                             reg_file[XE_GPU_REG_RB_ALPHA_REF].f32);

  bool dirty = false;
  dirty |= SetShadowRegister(&regs.rb_colorcontrol, XE_GPU_REG_RB_COLORCONTROL);
  dirty |= SetShadowRegister(&regs.rb_color_mask, XE_GPU_REG_RB_COLOR_MASK);
  dirty |=
      SetShadowRegister(&regs.rb_blendcontrol[0], XE_GPU_REG_RB_BLENDCONTROL_0);
  dirty |=
      SetShadowRegister(&regs.rb_blendcontrol[1], XE_GPU_REG_RB_BLENDCONTROL_1);
  dirty |=
      SetShadowRegister(&regs.rb_blendcontrol[2], XE_GPU_REG_RB_BLENDCONTROL_2);
  dirty |=
      SetShadowRegister(&regs.rb_blendcontrol[3], XE_GPU_REG_RB_BLENDCONTROL_3);
  XXH64_update(&hash_state_, &regs, sizeof(regs));
  if (!dirty) {
    return UpdateStatus::kCompatible;
  }

  state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  state_info.pNext = nullptr;
  state_info.flags = 0;

  state_info.logicOpEnable = VK_FALSE;
  state_info.logicOp = VK_LOGIC_OP_NO_OP;

  static const VkBlendFactor kBlendFactorMap[] = {
      /*  0 */ VK_BLEND_FACTOR_ZERO,
      /*  1 */ VK_BLEND_FACTOR_ONE,
      /*  2 */ VK_BLEND_FACTOR_ZERO,  // ?
      /*  3 */ VK_BLEND_FACTOR_ZERO,  // ?
      /*  4 */ VK_BLEND_FACTOR_SRC_COLOR,
      /*  5 */ VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      /*  6 */ VK_BLEND_FACTOR_SRC_ALPHA,
      /*  7 */ VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      /*  8 */ VK_BLEND_FACTOR_DST_COLOR,
      /*  9 */ VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      /* 10 */ VK_BLEND_FACTOR_DST_ALPHA,
      /* 11 */ VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      /* 12 */ VK_BLEND_FACTOR_CONSTANT_COLOR,
      /* 13 */ VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      /* 14 */ VK_BLEND_FACTOR_CONSTANT_ALPHA,
      /* 15 */ VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      /* 16 */ VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  static const VkBlendOp kBlendOpMap[] = {
      /*  0 */ VK_BLEND_OP_ADD,
      /*  1 */ VK_BLEND_OP_SUBTRACT,
      /*  2 */ VK_BLEND_OP_MIN,
      /*  3 */ VK_BLEND_OP_MAX,
      /*  4 */ VK_BLEND_OP_REVERSE_SUBTRACT,
  };
  auto& attachment_states = update_color_blend_attachment_states_;
  for (int i = 0; i < 4; ++i) {
    uint32_t blend_control = regs.rb_blendcontrol[i];
    auto& attachment_state = attachment_states[i];
    attachment_state.blendEnable = !(regs.rb_colorcontrol & 0x20);
    // A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND
    attachment_state.srcColorBlendFactor =
        kBlendFactorMap[(blend_control & 0x0000001F) >> 0];
    // A2XX_RB_BLEND_CONTROL_COLOR_DESTBLEND
    attachment_state.dstColorBlendFactor =
        kBlendFactorMap[(blend_control & 0x00001F00) >> 8];
    // A2XX_RB_BLEND_CONTROL_COLOR_COMB_FCN
    attachment_state.colorBlendOp =
        kBlendOpMap[(blend_control & 0x000000E0) >> 5];
    // A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND
    attachment_state.srcAlphaBlendFactor =
        kBlendFactorMap[(blend_control & 0x001F0000) >> 16];
    // A2XX_RB_BLEND_CONTROL_ALPHA_DESTBLEND
    attachment_state.dstAlphaBlendFactor =
        kBlendFactorMap[(blend_control & 0x1F000000) >> 24];
    // A2XX_RB_BLEND_CONTROL_ALPHA_COMB_FCN
    attachment_state.alphaBlendOp =
        kBlendOpMap[(blend_control & 0x00E00000) >> 21];
    // A2XX_RB_COLOR_MASK_WRITE_* == D3DRS_COLORWRITEENABLE
    // Lines up with VkColorComponentFlagBits, where R=bit 1, G=bit 2, etc..
    uint32_t write_mask = (regs.rb_color_mask >> (i * 4)) & 0xF;
    attachment_state.colorWriteMask = write_mask;
  }

  state_info.attachmentCount = 4;
  state_info.pAttachments = attachment_states;

  // Ignored; set dynamically.
  state_info.blendConstants[0] = 0.0f;
  state_info.blendConstants[1] = 0.0f;
  state_info.blendConstants[2] = 0.0f;
  state_info.blendConstants[3] = 0.0f;

  return UpdateStatus::kMismatch;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe