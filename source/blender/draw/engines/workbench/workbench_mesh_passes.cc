/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "workbench_private.hh"

namespace blender::workbench {

MeshPass::MeshPass(const char *name) : PassMain(name){};

/* Move to draw::Pass */
bool MeshPass::is_empty() const
{
  return false; /* TODO */
}

void MeshPass::init(ePipelineType pipeline,
                    eColorType color_type,
                    eShadingType shading,
                    SceneResources &resources,
                    DRWState state)
{
  ShaderCache &shaders = resources.shader_cache;

  this->PassMain::init();
  this->state_set(state);
  this->bind_texture(WB_MATCAP_SLOT, resources.matcap_tx);
  this->bind_ssbo(WB_MATERIAL_SLOT, &resources.material_buf);
  this->bind_ubo(WB_WORLD_SLOT, resources.world_buf);

  color_type_ = color_type;
  texture_subpass_map.clear();

  for (int geom = 0; geom < geometry_type_len; geom++) {
    eGeometryType geom_type = static_cast<eGeometryType>(geom);
    GPUShader *sh = shaders.prepass_shader_get(pipeline, geom_type, color_type, shading);
    PassMain::Sub *pass = &this->sub(get_name(geom_type));
    pass->shader_set(sh);
    geometry_passes_[geom] = pass;
  }
}

PassMain::Sub &MeshPass::sub_pass_get(eGeometryType geometry_type,
                                      ObjectRef & /*ref*/,
                                      ::Material * /*material*/)
{
  if (color_type_ == eColorType::TEXTURE) {
    /* TODO(fclem): Always query a layered texture so we can use only a single shader. */
    GPUTexture *texture = nullptr;  // ref.object->texture_get();
    GPUTexture *tilemap = nullptr;  // ref.object->texture_get();

    auto add_cb = [&] {
      PassMain::Sub *sub_pass = geometry_passes_[static_cast<int>(geometry_type)];
      sub_pass = &sub_pass->sub("Blender Texture Name" /* texture.name */);
      sub_pass->bind_texture(WB_TEXTURE_SLOT, texture);
      sub_pass->bind_texture(WB_TILEMAP_SLOT, tilemap);
      return sub_pass;
    };

    return *texture_subpass_map.lookup_or_add_cb(TextureSubPassKey(texture, geometry_type),
                                                 add_cb);
  }
  return *geometry_passes_[static_cast<int>(geometry_type)];
}

void OpaquePass::sync(DRWState cull_state,
                      DRWState clip_state,
                      eShadingType shading_type,
                      eColorType color_type,
                      SceneResources &resources)
{
  Texture &depth_tx = resources.depth_tx;
  Texture &depth_in_front_tx = resources.depth_in_front_tx;
  TextureFromPool &color_tx = resources.color_tx;
  ShaderCache &shaders = resources.shader_cache;
  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                   cull_state | clip_state;

  gbuffer_ps_.init(ePipelineType::OPAQUE, color_type, shading_type, resources, state);

  deferred_ps_.init();
  deferred_ps_.shader_set(shaders.resolve_shader_get(ePipelineType::OPAQUE, shading_type));
  deferred_ps_.bind_ubo(WB_WORLD_SLOT, resources.world_buf);
  deferred_ps_.bind_texture(WB_MATCAP_SLOT, resources.matcap_tx);
  deferred_ps_.bind_texture("normal_tx", &gbuffer_normal_tx);
  deferred_ps_.bind_texture("material_tx", &gbuffer_material_tx);
  deferred_ps_.bind_texture("depth_tx", &depth_tx);
  deferred_ps_.bind_image("out_color_img", &color_tx);
  deferred_ps_.dispatch(math::divide_ceil(int2(depth_tx.size()), int2(WB_RESOLVE_GROUP_SIZE)));
  deferred_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH);
}

void OpaquePass::draw_prepass(Manager &manager, View &view, Texture &depth_tx)
{
  gbuffer_material_tx.acquire(int2(depth_tx.size()), GPU_RGBA16F);
  gbuffer_normal_tx.acquire(int2(depth_tx.size()), GPU_RG16F);
  gbuffer_object_id_tx.acquire(int2(depth_tx.size()), GPU_R16UI);

  opaque_fb.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx),
                   GPU_ATTACHMENT_TEXTURE(gbuffer_material_tx),
                   GPU_ATTACHMENT_TEXTURE(gbuffer_normal_tx),
                   GPU_ATTACHMENT_TEXTURE(gbuffer_object_id_tx));
  opaque_fb.bind();
  opaque_fb.clear_depth(1.0f);

  manager.submit(gbuffer_ps_, view);
}

void OpaquePass::draw_resolve(Manager &manager, View &view)
{
  manager.submit(deferred_ps_, view);

  gbuffer_normal_tx.release();
  gbuffer_material_tx.release();
  gbuffer_object_id_tx.release();
}

bool OpaquePass::is_empty() const
{
  return gbuffer_ps_.is_empty();
}

void TransparentPass::sync(DRWState cull_state,
                           DRWState clip_state,
                           eShadingType shading_type,
                           eColorType color_type,
                           SceneResources &resources)
{
  ShaderCache &shaders = resources.shader_cache;
  Texture &depth_tx = resources.depth_tx;
  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                   cull_state | clip_state;

  accumulation_ps_.init(ePipelineType::TRANSPARENT, color_type, shading_type, resources, state);

  resolve_ps_.init();
  resolve_ps_.shader_set(
      shaders.resolve_shader_get(ePipelineType::TRANSPARENT, eShadingType::FLAT));
  resolve_ps_.bind_texture("accumulation_tx", accumulation_tx);
  resolve_ps_.bind_texture("reveal_tx", reveal_tx);
  resolve_ps_.dispatch(math::divide_ceil(depth_tx.size(), int3(WB_RESOLVE_GROUP_SIZE)));
}

void TransparentPass::draw_prepass(Manager &manager, View &view, Texture &depth_tx)
{
  accumulation_tx.acquire(int2(depth_tx.size()), GPU_RGBA16F);
  reveal_tx.acquire(int2(depth_tx.size()), GPU_R8);

  transparent_fb.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx),
                        GPU_ATTACHMENT_TEXTURE(accumulation_tx),
                        GPU_ATTACHMENT_TEXTURE(reveal_tx));
  transparent_fb.bind();

  manager.submit(accumulation_ps_, view);
}

void TransparentPass::draw_resolve(Manager &manager, View &view)
{
  manager.submit(resolve_ps_, view);

  accumulation_tx.release();
  reveal_tx.release();
}

bool TransparentPass::is_empty() const
{
  return accumulation_ps_.is_empty();
}

}  // namespace blender::workbench