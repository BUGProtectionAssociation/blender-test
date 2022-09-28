/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "ED_paint.h"
#include "ED_uvedit.h"

#include "BLI_math.h"
#include "BLI_math_color_blend.h"
#include "BLI_task.h"

#include "GPU_capabilities.h"
#include "GPU_compute.h"
#include "GPU_debug.h"
#include "GPU_shader.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"

#include "BKE_brush.h"
#include "BKE_image_wrappers.hh"
#include "BKE_material.h"
#include "BKE_pbvh.h"
#include "BKE_pbvh_pixels.hh"

#include "bmesh.h"

#include "NOD_shader.h"

#include "sculpt_intern.h"

namespace blender::ed::sculpt_paint::paint::image {

using namespace blender::bke::pbvh::pixels;
using namespace blender::bke::image;

struct ImageData {
  Image *image = nullptr;
  ImageUser *image_user = nullptr;

  ~ImageData() = default;

  static bool init_active_image(Object *ob,
                                ImageData *r_image_data,
                                PaintModeSettings *paint_mode_settings)
  {
    return BKE_paint_canvas_image_get(
        paint_mode_settings, ob, &r_image_data->image, &r_image_data->image_user);
  }
};

/* -------------------------------------------------------------------- */
/** \name CPU
 * \{ */

struct TexturePaintingUserData {
  Object *ob;
  Brush *brush;
  PBVHNode **nodes;
  ImageData image_data;
  int32_t nodes_len;
};

/** Reading and writing to image buffer with 4 float channels. */
class ImageBufferFloat4 {
 private:
  int pixel_offset;

 public:
  void set_image_position(ImBuf *image_buffer, ushort2 image_pixel_position)
  {
    pixel_offset = int(image_pixel_position.y) * image_buffer->x + int(image_pixel_position.x);
  }

  void next_pixel()
  {
    pixel_offset += 1;
  }

  float4 read_pixel(ImBuf *image_buffer) const
  {
    return &image_buffer->rect_float[pixel_offset * 4];
  }

  void write_pixel(ImBuf *image_buffer, const float4 pixel_data) const
  {
    copy_v4_v4(&image_buffer->rect_float[pixel_offset * 4], pixel_data);
  }

  const char *get_colorspace_name(ImBuf *image_buffer)
  {
    return IMB_colormanagement_get_float_colorspace(image_buffer);
  }
};

/** Reading and writing to image buffer with 4 byte channels. */
class ImageBufferByte4 {
 private:
  int pixel_offset;

 public:
  void set_image_position(ImBuf *image_buffer, ushort2 image_pixel_position)
  {
    pixel_offset = int(image_pixel_position.y) * image_buffer->x + int(image_pixel_position.x);
  }

  void next_pixel()
  {
    pixel_offset += 1;
  }

  float4 read_pixel(ImBuf *image_buffer) const
  {
    float4 result;
    rgba_uchar_to_float(result,
                        static_cast<const uchar *>(
                            static_cast<const void *>(&(image_buffer->rect[pixel_offset]))));
    return result;
  }

  void write_pixel(ImBuf *image_buffer, const float4 pixel_data) const
  {
    rgba_float_to_uchar(
        static_cast<uchar *>(static_cast<void *>(&image_buffer->rect[pixel_offset])), pixel_data);
  }

  const char *get_colorspace_name(ImBuf *image_buffer)
  {
    return IMB_colormanagement_get_rect_colorspace(image_buffer);
  }
};

template<typename ImageBuffer> class PaintingKernel {
  ImageBuffer image_accessor;

  SculptSession *ss;
  const Brush *brush;
  const int thread_id;
  const MVert *mvert;

  float4 brush_color;
  float brush_strength;

  SculptBrushTestFn brush_test_fn;
  SculptBrushTest test;
  /* Pointer to the last used image buffer to detect when buffers are switched. */
  void *last_used_image_buffer_ptr = nullptr;
  const char *last_used_color_space = nullptr;

 public:
  explicit PaintingKernel(SculptSession *ss,
                          const Brush *brush,
                          const int thread_id,
                          const MVert *mvert)
      : ss(ss), brush(brush), thread_id(thread_id), mvert(mvert)
  {
    init_brush_strength();
    init_brush_test();
  }

  bool paint(const Triangles &triangles, const PackedPixelRow &pixel_row, ImBuf *image_buffer)
  {
    image_accessor.set_image_position(image_buffer, pixel_row.start_image_coordinate);
    const TrianglePaintInput triangle = triangles.get_paint_input(pixel_row.triangle_index);
    float3 pixel_pos = get_start_pixel_pos(triangle, pixel_row);
    const float3 delta_pixel_pos = get_delta_pixel_pos(triangle, pixel_row, pixel_pos);
    bool pixels_painted = false;
    for (int x = 0; x < pixel_row.num_pixels; x++) {
      if (!brush_test_fn(&test, pixel_pos)) {
        pixel_pos += delta_pixel_pos;
        image_accessor.next_pixel();
        continue;
      }

      float4 color = image_accessor.read_pixel(image_buffer);
      const float3 normal(0.0f, 0.0f, 0.0f);
      const float3 face_normal(0.0f, 0.0f, 0.0f);
      const float mask = 0.0f;
      const float falloff_strength = SCULPT_brush_strength_factor(
          ss,
          brush,
          pixel_pos,
          sqrtf(test.dist),
          normal,
          face_normal,
          mask,
          BKE_pbvh_make_vref(PBVH_REF_NONE),
          thread_id);
      float4 paint_color = brush_color * falloff_strength * brush_strength;
      float4 buffer_color;
      blend_color_mix_float(buffer_color, color, paint_color);
      buffer_color *= brush->alpha;
      IMB_blend_color_float(color, color, buffer_color, static_cast<IMB_BlendMode>(brush->blend));
      image_accessor.write_pixel(image_buffer, color);
      pixels_painted = true;

      image_accessor.next_pixel();
      pixel_pos += delta_pixel_pos;
    }
    return pixels_painted;
  }

  void init_brush_color(ImBuf *image_buffer)
  {
    const char *to_colorspace = image_accessor.get_colorspace_name(image_buffer);
    if (last_used_color_space == to_colorspace) {
      return;
    }
    copy_v3_v3(brush_color,
               ss->cache->invert ? BKE_brush_secondary_color_get(ss->scene, brush) :
                                   BKE_brush_color_get(ss->scene, brush));
    /* NOTE: Brush colors are stored in sRGB. We use math color to follow other areas that
     * use brush colors. From there on we use IMB_colormanagement to convert the brush color to the
     * colorspace of the texture. This isn't ideal, but would need more refactoring to make sure
     * that brush colors are stored in scene linear by default. */
    srgb_to_linearrgb_v3_v3(brush_color, brush_color);
    brush_color[3] = 1.0f;

    const char *from_colorspace = IMB_colormanagement_role_colorspace_name_get(
        COLOR_ROLE_SCENE_LINEAR);
    ColormanageProcessor *cm_processor = IMB_colormanagement_colorspace_processor_new(
        from_colorspace, to_colorspace);
    IMB_colormanagement_processor_apply_v4(cm_processor, brush_color);
    IMB_colormanagement_processor_free(cm_processor);
    last_used_color_space = to_colorspace;
  }

 private:
  void init_brush_strength()
  {
    brush_strength = ss->cache->bstrength;
  }
  void init_brush_test()
  {
    brush_test_fn = SCULPT_brush_test_init_with_falloff_shape(ss, &test, brush->falloff_shape);
  }

  /**
   * Extract the starting pixel position from the given encoded_pixels belonging to the triangle.
   */
  float3 get_start_pixel_pos(const TrianglePaintInput &triangle,
                             const PackedPixelRow &encoded_pixels) const
  {
    return init_pixel_pos(triangle, encoded_pixels.start_barycentric_coord);
  }

  /**
   * Extract the delta pixel position that will be used to advance a Pixel instance to the next
   * pixel.
   */
  float3 get_delta_pixel_pos(const TrianglePaintInput &triangle,
                             const PackedPixelRow &encoded_pixels,
                             const float3 &start_pixel) const
  {
    float3 result = init_pixel_pos(
        triangle, encoded_pixels.start_barycentric_coord + triangle.delta_barycentric_coord_u);
    return result - start_pixel;
  }

  float3 init_pixel_pos(const TrianglePaintInput &triangle,
                        const float2 &barycentric_weights) const
  {
    const int4 &vert_indices = triangle.vert_indices;
    float3 result;
    const float3 barycentric(barycentric_weights.x,
                             barycentric_weights.y,
                             1.0f - barycentric_weights.x - barycentric_weights.y);
    interp_v3_v3v3v3(result,
                     mvert[vert_indices[0]].co,
                     mvert[vert_indices[1]].co,
                     mvert[vert_indices[2]].co,
                     barycentric);
    return result;
  }
};

static std::vector<bool> init_triangle_brush_test(SculptSession *ss,
                                                  Triangles &triangles,
                                                  const MVert *mvert)
{
  std::vector<bool> brush_test(triangles.size());
  SculptBrushTest test;
  SCULPT_brush_test_init(ss, &test);
  float3 brush_min_bounds(test.location[0] - test.radius,
                          test.location[1] - test.radius,
                          test.location[2] - test.radius);
  float3 brush_max_bounds(test.location[0] + test.radius,
                          test.location[1] + test.radius,
                          test.location[2] + test.radius);
  for (int triangle_index = 0; triangle_index < triangles.size(); triangle_index++) {
    TrianglePaintInput &triangle = triangles.get_paint_input(triangle_index);

    float3 triangle_min_bounds(mvert[triangle.vert_indices[0]].co);
    float3 triangle_max_bounds(triangle_min_bounds);
    for (int i = 1; i < 3; i++) {
      const float3 &pos = mvert[triangle.vert_indices[i]].co;
      triangle_min_bounds.x = min_ff(triangle_min_bounds.x, pos.x);
      triangle_min_bounds.y = min_ff(triangle_min_bounds.y, pos.y);
      triangle_min_bounds.z = min_ff(triangle_min_bounds.z, pos.z);
      triangle_max_bounds.x = max_ff(triangle_max_bounds.x, pos.x);
      triangle_max_bounds.y = max_ff(triangle_max_bounds.y, pos.y);
      triangle_max_bounds.z = max_ff(triangle_max_bounds.z, pos.z);
    }
    brush_test[triangle_index] = isect_aabb_aabb_v3(
        brush_min_bounds, brush_max_bounds, triangle_min_bounds, triangle_max_bounds);
  }
  return brush_test;
}

static void do_paint_pixels(void *__restrict userdata,
                            const int n,
                            const TaskParallelTLS *__restrict tls)
{
  TexturePaintingUserData *data = static_cast<TexturePaintingUserData *>(userdata);
  Object *ob = data->ob;
  SculptSession *ss = ob->sculpt;
  const Brush *brush = data->brush;
  PBVHNode *node = data->nodes[n];

  NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
  const int thread_id = BLI_task_parallel_thread_id(tls);
  MVert *mvert = SCULPT_mesh_deformed_mverts_get(ss);

  std::vector<bool> brush_test = init_triangle_brush_test(ss, node_data.triangles, mvert);

  PaintingKernel<ImageBufferFloat4> kernel_float4(ss, brush, thread_id, mvert);
  PaintingKernel<ImageBufferByte4> kernel_byte4(ss, brush, thread_id, mvert);

  ImageUser image_user = *data->image_data.image_user;
  bool pixels_updated = false;
  for (UDIMTilePixels &tile_data : node_data.tiles) {
    LISTBASE_FOREACH (ImageTile *, tile, &data->image_data.image->tiles) {
      ImageTileWrapper image_tile(tile);
      if (image_tile.get_tile_number() == tile_data.tile_number) {
        image_user.tile = image_tile.get_tile_number();

        ImBuf *image_buffer = BKE_image_acquire_ibuf(data->image_data.image, &image_user, nullptr);
        if (image_buffer == nullptr) {
          continue;
        }

        if (image_buffer->rect_float != nullptr) {
          kernel_float4.init_brush_color(image_buffer);
        }
        else {
          kernel_byte4.init_brush_color(image_buffer);
        }

        for (const PackedPixelRow &pixel_row : tile_data.pixel_rows) {
          if (!brush_test[pixel_row.triangle_index]) {
            continue;
          }
          bool pixels_painted = false;
          if (image_buffer->rect_float != nullptr) {
            pixels_painted = kernel_float4.paint(node_data.triangles, pixel_row, image_buffer);
          }
          else {
            pixels_painted = kernel_byte4.paint(node_data.triangles, pixel_row, image_buffer);
          }

          if (pixels_painted) {
            tile_data.mark_dirty(pixel_row);
          }
        }

        BKE_image_release_ibuf(data->image_data.image, image_buffer, nullptr);
        pixels_updated |= tile_data.flags.dirty;
        break;
      }
    }
  }

  node_data.flags.dirty |= pixels_updated;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Undo
 * \{ */

static void undo_region_tiles(
    ImBuf *ibuf, int x, int y, int w, int h, int *tx, int *ty, int *tw, int *th)
{
  int srcx = 0, srcy = 0;
  IMB_rectclip(ibuf, nullptr, &x, &y, &srcx, &srcy, &w, &h);
  *tw = ((x + w - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *th = ((y + h - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *tx = (x >> ED_IMAGE_UNDO_TILE_BITS);
  *ty = (y >> ED_IMAGE_UNDO_TILE_BITS);
}

static void push_undo(const NodeData &node_data,
                      Image &image,
                      ImageUser &image_user,
                      const image::ImageTileWrapper &image_tile,
                      ImBuf &image_buffer,
                      ImBuf **tmpibuf)
{
  for (const UDIMTileUndo &tile_undo : node_data.undo_regions) {
    if (tile_undo.tile_number != image_tile.get_tile_number()) {
      continue;
    }
    int tilex, tiley, tilew, tileh;
    PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();
    undo_region_tiles(&image_buffer,
                      tile_undo.region.xmin,
                      tile_undo.region.ymin,
                      BLI_rcti_size_x(&tile_undo.region),
                      BLI_rcti_size_y(&tile_undo.region),
                      &tilex,
                      &tiley,
                      &tilew,
                      &tileh);
    for (int ty = tiley; ty <= tileh; ty++) {
      for (int tx = tilex; tx <= tilew; tx++) {
        ED_image_paint_tile_push(undo_tiles,
                                 &image,
                                 &image_buffer,
                                 tmpibuf,
                                 &image_user,
                                 tx,
                                 ty,
                                 nullptr,
                                 nullptr,
                                 true,
                                 true);
      }
    }
  }
}

static void do_push_undo_tile(void *__restrict userdata,
                              const int n,
                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  TexturePaintingUserData *data = static_cast<TexturePaintingUserData *>(userdata);
  PBVHNode *node = data->nodes[n];

  NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
  Image *image = data->image_data.image;
  ImageUser *image_user = data->image_data.image_user;

  ImBuf *tmpibuf = nullptr;
  ImageUser local_image_user = *image_user;
  LISTBASE_FOREACH (ImageTile *, tile, &image->tiles) {
    image::ImageTileWrapper image_tile(tile);
    local_image_user.tile = image_tile.get_tile_number();
    ImBuf *image_buffer = BKE_image_acquire_ibuf(image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    push_undo(node_data, *image, *image_user, image_tile, *image_buffer, &tmpibuf);
    BKE_image_release_ibuf(image, image_buffer, nullptr);
  }
  if (tmpibuf) {
    IMB_freeImBuf(tmpibuf);
  }
}

static void do_mark_dirty_regions(void *__restrict userdata,
                                  const int n,
                                  const TaskParallelTLS *__restrict UNUSED(tls))
{
  TexturePaintingUserData *data = static_cast<TexturePaintingUserData *>(userdata);
  PBVHNode *node = data->nodes[n];
  BKE_pbvh_pixels_mark_image_dirty(*node, *data->image_data.image, *data->image_data.image_user);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU
 * \{ */

static void ensure_gpu_buffers(TexturePaintingUserData &data)
{
  for (PBVHNode *node : MutableSpan<PBVHNode *>(data.nodes, data.nodes_len)) {
    NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
    node_data.ensure_gpu_buffers();
  }
}

static void dispatch_gpu_painting(TexturePaintingUserData &data)
{
  GPUShader *shader = SCULPT_shader_paint_image_get();
  GPU_shader_bind(shader);

  ImageUser local_image_user = *data.image_data.image_user;
  GPUTexture *tex = nullptr;

  LISTBASE_FOREACH (ImageTile *, tile, &data.image_data.image->tiles) {
    ImageTileWrapper image_tile(tile);
    local_image_user.tile = image_tile.get_tile_number();

    ImBuf *image_buffer = BKE_image_acquire_ibuf(
        data.image_data.image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    bool texture_needs_clearing = true;

    /* Ensure that texture size is same as tile size. */
    if (tex == nullptr || GPU_texture_width(tex) != image_buffer->x ||
        GPU_texture_height(tex) != image_buffer->y) {
      if (tex) {
        GPU_texture_free(tex);
        tex = nullptr;
      }
      tex = GPU_texture_create_2d(
          __func__, image_buffer->x, image_buffer->y, 1, GPU_RGBA32F, nullptr);
    }

    /* Dispatch all nodes that paint on the active tile. */
    for (PBVHNode *node : MutableSpan<PBVHNode *>(data.nodes, data.nodes_len)) {
      NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);

      for (UDIMTilePixels &tile_pixels : node_data.tiles) {
        if (tile_pixels.tile_number != image_tile.get_tile_number()) {
          continue;
        }

        /* Only clear the texture when it is used for the first time. */
        if (texture_needs_clearing) {
          GPU_texture_clear(tex, GPU_DATA_FLOAT, float4(0.0f, 0.0f, 0.0f, 0.0f));
          texture_needs_clearing = false;
        }

        GPU_shader_bind(shader);
        GPU_texture_image_bind(tex, GPU_shader_get_texture_binding(shader, "out_img"));
        GPU_storagebuf_bind(node_data.triangles.gpu_buffer,
                            GPU_shader_get_ssbo(shader, "paint_input"));
        GPU_storagebuf_bind(node_data.gpu_buffers.pixels,
                            GPU_shader_get_ssbo(shader, "pixel_row_buf"));
        GPU_shader_uniform_1i(shader, "pixel_row_offset", tile_pixels.gpu_buffer_offset);

        GPU_compute_dispatch(shader, tile_pixels.pixel_rows.size(), 1, 1);
      }
      node_data.ensure_gpu_buffers();
    }

#if 0
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    float *tex_data = static_cast<float *>(GPU_texture_read(tex, GPU_DATA_FLOAT, 0));
    for (int i = 0; i < 10; i++) {
      printf("%f,", tex_data[i]);
    }
    printf("\n");
    MEM_freeN(tex_data);
#endif

    /* Integrate active tile to draw engine texture. */

    BKE_image_release_ibuf(data.image_data.image, image_buffer, nullptr);
  }

  if (tex) {
    GPU_texture_free(tex);
    tex = nullptr;
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::paint::image

extern "C" {

using namespace blender::ed::sculpt_paint::paint::image;

bool SCULPT_paint_image_canvas_get(PaintModeSettings *paint_mode_settings,
                                   Object *ob,
                                   Image **r_image,
                                   ImageUser **r_image_user)
{
  *r_image = nullptr;
  *r_image_user = nullptr;

  ImageData image_data;
  if (!ImageData::init_active_image(ob, &image_data, paint_mode_settings)) {
    return false;
  }

  *r_image = image_data.image;
  *r_image_user = image_data.image_user;
  return true;
}

bool SCULPT_use_image_paint_brush(PaintModeSettings *settings, Object *ob)
{
  if (!U.experimental.use_sculpt_texture_paint) {
    return false;
  }
  if (ob->type != OB_MESH) {
    return false;
  }
  Image *image;
  ImageUser *image_user;
  return BKE_paint_canvas_image_get(settings, ob, &image, &image_user);
}

/** Can the sculpt paint be performed on the GPU? */
static bool SCULPT_use_image_paint_compute()
{
  return GPU_compute_shader_support() && GPU_shader_storage_buffer_objects_support() &&
         GPU_shader_image_load_store_support();
}

void SCULPT_do_paint_brush_image(
    PaintModeSettings *paint_mode_settings, Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode)
{
  Brush *brush = BKE_paint_brush(&sd->paint);

  TexturePaintingUserData data = {nullptr};
  data.ob = ob;
  data.brush = brush;
  data.nodes = nodes;
  data.nodes_len = totnode;

  if (!ImageData::init_active_image(ob, &data.image_data, paint_mode_settings)) {
    return;
  }

  if (SCULPT_use_image_paint_compute()) {
    GPU_debug_group_begin("SCULPT_paint_image");
    ensure_gpu_buffers(data);
    // update GPU buffers when they don't exist.
    // update Image when it doesn't exist.
    // go over given nodes and dispatch those nodes.
    dispatch_gpu_painting(data);
    // Copy result to the image gpu buffer
    // Copy back should happen at the end of the stroke, but for testing we could do it here and
    // move it.
    GPU_debug_group_end();
  }
  else {
    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, totnode);
    BLI_task_parallel_range(0, totnode, &data, do_push_undo_tile, &settings);
    BLI_task_parallel_range(0, totnode, &data, do_paint_pixels, &settings);

    TaskParallelSettings settings_flush;
    BKE_pbvh_parallel_range_settings(&settings_flush, false, totnode);
    BLI_task_parallel_range(0, totnode, &data, do_mark_dirty_regions, &settings_flush);
  }
}
}
