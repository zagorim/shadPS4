// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "common/scope_exit.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

using Shader::LogicalStage;

static constexpr auto gp_stage_flags =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

GraphicsPipeline::GraphicsPipeline(const Instance& instance_, Scheduler& scheduler_,
                                   DescriptorHeap& desc_heap_, const GraphicsPipelineKey& key_,
                                   vk::PipelineCache pipeline_cache,
                                   std::span<const Shader::Info*, MaxShaderStages> infos,
                                   std::span<const vk::ShaderModule> modules)
    : Pipeline{instance_, scheduler_, desc_heap_, pipeline_cache}, key{key_} {
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout();
    const bool uses_tessellation = stages[u32(LogicalStage::TessellationControl)];

    const vk::PushConstantRange push_constants = {
        .stageFlags = gp_stage_flags,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);

    boost::container::static_vector<vk::VertexInputBindingDescription, 32> vertex_bindings;
    boost::container::static_vector<vk::VertexInputAttributeDescription, 32> vertex_attributes;
    if (!instance.IsVertexInputDynamicState()) {
        const auto& vs_info = stages[u32(LogicalStage::Vertex)];
        for (const auto& input : vs_info->vs_inputs) {
            if (input.instance_step_rate == Shader::Info::VsInput::InstanceIdType::OverStepRate0 ||
                input.instance_step_rate == Shader::Info::VsInput::InstanceIdType::OverStepRate1) {
                // Skip attribute binding as the data will be pulled by shader
                continue;
            }

            const auto buffer =
                vs_info->ReadUdReg<AmdGpu::Buffer>(input.sgpr_base, input.dword_offset);
            if (buffer.GetSize() == 0) {
                continue;
            }
            vertex_attributes.push_back({
                .location = input.binding,
                .binding = input.binding,
                .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
                .offset = 0,
            });
            vertex_bindings.push_back({
                .binding = input.binding,
                .stride = buffer.GetStride(),
                .inputRate = input.instance_step_rate == Shader::Info::VsInput::None
                                 ? vk::VertexInputRate::eVertex
                                 : vk::VertexInputRate::eInstance,
            });
        }
    }

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = static_cast<u32>(vertex_bindings.size()),
        .pVertexBindingDescriptions = vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes.size()),
        .pVertexAttributeDescriptions = vertex_attributes.data(),
    };

    if (key.prim_type == AmdGpu::PrimitiveType::RectList && !IsEmbeddedVs()) {
        LOG_WARNING(Render_Vulkan,
                    "Rectangle List primitive type is only supported for embedded VS");
    }

    auto prim_restart = key.enable_primitive_restart != 0;
    if (prim_restart && IsPrimitiveListTopology() && !instance.IsListRestartSupported()) {
        LOG_WARNING(Render_Vulkan,
                    "Primitive restart is enabled for list topology but not supported by driver.");
        prim_restart = false;
    }
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = LiverpoolToVK::PrimitiveType(key.prim_type),
        .primitiveRestartEnable = prim_restart,
    };
    ASSERT_MSG(!prim_restart || key.primitive_restart_index == 0xFFFF ||
                   key.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        // TODO how to handle optional member of graphics key when dynamic state not supported?
        //.patchControlPoints = key.
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
        .cullMode = LiverpoolToVK::CullMode(key.cull_mode),
        .frontFace = key.front_face == Liverpool::FrontFace::Clockwise
                         ? vk::FrontFace::eClockwise
                         : vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = bool(key.depth_bias_enable),
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples =
            LiverpoolToVK::NumSamples(key.num_samples, instance.GetFramebufferSampleCounts()),
        .sampleShadingEnable = false,
    };

    const vk::Viewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        .negativeOneToOne = key.clip_space == Liverpool::ClipSpace::MinusWToW,
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    boost::container::static_vector<vk::DynamicState, 14> dynamic_states = {
        vk::DynamicState::eViewport,           vk::DynamicState::eScissor,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthBounds,
        vk::DynamicState::eDepthBias,          vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
    };

    if (instance.IsColorWriteEnableSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteEnableEXT);
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else {
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStrideEXT);
    }
    ASSERT(instance.IsPatchControlPointsDynamicState()); // TODO remove
    if (instance.IsPatchControlPointsDynamicState() && uses_tessellation) {
        dynamic_states.push_back(vk::DynamicState::ePatchControlPointsEXT);
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = key.depth_stencil.depth_enable,
        .depthWriteEnable = key.depth_stencil.depth_write_enable,
        .depthCompareOp = LiverpoolToVK::CompareOp(key.depth_stencil.depth_func),
        .depthBoundsTestEnable = key.depth_stencil.depth_bounds_enable,
        .stencilTestEnable = key.depth_stencil.stencil_enable,
        .front{
            .failOp = LiverpoolToVK::StencilOp(key.stencil.stencil_fail_front),
            .passOp = LiverpoolToVK::StencilOp(key.stencil.stencil_zpass_front),
            .depthFailOp = LiverpoolToVK::StencilOp(key.stencil.stencil_zfail_front),
            .compareOp = LiverpoolToVK::CompareOp(key.depth_stencil.stencil_ref_func),
        },
        .back{
            .failOp = LiverpoolToVK::StencilOp(key.depth_stencil.backface_enable
                                                   ? key.stencil.stencil_fail_back.Value()
                                                   : key.stencil.stencil_fail_front.Value()),
            .passOp = LiverpoolToVK::StencilOp(key.depth_stencil.backface_enable
                                                   ? key.stencil.stencil_zpass_back.Value()
                                                   : key.stencil.stencil_zpass_front.Value()),
            .depthFailOp = LiverpoolToVK::StencilOp(key.depth_stencil.backface_enable
                                                        ? key.stencil.stencil_zfail_back.Value()
                                                        : key.stencil.stencil_zfail_front.Value()),
            .compareOp = LiverpoolToVK::CompareOp(key.depth_stencil.backface_enable
                                                      ? key.depth_stencil.stencil_bf_func.Value()
                                                      : key.depth_stencil.stencil_ref_func.Value()),
        },
    };

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(LogicalStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(LogicalStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(LogicalStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(LogicalStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(LogicalStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    }

    const auto it = std::ranges::find(key.color_formats, vk::Format::eUndefined);
    const u32 num_color_formats = std::distance(key.color_formats.begin(), it);
    const vk::PipelineRenderingCreateInfoKHR pipeline_rendering_ci = {
        .colorAttachmentCount = num_color_formats,
        .pColorAttachmentFormats = key.color_formats.data(),
        .depthAttachmentFormat = key.depth_format,
        .stencilAttachmentFormat = key.stencil_format,
    };

    std::array<vk::PipelineColorBlendAttachmentState, Liverpool::NumColorBuffers> attachments;
    for (u32 i = 0; i < num_color_formats; i++) {
        const auto& control = key.blend_controls[i];
        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);
        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = control.separate_alpha_blend
                                       ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                       : src_color,
            .dstAlphaBlendFactor = control.separate_alpha_blend
                                       ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                       : dst_color,
            .alphaBlendOp = control.separate_alpha_blend
                                ? LiverpoolToVK::BlendOp(control.alpha_func)
                                : color_blend,
            .colorWriteMask =
                instance.IsColorWriteEnableSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & Liverpool::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = num_color_formats,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = (uses_tessellation && !instance.IsPatchControlPointsDynamicState())
                                  ? &tessellation_state
                                  : nullptr,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_info,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    auto [pipeline_result, pipe] =
        device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
}

GraphicsPipeline::~GraphicsPipeline() = default;

void GraphicsPipeline::BuildDescSetLayout() {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }

        if (stage->has_readconst) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .descriptorCount = 1,
                .stageFlags = gp_stage_flags,
            });
        }
        for (const auto& buffer : stage->buffers) {
            const auto sharp = buffer.GetSharp(*stage);
            bindings.push_back({
                .binding = binding++,
                .descriptorType = buffer.IsStorage(sharp) ? vk::DescriptorType::eStorageBuffer
                                                          : vk::DescriptorType::eUniformBuffer,
                .descriptorCount = 1,
                .stageFlags = gp_stage_flags,
            });
        }
        for (const auto& tex_buffer : stage->texture_buffers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = tex_buffer.is_written ? vk::DescriptorType::eStorageTexelBuffer
                                                        : vk::DescriptorType::eUniformTexelBuffer,
                .descriptorCount = 1,
                .stageFlags = gp_stage_flags,
            });
        }
        for (const auto& image : stage->images) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = image.is_storage ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = 1,
                .stageFlags = gp_stage_flags,
            });
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = gp_stage_flags,
            });
        }
    }
    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

void GraphicsPipeline::BindResources(const Liverpool::Regs& regs,
                                     VideoCore::BufferCache& buffer_cache,
                                     VideoCore::TextureCache& texture_cache) const {
    // Bind resource buffers and textures.
    boost::container::small_vector<vk::WriteDescriptorSet, 16> set_writes;
    BufferBarriers buffer_barriers;
    Shader::PushData push_data{};
    Shader::Backend::Bindings binding{};

    buffer_infos.clear();
    buffer_views.clear();
    image_infos.clear();

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        if (stage->uses_step_rates) {
            push_data.step0 = regs.vgt_instance_step_rate_0;
            push_data.step1 = regs.vgt_instance_step_rate_1;
        }
        stage->PushUd(binding, push_data);

        BindBuffers(buffer_cache, texture_cache, *stage, binding, push_data, set_writes,
                    buffer_barriers);

        BindTextures(texture_cache, *stage, binding, set_writes);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    SCOPE_EXIT {
        cmdbuf.pushConstants(*pipeline_layout, gp_stage_flags, 0U, sizeof(push_data), &push_data);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, Handle());
    };

    if (set_writes.empty()) {
        return;
    }

    if (!buffer_barriers.empty()) {
        const auto dependencies = vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = u32(buffer_barriers.size()),
            .pBufferMemoryBarriers = buffer_barriers.data(),
        };
        scheduler.EndRendering();
        cmdbuf.pipelineBarrier2(dependencies);
    }

    // Bind descriptor set.
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                    set_writes);
        return;
    }
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, desc_set, {});
}

} // namespace Vulkan
