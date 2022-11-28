/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "BKE_node_runtime.hh"

namespace blender::nodes::node_shader_bsdf_principled_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  auto &subsurface = b.add_section(N_("Subsurface"));
  auto &specular = b.add_section(N_("Specular"));
  auto &sheen = b.add_section(N_("Sheen"));
  auto &clearcoat = b.add_section(N_("Clearcoat"));
  auto &emission = b.add_section(N_("Emission"));
  auto &thin_film = b.add_section(N_("Thin Film"));

  /* TODO: Tooltips depending on old/new model. */
  b.add_input<decl::Color>(N_("Base Color")).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Float>(N_("Subsurface"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(subsurface);
  /* TODO: Somehow merge with "Subsurface". Needs different subtype though... */
  b.add_input<decl::Float>(N_("Subsurface Scale"))
      .default_value(0.0f)
      .min(0.0f)
      .max(100.0f)
      .section(subsurface);
  b.add_input<decl::Vector>(N_("Subsurface Radius"))
      .default_value({1.0f, 0.2f, 0.1f})
      .min(0.0f)
      .max(100.0f)
      .compact()
      .section(subsurface);
  b.add_input<decl::Color>(N_("Subsurface Color"))
      .default_value({0.8f, 0.8f, 0.8f, 1.0f})
      .section(subsurface);
  b.add_input<decl::Float>(N_("Subsurface IOR"))
      .default_value(1.4f)
      .min(1.01f)
      .max(3.8f)
      .subtype(PROP_FACTOR)
      .section(subsurface);
  b.add_input<decl::Float>(N_("Subsurface Anisotropy"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(subsurface);
  b.add_input<decl::Float>(N_("Metallic"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  /* TODO: Also add support to Principled v1? Would be compatible at defaults afaics. */
  b.add_input<decl::Color>(N_("Metallic Edge"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .section(specular);
  b.add_input<decl::Float>(N_("Specular"))
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  /* TODO: Should be a color input in v2. Any way to keep compatibility?
   * Maybe change to color everywhere and detect special case when float is connected? */
  b.add_input<decl::Float>(N_("Specular Tint"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(subsurface);
  b.add_input<decl::Float>(N_("Roughness"))
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>(N_("Anisotropic"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(specular);
  b.add_input<decl::Float>(N_("Anisotropic Rotation"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(specular);
  b.add_input<decl::Float>(N_("Sheen"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(sheen);
  /* TODO: Should be a color input in v2. Any way to keep compatibility?
   * Maybe change to color everywhere and detect special case when float is connected? */
  b.add_input<decl::Float>(N_("Sheen Tint"))
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(sheen);
  b.add_input<decl::Float>(N_("Sheen Roughness"))
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(sheen);
  b.add_input<decl::Float>(N_("Clearcoat"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(clearcoat);
  b.add_input<decl::Float>(N_("Clearcoat Roughness"))
      .default_value(0.03f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .section(clearcoat);
  /* TODO: Also add support to Principled v1? Would remain compatible and reduce differences. */
  b.add_input<decl::Color>(N_("Clearcoat Tint"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .section(clearcoat);
  b.add_input<decl::Float>(N_("Thin Film Thickness"))
      .default_value(0.0f)
      .min(0.0f)
      .max(10000.0f)
      .subtype(PROP_WAVELENGTH)
      .section(thin_film);
  b.add_input<decl::Float>(N_("Thin Film IOR"))
      .default_value(1.5f)
      .min(1.0f)
      .max(10.0f)
      .section(thin_film);
  /* TODO: Restrict min/max (e.g. 0.1 to 10) */
  b.add_input<decl::Float>(N_("IOR")).default_value(1.45f).min(0.0f).max(1000.0f);
  b.add_input<decl::Float>(N_("Transmission"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>(N_("Transmission Roughness"))
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  /* TODO: Swap defaults (white, strength 0)? */
  b.add_input<decl::Color>(N_("Emission"))
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .section(emission);
  b.add_input<decl::Float>(N_("Emission Strength"))
      .default_value(1.0)
      .min(0.0f)
      .max(1000000.0f)
      .section(emission);
  b.add_input<decl::Float>(N_("Alpha"))
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Vector>(N_("Normal")).hide_value();
  b.add_input<decl::Vector>(N_("Clearcoat Normal")).hide_value().section(clearcoat);
  b.add_input<decl::Vector>(N_("Tangent")).hide_value().section(specular);
  b.add_input<decl::Float>(N_("Weight")).unavailable();
  b.add_output<decl::Shader>(N_("BSDF"));
}

static void node_shader_buts_principled(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "distribution", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
  uiItemR(layout, ptr, "subsurface_method", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static void node_shader_init_principled(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_PRINCIPLED_V2;
  node->custom2 = SHD_SUBSURFACE_RANDOM_WALK;
}

#define socket_not_zero(sock) (in[sock].link || (clamp_f(in[sock].vec[0], 0.0f, 1.0f) > 1e-5f))
#define socket_not_one(sock) \
  (in[sock].link || (clamp_f(in[sock].vec[0], 0.0f, 1.0f) < 1.0f - 1e-5f))

static int node_shader_gpu_bsdf_principled(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData * /*execdata*/,
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  in[2].type = GPU_NONE;
  in[8].type = GPU_NONE;
  in[16].type = GPU_NONE;
  in[19].type = GPU_NONE;
  in[20].type = GPU_NONE;
  in[21].type = GPU_NONE;

  /* Normals */
  if (!in[28].link) {
    GPU_link(mat, "world_normals_get", &in[28].link);
  }

  /* Clearcoat Normals */
  if (!in[29].link) {
    GPU_link(mat, "world_normals_get", &in[29].link);
  }

#if 0 /* Not used at the moment. */
  /* Tangents */
  if (!in[30].link) {
    GPUNodeLink *orco = GPU_attribute(CD_ORCO, "");
    GPU_link(mat, "tangent_orco_z", orco, &in[30].link);
    GPU_link(mat, "node_tangent", in[30].link, &in[30].link);
  }
#endif

  bool use_diffuse = socket_not_one(7) && socket_not_one(23);
  bool use_subsurf = socket_not_zero(1) && use_diffuse;
  bool use_refract = socket_not_one(7) && socket_not_zero(23);
  bool use_transparency = socket_not_one(27);
  bool use_clear = socket_not_zero(17);

  eGPUMaterialFlag flag = GPU_MATFLAG_GLOSSY;
  if (use_diffuse) {
    flag |= GPU_MATFLAG_DIFFUSE;
  }
  if (use_refract) {
    flag |= GPU_MATFLAG_REFRACT;
  }
  if (use_subsurf) {
    flag |= GPU_MATFLAG_SUBSURFACE;
  }
  if (use_transparency) {
    flag |= GPU_MATFLAG_TRANSPARENT;
  }
  if (use_clear) {
    flag |= GPU_MATFLAG_CLEARCOAT;
  }

  /* Ref. T98190: Defines are optimizations for old compilers.
   * Might become unnecessary with EEVEE-Next. */
  if (use_diffuse == false && use_refract == false && use_clear == true) {
    flag |= GPU_MATFLAG_PRINCIPLED_CLEARCOAT;
  }
  else if (use_diffuse == false && use_refract == false && use_clear == false) {
    flag |= GPU_MATFLAG_PRINCIPLED_METALLIC;
  }
  else if (use_diffuse == true && use_refract == false && use_clear == false) {
    flag |= GPU_MATFLAG_PRINCIPLED_DIELECTRIC;
  }
  else if (use_diffuse == false && use_refract == true && use_clear == false) {
    flag |= GPU_MATFLAG_PRINCIPLED_GLASS;
  }
  else {
    flag |= GPU_MATFLAG_PRINCIPLED_ANY;
  }

  if (use_subsurf) {
    bNodeSocket *socket = (bNodeSocket *)BLI_findlink(&node->runtime->original->inputs, 2);
    bNodeSocketValueRGBA *socket_data = (bNodeSocketValueRGBA *)socket->default_value;
    /* For some reason it seems that the socket value is in ARGB format. */
    use_subsurf = GPU_material_sss_profile_create(mat, &socket_data->value[1]);
  }

  float use_multi_scatter = (node->custom1 != SHD_PRINCIPLED_GGX) ? 1.0f : 0.0f;
  float use_sss = (use_subsurf) ? 1.0f : 0.0f;
  float use_diffuse_f = (use_diffuse) ? 1.0f : 0.0f;
  float use_clear_f = (use_clear) ? 1.0f : 0.0f;
  float use_refract_f = (use_refract) ? 1.0f : 0.0f;

  GPU_material_flag_set(mat, flag);

  return GPU_stack_link(mat,
                        node,
                        "node_bsdf_principled",
                        in,
                        out,
                        GPU_constant(&use_diffuse_f),
                        GPU_constant(&use_clear_f),
                        GPU_constant(&use_refract_f),
                        GPU_constant(&use_multi_scatter),
                        GPU_uniform(&use_sss));
}

static void node_shader_update_principled(bNodeTree *ntree, bNode *node)
{
  const int distribution = node->custom1;
  const int sss_method = node->custom2;
  const bool is_v2 = (distribution == SHD_PRINCIPLED_V2);

  LISTBASE_FOREACH (bNodeSocket *, sock, &node->inputs) {
    if (STREQ(sock->name, "Transmission Roughness")) {
      /* Only supported by the old separable glass model. */
      nodeSetSocketAvailability(ntree, sock, distribution == SHD_PRINCIPLED_GGX);
    }

    if (STR_ELEM(sock->name, "Subsurface Anisotropy")) {
      /* Only available with random-walk SSS.
       * Principled v2 always uses random-walk SSS, so enable regardless of sss_method there. */
      nodeSetSocketAvailability(ntree, sock, is_v2 || (sss_method != SHD_SUBSURFACE_BURLEY));
    }

    if (STR_ELEM(sock->name, "Subsurface IOR")) {
      /* Only available with random-walk SSS. Principled v2 uses the regular IOR input, however. */
      nodeSetSocketAvailability(ntree, sock, !is_v2 && (sss_method != SHD_SUBSURFACE_BURLEY));
    }

    if (STR_ELEM(sock->name,
                 "Subsurface",
                 "Subsurface Color",
                 "Specular",
                 "Specular Tint",
                 "Sheen Tint")) {
      /* Sockets exclusive to Principled v1. */
      nodeSetSocketAvailability(ntree, sock, !is_v2);
    }

    if (STR_ELEM(sock->name,
                 "Subsurface Scale",
                 "Clearcoat Tint",
                 "Sheen Roughness",
                 "Metallic Edge",
                 "Thin Film Thickness",
                 "Thin Film IOR")) {
      /* Sockets exclusive to Principled v2. */
      nodeSetSocketAvailability(ntree, sock, is_v2);
    }
  }

  LISTBASE_FOREACH (bNodeSection *, section, &node->sections) {
    if (STREQ(section->name, "Thin Film")) {
      /* Sections exclusive to Principled v2. */
      nodeSetSectionAvailability(section, is_v2);
    }
  }
}

}  // namespace blender::nodes::node_shader_bsdf_principled_cc

/* node type definition */
void register_node_type_sh_bsdf_principled()
{
  namespace file_ns = blender::nodes::node_shader_bsdf_principled_cc;

  static bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_BSDF_PRINCIPLED, "Principled BSDF", NODE_CLASS_SHADER);
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_principled;
  node_type_size_preset(&ntype, NODE_SIZE_LARGE);
  ntype.initfunc = file_ns::node_shader_init_principled;
  ntype.gpu_fn = file_ns::node_shader_gpu_bsdf_principled;
  ntype.updatefunc = file_ns::node_shader_update_principled;

  nodeRegisterType(&ntype);
}
