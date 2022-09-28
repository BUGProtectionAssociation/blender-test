/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(sculpt_paint_image_compute)
    .local_group_size(1, 1, 1)
    .image(0, GPU_RGBA32F, Qualifier::READ_WRITE, ImageType::FLOAT_2D, "out_img")
    .storage_buf(0, Qualifier::READ, "PackedPixelRow", "pixel_row_buf[]")
    .storage_buf(1, Qualifier::READ, "TrianglePaintInput", "paint_input[]")
    .push_constant(Type::INT, "pixel_row_offset")
    .compute_source("sculpt_paint_image_comp.glsl")
    .typedef_source("GPU_sculpt_shader_shared.h")
    .do_static_compilation(true);
