/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */
#include "BKE_curves.hh"
#include "BLI_math_vec_types.hh"

#include "gpencil_new_proposal.hh"
#include "testing/testing.h"

namespace blender::bke {

class GPencilFrame : public CurvesGeometry {
 public:
  GPencilFrame(){};
  ~GPencilFrame() = default;

  CurvesGeometry &as_curves_geometry()
  {
    CurvesGeometry *geometry = reinterpret_cast<CurvesGeometry *>(this);
    return *geometry;
  }

  bool bounds_min_max(float3 &min, float3 &max)
  {
    return as_curves_geometry().bounds_min_max(min, max);
  }
};

class GPData : ::GPData {
 public:
  GPData();
  ~GPData();
};

}  // namespace blender::bke

namespace blender::bke::gpencil::tests {

TEST(gpencil_proposal, Foo)
{
  GPencilFrame my_frame;
  float3 min, max;
  EXPECT_FALSE(my_frame.bounds_min_max(min, max));
}

}  // namespace blender::bke::gpencil::tests