/*
 * Copyright 2021 Collabora, Ltd.
 * Copyright 2021 Advanced Micro Devices, Inc.
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

#ifndef WESTON_COLOR_H
#define WESTON_COLOR_H

#include <stdbool.h>
#include <stdint.h>
#include <libweston/libweston.h>

#include "backend-drm/drm-kms-enums.h"

/**
 * The only tf which is parametric (WESTON_TF_POWER) has a single parameter.
 */
#define MAX_PARAMS_TF 1

enum weston_hdr_metadata_type1_groups {
	/** weston_hdr_metadata_type1::primary is set */
	WESTON_HDR_METADATA_TYPE1_GROUP_PRIMARIES	= 0x01,

	/** weston_hdr_metadata_type1::white is set */
	WESTON_HDR_METADATA_TYPE1_GROUP_WHITE		= 0x02,

	/** weston_hdr_metadata_type1::maxDML is set */
	WESTON_HDR_METADATA_TYPE1_GROUP_MAXDML		= 0x04,

	/** weston_hdr_metadata_type1::minDML is set */
	WESTON_HDR_METADATA_TYPE1_GROUP_MINDML		= 0x08,

	/** weston_hdr_metadata_type1::maxCLL is set */
	WESTON_HDR_METADATA_TYPE1_GROUP_MAXCLL		= 0x10,

	/** weston_hdr_metadata_type1::maxFALL is set */
	WESTON_HDR_METADATA_TYPE1_GROUP_MAXFALL		= 0x20,

	/** all valid bits */
	WESTON_HDR_METADATA_TYPE1_GROUP_ALL_MASK	= 0x3f
};

/** HDR static metadata type 1
 *
 * The fields are defined by CTA-861-G except here they use float encoding.
 *
 * In Weston used only with HDR display modes.
 */
struct weston_hdr_metadata_type1 {
	/** Which fields are valid
	 *
	 * A bitmask of values from enum weston_hdr_metadata_type1_groups.
	 */
	uint32_t group_mask;

	/* EOTF is tracked externally with enum weston_eotf_mode */

	/** Chromaticities of the primaries, in any order */
	struct weston_CIExy primary[3];

	/** White point chromaticity */
	struct weston_CIExy white;

	/** Maximum display mastering luminance, 1 - 65535 cd/m² */
	float maxDML;

	/** Minimum display mastering luminance, 0.0001 - 6.5535 cd/m² */
	float minDML;

	/** Maximum content light level, 1 - 65535 cd/m² */
	float maxCLL;

	/** Maximum frame-average light level, 1 - 65535 cd/m² */
	float maxFALL;
};

/** Output properties derived from its color characteristics and profile
 *
 * These are constructed by a color manager.
 *
 * A weston_output_color_outcome owns (a reference to) everything it contains.
 *
 * \ingroup output
 * \internal
 */
struct weston_output_color_outcome {
	/** sRGB to output color space transformation */
	struct weston_color_transform *from_sRGB_to_output;

	/** sRGB to blending color space transformation */
	struct weston_color_transform *from_sRGB_to_blend;

	/** Blending to output color space transformation */
	struct weston_color_transform *from_blend_to_output;

	/** HDR Static Metadata Type 1 for WESTON_EOTF_MODE_ST2084 */
	struct weston_hdr_metadata_type1 hdr_meta;
};

/**
 * Represents a color profile description (an ICC color profile)
 *
 * Sub-classed by the color manager that created this.
 */
struct weston_color_profile {
	struct weston_color_manager *cm;
	int ref_count;
	char *description;

	/* Unique id to be used by the CM&HDR protocol extension. Should not
	 * be confused with the protocol object id. */
	uint32_t id;
};

/** Parameters that define a parametric color profile */
struct weston_color_profile_params {
	/* Primary color volume; always set. */
	struct weston_color_gamut primaries;

	/* Primary color volume by enumeration; optional, may be NULL. */
	const struct weston_color_primaries_info *primaries_info;

	/* Encoding transfer characteristic by enumeration; always set. */
	const struct weston_color_tf_info *tf_info;

	/* Transfer characteristic's parameters; depends on tf_info. */
	float tf_params[MAX_PARAMS_TF];

	/* Primary color volume luminance parameters cd/m²; always set. */
	float min_luminance, max_luminance;
	float reference_white_luminance;

	/* Target color volume; always set. */
	struct weston_color_gamut target_primaries;

	/* Target luminance parameters cd/m²; negative when not set */
	float target_min_luminance, target_max_luminance;
	float maxCLL, maxFALL;

	char padding[8];
};

/** Type for parametric curves */
enum weston_color_curve_parametric_type {
	/** Transfer function named LINPOW
	 *
	 * y = (a * x + b) ^ g | x >= d
	 * y = c * x           | 0 <= x < d
	 *
	 * We gave it the name LINPOW because the first operation with the input
	 * x is a linear one, and then the result is raised to g.
	 *
	 * As all parametric curves, this one should be represented using struct
	 * weston_color_curve_parametric. For each color channel RGB we may have
	 * different params, see weston_color_curve_parametric::params.
	 *
	 * For LINPOW, the params g, a, b, c, and d are respectively
	 * params[channel][0], ... , params[channel][4].
	 *
	 * The input for all color channels may be clamped to [0.0, 1.0]. In
	 * such case, weston_color_curve_parametric::clamped_input is true.
	 * If the input is not clamped and LINPOW needs to evaluate a negative
	 * input value, it uses mirroring (i.e. -f(-x)).
	 */
	WESTON_COLOR_CURVE_PARAMETRIC_TYPE_LINPOW,

	/** Transfer function named POWLIN
	 *
	 * y = (a * (x ^ g)) + b | x >= d
	 * y = c * x             | 0 <= x < d
	 *
	 * We gave it the name POWLIN because the first operation with the input
	 * x is an exponential one, and then the result is multiplied by a.
	 *
	 * As all parametric curves, this one should be represented using struct
	 * weston_color_curve_parametric. For each color channel RGB we may have
	 * different params, see weston_color_curve_parametric::params.
	 *
	 * For POWLIN, the params g, a, b, c, and d are respectively
	 * params[channel][0], ... , params[channel][4].
	 *
	 * The input for all color channels may be clamped to [0.0, 1.0]. In
	 * such case, weston_color_curve_parametric::clamped_input is true.
	 * If the input is not clamped and POWLIN needs to evaluate a negative
	 * input value, it uses mirroring (i.e. -f(-x)).
	 */
	WESTON_COLOR_CURVE_PARAMETRIC_TYPE_POWLIN,
};

/** Type or formula for a curve */
enum weston_color_curve_type {
	/** Identity function, no-op */
	WESTON_COLOR_CURVE_TYPE_IDENTITY = 0,

	/** Three-channel, one-dimensional look-up table */
	WESTON_COLOR_CURVE_TYPE_LUT_3x1D,

	/** Enumerated color curve */
	WESTON_COLOR_CURVE_TYPE_ENUM,

	/** Parametric color curve */
	WESTON_COLOR_CURVE_TYPE_PARAMETRIC,
};

/** LUT_3x1D parameters */
struct weston_color_curve_lut_3x1d {
	/**
	 * Approximate a color curve with three 1D LUTs
	 *
	 * A 1D LUT is a mapping from [0.0, 1.0] to arbitrary values. The first
	 * element in the LUT corresponds to input value 0.0, and the last
	 * element corresponds to input value 1.0. The step from one element
	 * to the next in input space is 1.0 / (len - 1). When input value is
	 * between two elements, linear interpolation should be used.
	 *
	 * This function fills in the given array with the LUT values.
	 *
	 * \param xform This color transformation object.
	 * \param len The number of elements in each 1D LUT.
	 * \param values Array of 3 x len elements. First R channel
	 * LUT, immediately followed by G channel LUT, and then B channel LUT.
	 */
	void
	(*fill_in)(struct weston_color_transform *xform,
		   float *values, unsigned len);

	/** Optimal 1D LUT length for storage vs. precision */
	unsigned optimal_len;
};

/** Direct or inverse of a tf. */
enum weston_tf_direction {
	WESTON_FORWARD_TF,
	WESTON_INVERSE_TF,
};

/** Enumerated color curve */
struct weston_color_curve_enum {
	const struct weston_color_tf_info *tf;

	/* Determines if the direct or inverse of the tf should be used. */
	enum weston_tf_direction tf_direction;

	/* Some tf are parametric, and we keep the params here. They may be
	 * different for each color channel, and channels are in RGB order. */
	float params[3][MAX_PARAMS_TF];
};

/**
 * The only params curve we have are WESTON_COLOR_CURVE_PARAMETRIC_TYPE_LINPOW
 * and WESTON_COLOR_CURVE_PARAMETRIC_TYPE_POWLIN, and they have exactly 5
 * params.
 */
union weston_color_curve_parametric_chan_data {
	struct {
		float g, a, b, c, d;
	};
	float data[5];
};

union weston_color_curve_parametric_data {
	/** Channels in RGB order. */
	union weston_color_curve_parametric_chan_data chan[3];
	float array[15];
};

static_assert(sizeof (union weston_color_curve_parametric_data){}.array ==
	      sizeof (union weston_color_curve_parametric_data){}.chan,
	      "union size error");

/** Parametric color curve parameters */
struct weston_color_curve_parametric {
	enum weston_color_curve_parametric_type type;

	/* For each color channel we may have curves with different params. The
	 * channels are in RGB order. */
	union weston_color_curve_parametric_data params;

	/* The input of the curve should be clamped from 0.0 to 1.0? */
	bool clamped_input;
};

/**
 * A scalar function for color encoding and decoding
 *
 * This object can represent a one-dimensional function that is applied
 * independently to each of the color channels. Depending on the type and
 * parameterization of the curve, all color channels may use the
 * same function or each may have separate parameters.
 *
 * This is usually used for EOTF or EOTF^-1 and to optimize a 3D LUT size
 * without sacrificing precision, both in one step.
 */
struct weston_color_curve {
	/** Which member of 'u' defines the curve. */
	enum weston_color_curve_type type;

	/** Parameters for the curve. */
	union {
		/* identity: no parameters */
		struct weston_color_curve_lut_3x1d lut_3x1d;
		struct weston_color_curve_enum enumerated;
		struct weston_color_curve_parametric parametric;
	} u;
};

/** Type or formula for a color mapping */
enum weston_color_mapping_type {
	/** Identity function, no-op */
	WESTON_COLOR_MAPPING_TYPE_IDENTITY = 0,

	/** matrix */
	WESTON_COLOR_MAPPING_TYPE_MATRIX,
};

/**
 * A 3x3 matrix and data is arranged as column major
 */
struct weston_color_mapping_matrix {
	struct weston_mat3f matrix;
	struct weston_vec3f offset;
};

/**
 * Color mapping function
 *
 * This object can represent a 3D LUT to do a color space conversion
 *
 */
struct weston_color_mapping {
	/** Which member of 'u' defines the color mapping type */
	enum weston_color_mapping_type type;

	/** Parameters for the color mapping function */
	union {
		/* identity: no parameters */
		struct weston_color_mapping_matrix mat;
	} u;
};

/**
 * Describes a color transformation formula
 *
 * Guaranteed unique, de-duplicated.
 *
 * Sub-classed by the color manager that created this.
 *
 * For a renderer to support WESTON_CAP_COLOR_OPS it must implement everything
 * that this structure can represent.
 */
struct weston_color_transform {
	struct weston_color_manager *cm;
	int ref_count;
	uint32_t id; /* For debug */

	/* for renderer or backend to attach their own cached objects */
	struct wl_signal destroy_signal;

	/**
	 * When this is true, users are allowed to use the steps described below
	 * (pre curve, color mapping and post curve) and implement the color
	 * transformation themselves. Otherwise this is forbidden and
	 * to_shaper_plus_3dlut() must be used.
	 */
	bool steps_valid;

	/* Color transform is the series of steps: */

	/** Step 1: color model change */
	/* YCbCr→RGB conversion, but that is done elsewhere */

	/** Step 2: color curve before color mapping */
	struct weston_color_curve pre_curve;

	/** Step 3: color mapping */
	struct weston_color_mapping mapping;

	/** Step 4: color curve after color mapping */
	struct weston_color_curve post_curve;

	/**
	 * Decompose the color transformation into a shaper (3x1D LUT) and a 3D
	 * LUT.
	 *
	 * \param xform_base The color transformation to decompose.
	 * \param len_shaper Number of taps in each of the 1D LUT.
	 * \param shaper Where the shaper is saved, caller's responsibility to
	 * allocate.
	 * \param len_lut3d The 3D LUT's length for each dimension.
	 * \param lut3d Where the 3D LUT is saved, caller's responsibility to
	 * allocate. Its layout on memory is: lut3d[B][G][R], i.e. R is the
	 * innermost and its index grow faster, followed by G and then B.
	 * \return True on success, false otherwise.
	 */
	bool
	(*to_shaper_plus_3dlut)(struct weston_color_transform *xform_base,
				uint32_t len_shaper, float *shaper,
				uint32_t len_lut3d, float *lut3d);
};

/**
 * How content color needs to be transformed
 *
 * This object is specific to the color properties of the weston_surface and
 * weston_output it was created for. It is automatically destroyed if any
 * relevant color properties change.
 *
 * Fundamentally this contains the color transformation from content color
 * space to an output's blending color space. This is stored in field
 * 'transform' with NULL value corresponding to identity transformation.
 *
 * For graphics pipeline optimization purposes, the field 'identity_pipeline'
 * indicates whether the combination of 'transform' here and the output's
 * blending color space to monitor color space transformation total to
 * identity transformation. This helps detecting cases where renderer bypass
 * (direct scanout) is possible.
 */
struct weston_surface_color_transform {
	/** Transformation from source to blending space */
	struct weston_color_transform *transform;

	/** True, if source colorspace is identical to monitor color space */
	bool identity_pipeline;
};

struct cm_image_desc_info;

struct weston_color_manager {
	/** Identifies this CMS component */
	const char *name;

	/** This compositor instance */
	struct weston_compositor *compositor;

	/** Supports the Wayland CM&HDR protocol extension? */
	bool supports_client_protocol;

	/**
	 * Supported color features from Wayland CM&HDR protocol extension.
	 *
	 * If v (v being enum weston_color_feature v) is a supported color
	 * feature, the bit v of this will be set to 1.
	 */
	uint32_t supported_color_features;

	/**
	 * Supported rendering intents from Wayland CM&HDR protocol extension.
	 *
	 * If v (v being enum weston_render_intent v) is a supported rendering
	 * intent, the bit v of this will be set to 1.
	 */
	uint32_t supported_rendering_intents;

	/**
	 * Supported primaries named from Wayland CM&HDR protocol extension.
	 *
	 * If v (v being enum weston_color_primaries v) is a supported
	 * primaries named, the bit v of this will be set to 1.
	 */
	uint32_t supported_primaries_named;

	/**
	 * Supported tf named from Wayland CM&HDR protocol extension.
	 *
	 * If v (v being enum weston_transfer_function v) is a supported
	 * tf named, the bit v of this will be set to 1.
	 */
	uint32_t supported_tf_named;

	/** Initialize color manager */
	bool
	(*init)(struct weston_color_manager *cm);

	/** Destroy color manager */
	void
	(*destroy)(struct weston_color_manager *cm);

	/** Destroy a color profile after refcount fell to zero */
	void
	(*destroy_color_profile)(struct weston_color_profile *cprof);

	/** Gets a new reference to the stock sRGB color profile
	 *
	 * \param cm The color manager.
	 * \return A new reference to the stock sRGB profile, never returns NULL.
	 */
	struct weston_color_profile *
	(*ref_stock_sRGB_color_profile)(struct weston_color_manager *cm);

	/** Create a color profile from ICC data
	 *
	 * \param cm The color manager.
	 * \param icc_data Pointer to the ICC binary data.
	 * \param icc_len Length of the ICC data in bytes.
	 * \param name_part A string to be used in describing the profile.
	 * \param cprof_out On success, the created object is returned here.
	 * On failure, untouched.
	 * \param errmsg On success, untouched. On failure, a pointer to a
	 * string describing the error is stored here. The string must be
	 * free()'d.
	 * \return True on success, false on failure.
	 *
	 * This may return a new reference to an existing color profile if
	 * that profile is identical to the one that would be created, apart
	 * from name_part.
	 */
	bool
	(*get_color_profile_from_icc)(struct weston_color_manager *cm,
				      const void *icc_data,
				      size_t icc_len,
				      const char *name_part,
				      struct weston_color_profile **cprof_out,
				      char **errmsg);

	/** Create a color profile from parameters
	 *
	 * \param cm The color manager.
	 * \param params The struct weston_color_profile_params with the params.
	 * \param name_part A string to be used in describing the profile.
	 * \param cprof_out On success, the created object is returned here.
	 * On failure, untouched.
	 * \param errmsg On success, untouched. On failure, a pointer to a
	 * string describing the error is stored here. The string must be
	 * free()'d.
	 * \return True on success, false on failure.
	 *
	 * This may return a new reference to an existing color profile if
	 * that profile is identical to the one that would be created, apart
	 * from name_part.
	 */
	bool
	(*get_color_profile_from_params)(struct weston_color_manager *cm,
					 const struct weston_color_profile_params *params,
					 const char *name_part,
					 struct weston_color_profile **cprof_out,
					 char **errmsg);

	/** Send image description to clients.
	 *
	 * \param cm_image_desc_info The image description info object
	 * \param cprof_base The color profile that backs the image description
	 * \return True on success, false on failure
	 *
	 * This should be used only by the CM&HDR protocol extension
	 * implementation.
	 *
	 * The color manager implementing this function should use the helpers
	 * from color-management.c (weston_cm_send_primaries(), etc) to send the
	 * information to clients.
	 */
	bool
	(*send_image_desc_info)(struct cm_image_desc_info *cm_image_desc_info,
				struct weston_color_profile *cprof_base);

	/** Given a color profile, returns a reference to a profile that is
	 * guaranteed to be parametric and which is equivalent to the given
	 * profile.
	 *
	 * \param cprof_base The color profile.
	 * \param errmsg On success, untouched. On failure, a pointer to a
	 * string describing the error is stored here. The string must be
	 * free()'d.
	 * \return A reference to an equivalent parametric color profile, or
	 * NULL on failure.
	 */
	struct weston_color_profile *
	(*get_parametric_color_profile)(struct weston_color_profile *cprof_base,
					char **errmsg);

	/** Destroy a color transform after refcount fell to zero */
	void
	(*destroy_color_transform)(struct weston_color_transform *xform);

	/** Get surface to output's blending space transformation
	 *
	 * \param cm The color manager.
	 * \param surface The surface for the source color space.
	 * \param output The output for the destination blending color space.
	 * \param surf_xform For storing the color transformation and
	 * additional information.
	 *
	 * The callee is responsible for increasing the reference count on the
	 * weston_color_transform it stores into surf_xform.
	 */
	bool
	(*get_surface_color_transform)(struct weston_color_manager *cm,
				       struct weston_surface *surface,
				       struct weston_output *output,
				       struct weston_surface_color_transform *surf_xform);

	/** Compute derived color properties for an output
	 *
	 * \param cm The color manager.
	 * \param output The output.
	 * \return A new color_outcome object on success, NULL on failure.
	 *
	 * The callee (color manager) must inspect the weston_output (color
	 * profile, EOTF mode, etc.) and create a fully populated
	 * weston_output_color_outcome object.
	 */
	struct weston_output_color_outcome *
	(*create_output_color_outcome)(struct weston_color_manager *cm,
				       struct weston_output *output);
};

void
weston_color_profile_init(struct weston_color_profile *cprof,
			  struct weston_color_manager *cm);

char *
weston_color_profile_params_to_str(struct weston_color_profile_params *params,
				   const char *ident);

bool
weston_color_curve_enum_get_parametric(struct weston_compositor *compositor,
				       const struct weston_color_curve_enum *curve,
				       struct weston_color_curve_parametric *out);

void
weston_color_curve_from_tf_info(struct weston_color_curve *curve,
				const struct weston_color_tf_info *tf_info,
				const float tf_params[MAX_PARAMS_TF],
				enum weston_tf_direction tf_direction);

enum weston_color_curve_step {
	WESTON_COLOR_CURVE_STEP_PRE,
	WESTON_COLOR_CURVE_STEP_POST,
};

float *
weston_color_curve_to_3x1D_LUT(struct weston_compositor *compositor,
			       struct weston_color_transform *xform,
			       enum weston_color_curve_step step,
			       uint32_t lut_size, char **err_msg);

void
find_neighbors(struct weston_compositor *compositor, uint32_t len, float *array,
	       float val, uint32_t *neigh_A_index, uint32_t *neigh_B_index);

float
weston_inverse_evaluate_lut1d(struct weston_compositor *compositor,
			      uint32_t len_lut, float *lut, float input);

struct weston_color_transform *
weston_color_transform_ref(struct weston_color_transform *xform);

void
weston_color_transform_unref(struct weston_color_transform *xform);

void
weston_color_transform_init(struct weston_color_transform *xform,
			    struct weston_color_manager *cm);

char *
weston_color_transform_string(const struct weston_color_transform *xform);

char *
weston_color_transform_details_string(int indent,
				      const struct weston_color_transform *xform);

void
weston_surface_color_transform_copy(struct weston_surface_color_transform *dst,
				    const struct weston_surface_color_transform *src);

void
weston_surface_color_transform_fini(struct weston_surface_color_transform *surf_xform);

struct weston_paint_node;

void
weston_paint_node_ensure_color_transform(struct weston_paint_node *pnode);

struct weston_color_manager *
weston_color_manager_noop_create(struct weston_compositor *compositor);

/* DSO module entrypoint */
struct weston_color_manager *
weston_color_manager_create(struct weston_compositor *compositor);

char *
weston_eotf_mask_to_str(uint32_t eotf_mask);

struct weston_colorimetry_mode_info {
	/** Primary key: the colorimetry mode */
	enum weston_colorimetry_mode mode;

	/** Its name as a string for logging. */
	const char *name;

	/** wdrm equivalent, or WDRM_COLORSPACE__COUNT if none */
	enum wdrm_colorspace wdrm;
};

const struct weston_colorimetry_mode_info *
weston_colorimetry_mode_info_get(enum weston_colorimetry_mode c);

const struct weston_colorimetry_mode_info *
weston_colorimetry_mode_info_get_by_wdrm(enum wdrm_colorspace cs);

char *
weston_colorimetry_mask_to_str(uint32_t colorimetry_mask);

void
weston_output_color_outcome_destroy(struct weston_output_color_outcome **pco);

#endif /* WESTON_COLOR_H */
