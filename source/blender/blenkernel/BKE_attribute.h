/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 * \brief Generic geometry attributes built on CustomData.
 */

#pragma once

#include "BLI_sys_types.h"

#include "BKE_customdata.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CustomData;
struct CustomDataLayer;
struct ID;
struct ReportList;

/* Attribute.domain */
typedef enum AttributeDomain {
  ATTR_DOMAIN_AUTO = -1,    /* Use for nodes to choose automatically based on other data. */
  ATTR_DOMAIN_POINT = 0,    /* Mesh, Curve or Point Cloud Point */
  ATTR_DOMAIN_EDGE = 1,     /* Mesh Edge */
  ATTR_DOMAIN_FACE = 2,     /* Mesh Face */
  ATTR_DOMAIN_CORNER = 3,   /* Mesh Corner */
  ATTR_DOMAIN_CURVE = 4,    /* A single curve in a larger curve data-block */
  ATTR_DOMAIN_INSTANCE = 5, /* Instance */

  ATTR_DOMAIN_NUM
} AttributeDomain;

typedef enum AttributeDomainMask {
  ATTR_DOMAIN_MASK_POINT = (1 << 0),
  ATTR_DOMAIN_MASK_EDGE = (1 << 1),
  ATTR_DOMAIN_MASK_FACE = (1 << 2),
  ATTR_DOMAIN_MASK_CORNER = (1 << 3),
  ATTR_DOMAIN_MASK_CURVE = (1 << 4),
  ATTR_DOMAIN_MASK_ALL = (1 << 5) - 1
} AttributeDomainMask;

/* All domains that support color attributes */
#define ATTR_DOMAIN_MASK_COLOR \
  ((AttributeDomainMask)((ATTR_DOMAIN_MASK_POINT | ATTR_DOMAIN_MASK_CORNER)))

/* Attributes */

bool BKE_id_attributes_supported(struct ID *id);

/**  Create a new attribute layer
 */
struct CustomDataLayer *BKE_id_attribute_new(
    struct ID *id, const char *name, int type, AttributeDomain domain, struct ReportList *reports);
bool BKE_id_attribute_remove(struct ID *id,
                             struct CustomDataLayer *layer,
                             struct ReportList *reports);

struct CustomDataLayer *BKE_id_attribute_find(const struct ID *id,
                                              const char *name,
                                              int type,
                                              AttributeDomain domain);

AttributeDomain BKE_id_attribute_domain(struct ID *id, const struct CustomDataLayer *layer);
int BKE_id_attribute_data_length(struct ID *id, struct CustomDataLayer *layer);
bool BKE_id_attribute_required(struct ID *id, struct CustomDataLayer *layer);
bool BKE_id_attribute_rename(struct ID *id,
                             struct CustomDataLayer *layer,
                             const char *new_name,
                             struct ReportList *reports);

int BKE_id_attributes_length(const struct ID *id,
                             const AttributeDomainMask domain_mask,
                             const CustomDataMask mask);

struct CustomDataLayer *BKE_id_attributes_active_get(struct ID *id);
void BKE_id_attributes_active_set(struct ID *id, struct CustomDataLayer *layer);
int *BKE_id_attributes_active_index_p(struct ID *id);

CustomData *BKE_id_attributes_iterator_next_domain(struct ID *id, struct CustomDataLayer *layers);
CustomDataLayer *BKE_id_attribute_from_index(struct ID *id,
                                             int lookup_index,
                                             AttributeDomainMask domain_mask,
                                             CustomDataMask layer_mask);

/** Layer is allowed to be nullptr; if so -1 (layer not found) will be returned. */
int BKE_id_attribute_to_index(const struct ID *id,
                              const CustomDataLayer *layer,
                              AttributeDomainMask domain_mask,
                              CustomDataMask layer_mask);

struct CustomDataLayer *BKE_id_attribute_subset_active_get(struct ID *id,
                                                           int active_flag,
                                                           AttributeDomainMask domain_mask,
                                                           CustomDataMask mask);
void BKE_id_attribute_subset_active_set(struct ID *id,
                                        struct CustomDataLayer *layer,
                                        int active_flag,
                                        AttributeDomainMask domain_mask,
                                        CustomDataMask mask);

/** Copies CustomData instances into a (usually stack-allocated) ID.  This is a shallow copy, the
    purpose is to create a bride for using the C attribute API on arbitrary sets of CustomData
    domains.
*/

void BKE_id_attribute_copy_domains_temp(struct ID *temp_id,
                                        const struct CustomData *vdata,
                                        const struct CustomData *edata,
                                        const struct CustomData *ldata,
                                        const struct CustomData *pdata,
                                        const struct CustomData *cdata);

struct CustomDataLayer *BKE_id_attributes_active_color_get(struct ID *id);
void BKE_id_attributes_active_color_set(struct ID *id, struct CustomDataLayer *active_layer);
struct CustomDataLayer *BKE_id_attributes_render_color_get(struct ID *id);
void BKE_id_attributes_render_color_set(struct ID *id, struct CustomDataLayer *active_layer);

bool BKE_id_attribute_find_unique_name(struct ID *id, const char *name, char *outname);

#ifdef __cplusplus
}
#endif
