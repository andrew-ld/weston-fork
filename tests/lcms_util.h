/*
 * Copyright 2022 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <lcms2.h>
#include <libweston/linalg-types.h>

#include "color_util.h"

struct lcms_pipeline {
	/**
	 * Color space name
	 */
	const char *color_space;
	/**
	 * Chromaticities for output profile
	 * White point is assumed to be D65.
	 */
	cmsCIExyYTRIPLE prim_output;
	/**
	 * tone curve enum
	 */
	enum transfer_fn pre_fn;
	/**
	 * Transform matrix from sRGB to target chromaticities in prim_output
	 */
	struct weston_mat3f mat;
	/**
	 * tone curve enum
	 */
	enum transfer_fn post_fn;
};

cmsToneCurve *
build_MPE_curve(cmsContext ctx, enum transfer_fn fn);

cmsStage *
build_MPE_curve_stage(cmsContext context_id, enum transfer_fn fn);

cmsBool
SetTextTags(cmsHPROFILE hProfile, const wchar_t* Description);

cmsHPROFILE
build_lcms_matrix_shaper_profile_output(cmsContext context_id,
                                        const struct lcms_pipeline *pipeline,
					const double vcgt_exponents[COLOR_CHAN_NUM]);

cmsHPROFILE
build_lcms_clut_profile_output(cmsContext context_id,
                               const struct lcms_pipeline *pipeline,
			       const double vcgt_exponents[COLOR_CHAN_NUM],
			       int clut_dim_size, float clut_roundtrip_tolerance);
