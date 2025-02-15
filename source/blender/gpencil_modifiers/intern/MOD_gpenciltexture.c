/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_rand.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_gpencil_geom.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_query.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"

#include "MOD_gpencil_modifiertypes.h"
#include "MOD_gpencil_ui_common.h"
#include "MOD_gpencil_util.h"

static void initData(GpencilModifierData *md)
{
  TextureGpencilModifierData *gpmd = (TextureGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(TextureGpencilModifierData), modifier);
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  BKE_gpencil_modifier_copydata_generic(md, target);
}

/* change stroke uv texture values */
static void deformStroke(GpencilModifierData *md,
                         Depsgraph *UNUSED(depsgraph),
                         Object *ob,
                         bGPDlayer *gpl,
                         bGPDframe *gpf,
                         bGPDstroke *gps)
{
  TextureGpencilModifierData *mmd = (TextureGpencilModifierData *)md;
  const int def_nr = BKE_object_defgroup_name_index(ob, mmd->vgname);
  bGPdata *gpd = ob->data;

  if (!is_stroke_affected_by_modifier(ob,
                                      mmd->layername,
                                      mmd->material,
                                      mmd->pass_index,
                                      mmd->layer_pass,
                                      1,
                                      gpl,
                                      gps,
                                      mmd->flag & GP_TEX_INVERT_LAYER,
                                      mmd->flag & GP_TEX_INVERT_PASS,
                                      mmd->flag & GP_TEX_INVERT_LAYERPASS,
                                      mmd->flag & GP_TEX_INVERT_MATERIAL)) {
    return;
  }

  if (ELEM(mmd->mode, FILL, STROKE_AND_FILL)) {
    gps->uv_rotation += mmd->fill_rotation;
    gps->uv_translation[0] += mmd->fill_offset[0];
    gps->uv_translation[1] += mmd->fill_offset[1];
    gps->uv_scale *= mmd->fill_scale;
    BKE_gpencil_stroke_geometry_update(gpd, gps);
  }

  if (ELEM(mmd->mode, STROKE, STROKE_AND_FILL)) {
    float totlen = 1.0f;
    if (mmd->fit_method == GP_TEX_FIT_STROKE) {
      totlen = 0.0f;
      for (int i = 1; i < gps->totpoints; i++) {
        totlen += len_v3v3(&gps->points[i - 1].x, &gps->points[i].x);
      }
    }

    const bool is_randomized = (mmd->rnd_offset != 0.0f || mmd->rnd_scale != 0.0f);

    int seed = mmd->seed;
    /* Make sure different modifiers get different seeds. */
    seed += BLI_hash_string(ob->id.name + 2);
    seed += BLI_hash_string(md->name);

    float rand_offset = BLI_hash_int_01(seed);
    float rand_value;
    float rand_scale;

    if (is_randomized) {
      /* Get stroke index for random offset. */
      int rnd_index = BLI_findindex(&gpf->strokes, gps);

      const uint primes[2] = {2, 3};
      double offset[2] = {0.0f, 0.0f};
      double r[2];
      /* To ensure a nice distribution, we use halton sequence and offset using the seed. */
      BLI_halton_2d(primes, offset, rnd_index, r);

      rand_value = fmodf(r[0] * 2.0f - 1.0f + rand_offset, 1.0f);
      rand_value = fmodf(sin(rand_value * 12.9898f) * 43758.5453f, 1.0f);
      rand_scale = fmodf(r[1] * 2.0f - 1.0f + rand_offset, 1.0f);
      rand_scale = fmodf(sin(rand_scale * 12.9898f + 78.233f) * 43758.5453f, 1.0f);
    }

    for (int i = 0; i < gps->totpoints; i++) {
      bGPDspoint *pt = &gps->points[i];
      MDeformVert *dvert = gps->dvert != NULL ? &gps->dvert[i] : NULL;
      /* Verify point is part of vertex group. */
      float weight = get_modifier_point_weight(
          dvert, (mmd->flag & GP_TEX_INVERT_VGROUP) != 0, def_nr);
      if (weight < 0.0f) {
        continue;
      }

      /* Calculate Random matrix. */
      if (is_randomized) {
        float rnd_loc, rnd_scale_weight;
        float rnd_scale = 1.0f;

        rnd_loc = rand_value * weight;
        rnd_loc *= mmd->rnd_offset * rnd_loc;

        rnd_scale_weight = rand_scale * weight;
        rnd_scale += mmd->rnd_scale * rnd_scale_weight;

        pt->uv_fac *= rnd_scale;
        pt->uv_fac += rnd_loc;
      }

      pt->uv_fac /= totlen;
      pt->uv_fac *= mmd->uv_scale;
      pt->uv_fac += mmd->uv_offset;
      pt->uv_rot += mmd->alignment_rotation;
    }
  }
}

static void bakeModifier(struct Main *UNUSED(bmain),
                         Depsgraph *depsgraph,
                         GpencilModifierData *md,
                         Object *ob)
{
  generic_bake_deform_stroke(depsgraph, md, ob, false, deformStroke);
}

static void foreachIDLink(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  TextureGpencilModifierData *mmd = (TextureGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  int mode = RNA_enum_get(ptr, "mode");

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "mode", 0, NULL, ICON_NONE);

  if (ELEM(mode, STROKE, STROKE_AND_FILL)) {
    col = uiLayoutColumn(layout, false);
    uiItemR(col, ptr, "fit_method", 0, IFACE_("Stroke Fit Method"), ICON_NONE);
    uiItemR(col, ptr, "uv_offset", 0, NULL, ICON_NONE);
    uiItemR(col, ptr, "alignment_rotation", 0, NULL, ICON_NONE);
    uiItemR(col, ptr, "uv_scale", 0, IFACE_("Scale"), ICON_NONE);
  }

  if (mode == STROKE_AND_FILL) {
    uiItemS(layout);
  }

  if (ELEM(mode, FILL, STROKE_AND_FILL)) {
    col = uiLayoutColumn(layout, false);
    uiItemR(col, ptr, "fill_rotation", 0, NULL, ICON_NONE);
    uiItemR(col, ptr, "fill_offset", 0, IFACE_("Offset"), ICON_NONE);
    uiItemR(col, ptr, "fill_scale", 0, IFACE_("Scale"), ICON_NONE);
  }

  gpencil_modifier_panel_end(layout, ptr);
}

static void random_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "random_offset", 0, IFACE_("Offset"), ICON_NONE);
  uiItemR(layout, ptr, "random_scale", 0, IFACE_("Scale"), ICON_NONE);
  uiItemR(layout, ptr, "seed", 0, NULL, ICON_NONE);
}

static void mask_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, true, true);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_Texture, panel_draw);
  gpencil_modifier_subpanel_register(
      region_type, "randomize", "Randomize", NULL, random_panel_draw, panel_type);
  gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", NULL, mask_panel_draw, panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_Texture = {
    /* name */ N_("TextureMapping"),
    /* structName */ "TextureGpencilModifierData",
    /* structSize */ sizeof(TextureGpencilModifierData),
    /* type */ eGpencilModifierTypeType_Gpencil,
    /* flags */ eGpencilModifierTypeFlag_SupportsEditmode,

    /* copyData */ copyData,

    /* deformStroke */ deformStroke,
    /* generateStrokes */ NULL,
    /* bakeModifier */ bakeModifier,
    /* remapTime */ NULL,

    /* initData */ initData,
    /* freeData */ NULL,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* panelRegister */ panelRegister,
};
