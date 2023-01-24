/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 *
 * The shadow module manages shadow update tagging & shadow rendering.
 */

#include "BKE_global.h"
#include "BLI_rect.h"

#include "eevee_instance.hh"

#include "draw_debug.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Tile map
 *
 * \{ */

void ShadowTileMap::sync_clipmap(const float4x4 &object_mat_,
                                 int2 origin_offset,
                                 int clipmap_level)
{
  if (is_cubeface || (level != clipmap_level)) {
    set_dirty();
  }
  is_cubeface = false;
  level = clipmap_level;

  if (grid_shift == int2(0)) {
    /* Only replace shift if it is not already dirty. */
    grid_shift = origin_offset - grid_offset;
  }
  grid_offset = origin_offset;

  if (!equals_m4m4(object_mat.ptr(), object_mat_.ptr())) {
    object_mat = object_mat_;
    set_dirty();
  }

  float tile_size = clipmap_tile_size_get(level);

  /* object_mat is a rotation matrix. Reduce imprecision by taking the transpose which is also the
   * inverse in this particular case. */
  viewmat = object_mat.transposed();

  float half_size = clipmap_level_coverage_get(level) / 2.0f;
  float2 win_offset = float2(grid_offset) * tile_size;
  orthographic_m4(winmat.ptr(),
                  -half_size + win_offset.x,
                  half_size + win_offset.x,
                  -half_size + win_offset.y,
                  half_size + win_offset.y,
                  /* Near/far is computed on GPU using casters bounds. */
                  -1.0,
                  1.0);
}

void ShadowTileMap::sync_cubeface(const float4x4 &object_mat_,
                                  float near_,
                                  float far_,
                                  eCubeFace face)
{
  if (!is_cubeface || (cubeface != face) || (near != near_) || (far != far_)) {
    set_dirty();
  }
  is_cubeface = true;
  cubeface = face;
  near = near_;
  far = far_;
  grid_offset = int2(0);

  if (!equals_m4m4(object_mat.ptr(), object_mat_.ptr())) {
    object_mat = object_mat_;
    set_dirty();
  }

  perspective_m4(winmat.ptr(), -near, near, -near, near, near, far);
  viewmat = float4x4(shadow_face_mat[cubeface]) * object_mat.inverted_affine();

  /* Update corners. */
  float4x4 viewinv = object_mat;
  *reinterpret_cast<float3 *>(&corners[0]) = viewinv.translation();
  *reinterpret_cast<float3 *>(&corners[1]) = viewinv * float3(-far, -far, -far);
  *reinterpret_cast<float3 *>(&corners[2]) = viewinv * float3(far, -far, -far);
  *reinterpret_cast<float3 *>(&corners[3]) = viewinv * float3(-far, far, -far);
  /* Store deltas. */
  corners[2] = (corners[2] - corners[1]) / float(SHADOW_TILEMAP_RES);
  corners[3] = (corners[3] - corners[1]) / float(SHADOW_TILEMAP_RES);
}

void ShadowTileMap::debug_draw() const
{
  /** Used for debug drawing. */
  float4 debug_color[6] = {{1.0f, 0.1f, 0.1f, 1.0f},
                           {0.1f, 1.0f, 0.1f, 1.0f},
                           {0.0f, 0.2f, 1.0f, 1.0f},
                           {1.0f, 1.0f, 0.3f, 1.0f},
                           {0.1f, 0.1f, 0.1f, 1.0f},
                           {1.0f, 1.0f, 1.0f, 1.0f}};
  float4 color = debug_color[((is_cubeface ? cubeface : level) + 9999) % 6];

  float4x4 persinv = winmat * viewmat;
  drw_debug_matrix_as_bbox(persinv.inverted(), color);

  // int64_t div = ShadowTileMapPool::maps_per_row;
  // std::stringstream ss;
  // ss << "[" << tiles_index % div << ":" << tiles_index / div << "]";
  // std::string text = ss.str();

  // float3 pos = persinv * float3(0.0f, 0.0f, (is_cubeface) ? 1.0f : 0.0f);

  // uchar ucolor[4];
  // rgba_float_to_uchar(ucolor, color);
  // struct DRWTextStore *dt = DRW_text_cache_ensure();
  // DRW_text_cache_add(dt, pos, text.c_str(), text.size(), 0, 0, DRW_TEXT_CACHE_GLOBALSPACE,
  // ucolor);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile map pool
 *
 * \{ */

ShadowTileMapPool::ShadowTileMapPool()
{
  free_indices.reserve(SHADOW_MAX_TILEMAP);
  /* Reverse order to help debugging (first allocated tilemap will get 0). */
  for (int i = SHADOW_MAX_TILEMAP - 1; i >= 0; i--) {
    free_indices.append(i * SHADOW_TILEDATA_PER_TILEMAP);
  }

  int2 extent;
  extent.x = min_ii(SHADOW_MAX_TILEMAP, maps_per_row) * ShadowTileMap::tile_map_resolution;
  extent.y = (SHADOW_MAX_TILEMAP / maps_per_row) * ShadowTileMap::tile_map_resolution;

  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  tilemap_tx.ensure_2d(GPU_R32UI, extent, usage);
  tilemap_tx.clear(uint4(0));
}

ShadowTileMap *ShadowTileMapPool::acquire()
{
  if (free_indices.is_empty()) {
    /* Grow the tilemap buffer. See `end_sync`. */
    for (auto i : IndexRange(free_indices.size(), SHADOW_MAX_TILEMAP)) {
      free_indices.append(i * SHADOW_TILEDATA_PER_TILEMAP);
    }
  }
  int index = free_indices.pop_last();
  return &tilemap_pool.construct(ShadowTileMap(index));
}

void ShadowTileMapPool::release(Span<ShadowTileMap *> free_list)
{
  for (ShadowTileMap *map : free_list) {
    free_indices.append(map->tiles_index);
    tilemap_pool.destruct(*map);
  }
}

void ShadowTileMapPool::end_sync(ShadowModule &module)
{
  tilemaps_data.push_update();

  uint needed_tilemap_capacity = (free_indices.size() + tilemap_pool.size());
  if (needed_tilemap_capacity != (tiles_data.size() / SHADOW_TILEDATA_PER_TILEMAP)) {
    tiles_data.resize(needed_tilemap_capacity * SHADOW_TILEDATA_PER_TILEMAP);
    tilemaps_clip.resize(needed_tilemap_capacity);
    /* We reallocated the tile-map buffer, discarding all the data it contained.
     * We need to re-init the page heaps. */
    module.do_full_update = true;
  }

  tilemaps_unused.clear();
  int64_t newly_unused_count = free_indices.size() - last_free_len;
  if (newly_unused_count > 0) {
    /* Upload tile-map indices which pages needs to be pushed back to the free page heap. */
    Span<uint> newly_unused_indices = free_indices.as_span().slice(last_free_len,
                                                                   newly_unused_count);
    for (uint index : newly_unused_indices) {
      /* Push a dummy tilemap to a unused tilemap buffer. It is then processed through the some of
       * the setup steps to release the pages. */
      ShadowTileMapData tilemap_data = {};
      tilemap_data.tiles_index = index;
      tilemap_data.clip_data_index = 0;
      tilemap_data.grid_shift = int2(SHADOW_TILEMAP_RES);
      tilemap_data.is_cubeface = true;

      tilemaps_unused.append(tilemap_data);
    }
    tilemaps_unused.push_update();
  }

  last_free_len = free_indices.size();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Punctual
 *
 * \{ */

void ShadowPunctual::sync(eLightType light_type,
                          const float4x4 &object_mat,
                          float cone_aperture,
                          float near_clip,
                          float far_clip)
{
  if (light_type == LIGHT_SPOT) {
    tilemaps_needed_ = (cone_aperture > DEG2RADF(90.0f)) ? 5 : 1;
  }
  else if (is_area_light(light_type)) {
    tilemaps_needed_ = 5;
  }
  else {
    tilemaps_needed_ = 6;
  }

  far_ = max_ff(far_clip, 3e-4f);
  near_ = min_ff(near_clip, far_clip - 1e-4f);
  light_type_ = light_type;

  /* Keep custom data. */
  size_x_ = _area_size_x;
  size_y_ = _area_size_y;

  position_ = float3(object_mat[3]);
}

void ShadowPunctual::release_excess_tilemaps()
{
  if (tilemaps_.size() <= tilemaps_needed_) {
    return;
  }
  auto span = tilemaps_.as_span();
  shadows_.tilemap_pool.release(span.drop_front(tilemaps_needed_));
  tilemaps_ = span.take_front(tilemaps_needed_);
}

void ShadowPunctual::end_sync(Light &light)
{
  ShadowTileMapPool &tilemap_pool = shadows_.tilemap_pool;

  float4x4 obmat_tmp = light.object_mat;

  /* Clear embedded custom data. */
  obmat_tmp.values[0][3] = obmat_tmp.values[1][3] = obmat_tmp.values[2][3] = 0.0f;
  obmat_tmp.values[3][3] = 1.0f;

  /* Acquire missing tilemaps. */
  while (tilemaps_.size() < tilemaps_needed_) {
    tilemaps_.append(tilemap_pool.acquire());
  }

  tilemaps_[Z_NEG]->sync_cubeface(obmat_tmp, near_, far_, Z_NEG);
  if (tilemaps_needed_ >= 5) {
    tilemaps_[X_POS]->sync_cubeface(obmat_tmp, near_, far_, X_POS);
    tilemaps_[X_NEG]->sync_cubeface(obmat_tmp, near_, far_, X_NEG);
    tilemaps_[Y_POS]->sync_cubeface(obmat_tmp, near_, far_, Y_POS);
    tilemaps_[Y_NEG]->sync_cubeface(obmat_tmp, near_, far_, Y_NEG);
  }
  if (tilemaps_needed_ == 6) {
    tilemaps_[Z_POS]->sync_cubeface(obmat_tmp, near_, far_, Z_POS);
  }

  /* Normal matrix to convert geometric normal to optimal bias. */
  float4x4 &winmat = tilemaps_[Z_NEG]->winmat;
  float4x4 normal_mat = winmat.transposed().inverted();
  light.normal_mat_packed.x = normal_mat[3][2];
  light.normal_mat_packed.y = normal_mat[3][3];

  light.tilemap_index = tilemap_pool.tilemaps_data.size();

  /* A bit weird give we are inside a punctual shadow, but this is
   * in order to make light_tilemap_max_get() work. */
  light.clipmap_lod_min = 0;
  light.clipmap_lod_max = tilemaps_needed_ - 1;

  union {
    float f;
    int32_t i;
  } as_int;
  as_int.f = near_;
  light.clip_near = as_int.i;
  as_int.f = far_;
  light.clip_far = as_int.i;

  for (ShadowTileMap *tilemap : tilemaps_) {
    /* Add shadow tile-maps grouped by lights to the GPU buffer. */
    tilemap_pool.tilemaps_data.append(*tilemap);
    tilemap->set_updated();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Directional Shadow Maps
 *
 * \{ */

IndexRange ShadowDirectional::clipmap_level_range(const Camera &camera)
{
  int user_min_level = floorf(log2(min_resolution_));
  /* Covers the farthest points of the view. */
  int max_level = ceil(
      log2(camera.bound_radius() + math::distance(camera.bound_center(), camera.position())));
  /* We actually need to cover a bit more because of clipmap origin snapping. */
  max_level += 1;
  /* Covers the closest points of the view. */
  int min_level = floor(log2(abs(camera.data_get().clip_near)));
  min_level = clamp_i(user_min_level, min_level, max_level);

  if (camera.is_orthographic()) {
    /* FIXME: Single level for now. Should find a better mapping. */
    min_level = max_level;
  }

  IndexRange range(min_level, max_level - min_level + 1);
  /* The maximum level count is bounded by the mantissa of a 32bit float. Take top-most level to
   * still cover the whole view. */
  /* Take 16 to be able to pack offset into a single int2. */
  range = range.take_back(16);

  return range;
}

void ShadowDirectional::sync(const float4x4 &object_mat, float min_resolution)
{
  object_mat_ = object_mat;
  /* Clear embedded custom data. */
  object_mat_.values[0][3] = object_mat_.values[1][3] = object_mat_.values[2][3] = 0.0f;
  object_mat_.values[3][3] = 1.0f;
  /* Remove translation. */
  zero_v3(object_mat_.values[3]);

  min_resolution_ = min_resolution;
}

void ShadowDirectional::release_excess_tilemaps(const Camera &camera)
{
  IndexRange lods_new = clipmap_level_range(camera);
  if (lods_range == lods_new) {
    return;
  }

  IndexRange isect_range = lods_range.intersect(lods_new);
  IndexRange before_range(lods_range.start(), isect_range.start() - lods_range.start());
  IndexRange after_range(isect_range.one_after_last(),
                         lods_range.one_after_last() - isect_range.one_after_last());

  auto span = tilemaps_.as_span();
  shadows_.tilemap_pool.release(span.slice(before_range.shift(-lods_range.start())));
  shadows_.tilemap_pool.release(span.slice(after_range.shift(-lods_range.start())));
  tilemaps_ = span.slice(isect_range.shift(-lods_range.start()));
  lods_range = isect_range;
}

void ShadowDirectional::end_sync(Light &light, const Camera &camera)
{
  ShadowTileMapPool &tilemap_pool = shadows_.tilemap_pool;
  IndexRange lods_new = clipmap_level_range(camera);

  if (lods_range != lods_new) {
    /* Acquire missing tilemaps. */
    IndexRange isect_range = lods_new.intersect(lods_range);
    int64_t before_range = isect_range.start() - lods_new.start();
    int64_t after_range = lods_new.one_after_last() - isect_range.one_after_last();

    Vector<ShadowTileMap *> cached_tilemaps = tilemaps_;
    tilemaps_.clear();
    for (int64_t i = 0; i < before_range; i++) {
      tilemaps_.append(tilemap_pool.acquire());
    }
    /* Keep cached lods. */
    tilemaps_.extend(cached_tilemaps);
    for (int64_t i = 0; i < after_range; i++) {
      tilemaps_.append(tilemap_pool.acquire());
    }
    lods_range = lods_new;
  }

  light.tilemap_index = tilemap_pool.tilemaps_data.size();
  light.clip_near = int(0xFF7FFFFFu ^ 0x7FFFFFFFu); /* floatBitsToOrderedInt(-FLT_MAX) */
  light.clip_far = 0x7F7FFFFF;                      /* floatBitsToOrderedInt(FLT_MAX) */

  for (int lod : IndexRange(lods_range.size())) {
    ShadowTileMap *tilemap = tilemaps_[lod];

    int level = lods_range.first() + lod;
    /* Compute full offset from world origin to the smallest clipmap tile centered around the
     * camera position. The offset is computed in smallest tile unit. */
    float tile_size = ShadowTileMap::clipmap_tile_size_get(level);
    int2 level_offset = int2(
        roundf(math::dot(float3(object_mat_.values[0]), camera.position()) / tile_size),
        roundf(math::dot(float3(object_mat_.values[1]), camera.position()) / tile_size));

    tilemap->sync_clipmap(object_mat_, level_offset, level);

    /* Add shadow tile-maps grouped by lights to the GPU buffer. */
    tilemap_pool.tilemaps_data.append(*tilemap);
    tilemap->set_updated();
  }

  int2 pos_offset = int2(0);
  int2 neg_offset = int2(0);
  for (int lod : IndexRange(lods_range.size() - 1)) {
    /* Since offset can only differ by one tile from the higher level, we can compress that as a
     * single integer where one bit contains offset between 2 levels. Then a single bit shift in
     * the shader gives the number of tile to offset in the given tilemap space. However we need
     * also the sign of the offset for each level offset. To this end, we split the negative
     * offsets to a separate int.
     * Recovering the offset with: (pos_offset >> lod) - (neg_offset >> lod). */
    int2 lvl_offset_next = tilemaps_[lod + 1]->grid_offset;
    int2 lvl_offset = tilemaps_[lod]->grid_offset;
    int2 lvl_delta = lvl_offset - (lvl_offset_next << 1);
    BLI_assert(math::abs(lvl_delta.x) <= 1 && math::abs(lvl_delta.y) <= 1);
    pos_offset |= math::max(lvl_delta, int2(0)) << lod;
    neg_offset |= math::max(-lvl_delta, int2(0)) << lod;
  }

  /* Compressing to a single value to save up storage in light data. Number of levels is limited to
   * 16 by `clipmap_level_range()` for this reason. */
  light.clipmap_base_offset = pos_offset | (neg_offset << 16);

  // float tile_size_min = ShadowTileMap::clipmap_tile_size_get(lods_range.first());
  float tile_size_max = ShadowTileMap::clipmap_tile_size_get(lods_range.last());
  // int2 level_offset_min = tilemaps_[0]->grid_offset;
  int2 level_offset_max = tilemaps_[lods_range.size() - 1]->grid_offset;

  /* Used for selecting the clipmap level. */
  float camera_x = math::dot(float3(object_mat_.values[0]), camera.position());
  float camera_y = math::dot(float3(object_mat_.values[1]), camera.position());
  float camera_z = math::dot(float3(object_mat_.values[2]), camera.position());
  /* TODO(fclem): Cleanup this once matrix API rolls out. */
  *reinterpret_cast<float3 *>(light._position) = float3(camera_x, camera_y, camera_z);
  /* Used as origin for the clipmap_base_offset trick. */
  light._clipmap_origin_x = level_offset_max.x * tile_size_max;
  light._clipmap_origin_y = level_offset_max.y * tile_size_max;

  light.clipmap_lod_min = lods_range.first();
  light.clipmap_lod_max = lods_range.last();

  /* Half size of the min level. */
  float half_size = ShadowTileMap::clipmap_tile_size_get(lods_range.first()) / 2.0f;
  light.normal_mat_packed.x = half_size;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Module
 *
 * \{ */

void ShadowModule::init()
{
  /* TODO(@fclem): Make atlas size dependent on the final resolution. This would give roughly same
   * memory usage for multiple viewport versus one big viewport. */

  int2 atlas_extent = int2(shadow_page_size_ * SHADOW_PAGE_PER_ROW,
                           shadow_page_size_ * (shadow_page_len_ / SHADOW_PAGE_PER_ROW));

  eGPUTextureUsage tex_usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  if (atlas_tx_.ensure_2d(atlas_type, atlas_extent, tex_usage)) {
    /* Global update. */
    do_full_update = true;
  }

  /* Make allocation safe. Avoids crash later on. */
  if (!atlas_tx_.is_valid()) {
    atlas_tx_.ensure_2d(atlas_type, int2(1));
    inst_.info = "Error: Could not allocate shadow atlas. Most likely out of GPU memory.";
  }

  atlas_tx_.filter_mode(false);

  render_map_tx_.ensure_mip_views();
}

void ShadowModule::begin_sync()
{
  past_casters_updated_.clear();
  curr_casters_updated_.clear();
  curr_casters_.clear();

  {
    Manager &manager = *inst_.manager;
    RenderBuffers &render_buffers = inst_.render_buffers;

    PassMain &pass = tilemap_usage_ps_;
    pass.init();

    {
      /** Use depth buffer to tag needed shadow pages for opaque geometry. */
      PassMain::Sub &sub = pass.sub("Opaque");
      sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_USAGE_OPAQUE));
      sub.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
      sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
      sub.bind_texture("depth_tx", &render_buffers.depth_tx);
      sub.push_constant("tilemap_projection_ratio", &tilemap_projection_ratio_);
      inst_.lights.bind_resources(&sub);
      sub.dispatch(&dispatch_depth_scan_size_);
    }
    {
      /** Use bounding boxes for transparent geometry. */
      PassMain::Sub &sub = pass.sub("Transparent");
      /* WORKAROUND: The DRW_STATE_WRITE_STENCIL is here only to avoid enabling the rasterizer
       * discard inside draw manager. */
      sub.state_set(DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_WRITE_STENCIL);
      sub.state_stencil(0, 0, 0);
      sub.framebuffer_set(&usage_tag_fb);
      sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_USAGE_TRANSPARENT));
      sub.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
      sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
      sub.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
      sub.push_constant("tilemap_projection_ratio", &tilemap_projection_ratio_);
      inst_.lights.bind_resources(&sub);

      box_batch_ = DRW_cache_cube_get();
      tilemap_usage_transparent_ps_ = &sub;
    }
  }
}

void ShadowModule::sync_object(const ObjectHandle &handle,
                               const ResourceHandle &resource_handle,
                               bool is_shadow_caster,
                               bool is_alpha_blend)
{
  if (!is_shadow_caster && !is_alpha_blend) {
    return;
  }

  ShadowObject &shadow_ob = objects_.lookup_or_add_default(handle.object_key);
  shadow_ob.used = true;
  const bool is_initialized = shadow_ob.resource_handle.raw != 0;
  if ((handle.recalc != 0 || !is_initialized) && is_shadow_caster) {
    if (shadow_ob.resource_handle.raw != 0) {
      past_casters_updated_.append(shadow_ob.resource_handle.raw);
    }
    curr_casters_updated_.append(resource_handle.raw);
  }
  shadow_ob.resource_handle = resource_handle;

  if (is_shadow_caster) {
    curr_casters_.append(resource_handle.raw);
  }

  if (is_alpha_blend) {
    tilemap_usage_transparent_ps_->draw(box_batch_, resource_handle);
  }
}

void ShadowModule::end_sync()
{
  /* Delete unused shadows first to release tilemaps that could be reused for new lights. */
  for (Light &light : inst_.lights.light_map_.values()) {
    if (!light.used) {
      light.shadow_discard_safe(*this);
    }
    else if (light.directional != nullptr) {
      light.directional->release_excess_tilemaps(inst_.camera);
    }
    else if (light.punctual != nullptr) {
      light.punctual->release_excess_tilemaps();
    }
  }

  /* Allocate new tilemaps and fill shadow data of the lights. */
  tilemap_pool.tilemaps_data.clear();
  for (Light &light : inst_.lights.light_map_.values()) {
    if (light.directional != nullptr) {
      light.directional->end_sync(light, inst_.camera);
    }
    else if (light.punctual != nullptr) {
      light.punctual->end_sync(light);
    }
    else {
      light.tilemap_index = LIGHT_NO_SHADOW;
    }
  }
  tilemap_pool.end_sync(*this);

  /* Search for deleted or updated shadow casters */
  auto it_end = objects_.items().end();
  for (auto it = objects_.items().begin(); it != it_end; ++it) {
    ShadowObject &shadow_ob = (*it).value;
    if (!shadow_ob.used) {
      /* May not be a caster, but it does not matter, be conservative. */
      past_casters_updated_.append(shadow_ob.resource_handle.raw);
      objects_.remove(it);
    }
    else {
      /* Clear for next sync. */
      shadow_ob.used = false;
    }
  }
  if (!past_casters_updated_.is_empty() || !curr_casters_updated_.is_empty()) {
    inst_.sampling.reset();
  }
  past_casters_updated_.push_update();
  curr_casters_updated_.push_update();

  curr_casters_.push_update();

  if (do_full_update) {
    do_full_update = false;
    /* Put all pages in the free heap. */
    for (uint i : IndexRange(SHADOW_MAX_PAGE)) {
      uint2 page = {i % SHADOW_PAGE_PER_ROW, i / SHADOW_PAGE_PER_ROW};
      pages_free_data_[i] = page.x | (page.y << 16u);
    }
    pages_free_data_.push_update();

    /* Clear tiles to not reference any page. */
    tilemap_pool.tiles_data.clear_to_zero();

    /* Clear tilemap clip buffer. */
    union {
      ShadowTileMapClip clip;
      int4 i;
    } u;
    u.clip.clip_near_stored = 0.0f;
    u.clip.clip_far_stored = 0.0f;
    u.clip.clip_near = int(0xFF7FFFFFu ^ 0x7FFFFFFFu); /* floatBitsToOrderedInt(-FLT_MAX) */
    u.clip.clip_far = 0x7F7FFFFF;                      /* floatBitsToOrderedInt(FLT_MAX) */
    GPU_storagebuf_clear(tilemap_pool.tilemaps_clip, GPU_RGBA32I, GPU_DATA_INT, &u.i);

    /* Clear cached page buffer. */
    int2 data = {-1, -1};
    GPU_storagebuf_clear(pages_cached_data_, GPU_RG32I, GPU_DATA_INT, &data);

    /* Reset info to match new state. */
    pages_infos_data_.page_free_count = SHADOW_MAX_PAGE;
    pages_infos_data_.page_alloc_count = 0;
    pages_infos_data_.page_cached_next = 0u;
    pages_infos_data_.page_cached_start = 0u;
    pages_infos_data_.page_cached_end = 0u;
    pages_infos_data_.page_size = shadow_page_size_;
    pages_infos_data_.push_update();
  }

  {
    Manager &manager = *inst_.manager;

    {
      PassSimple &pass = tilemap_setup_ps_;
      pass.init();

      {
        /** Compute near/far clip distances for directional shadows based on casters bounds. */
        PassSimple::Sub &sub = pass.sub("DirectionalBounds");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_BOUNDS));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tilemaps_clip_buf", tilemap_pool.tilemaps_clip);
        sub.bind_ssbo("casters_id_buf", curr_casters_);
        sub.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
        sub.push_constant("resource_len", int(curr_casters_.size()));
        inst_.lights.bind_resources(&sub);
        sub.dispatch(int3(divide_ceil_u(curr_casters_.size(), SHADOW_BOUNDS_GROUP_SIZE), 1, 1));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /** Clear usage bits. Tag update from the tilemap for sun shadow clip-maps shifting. */
        PassSimple::Sub &sub = pass.sub("Init");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_INIT));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tilemaps_clip_buf", tilemap_pool.tilemaps_clip);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        /** Free unused tiles from tile-maps not used by any shadow. */
        if (tilemap_pool.tilemaps_unused.size() > 0) {
          sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_unused);
          sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_unused.size()));
        }
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /** Mark for update all shadow pages touching an updated shadow caster. */
        PassSimple::Sub &sub = pass.sub("CasterUpdate");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_UPDATE));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        /* Past caster transforms. */
        if (past_casters_updated_.size() > 0) {
          sub.bind_ssbo("bounds_buf", &manager.bounds_buf.previous());
          sub.bind_ssbo("resource_ids_buf", past_casters_updated_);
          sub.dispatch(int3(past_casters_updated_.size(), 1, tilemap_pool.tilemaps_data.size()));
        }
        /* Current caster transforms. */
        if (curr_casters_updated_.size() > 0) {
          sub.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
          sub.bind_ssbo("resource_ids_buf", curr_casters_updated_);
          sub.dispatch(int3(curr_casters_updated_.size(), 1, tilemap_pool.tilemaps_data.size()));
        }
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
    }

    /* Usage tagging happens between these two steps. */

    {
      PassSimple &pass = tilemap_update_ps_;
      pass.init();
      {
        /** Mark tiles that are redundant in the mipmap chain as unused. */
        PassSimple::Sub &sub = pass.sub("MaskLod");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_MASK));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /** Free unused pages & Reclaim cached pages. */
        PassSimple::Sub &sub = pass.sub("Free");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_FREE));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("pages_free_buf", pages_free_data_);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        /** Free unused tiles from tile-maps not used by any shadow. */
        if (tilemap_pool.tilemaps_unused.size() > 0) {
          sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_unused);
          sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_unused.size()));
        }
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /** De-fragment the free page heap after cache reuse phase which can leave hole. */
        PassSimple::Sub &sub = pass.sub("Defrag");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_DEFRAG));
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("pages_free_buf", pages_free_data_);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.bind_ssbo("clear_dispatch_buf", clear_dispatch_buf_);
        sub.dispatch(int3(1, 1, 1));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /** Assign pages to tiles that have been marked as used but possess no page. */
        PassSimple::Sub &sub = pass.sub("AllocatePages");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_ALLOCATE));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("pages_free_buf", pages_free_data_);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /** Convert the unordered tiles into a texture used during shading. Creates views. */
        PassSimple::Sub &sub = pass.sub("Finalize");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_FINALIZE));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tilemaps_clip_buf", tilemap_pool.tilemaps_clip);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.bind_ssbo("view_infos_buf", &shadow_multi_view_.matrices_ubo_get());
        sub.bind_ssbo("clear_dispatch_buf", clear_dispatch_buf_);
        sub.bind_ssbo("clear_page_buf", clear_page_buf_);
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_image("tilemaps_img", tilemap_pool.tilemap_tx);
        sub.bind_image("render_map_lod0_img", render_map_tx_.mip_view(0));
        sub.bind_image("render_map_lod1_img", render_map_tx_.mip_view(1));
        sub.bind_image("render_map_lod2_img", render_map_tx_.mip_view(2));
        sub.bind_image("render_map_lod3_img", render_map_tx_.mip_view(3));
        sub.bind_image("render_map_lod4_img", render_map_tx_.mip_view(4));
        sub.bind_image("render_map_lod5_img", render_map_tx_.mip_view(5));
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_UNIFORM | GPU_BARRIER_TEXTURE_FETCH |
                    GPU_BARRIER_SHADER_IMAGE_ACCESS);
      }
      {
        /** Clear pages that need to be rendered. */
        PassSimple::Sub &sub = pass.sub("RenderClear");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_CLEAR));
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("clear_dispatch_buf", clear_dispatch_buf_);
        sub.bind_image("atlas_img", atlas_tx_);
        sub.dispatch(clear_dispatch_buf_);
        sub.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
      }
    }
  }

  debug_end_sync();
}

void ShadowModule::debug_end_sync()
{
  if (!ELEM(inst_.debug_mode,
            eDebugMode::DEBUG_SHADOW_TILEMAPS,
            eDebugMode::DEBUG_SHADOW_VALUES,
            eDebugMode::DEBUG_SHADOW_TILE_RANDOM_COLOR)) {
    return;
  }

  /* Init but not filled if no active object. */
  debug_draw_ps_.init();

  Object *object_active = DRW_context_state_get()->obact;
  if (object_active == nullptr) {
    return;
  }

  ObjectKey object_key(DEG_get_original_object(object_active));

  if (inst_.lights.light_map_.contains(object_key) == false) {
    return;
  }

  Light &light = inst_.lights.light_map_.lookup(object_key);

  if (light.tilemap_index >= SHADOW_MAX_TILEMAP) {
    return;
  }

  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                   DRW_STATE_BLEND_CUSTOM;

  debug_draw_ps_.state_set(state);
  debug_draw_ps_.shader_set(inst_.shaders.static_shader_get(SHADOW_DEBUG));
  debug_draw_ps_.push_constant("debug_mode", (int)inst_.debug_mode);
  debug_draw_ps_.push_constant("debug_tilemap_index", light.tilemap_index);
  debug_draw_ps_.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
  debug_draw_ps_.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
  inst_.hiz_buffer.bind_resources(&debug_draw_ps_);
  inst_.lights.bind_resources(&debug_draw_ps_);
  inst_.shadows.bind_resources(&debug_draw_ps_);
  debug_draw_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

/* Compute approximate screen pixel density (as world space radius). */
float ShadowModule::screen_pixel_radius(const View &view, const int2 &extent)
{
  float min_dim = float(min_ii(extent.x, extent.y));
  float3 p0 = float3(-1.0f, -1.0f, 0.0f);
  float3 p1 = float3(float2(min_dim / extent) * 2.0f - 1.0f, 0.0f);
  mul_project_m4_v3(view.wininv().ptr(), p0);
  mul_project_m4_v3(view.wininv().ptr(), p1);
  /* Compute radius at unit plane from the camera. This is NOT the perspective division. */
  if (view.is_persp()) {
    p0 = p0 / p0.z;
    p1 = p1 / p1.z;
  }
  return math::distance(p0, p1) / min_dim;
}

/* Compute approximate screen pixel world space radius at 1 unit away of the light. */
float ShadowModule::tilemap_pixel_radius()
{
  /* This is a really rough approximation. Ideally, the cube-map distortion should be taken into
   * account per pixel. But this would make this pre-computation impossible.
   * So for now compute for the center of the cube-map. */
  const float cubeface_diagonal = M_SQRT2 * 2.0f;
  const float pixel_count = SHADOW_TILEMAP_RES * shadow_page_size_;
  return cubeface_diagonal / pixel_count;
}

/* Update all shadow regions visible inside the view.
 * If called multiple time for the same view, it will only do the depth buffer scanning
 * to check any new opaque surfaces.
 * Needs to be called after LightModule::set_view(); */
void ShadowModule::set_view(View &view)
{
  GPUFrameBuffer *prev_fb = GPU_framebuffer_active_get();

  int3 target_size = inst_.render_buffers.depth_tx.size();
  dispatch_depth_scan_size_ = math::divide_ceil(target_size, int3(SHADOW_DEPTH_SCAN_GROUP_SIZE));

  tilemap_projection_ratio_ = tilemap_pixel_radius() /
                              screen_pixel_radius(view, int2(target_size));

  usage_tag_fb.ensure(int2(target_size));
  render_fb_.ensure(int2(SHADOW_TILEMAP_RES * shadow_page_size_));

  GPU_uniformbuf_clear_to_zero(shadow_multi_view_.matrices_ubo_get());

  DRW_stats_group_start("Shadow");
  {
    inst_.manager->submit(tilemap_setup_ps_, view);

    inst_.manager->submit(tilemap_usage_ps_, view);

    inst_.manager->submit(tilemap_update_ps_, view);

    shadow_multi_view_.compute_procedural_bounds();

    inst_.pipelines.shadow.render(shadow_multi_view_);
  }
  DRW_stats_group_end();

  if (prev_fb) {
    GPU_framebuffer_bind(prev_fb);
  }
}

void ShadowModule::debug_draw(View &view, GPUFrameBuffer *view_fb)
{
  if (!ELEM(inst_.debug_mode,
            eDebugMode::DEBUG_SHADOW_TILEMAPS,
            eDebugMode::DEBUG_SHADOW_VALUES,
            eDebugMode::DEBUG_SHADOW_TILE_RANDOM_COLOR)) {
    return;
  }

  switch (inst_.debug_mode) {
    case DEBUG_SHADOW_TILEMAPS:
      inst_.info = "Debug Mode: Shadow Tilemap\n";
      break;
    case DEBUG_SHADOW_VALUES:
      inst_.info = "Debug Mode: Shadow Values\n";
      break;
    case DEBUG_SHADOW_TILE_RANDOM_COLOR:
      inst_.info = "Debug Mode: Shadow Tile Random Color\n";
      break;
    default:
      break;
  }

  inst_.hiz_buffer.update();

  GPU_framebuffer_bind(view_fb);
  inst_.manager->submit(debug_draw_ps_, view);
}

/** \} */

}  // namespace blender::eevee