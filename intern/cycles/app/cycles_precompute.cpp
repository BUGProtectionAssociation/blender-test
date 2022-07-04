#include "util/math.h"
#include "util/string.h"
#include "util/system.h"

#include "util/hash.h"
#include "util/task.h"

#include "kernel/device/cpu/compat.h"
#include "kernel/device/cpu/globals.h"

#include "kernel/sample/lcg.h"
#include "kernel/sample/mapping.h"

#include "kernel/closure/bsdf_microfacet.h"
#include "kernel/closure/bsdf_microfacet_glass.h"

#include <iostream>

CCL_NAMESPACE_BEGIN

/* From PBRT: core/montecarlo.h */
inline float VanDerCorput(uint32_t n, uint32_t scramble)
{
  n = (n << 16) | (n >> 16);
  n = ((n & 0x00ff00ff) << 8) | ((n & 0xff00ff00) >> 8);
  n = ((n & 0x0f0f0f0f) << 4) | ((n & 0xf0f0f0f0) >> 4);
  n = ((n & 0x33333333) << 2) | ((n & 0xcccccccc) >> 2);
  n = ((n & 0x55555555) << 1) | ((n & 0xaaaaaaaa) >> 1);
  n ^= scramble;
  return ((n >> 8) & 0xffffff) / float(1 << 24);
}
inline float Sobol2(uint32_t n, uint32_t scramble)
{
  for (uint32_t v = 1 << 31; n != 0; n >>= 1, v ^= v >> 1)
    if (n & 0x1)
      scramble ^= v;
  return ((scramble >> 8) & 0xffffff) / float(1 << 24);
}

static float precompute_ggx_E(float rough, float mu, float u1, float u2)
{
  MicrofacetBsdf bsdf;
  bsdf.weight = one_float3();
  bsdf.type = CLOSURE_BSDF_MICROFACET_GGX_ID;
  bsdf.sample_weight = 1.0f;
  bsdf.N = make_float3(0.0f, 0.0f, 1.0f);
  bsdf.alpha_x = bsdf.alpha_y = sqr(rough);
  bsdf.ior = 1.0f;
  bsdf.extra = nullptr;
  bsdf.T = make_float3(1.0f, 0.0f, 0.0f);

  float3 eval, omega_in, domega_in_dx, domega_in_dy;
  float pdf = 0.0f;
  bsdf_microfacet_ggx_sample((ShaderClosure *)&bsdf,
                             make_float3(0.0f, 0.0f, 1.0f),
                             make_float3(sqrtf(1.0f - sqr(mu)), 0.0f, mu),
                             zero_float3(),
                             zero_float3(),
                             u1,
                             u2,
                             &eval,
                             &omega_in,
                             &domega_in_dx,
                             &domega_in_dy,
                             &pdf);
  if (pdf != 0.0f) {
    return average(eval) / pdf;
  }
  return 0.0f;
}

static float precompute_ggx_refract_E(float rough, float mu, float eta, float u1, float u2)
{
  MicrofacetBsdf bsdf;
  bsdf.weight = one_float3();
  bsdf.type = CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID;
  bsdf.sample_weight = 1.0f;
  bsdf.N = make_float3(0.0f, 0.0f, 1.0f);
  bsdf.alpha_x = bsdf.alpha_y = sqr(rough);
  bsdf.ior = eta;
  bsdf.extra = nullptr;
  bsdf.T = make_float3(1.0f, 0.0f, 0.0f);

  float3 eval, omega_in, domega_in_dx, domega_in_dy;
  float pdf = 0.0f;
  bsdf_microfacet_ggx_sample((ShaderClosure *)&bsdf,
                             make_float3(0.0f, 0.0f, 1.0f),
                             make_float3(sqrtf(1.0f - sqr(mu)), 0.0f, mu),
                             zero_float3(),
                             zero_float3(),
                             u1,
                             u2,
                             &eval,
                             &omega_in,
                             &domega_in_dx,
                             &domega_in_dy,
                             &pdf);
  if (pdf != 0.0f) {
    return average(eval) / pdf;
  }
  return 0.0f;
}

static float precompute_ggx_glass_E(
    float rough, float mu, float eta, float u1, float u2, uint *rng)
{
  MicrofacetBsdf bsdf;
  bsdf.weight = one_float3();
  bsdf.type = CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID;
  bsdf.sample_weight = 1.0f;
  bsdf.N = make_float3(0.0f, 0.0f, 1.0f);
  bsdf.alpha_x = bsdf.alpha_y = sqr(rough);
  bsdf.ior = eta;
  bsdf.extra = nullptr;
  bsdf.T = make_float3(1.0f, 0.0f, 0.0f);

  float3 eval, omega_in, domega_in_dx, domega_in_dy;
  float pdf = 0.0f;
  bsdf_microfacet_ggx_glass_sample((ShaderClosure *)&bsdf,
                                   make_float3(0.0f, 0.0f, 1.0f),
                                   make_float3(sqrtf(1.0f - sqr(mu)), 0.0f, mu),
                                   zero_float3(),
                                   zero_float3(),
                                   u1,
                                   u2,
                                   &eval,
                                   &omega_in,
                                   &domega_in_dx,
                                   &domega_in_dy,
                                   &pdf,
                                   rng);
  if (pdf != 0.0f) {
    return average(eval) / pdf;
  }
  return 0.0f;
}

struct PrecomputeTerm {
  int dim, samples, res;
  std::function<float(float, float, float, float, float, uint *)> evaluation;
};

bool cycles_precompute(std::string name);
bool cycles_precompute(std::string name)
{
  std::map<string, PrecomputeTerm> precompute_terms;
  precompute_terms["ggx_E"] = {
      2, 1 << 23, 32, [](float rough, float mu, float ior, float u1, float u2, uint *rng) {
        return precompute_ggx_E(rough, mu, u1, u2);
      }};
  precompute_terms["ggx_E_avg"] = {
      1, 1 << 23, 32, [](float rough, float mu, float ior, float u1, float u2, uint *rng) {
        return 2.0f * mu * precompute_ggx_E(rough, mu, u1, u2);
      }};
  precompute_terms["ggx_glass_E"] = {
      3, 1 << 20, 16, [](float rough, float mu, float ior, float u1, float u2, uint *rng) {
        return precompute_ggx_glass_E(rough, mu, ior, u1, u2, rng);
      }};
  precompute_terms["ggx_glass_inv_E"] = {
      3, 1 << 20, 16, [](float rough, float mu, float ior, float u1, float u2, uint *rng) {
        return precompute_ggx_glass_E(rough, mu, 1.0f / ior, u1, u2, rng);
      }};
  precompute_terms["ggx_refract_E"] = {
      3, 1 << 20, 16, [](float rough, float mu, float ior, float u1, float u2, uint *rng) {
        return precompute_ggx_refract_E(rough, mu, ior, u1, u2);
      }};
  precompute_terms["ggx_refract_inv_E"] = {
      3, 1 << 20, 16, [](float rough, float mu, float ior, float u1, float u2, uint *rng) {
        return precompute_ggx_refract_E(rough, mu, 1.0f / ior, u1, u2);
      }};

  if (precompute_terms.count(name) == 0) {
    return false;
  }

  const PrecomputeTerm &term = precompute_terms[name];

  const int samples = term.samples;
  const int res = term.res;
  const int nz = (term.dim > 2) ? res : 1, ny = res, nx = (term.dim > 1) ? res : 1;

  if (nz > 1) {
    std::cout << "static const float table_" << name << "[" << nz << "][" << ny << "][" << nx
              << "] = {" << std::endl;
  }
  for (int z = 0; z < nz; z++) {
    float *data = new float[nx * ny];
    parallel_for(0, ny, [&](int64_t y) {
      for (int x = 0; x < nx; x++) {
        uint rng = hash_uint2(x, y);
        uint scramble1 = lcg_step_uint(&rng), scramble2 = lcg_step_uint(&rng);
        double sum = 0.0;
        for (int i = 0; i < samples; i++) {
          float rough = 1.0f - (float(y) + lcg_step_float(&rng)) / float(ny);
          float mu = (float(x) + lcg_step_float(&rng)) / float(nx);
          float ior = (float(z) + lcg_step_float(&rng)) / float(nz);
          /* Encode IOR remapped as sqrt(0.5*(IOR-1)) for more resolution at the start, where most of the
           * changes happen (also places the most common range around 1.5 in the center) */
          ior = 1.0f + 2.0f * sqr(ior);
          float u1 = VanDerCorput(i, scramble1);
          float u2 = Sobol2(i, scramble2);

          float value = term.evaluation(rough, mu, ior, u1, u2, &rng);
          if (isnan(value)) {
            value = 0.0f;
          }
          sum += (double)value;
        }
        data[y * nx + x] = float(sum / double(samples));
      }
    });

    string filename = name;
    if (nz > 1) {
      filename += string_printf("_%02d", z);
      std::cout << "  {" << std::endl;
    }
    else {
      std::cout << "static const float table_" << name << "[" << ny << "][" << nx << "] = {"
                << std::endl;
    }

    for (int y = 0; y < ny; y++) {
      std::cout << "    {";
      for (int x = 0; x < nx; x++) {
        std::cout << data[y * nx + x] << ((x + 1 == nx) ? "f" : "f, ");
      }
      std::cout << ((y + 1 == ny) ? "}" : "},") << std::endl;
    }
    if (nz > 1) {
      std::cout << ((z + 1 == nz) ? "  }" : "  },") << std::endl;
    }
    else {
      std::cout << "};" << std::endl;
    }

    FILE *f = fopen((filename + ".pfm").c_str(), "w");
    fprintf(f, "Pf\n%d %d\n-1.0\n", nx, ny);
    fwrite(data, sizeof(float), nx * ny, f);
    fclose(f);
  }
  if (nz > 1) {
    std::cout << "};" << std::endl;
  }

  return true;
}

CCL_NAMESPACE_END