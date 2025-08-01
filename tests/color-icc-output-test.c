/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 * Copyright 2020, 2022 Collabora, Ltd.
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

#include "config.h"

#include <lcms2.h>
#include <libweston/linalg-3.h>

#include "weston-test-client-helper.h"
#include "weston-test-assert.h"
#include "image-iter.h"
#include "lcms_util.h"

static const int WINDOW_WIDTH  = 256;
static const int WINDOW_HEIGHT = 24;

enum profile_type {
	PTYPE_MATRIX_SHAPER,
	PTYPE_CLUT,
};

/*
 * Using currently destination gamut bigger than source.
 * Using https://www.colour-science.org/ we can extract conversion matrix:
 * import colour
 * colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['sRGB'], colour.RGB_COLOURSPACES['Adobe RGB (1998)'], None)
 * colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['sRGB'], colour.RGB_COLOURSPACES['ITU-R BT.2020'], None)
 */

const struct lcms_pipeline pipeline_sRGB = {
	.color_space = "sRGB",
	.prim_output = {
		.Red =   { 0.640, 0.330, 1.0 },
		.Green = { 0.300, 0.600, 1.0 },
		.Blue =  { 0.150, 0.060, 1.0 }
	},
	.pre_fn = TRANSFER_FN_POWER2_2_EOTF,
	.mat = WESTON_MAT3F_IDENTITY,
	.post_fn = TRANSFER_FN_POWER2_2_EOTF_INVERSE
};

const struct lcms_pipeline pipeline_adobeRGB = {
	.color_space = "adobeRGB",
	.prim_output = {
		.Red =   { 0.640, 0.330, 1.0 },
		.Green = { 0.210, 0.710, 1.0 },
		.Blue =  { 0.150, 0.060, 1.0 }
	},
	.pre_fn = TRANSFER_FN_POWER2_2_EOTF,
	.mat = WESTON_MAT3F(
			 0.715127, 0.284868, 0.000005,
			 0.000001, 0.999995, 0.000004,
			-0.000003, 0.041155, 0.958848),
	.post_fn = TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE
};

const struct lcms_pipeline pipeline_BT2020 = {
	.color_space = "bt2020",
	.prim_output = {
		.Red =   { 0.708, 0.292, 1.0 },
		.Green = { 0.170, 0.797, 1.0 },
		.Blue =  { 0.131, 0.046, 1.0 }
	},
	.pre_fn = TRANSFER_FN_POWER2_2_EOTF,
	.mat = WESTON_MAT3F(
			0.627402, 0.329292, 0.043306,
			0.069095, 0.919544, 0.011360,
			0.016394, 0.088028, 0.895578),
	/* this is equivalent to BT.1886 with zero black level */
	.post_fn = TRANSFER_FN_POWER2_4_EOTF_INVERSE,
};

struct setup_args {
	struct fixture_metadata meta;
	int ref_image_index;
	const struct lcms_pipeline *pipeline;

	/**
	 * Two-norm color error tolerance in units of 1.0/255, computed in
	 * output electrical space, of the average error in the whole strip.
	 */
	float avg_tol;

	/**
	 * Two-norm color error tolerance in units of 1.0/255, computed in
	 * output electrical space, of the maximum error in the whole strip.
	 *
	 * Tolerance depends more on the 1D LUT used for the
	 * inv EOTF than the tested 3D LUT size:
	 * 9x9x9, 17x17x17, 33x33x33, 127x127x127
	 */
	float max_tol;

	/**
	 * 3DLUT dimension size
	 */
	int dim_size;
	enum profile_type type;

	/** Two-norm error limit for cLUT DToB->BToD roundtrip */
	float clut_roundtrip_tolerance;

	/**
	 * VCGT tag exponents for each channel. If any is zeroed, we ignore
	 * the VCGT tag.
	 */
	double vcgt_exponents[COLOR_CHAN_NUM];
};

static const struct setup_args my_setup_args[] = {
	{
		.meta = { "sRGB->sRGB MAT" },
		.ref_image_index = 0,
		.pipeline = &pipeline_sRGB,
		.avg_tol = 0.0,
		.max_tol = 0.0,
		.dim_size = 0,
		.type = PTYPE_MATRIX_SHAPER,
	},
	{
		.meta = { "sRGB->sRGB MAT VCGT" },
		.ref_image_index = 3,
		.pipeline = &pipeline_sRGB,
		.avg_tol = 0.34,
		.max_tol = 1,
		.dim_size = 0,
		.type = PTYPE_MATRIX_SHAPER,
		.vcgt_exponents = { 1.1, 1.2, 1.3 },
	},
	{
		.meta = { "sRGB->adobeRGB MAT" },
		.ref_image_index = 1,
		.pipeline = &pipeline_adobeRGB,
		.avg_tol = 0.46,
		.max_tol = 2,
		.dim_size = 0,
		.type = PTYPE_MATRIX_SHAPER,
	},
	{
		.meta = { "sRGB->adobeRGB MAT VCGT" },
		.ref_image_index = 4,
		.pipeline = &pipeline_adobeRGB,
		.avg_tol = 0.46,
		.max_tol = 1,
		.dim_size = 0,
		.type = PTYPE_MATRIX_SHAPER,
		.vcgt_exponents = { 1.1, 1.2, 1.3 },
	},
	{
		.meta = { "sRGB->BT2020 MAT" },
		.ref_image_index = 2,
		.pipeline = &pipeline_BT2020,
		.avg_tol = 0.47,
		.max_tol = 1,
		.dim_size = 0,
		.type = PTYPE_MATRIX_SHAPER,
	},
	{
		.meta = { "sRGB->sRGB CLUT" },
		.ref_image_index = 0,
		.pipeline = &pipeline_sRGB,
		.avg_tol = 1.06,
		.max_tol = 2,
		.dim_size = 17,
		.type = PTYPE_CLUT,
		.clut_roundtrip_tolerance = 0.01,
	},
	{
		.meta = { "sRGB->sRGB CLUT VCGT" },
		.ref_image_index = 3,
		.pipeline = &pipeline_sRGB,
		.avg_tol = 0.72,
		.max_tol = 2,
		.dim_size = 17,
		.type = PTYPE_CLUT,
		.clut_roundtrip_tolerance = 0.01,
		.vcgt_exponents = { 1.1, 1.2, 1.3 },
	},
	{
		.meta = { "sRGB->adobeRGB CLUT" },
		.ref_image_index = 1,
		.pipeline = &pipeline_adobeRGB,
		.avg_tol = 0.51,
		.max_tol = 2,
		.dim_size = 17,
		.type = PTYPE_CLUT,
		.clut_roundtrip_tolerance = 0.0065,
	},
	{
		.meta = { "sRGB->adobeRGB CLUT VCGT" },
		.ref_image_index = 4,
		.pipeline = &pipeline_adobeRGB,
		.avg_tol = 0.52,
		.max_tol = 2,
		.dim_size = 17,
		.type = PTYPE_CLUT,
		.clut_roundtrip_tolerance = 0.0065,
		.vcgt_exponents = { 1.1, 1.2, 1.3 },
	},
};

/*
 * Originally the cLUT profile test attempted to use the AToB/BToA tags. Those
 * come with serious limitations though: at most uint16 representation for
 * values in a LUT which means LUT entry precision is limited and range is
 * [0.0, 1.0]. This poses difficulties such as:
 * - for AToB, the resulting PCS XYZ values may need to be > 1.0
 * - for BToA, it is easy to fall outside of device color volume meaning that
 *   out-of-range values are needed in the 3D LUT
 * Working around these could require offsetting and scaling of values
 * before and after the 3D LUT, and even that may not always be possible.
 *
 * DToB/BToD tags do not have most of these problems, because there pipelines
 * use float32 representation throughout. We have much more precision, and
 * we can mostly use negative and greater than 1.0 values. LUT elements
 * still clamp their input to [0.0, 1.0] before applying the LUT. This type of
 * pipeline is called multiProcessElement (MPE).
 *
 * MPE also allows us to represent curves in a few analytical forms. These are
 * just enough to represent the EOTF curves we have and their inverses, but
 * they do not allow encoding extended EOTF curves or their inverses
 * (defined for all real numbers by extrapolation, and mirroring for negative
 * inputs). Using MPE curves we avoid the precision problems that arise from
 * attempting to represent an inverse-EOTF as a LUT. For the precision issue,
 * see: https://gitlab.freedesktop.org/pq/color-and-hdr/-/merge_requests/9
 *
 * MPE is not a complete remedy, because 3D LUT inputs are still always clamped
 * to [0.0, 1.0]. Therefore a 3D LUT cannot represent the inverse of a matrix
 * that can produce negative or greater than 1.0 values without further tricks
 * (scaling and offsetting) in the pipeline. Rather than implementing that
 * complication, we decided to just not test with such matrices. Therefore
 * BT.2020 color space is not used in the cLUT test. AdobeRGB is enough.
 */
static cmsHPROFILE
build_lcms_profile_output(const struct setup_args *arg)
{
	switch (arg->type) {
	case PTYPE_MATRIX_SHAPER:
		return build_lcms_matrix_shaper_profile_output(NULL,
							       arg->pipeline,
							       arg->vcgt_exponents);
	case PTYPE_CLUT:
		return build_lcms_clut_profile_output(NULL,
						      arg->pipeline,
						      arg->vcgt_exponents,
						      arg->dim_size,
						      arg->clut_roundtrip_tolerance);
	}

	return NULL;
}

static void
build_output_icc_profile(const struct setup_args *arg, const char *filename)
{
	cmsHPROFILE profile = NULL;
	bool saved;

	profile = build_lcms_profile_output(arg);
	test_assert_ptr_not_null(profile);

	saved = cmsSaveProfileToFile(profile, filename);
	test_assert_true(saved);

	cmsCloseProfile(profile);
}

static void
test_lcms_error_logger(cmsContext context_id,
		       cmsUInt32Number error_code,
		       const char *text)
{
	testlog("LittleCMS error: %s\n", text);
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *arg)
{
	struct compositor_setup setup;
	char *file_name;

#if !HAVE_CMS_GET_TONE_CURVE_SEGMENT
	/* When cmsGetToneCurveSegment() is at disposal, Weston is able to
	 * inspect the LittleCMS color curves and convert them to Weston's
	 * internal representation of color curves. In such case, we don't need
	 * to fallback to a more generic solution (usage of LUT's), which is
	 * less precise. Thanks to that, we are able to decrease the
	 * tolerance in this test. We already have cmsGetToneCurveSegment() in
	 * our CI, so simply skip this test when this is not available. */
	return RESULT_SKIP;
#endif

	cmsSetLogErrorHandler(test_lcms_error_logger);

	compositor_setup_defaults(&setup);
	setup.renderer = WESTON_RENDERER_GL;
	setup.backend = WESTON_BACKEND_HEADLESS;
	setup.width = WINDOW_WIDTH;
	setup.height = WINDOW_HEIGHT;
	setup.shell = SHELL_TEST_DESKTOP;
	setup.logging_scopes = "log,color-lcms-profiles,color-lcms-transformations,color-lcms-optimizer";
	setup.refresh = HIGHEST_OUTPUT_REFRESH;

	file_name = output_filename_for_fixture(THIS_TEST_NAME, harness,
						arg->meta.name, "icm");
	build_output_icc_profile(arg, file_name);

	weston_ini_setup(&setup,
		cfgln("[core]"),
		cfgln("output-decorations=true"),
		cfgln("color-management=true"),
		cfgln("[output]"),
		cfgln("name=headless"),
		cfgln("icc_profile=%s", file_name));

	free(file_name);

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, my_setup_args, meta);

static void
gen_ramp_rgb(pixman_image_t *image, int bitwidth, int width_bar)
{
	static const int hue[][COLOR_CHAN_NUM] = {
		{ 1, 1, 1 },	/* White	*/
		{ 1, 1, 0 },	/* Yellow 	*/
		{ 0, 1, 1 },	/* Cyan 	*/
		{ 0, 1, 0 },	/* Green 	*/
		{ 1, 0, 1 },	/* Magenta 	*/
		{ 1, 0, 0 },	/* Red 		*/
		{ 0, 0, 1 },	/* Blue 	*/
	};
	const int num_hues = ARRAY_LENGTH(hue);

	struct image_header ih = image_header_from(image);
	float val_max;
	int x, y;
	int hue_index;
	int chan;
	float value;
	unsigned char r, g, b;
	uint32_t *pixel;

	float n_steps = width_bar - 1;

	val_max = (1 << bitwidth) - 1;

	for (y = 0; y < ih.height; y++) {
		hue_index = (y * num_hues) / (ih.height - 1);
		hue_index = MIN(hue_index, num_hues - 1);

		pixel = image_header_get_row_u32(&ih, y);
		for (x = 0; x < ih.width; x++, pixel++) {
			struct color_float rgb = { .rgb = { 0, 0, 0 } };

			value = (float)x / (float)(ih.width - 1);

			if (width_bar > 1)
				value = floor(value * n_steps) / n_steps;

			for (chan = 0; chan < COLOR_CHAN_NUM; chan++) {
				if (hue[hue_index][chan])
					rgb.rgb[chan] = value;
			}

			sRGB_delinearize(&rgb);

			r = round(rgb.r * val_max);
			g = round(rgb.g * val_max);
			b = round(rgb.b * val_max);

			*pixel = (255U << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

static bool
process_pipeline_comparison(const struct buffer *src_buf,
			    const struct buffer *shot_buf,
			    const struct setup_args * arg)
{
	FILE *dump = NULL;
#if 0
	/*
	 * This file can be loaded in Octave for visualization. Find the script
	 * in tests/visualization/weston_plot_rgb_diff_stat.m and call it with
	 *
	 * weston_plot_rgb_diff_stat('opaque_pixel_conversion-f05-dump.txt')
	 */
	dump = fopen_dump_file(arg->meta.name);
#endif

	struct image_header ih_src = image_header_from(src_buf->image);
	struct image_header ih_shot = image_header_from(shot_buf->image);
	int y, x;
	struct color_float pix_src;
	struct color_float pix_src_pipeline;
	struct color_float pix_shot;
	struct rgb_diff_stat diffstat = { .dump = dump };
	float avg_err;
	float max_err;
	bool ok;

	/* no point to compare different images */
	test_assert_int_eq(ih_src.width, ih_shot.width);
	test_assert_int_eq(ih_src.height, ih_shot.height);

	for (y = 0; y < ih_src.height; y++) {
		uint32_t *row_ptr = image_header_get_row_u32(&ih_src, y);
		uint32_t *row_ptr_shot = image_header_get_row_u32(&ih_shot, y);

		for (x = 0; x < ih_src.width; x++) {
			pix_src = a8r8g8b8_to_float(row_ptr[x]);
			pix_shot = a8r8g8b8_to_float(row_ptr_shot[x]);

			process_pixel_using_pipeline(arg->pipeline->pre_fn,
						     arg->pipeline->mat,
						     arg->pipeline->post_fn,
						     arg->vcgt_exponents,
						     &pix_src, &pix_src_pipeline);

			rgb_diff_stat_update(&diffstat,
					     &pix_src_pipeline, &pix_shot,
					     &pix_src);
		}
	}

	avg_err = diffstat.two_norm.sum / diffstat.two_norm.count * 255.0f;
	max_err = diffstat.two_norm.max * 255.0f;
	ok = max_err <= arg->max_tol && avg_err <= arg->avg_tol;

	testlog("%s %s %s tolerances avg: %.2f <= %.2f, max: %.2f <= %.2f\n",
		__func__, ok ? "SUCCESS" : "FAILURE", arg->meta.name,
		avg_err, arg->avg_tol, max_err, arg->max_tol);

	rgb_diff_stat_print(&diffstat, __func__, 8);

	if (dump)
		fclose(dump);

	return ok;
}

/*
 * Test that opaque client pixels produce the expected output when converted
 * from the implicit sRGB input to ICC profile described output.
 *
 * The groundtruth conversion comes from the struct lcms_pipeline definitions.
 * The first error source is converting those to ICC files. The second error
 * source is Weston.
 *
 * This tests particularly the chain of input-to-blend followed by
 * blend-to-output categories of color transformations.
 */
TEST(opaque_pixel_conversion)
{
	int seq_no = get_test_fixture_index();
	const struct setup_args *arg = &my_setup_args[seq_no];
	const int width = WINDOW_WIDTH;
	const int height = WINDOW_HEIGHT;
	const int bitwidth = 8;
	const int width_bar = 32;

	struct client *client;
	struct buffer *buf;
	struct buffer *shot;
	struct wl_surface *surface;
	bool match;

	client = create_client_and_test_surface(0, 0, width, height);
	test_assert_ptr_not_null(client);
	surface = client->surface->wl_surface;

	buf = create_shm_buffer_a8r8g8b8(client, width, height);
	gen_ramp_rgb(buf->image, bitwidth, width_bar);

	wl_surface_attach(surface, buf->proxy, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	shot = capture_screenshot_of_output(client, NULL);
	test_assert_ptr_not_null(shot);

	match = verify_image(shot->image, "shaper_matrix", arg->ref_image_index,
			     NULL, seq_no);
	test_assert_true(process_pipeline_comparison(buf, shot, arg));
	test_assert_true(match);
	buffer_destroy(shot);
	buffer_destroy(buf);
	client_destroy(client);

	return RESULT_OK;
}

static struct color_float
convert_to_blending_space(const struct lcms_pipeline *pip,
			  struct color_float cf)
{
	/* Blending space is the linearized output space,
	 * or simply output space without the non-linear encoding
	 */
	cf = color_float_apply_curve(pip->pre_fn, cf);
	return color_float_apply_matrix(pip->mat, cf);
}

static void
compare_blend(const struct lcms_pipeline *pip,
	      const double vcgt_exponents[COLOR_CHAN_NUM],
	      struct color_float bg,
	      struct color_float fg,
	      const struct color_float *shot,
	      struct rgb_diff_stat *diffstat)
{
	struct color_float ref;
	unsigned i;

	/* convert sources to straight alpha */
	test_assert_f32_eq(bg.a, 1.0f);
	fg = color_float_unpremult(fg);

	bg = convert_to_blending_space(pip, bg);
	fg = convert_to_blending_space(pip, fg);

	/* blend */
	for (i = 0; i < COLOR_CHAN_NUM; i++)
		ref.rgb[i] = (1.0f - fg.a) * bg.rgb[i] + fg.a * fg.rgb[i];

	/* non-linear encoding for output */
	ref = color_float_apply_curve(pip->post_fn, ref);

	if (should_include_vcgt(vcgt_exponents))
		for (i = 0; i < COLOR_CHAN_NUM; i++)
			ref.rgb[i] = pow(ref.rgb[i], vcgt_exponents[i]);

	rgb_diff_stat_update(diffstat, &ref, shot, &fg);
}

/* Alpha blending test pattern parameters */
static const int ALPHA_STEPS = 256;
static const int BLOCK_WIDTH = 1;

static void *
get_middle_row(struct buffer *buf)
{
	struct image_header ih = image_header_from(buf->image);

	test_assert_int_ge(ih.width, BLOCK_WIDTH * ALPHA_STEPS);
	test_assert_int_ge(ih.height, BLOCK_WIDTH);

	return image_header_get_row_u32(&ih, (BLOCK_WIDTH - 1) / 2);
}

static bool
check_blend_pattern(struct buffer *bg_buf,
		    struct buffer *fg_buf,
		    struct buffer *shot_buf,
		    const struct setup_args *arg)
{
	FILE *dump = NULL;
#if 0
	/*
	 * This file can be loaded in Octave for visualization. Find the script
	 * in tests/visualization/weston_plot_rgb_diff_stat.m and call it with
	 *
	 * weston_plot_rgb_diff_stat('output_icc_alpha_blend-f01-dump.txt', 255, 8)
	 */
	dump = fopen_dump_file(arg->meta.name);
#endif

	uint32_t *bg_row = get_middle_row(bg_buf);
	uint32_t *fg_row = get_middle_row(fg_buf);
	uint32_t *shot_row = get_middle_row(shot_buf);
	struct rgb_diff_stat diffstat = { .dump = dump };
	int x;

	for (x = 0; x < BLOCK_WIDTH * ALPHA_STEPS; x++) {
		struct color_float bg = a8r8g8b8_to_float(bg_row[x]);
		struct color_float fg = a8r8g8b8_to_float(fg_row[x]);
		struct color_float shot = a8r8g8b8_to_float(shot_row[x]);

		compare_blend(arg->pipeline, arg->vcgt_exponents, bg, fg, &shot, &diffstat);
	}

	rgb_diff_stat_print(&diffstat, "Blending", 8);

	if (dump)
		fclose(dump);

	/* Test success condition: */
	return diffstat.two_norm.max < 1.72f / 255.0f;
}

static uint32_t
premult_color(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
	uint32_t c = 0;

	c |= a << 24;
	c |= (a * r / 255) << 16;
	c |= (a * g / 255) << 8;
	c |= a * b / 255;

	return c;
}

static void
fill_alpha_pattern(struct buffer *buf)
{
	struct image_header ih = image_header_from(buf->image);
	int y;

	test_assert_enum(ih.pixman_format, PIXMAN_a8r8g8b8);
	test_assert_int_eq(ih.width, BLOCK_WIDTH * ALPHA_STEPS);

	for (y = 0; y < ih.height; y++) {
		uint32_t *row = image_header_get_row_u32(&ih, y);
		uint32_t step;

		for (step = 0; step < (uint32_t)ALPHA_STEPS; step++) {
			uint32_t alpha = step * 255 / (ALPHA_STEPS - 1);
			uint32_t color;
			int i;

			color = premult_color(alpha, 0, 255 - alpha, 255);
			for (i = 0; i < BLOCK_WIDTH; i++)
				*row++ = color;
		}
	}
}

/*
 * Test that alpha blending is correct when an output ICC profile is installed.
 *
 * The background is a constant color. On top of that, there is an
 * alpha-blended gradient with ramps in both alpha and color. Sub-surface
 * ensures the correct positioning and stacking.
 *
 * The gradient consists of ALPHA_STEPS number of blocks. Block size is
 * BLOCK_WIDTH x BLOCK_WIDTH and a block has a uniform color.
 *
 * In the blending result over x axis:
 * - red goes from 1.0 to 0.0, monotonic
 * - green is not monotonic
 * - blue goes from 0.0 to 1.0, monotonic
 *
 * The test has sRGB encoded input pixels (non-linear). These are converted to
 * linear light (optical) values in output color space, blended, and converted
 * to non-linear (electrical) values according to the output ICC profile.
 *
 * Specifically, this test exercises the linearization of output ICC profiles,
 * retrieve_eotf_and_output_inv_eotf().
 */
TEST(output_icc_alpha_blend)
{
	const int width = BLOCK_WIDTH * ALPHA_STEPS;
	const int height = BLOCK_WIDTH;
	const pixman_color_t background_color = {
		.red   = 0xffff,
		.green = 0x8080,
		.blue  = 0x0000,
		.alpha = 0xffff
	};
	int seq_no = get_test_fixture_index();
	const struct setup_args *arg = &my_setup_args[seq_no];
	struct client *client;
	struct buffer *bg;
	struct buffer *fg;
	struct wl_subcompositor *subco;
	struct wl_surface *surf;
	struct wl_subsurface *sub;
	struct buffer *shot;
	bool match;

	client = create_client();
	subco = bind_to_singleton_global(client, &wl_subcompositor_interface, 1);

	/* background window content */
	bg = create_shm_buffer_a8r8g8b8(client, width, height);
	fill_image_with_color(bg->image, &background_color);

	/* background window, main surface */
	client->surface = create_test_surface(client);
	client->surface->width = width;
	client->surface->height = height;
	client->surface->buffer = bg; /* pass ownership */
	surface_set_opaque_rect(client->surface,
				&(struct rectangle){ 0, 0, width, height });

	/* foreground blended content */
	fg = create_shm_buffer_a8r8g8b8(client, width, height);
	fill_alpha_pattern(fg);

	/* foreground window, sub-surface */
	surf = wl_compositor_create_surface(client->wl_compositor);
	sub = wl_subcompositor_get_subsurface(subco, surf, client->surface->wl_surface);
	/* sub-surface defaults to position 0, 0, top-most, synchronized */
	wl_surface_attach(surf, fg->proxy, 0, 0);
	wl_surface_damage(surf, 0, 0, width, height);
	wl_surface_commit(surf);

	/* attach, damage, commit background window */
	move_client(client, 0, 0);

	shot = capture_screenshot_of_output(client, NULL);
	test_assert_ptr_not_null(shot);
	match = verify_image(shot->image, "output_icc_alpha_blend", arg->ref_image_index,
			     NULL, seq_no);
	test_assert_true(check_blend_pattern(bg, fg, shot, arg));
	test_assert_true(match);

	buffer_destroy(shot);

	wl_subsurface_destroy(sub);
	wl_surface_destroy(surf);
	buffer_destroy(fg);
	wl_subcompositor_destroy(subco);
	client_destroy(client); /* destroys bg */

	return RESULT_OK;
}

/*
 * Test that output decorations have the expected colors.
 *
 * This is the only way to test input-to-output category of color
 * transformations. They are used only for output decorations and some other
 * debug-like features. The input color space is hardcoded to sRGB in the
 * compositor.
 *
 * Because the output decorations are drawn with Cairo, we do not have an
 * easy access to the ground-truth image and so do not check the results
 * against a reference formula.
 */
TEST(output_icc_decorations)
{
	int seq_no = get_test_fixture_index();
	const struct setup_args *arg = &my_setup_args[seq_no];
	struct client *client;
	struct buffer *shot;
	pixman_image_t *img;
	bool match;

	client = create_client();

	shot = client_capture_output(client, client->output,
				     WESTON_CAPTURE_V1_SOURCE_FULL_FRAMEBUFFER);
	img = image_convert_to_a8r8g8b8(shot->image);

	match = verify_image(img, "output-icc-decorations",
			     arg->ref_image_index, NULL, seq_no);
	test_assert_true(match);

	pixman_image_unref(img);
	buffer_destroy(shot);
	client_destroy(client);

	return RESULT_OK;
}
