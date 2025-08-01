/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2017, 2018 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
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

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libweston/libweston.h>
#include <libweston/backend-drm.h>
#include <libweston/pixel-formats.h>

#include "drm-internal.h"

#include "color.h"
#include "linux-dmabuf.h"
#include "presentation-time-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "shared/string-helpers.h"
#include "shared/weston-assert.h"

enum drm_output_propose_state_mode {
	DRM_OUTPUT_PROPOSE_STATE_MIXED, /**< mix renderer & planes */
	DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR, /**< only assign to renderer & cursor plane */
	DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY, /**< only assign to renderer */
	DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY, /**< no renderer use, only planes */
};

static const char *const drm_output_propose_state_mode_as_string[] = {
	[DRM_OUTPUT_PROPOSE_STATE_MIXED] = "mixed state",
	[DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR] = "renderer-and-cursor state",
	[DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY] = "renderer-only state",
	[DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY]	= "plane-only state"
};

static const char *
drm_propose_state_mode_to_string(enum drm_output_propose_state_mode mode)
{
	if (mode < 0 || mode >= ARRAY_LENGTH(drm_output_propose_state_mode_as_string))
		return " unknown compositing mode";

	return drm_output_propose_state_mode_as_string[mode];
}

static bool
drm_mixed_mode_check_underlay(enum drm_output_propose_state_mode mode,
                              struct drm_plane_state *scanout_state,
                              uint64_t zpos)
{
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		assert(scanout_state != NULL);
		if (scanout_state->zpos >= zpos)
			return true;
	}

	return false;
}

static bool
drm_output_check_plane_has_view_assigned(struct drm_plane *plane,
                                         struct drm_output_state *output_state)
{
	struct drm_plane_state *ps;
	wl_list_for_each(ps, &output_state->plane_list, link) {
		if (ps->plane == plane && ps->fb)
			return true;
	}
	return false;
}

static struct drm_plane_state *
drm_output_try_paint_node_on_plane(struct drm_plane *plane,
				   struct drm_output_state *output_state,
				   struct weston_paint_node *node,
				   enum drm_output_propose_state_mode mode,
				   struct drm_fb *fb, uint64_t zpos)
{
	struct drm_output *output = output_state->output;
	struct weston_view *ev = node->view;
	struct weston_surface *surface = ev->surface;
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct drm_plane_state *state = NULL;

	assert(!device->sprites_are_broken);
	assert(device->atomic_modeset);
	assert(fb);
	assert(mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY ||
	       (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED &&
	        plane->type == WDRM_PLANE_TYPE_OVERLAY));

	state = drm_output_state_get_plane(output_state, plane);
	/* we can't have a 'pending' framebuffer as never set one before reaching here */
	assert(!state->fb);
	state->output = output;

	if (!drm_plane_state_coords_for_paint_node(state, node, zpos)) {
		drm_debug(b, "\t\t\t\t[view] not placing view %p on plane: "
			     "unsuitable transform\n", ev);
		goto out;
	}

	/* Should've been ensured by weston_view_matches_entire_output. */
	if (plane->type == WDRM_PLANE_TYPE_PRIMARY) {
		assert(state->dest_x == 0 && state->dest_y == 0 &&
		       state->dest_w == (unsigned) output->base.current_mode->width &&
		       state->dest_h == (unsigned) output->base.current_mode->height);
	}

	/* We hold one reference for the lifetime of this function; from
	 * calling drm_fb_get_from_paint_node() in
	 * drm_output_prepare_plane_view(), so, we take another reference
	 * here to live within the state. */
	state->ev = ev;
	state->fb = drm_fb_ref(fb);
	state->in_fence_fd = ev->surface->acquire_fence_fd;

	if (fb->format && fb->format->color_model == COLOR_MODEL_YUV) {
		enum wdrm_plane_color_encoding color_encoding;
		enum wdrm_plane_color_range color_range;

		/* These values will become dynamic once we implement the
		 * color-representation protocol. */
		color_encoding = WDRM_PLANE_COLOR_ENCODING_DEFAULT;
		color_range = WDRM_PLANE_COLOR_RANGE_DEFAULT;

		if (plane->props[WDRM_PLANE_COLOR_ENCODING].prop_id == 0) {
			if (color_encoding != WDRM_PLANE_COLOR_ENCODING_DEFAULT) {
				drm_debug(b, "\t\t\t[view] not placing view %p on plane %lu: "
					  "non-default color encoding not supported\n",
					  ev, (unsigned long) plane->plane_id);
				goto out;
			}
		} else if (!drm_plane_supports_color_encoding(plane,
							      color_encoding)) {
			drm_debug(b, "\t\t\t[view] not placing view %p on plane %lu: "
				  "color encoding not supported\n", ev,
				  (unsigned long) plane->plane_id);
			goto out;
		}

		if (plane->props[WDRM_PLANE_COLOR_RANGE].prop_id == 0) {
			if (color_range != WDRM_PLANE_COLOR_RANGE_DEFAULT) {
				drm_debug(b, "\t\t\t[view] not placing view %p on plane %lu: "
					  "non-default color range not supported\n",
					  ev, (unsigned long) plane->plane_id);
				goto out;
			}
		} else if (!drm_plane_supports_color_range(plane,
							   color_range)) {
			drm_debug(b, "\t\t\t[view] not placing view %p on plane %lu: "
				  "color range not supported\n", ev,
				  (unsigned long) plane->plane_id);
			goto out;
		}

		state->color_encoding = color_encoding;
		state->color_range = color_range;
	}

	/* In planes-only mode, we don't have an incremental state to
	 * test against, so we just hope it'll work. */
	if (mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY &&
	    drm_pending_state_test(output_state->pending_state) != 0) {
		drm_debug(b, "\t\t\t[view] not placing view %p on plane %lu: "
		             "atomic test failed\n",
			  ev, (unsigned long) plane->plane_id);
		goto out;
	}

	drm_debug(b, "\t\t\t[view] provisionally placing view %p on plane %lu\n",
		  ev, (unsigned long) plane->plane_id);

	/* Take a reference on the buffer so that we don't release it
	 * back to the client until we're done with it; cursor buffers
	 * don't require a reference since we copy them. */
	assert(state->fb_ref.buffer.buffer == NULL);
	assert(state->fb_ref.release.buffer_release == NULL);
	weston_buffer_reference(&state->fb_ref.buffer,
				surface->buffer_ref.buffer,
				BUFFER_MAY_BE_ACCESSED);
	weston_buffer_release_reference(&state->fb_ref.release,
					surface->buffer_release_ref.buffer_release);

	return state;

out:
	drm_plane_state_put_back(state);
	return NULL;
}

#ifdef BUILD_DRM_GBM
static struct drm_plane_state *
drm_output_prepare_cursor_paint_node(struct drm_output_state *output_state,
				     struct weston_paint_node *node,
				     uint64_t zpos)
{
	struct drm_output *output = output_state->output;
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct drm_plane *plane = output->cursor_plane;
	struct weston_view *ev = node->view;
	struct drm_plane_state *plane_state;
	const char *p_name = drm_output_get_plane_type_name(plane);

	assert(!device->cursors_are_broken);
	assert(plane);
	assert(plane->state_cur->complete);
	assert(!plane->state_cur->output || plane->state_cur->output == output);

	/* We use GBM to import SHM buffers. */
	assert(b->gbm);

	plane_state = drm_output_state_get_plane(output_state, plane);
	assert(!plane_state->fb);

	/* We can't scale with the legacy API, and we don't try to account for
	 * simple cropping/translation in cursor_bo_update. */
	plane_state->output = output;
	if (!drm_plane_state_coords_for_paint_node(plane_state, node, zpos)) {
		drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			     "unsuitable transform\n", p_name, ev, p_name);
		goto err;
	}

	if (plane_state->src_x != 0 || plane_state->src_y != 0 ||
	    plane_state->src_w > (unsigned) device->cursor_width << 16 ||
	    plane_state->src_h > (unsigned) device->cursor_height << 16 ||
	    plane_state->src_w != plane_state->dest_w << 16 ||
	    plane_state->src_h != plane_state->dest_h << 16) {
		drm_debug(b, "\t\t\t\t[%s] not assigning view %p to %s plane "
			     "(positioning requires cropping or scaling)\n",
			     p_name, ev, p_name);
		goto err;
	}

	drm_output_set_cursor_view(output, ev);
	plane_state->ev = ev;
	/* We always test with cursor fb 0. There are two potential fbs, and
	 * they are identically allocated for cursor use specifically, so if
	 * one works the other almost certainly should as well.
	 *
	 * Later when we determine if the cursor needs an update, we'll
	 * select the correct fb to use.
	 */
	plane_state->fb = drm_fb_ref(output->gbm_cursor_fb[0]);

	/* The cursor API is somewhat special: in cursor_bo_update(), we upload
	 * a buffer which is always cursor_width x cursor_height, even if the
	 * surface we want to promote is actually smaller than this. Manually
	 * mangle the plane state to deal with this. */
	plane_state->src_w = device->cursor_width << 16;
	plane_state->src_h = device->cursor_height << 16;
	plane_state->dest_w = device->cursor_width;
	plane_state->dest_h = device->cursor_height;

	drm_debug(b, "\t\t\t\t[%s] provisionally assigned view %p to cursor\n",
		  p_name, ev);

	return plane_state;

err:
	drm_plane_state_put_back(plane_state);
	return NULL;
}
#else
static struct drm_plane_state *
drm_output_prepare_cursor_paint_node(struct drm_output_state *output_state,
				     struct weston_paint_node *node,
				     uint64_t zpos)
{
	return NULL;
}
#endif

static void
drm_output_check_zpos_plane_states(struct drm_output_state *state)
{
	struct drm_plane_state *ps;

	wl_list_for_each(ps, &state->plane_list, link) {
		struct wl_list *next_node = ps->link.next;
		bool found_dup = false;

		/* skip any plane that is not enabled */
		if (!ps->fb)
			continue;

		assert(ps->zpos != DRM_PLANE_ZPOS_INVALID_PLANE);

		/* find another plane with the same zpos value */
		if (next_node == &state->plane_list)
			break;

		while (next_node && next_node != &state->plane_list) {
			struct drm_plane_state *ps_next;

			ps_next = container_of(next_node,
					       struct drm_plane_state,
					       link);

			if (ps->zpos == ps_next->zpos) {
				found_dup = true;
				break;
			}
			next_node = next_node->next;
		}

		/* this should never happen so exit hard in case
		 * we screwed up that bad */
		assert(!found_dup);
	}
}

static const char *
action_needed_to_str(enum actions_needed_dmabuf_feedback action_needed)
{
      switch(action_needed) {
      case ACTION_NEEDED_ADD_SCANOUT_TRANCHE:
              return "add scanout tranche";
      case ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE:
              return "remove scanout tranche";
      case ACTION_NEEDED_NONE:
              return "no action needed";
      default:
              assert(0);
      }
}

static void
dmabuf_feedback_maybe_update(struct drm_device *device, struct weston_view *ev,
			     uint32_t try_view_on_plane_failure_reasons)
{
	struct weston_dmabuf_feedback *dmabuf_feedback = ev->surface->dmabuf_feedback;
	struct weston_dmabuf_feedback_tranche *scanout_tranche;
	struct drm_backend *b = device->backend;
	dev_t scanout_dev = device->drm.devnum;
	uint32_t scanout_flags = ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT;
	enum actions_needed_dmabuf_feedback action_needed = ACTION_NEEDED_NONE;
	struct timespec current_time, delta_time;
	const time_t MAX_TIME_SECONDS = 2;

	/* Look for scanout tranche. If not found, add it but in disabled mode
	 * (we still don't know if we'll have to send it to clients). This
	 * simplifies the code. */
	scanout_tranche =
		weston_dmabuf_feedback_find_tranche(dmabuf_feedback, scanout_dev,
						    scanout_flags, SCANOUT_PREF);
	if (!scanout_tranche) {
		scanout_tranche =
			weston_dmabuf_feedback_tranche_create(dmabuf_feedback,
					b->compositor->dmabuf_feedback_format_table,
					scanout_dev, scanout_flags, SCANOUT_PREF);
		scanout_tranche->active = false;
	}

	/* Direct scanout won't happen even if client re-allocates using
	 * params from the scanout tranche, so keep only the renderer tranche. */
	if (try_view_on_plane_failure_reasons & (FAILURE_REASONS_FORCE_RENDERER |
						 FAILURE_REASONS_NO_PLANES_AVAILABLE |
						 FAILURE_REASONS_INADEQUATE_CONTENT_PROTECTION)) {
		action_needed = ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE;
	/* Direct scanout may be possible if client re-allocates using the
	 * params from the scanout tranche. */
	} else if (try_view_on_plane_failure_reasons & (FAILURE_REASONS_ADD_FB_FAILED |
							FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE |
							FAILURE_REASONS_DMABUF_MODIFIER_INVALID |
							FAILURE_REASONS_GBM_BO_IMPORT_FAILED |
							FAILURE_REASONS_GBM_BO_GET_HANDLE_FAILED)) {
		action_needed = ACTION_NEEDED_ADD_SCANOUT_TRANCHE;
	/* Direct scanout is already possible, so include the scanout tranche. */
	} else if (try_view_on_plane_failure_reasons == FAILURE_REASONS_NONE) {
		action_needed = ACTION_NEEDED_ADD_SCANOUT_TRANCHE;
	}

	/* No actions needed, so disarm timer and return */
	if (action_needed == ACTION_NEEDED_NONE ||
	    (action_needed == ACTION_NEEDED_ADD_SCANOUT_TRANCHE && scanout_tranche->active) ||
	    (action_needed == ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE && !scanout_tranche->active)) {
		dmabuf_feedback->action_needed = ACTION_NEEDED_NONE;
		return;
	}

	/* We hit this if:
	 *
	 * 1. timer is still off, or
	 * 2. the action needed when it was set to on does not match the most
	 *    recent needed action we've detected.
	 *
	 * So we reset the timestamp, set the timer to on it with the most
	 * recent needed action, return and leave the timer running. */
	if (dmabuf_feedback->action_needed == ACTION_NEEDED_NONE ||
	    dmabuf_feedback->action_needed != action_needed) {
		clock_gettime(CLOCK_MONOTONIC, &dmabuf_feedback->timer);
		dmabuf_feedback->action_needed = action_needed;
		return;
	/* Timer is already on and the action needed when it was set to on does
	 * not conflict with the most recent needed action we've detected. If
	 * more than MAX_TIME_SECONDS has passed, we need to resend the dma-buf
	 * feedback. Otherwise, return and leave the timer running. */
	} else {
		clock_gettime(CLOCK_MONOTONIC, &current_time);
		delta_time.tv_sec = current_time.tv_sec -
				    dmabuf_feedback->timer.tv_sec;
		if (delta_time.tv_sec < MAX_TIME_SECONDS)
			return;
	}

	/* If we got here it means that the timer has triggered, so we have
	 * pending actions with the dma-buf feedback. So we update and resend
	 * them. */
	if (action_needed == ACTION_NEEDED_ADD_SCANOUT_TRANCHE)
		scanout_tranche->active = true;
	else if (action_needed == ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE)
		scanout_tranche->active = false;
	else
		assert(0);

	drm_debug(b, "\t[repaint] Need to update and resend the "
		     "dma-buf feedback for surface of view %p: %s\n",
		     ev, action_needed_to_str(action_needed));
	weston_dmabuf_feedback_send_all(b->compositor, dmabuf_feedback,
					b->compositor->dmabuf_feedback_format_table);

	/* Set the timer to off */
	dmabuf_feedback->action_needed = ACTION_NEEDED_NONE;
}

static const char *
failure_reasons_to_str(enum try_view_on_plane_failure_reasons failure_reasons)
{
	switch(failure_reasons) {
	case FAILURE_REASONS_NONE:			    return "none";
	case FAILURE_REASONS_FORCE_RENDERER:		    return "force renderer";
	case FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE:	    return "fb format incompatible";
	case FAILURE_REASONS_DMABUF_MODIFIER_INVALID:	    return "dmabuf modifier invalid";
	case FAILURE_REASONS_ADD_FB_FAILED:		    return "add fb failed";
	case FAILURE_REASONS_NO_PLANES_AVAILABLE:	    return "no planes available";
	case FAILURE_REASONS_PLANES_REJECTED:		    return "planes rejected";
	case FAILURE_REASONS_INADEQUATE_CONTENT_PROTECTION: return "inadequate content protection";
	case FAILURE_REASONS_INCOMPATIBLE_TRANSFORM:	    return "incompatible transform";
	case FAILURE_REASONS_NO_BUFFER:			    return "no buffer";
	case FAILURE_REASONS_BUFFER_TOO_BIG:		    return "buffer too big";
	case FAILURE_REASONS_BUFFER_TYPE:		    return "buffer type";
	case FAILURE_REASONS_GLOBAL_ALPHA:		    return "global alpha";
	case FAILURE_REASONS_NO_GBM:			    return "no gbm";
	case FAILURE_REASONS_GBM_BO_IMPORT_FAILED:	    return "gbm bo import failed";
	case FAILURE_REASONS_GBM_BO_GET_HANDLE_FAILED:	    return "gbm bo get handle failed";
	}
	return "???";
}

static struct drm_plane_state *
drm_output_find_plane_for_view(struct drm_output_state *state,
			       struct weston_paint_node *pnode,
			       enum drm_output_propose_state_mode mode,
			       struct drm_plane_state *scanout_state,
			       uint64_t current_lowest_zpos_overlay,
			       uint64_t current_lowest_zpos_underlay,
			       bool need_underlay)
{
	struct drm_output *output = state->output;
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;

	struct drm_plane_state *ps = NULL;
	struct drm_plane *plane;

	struct weston_view *ev = pnode->view;
	struct weston_buffer *buffer;
	struct drm_fb *fb = NULL;
	uint64_t current_lowest_zpos = need_underlay ?
	                               current_lowest_zpos_underlay :
	                               current_lowest_zpos_overlay;

	bool view_matches_entire_output, scanout_has_view_assigned;
	uint32_t possible_plane_mask = 0;
	uint32_t fb_failure_reasons = 0;
	bool any_candidate_picked = false;

	pnode->try_view_on_plane_failure_reasons = FAILURE_REASONS_NONE;

	/* renderer-only mode, so no view assignments to planes */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_FORCE_RENDERER;
		return NULL;
	}

	/* filter out non-cursor views in renderer-and-cursor mode */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR &&
	    ev->layer_link.layer != &b->compositor->cursor_layer) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_FORCE_RENDERER;
		return NULL;
	}

	/* check view for valid buffer, doesn't make sense to even try */
	if (!weston_view_has_valid_buffer(ev)) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_NO_BUFFER;
		return NULL;
	}

	buffer = ev->surface->buffer_ref.buffer;
	if (buffer->type == WESTON_BUFFER_SOLID) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_BUFFER_TYPE;
	} else if (buffer->type == WESTON_BUFFER_SHM) {
		if (!output->cursor_plane || device->cursors_are_broken)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_BUFFER_TYPE;

		/* Even though this is a SHM buffer, pixel_format stores
		 * the format code as DRM FourCC */
		if (buffer->pixel_format->format != DRM_FORMAT_ARGB8888) {
			drm_debug(b, "\t\t\t\t[view] not placing view %p on "
				     "plane; SHM buffers must be ARGB8888 for "
				     "cursor view\n", ev);
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE;
		}

		if (buffer->width > device->cursor_width ||
		    buffer->height > device->cursor_height) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
				     "(buffer (%dx%d) too large for cursor plane)\n",
				     ev, buffer->width, buffer->height);
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_BUFFER_TOO_BIG;
		}

		if (pnode->try_view_on_plane_failure_reasons == FAILURE_REASONS_NONE)
			possible_plane_mask = (1 << output->cursor_plane->plane_idx);
	} else {
		if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p "
				     "to plane: renderer-and-cursor mode\n", ev);
			return NULL;
		}

		wl_list_for_each(plane, &device->plane_list, link) {
			if (plane->type == WDRM_PLANE_TYPE_CURSOR)
				continue;

			if (drm_paint_node_transform_supported(pnode, plane))
				possible_plane_mask |= 1 << plane->plane_idx;
		}

		if (!possible_plane_mask)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_INCOMPATIBLE_TRANSFORM;

		fb = drm_fb_get_from_paint_node(state, pnode, &fb_failure_reasons);
		if (fb) {
			possible_plane_mask &= fb->plane_mask;
		} else {
			char *fr_str = bits_to_str(fb_failure_reasons, failure_reasons_to_str);
			weston_assert_ptr_not_null(b->compositor, fr_str);
			drm_debug(b, "\t\t\t[view] couldn't get FB for view: %s\n", fr_str);
			free(fr_str);
			pnode->try_view_on_plane_failure_reasons |= fb_failure_reasons;
		}
	}

	view_matches_entire_output =
		weston_view_matches_output_entirely(ev, &output->base);
	scanout_has_view_assigned =
		drm_output_check_plane_has_view_assigned(output->scanout_plane,
							 state);

	/* assemble a list with possible candidates */
	wl_list_for_each(plane, &device->plane_list, link) {
		const char *p_name = drm_output_get_plane_type_name(plane);
		uint64_t zpos;
		bool mm_underlay_only = false;

		if (possible_plane_mask == 0)
			break;

		if (!(possible_plane_mask & (1 << plane->plane_idx)))
			continue;

		possible_plane_mask &= ~(1 << plane->plane_idx);
		mm_underlay_only =
			drm_mixed_mode_check_underlay(mode, scanout_state, plane->zpos_max);

		switch (plane->type) {
		case WDRM_PLANE_TYPE_CURSOR:
			assert(buffer->shm_buffer);
			assert(plane == output->cursor_plane);
			break;
		case WDRM_PLANE_TYPE_PRIMARY:
			if (plane != output->scanout_plane)
				continue;
			if (mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY)
				continue;
			if (!view_matches_entire_output)
				continue;
			break;
		case WDRM_PLANE_TYPE_OVERLAY:
			assert(mode != DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR);
			/* if the view covers the whole output, put it in the
			 * scanout plane, not overlay */
			if (view_matches_entire_output &&
			    weston_view_is_opaque(ev, &ev->transform.boundingbox) &&
			    !scanout_has_view_assigned)
				continue;
			/* for alpha views, avoid placing them on the HW planes that
			 * are below the primary plane. */
			if (mm_underlay_only && !weston_view_is_opaque(ev, &ev->transform.boundingbox))
				continue;
			break;
		default:
			assert(false && "unknown plane type");
		}

		if (!drm_plane_is_available(plane, output))
			continue;

		if (drm_output_check_plane_has_view_assigned(plane, state)) {
			drm_debug(b, "\t\t\t\t[plane] not trying plane %d: "
				     "another view already assigned\n",
				     plane->plane_id);
			continue;
		}

		/* if view has alpha check if this plane supports plane alpha */
		if (ev->alpha != 1.0f && plane->alpha_max == plane->alpha_min) {
			drm_debug(b, "\t\t\t\t[plane] not trying plane %d:"
				     "plane-alpha not supported\n",
				     plane->plane_id);
			continue;
		}

		/* Pre-judge whether the plane will be set as underlay plane. If so, start
		 * trying to find underlay plane based on 'current_lowest_zpos_underlay'. */
		if (!need_underlay) {
			uint64_t tmp_next_lowest_zpos;
			if (current_lowest_zpos == DRM_PLANE_ZPOS_INVALID_PLANE)
				tmp_next_lowest_zpos = plane->zpos_max;
			else
				tmp_next_lowest_zpos = current_lowest_zpos - 1;
			if (drm_mixed_mode_check_underlay(mode, scanout_state, tmp_next_lowest_zpos)) {
				drm_debug(b, "\t\t\t\t[plane] could not use overlay planes, "
				             "attempting to find underlay plane\n");
				current_lowest_zpos = current_lowest_zpos_underlay;
			}
		}

		if (plane->zpos_min >= current_lowest_zpos) {
			drm_debug(b, "\t\t\t\t[plane] not trying plane %d: "
				     "plane's minimum zpos (%"PRIu64") above "
				     "current lowest zpos (%"PRIu64")\n",
				     plane->plane_id, plane->zpos_min,
				     current_lowest_zpos);
			continue;
		}

		/* If the surface buffer has an in-fence fd, but the plane doesn't
		 * support fences, we can't place the buffer on this plane. */
		if (ev->surface->acquire_fence_fd >= 0 &&
		    plane->props[WDRM_PLANE_IN_FENCE_FD].prop_id == 0) {
			drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			          "no in-fence support\n", p_name, ev, p_name);
			continue;
		}

		if (!b->has_underlay && mm_underlay_only) {
			drm_debug(b, "\t\t\t\t[plane] not adding plane %d to "
				     "candidate list: plane is below the primary "
				     "plane and backend format (%s) is opaque, "
				     "hole on primary plane will not work\n",
				     plane->plane_id, b->format->drm_format_name);

			continue;
		}

		if (current_lowest_zpos == DRM_PLANE_ZPOS_INVALID_PLANE)
			zpos = plane->zpos_max;
		else
			zpos = MIN(current_lowest_zpos - 1, plane->zpos_max);

		any_candidate_picked = true;
		drm_debug(b, "\t\t\t\t[plane] plane %d picked "
			     "from candidate list, type: %s\n",
			     plane->plane_id, p_name);

		if (plane->type == WDRM_PLANE_TYPE_CURSOR) {
			ps = drm_output_prepare_cursor_paint_node(state, pnode, zpos);
		} else {
			if (fb)
				ps = drm_output_try_paint_node_on_plane(plane, state,
									pnode, mode,
									fb, zpos);
		}

		if (ps) {
			/* Check if this ps is underlay plane, if so, the view
			 * needs through hole on primary plane. */
			pnode->need_hole =
				drm_mixed_mode_check_underlay(mode,
							      scanout_state,
							      ps->zpos);

			drm_debug(b, "\t\t\t\t[view] view %p has been placed to "
				     "%s plane as an %s with computed zpos "
				     "%"PRIu64"\n",
				     ev, p_name,
				     pnode->need_hole ? "underlay" : "overlay",
				     zpos);
			break;
		}

		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_PLANES_REJECTED;
	}

	if (!any_candidate_picked)
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_NO_PLANES_AVAILABLE;

	/* if we have a plane state, it has its own ref to the fb; if not then
	 * we drop ours here */
	drm_fb_unref(fb);
	return ps;
}

static struct drm_output_state *
drm_output_propose_state(struct weston_output *output_base,
			 struct drm_pending_state *pending_state,
			 enum drm_output_propose_state_mode mode)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_paint_node *pnode;
	struct drm_output_state *state;
	struct drm_plane_state *scanout_state = NULL;

	pixman_region32_t renderer_region;
	pixman_region32_t occluded_region;

	bool renderer_ok = (mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY);
	int ret;
	/* Record the current lowest zpos of the overlay planes */
	uint64_t current_lowest_zpos_overlay = DRM_PLANE_ZPOS_INVALID_PLANE;
	/* Record the current lowest zpos of the underlay plane */
	uint64_t current_lowest_zpos_underlay = DRM_PLANE_ZPOS_INVALID_PLANE;

	assert(!output->state_last);
	state = drm_output_state_duplicate(output->state_cur,
					   pending_state,
					   DRM_OUTPUT_STATE_CLEAR_PLANES);
	state->dpms = WESTON_DPMS_ON;

	/* Start with the assumption that we're going to do a tearing commit,
	 * if the hardware supports it and we're not compositing with the
	 * renderer.
	 * As soon as anything in the scene graph wants to be presented without
	 * tearing, or a test fails, drop the tear flag. */
	state->tear = device->tearing_supported &&
		      mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY;

	/* We implement mixed mode by progressively creating and testing
	 * incremental states, of scanout + overlay + cursor. Since we
	 * walk our views top to bottom, the scanout plane is last, however
	 * we always need it in our scene for the test modeset to be
	 * meaningful. To do this, we steal a reference to the last
	 * renderer framebuffer we have, if we think it's basically
	 * compatible. If we don't have that, then we conservatively fall
	 * back to only using the renderer for this repaint. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		struct drm_plane *plane = output->scanout_plane;
		struct drm_fb *scanout_fb = plane->state_cur->fb;

		if (!scanout_fb ||
		    (scanout_fb->type != BUFFER_GBM_SURFACE &&
		     scanout_fb->type != BUFFER_PIXMAN_DUMB)) {
			drm_debug(b, "\t\t[state] cannot propose mixed mode: "
			             "for output %s (%lu): no previous renderer "
			             "fb\n",
				  output->base.name,
				  (unsigned long) output->base.id);
			drm_output_state_free(state);
			return NULL;
		}

		if (scanout_fb->width != output_base->current_mode->width ||
		    scanout_fb->height != output_base->current_mode->height) {
			drm_debug(b, "\t\t[state] cannot propose mixed mode "
			             "for output %s (%lu): previous fb has "
				     "different size\n",
				  output->base.name,
				  (unsigned long) output->base.id);
			drm_output_state_free(state);
			return NULL;
		}

		scanout_state = drm_plane_state_duplicate(state,
							  plane->state_cur);
		/* assign the primary the lowest zpos value */
		scanout_state->zpos = plane->zpos_min;
		/* Set the initial lowest zpos used for the underlay plane
		 * (asuming capable platform) to the that of the the primary
		 * plane, matching the lowest possible value. As we parse views
		 * from top to bottom we also need a start-up point for
		 * underlays, below this initial lowest zpos value. */
		current_lowest_zpos_underlay = scanout_state->zpos;
		drm_debug(b, "\t\t[state] using renderer FB ID %lu for mixed "
			     "mode for output %s (%lu)\n",
			  (unsigned long) scanout_fb->fb_id, output->base.name,
			  (unsigned long) output->base.id);
		drm_debug(b, "\t\t[state] scanout will use for zpos %"PRIu64"\n",
				scanout_state->zpos);
	}

	/* - renderer_region contains the total region which which will be
	 *   covered by the renderer and underlay region.
	 * - occluded_region contains the total region which which will be
	 *   covered by the renderer and hardware planes, where the view's
	 *   visible-and-opaque region is added in both cases (the view's
	 *   opaque region  accumulates there for each view); it is being used
	 *   to skip the view, if it is completely occluded; includes the
	 *   situation where occluded_region covers entire output's region.
	 */
	pixman_region32_init(&renderer_region);
	pixman_region32_init(&occluded_region);

	wl_list_for_each(pnode, &output->base.paint_node_z_order_list,
			 z_order_link) {
		struct weston_view *ev = pnode->view;
		struct drm_plane_state *ps = NULL;
		bool force_renderer = false;
		pixman_region32_t clipped_view;
		pixman_region32_t surface_overlap;
		bool totally_occluded = false;
		bool need_underlay = false;

		drm_debug(b, "\t\t\t[view] evaluating view %p for "
		             "output %s (%lu)\n",
		          ev, output->base.name,
			  (unsigned long) output->base.id);

		assert(ev->output_mask & (1u << output->base.id));

		/* Cannot show anything without a color transform. */
		if (!pnode->surf_xform_valid) {
			drm_debug(b, "\t\t\t\t[view] ignoring view %p "
			             "(color transform failed)\n", ev);
			continue;
		}

		/* Ignore views we know to be totally occluded. */
		pixman_region32_init(&clipped_view);
		pixman_region32_intersect(&clipped_view,
					  &ev->transform.boundingbox,
					  &output->base.region);

		pixman_region32_init(&surface_overlap);
		pixman_region32_subtract(&surface_overlap, &clipped_view,
					 &occluded_region);
		/* if the view is completely occluded then ignore that
		 * view; includes the case where occluded_region covers
		 * the entire output */
		totally_occluded = !pixman_region32_not_empty(&surface_overlap);
		if (totally_occluded) {
			drm_debug(b, "\t\t\t\t[view] ignoring view %p "
			             "(occluded on our output)\n", ev);
			pixman_region32_fini(&surface_overlap);
			pixman_region32_fini(&clipped_view);
			continue;
		}

		if (!b->gbm) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(GBM not available)\n", ev);
			force_renderer = true;
		}

		if (!weston_view_has_valid_buffer(ev)) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(no buffer available)\n", ev);
			force_renderer = true;
		}

		/* We can support this with the 'CRTC background colour' property,
		 * if it is fullscreen (i.e. we disable the primary plane), and
		 * opaque (as it is only shown in the absence of any covering
		 * plane, not as a replacement for the primary plane per se). */
		if (ev->surface->buffer_ref.buffer &&
		    ev->surface->buffer_ref.buffer->type == WESTON_BUFFER_SOLID) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(solid-colour surface)\n", ev);
			force_renderer = true;
		}

		if (pnode->surf_xform.transform != NULL ||
		    !pnode->surf_xform.identity_pipeline) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(requires color transform)\n", ev);
			force_renderer = true;
		}

		/* Since we process views from top to bottom, we know that if
		 * the view intersects the calculated renderer region, it must
		 * be part of, or occluded by, it, and cannot go on an overlay
		 * plane. */
		pixman_region32_intersect(&surface_overlap, &renderer_region,
					  &clipped_view);
		if (pixman_region32_not_empty(&surface_overlap)) {
			if (b->has_underlay) {
				need_underlay = true;
			} else {
				force_renderer = true;
				drm_debug(b, "\t\t\t\t[view] not assigning view %p to a "
					     "plane (occluded by renderer views), current lowest "
					     "zpos change to %"PRIu64"\n", ev,
					     current_lowest_zpos_underlay);
			}
		}
		pixman_region32_fini(&surface_overlap);

		/* If need_underlay, but view contains alpha, then it needs to
		 * be rendered. Only fully-opaque views can go on an underlay.
		 */
		if (need_underlay &&
		    !weston_view_is_opaque(ev, &ev->transform.boundingbox)) {
			force_renderer = true;
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to "
				     "a plane (alpha view occluded by renderer "
				     "views)", ev);
		}

		/* In case of enforced mode of content-protection do not
		 * assign planes for a protected surface on an unsecured output.
		 */
		if (ev->surface->protection_mode == WESTON_SURFACE_PROTECTION_MODE_ENFORCED &&
		    ev->surface->desired_protection > output_base->current_protection) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
				     "(enforced protection mode on unsecured output)\n", ev);
			force_renderer = true;
		}

		if (pnode->view->surface->tear_control)
			state->tear &= pnode->view->surface->tear_control->may_tear;
		else
			state->tear = 0;

		/* Now try to place it on a plane if we can. */
		if (!force_renderer) {
			drm_debug(b, "\t\t\t[plane] started with zpos %"PRIu64"\n",
				      need_underlay ? current_lowest_zpos_underlay :
				      current_lowest_zpos_overlay);
			ps = drm_output_find_plane_for_view(state, pnode, mode,
							    scanout_state,
							    current_lowest_zpos_overlay,
							    current_lowest_zpos_underlay,
							    need_underlay);
		} else {
			/* We are forced to place the view in the renderer, set
			 * the failure reason accordingly. */
			pnode->try_view_on_plane_failure_reasons =
				FAILURE_REASONS_FORCE_RENDERER;
		}

		if (ps) {
			if (drm_mixed_mode_check_underlay(mode, scanout_state, ps->zpos))
				current_lowest_zpos_underlay = ps->zpos;
			else
				current_lowest_zpos_overlay = ps->zpos;
			drm_debug(b, "\t\t\t[plane] next overlay zpos to use %"PRIu64","
			             " next underlay zpos to use %"PRIu64"\n",
			             current_lowest_zpos_overlay,
			             current_lowest_zpos_underlay);
		} else if (!ps && !renderer_ok) {
			drm_debug(b, "\t\t[view] failing state generation: "
				      "placing view %p to renderer not allowed\n",
				  ev);
			pixman_region32_fini(&clipped_view);
			goto err_region;
		} else if (!ps) {
			char *fr_str = bits_to_str(pnode->try_view_on_plane_failure_reasons,
						   failure_reasons_to_str);
			weston_assert_ptr_not_null(b->compositor, fr_str);
			drm_debug(b, "\t\t\t\t[view] view %p will be placed "
				     "on the renderer: %s\n", ev, fr_str);
			free(fr_str);
		}

		if (!ps || drm_mixed_mode_check_underlay(mode, scanout_state, ps->zpos)) {
			/* clipped_view contains the area that's going to be
			 * visible on screen; add this to the renderer region */
			pixman_region32_union(&renderer_region,
					      &renderer_region,
					      &clipped_view);
		}

		/* Opaque areas of our clipped view occlude areas behind it;
		 * however, anything not in the opaque region (which is the
		 * entire clipped area if the whole view is known to be
		 * opaque) does not necessarily occlude what's behind it, as
		 * it could be alpha-blended. */
		if (!weston_view_is_opaque(ev, &clipped_view))
			pixman_region32_intersect(&clipped_view,
						  &clipped_view,
						  &ev->transform.opaque);
		pixman_region32_union(&occluded_region,
				      &occluded_region,
				      &clipped_view);

		pixman_region32_fini(&clipped_view);
	}

	pixman_region32_fini(&renderer_region);
	pixman_region32_fini(&occluded_region);

	/* In renderer-only and renderer-and-cursor modes, we can't test the
	 * state as we don't have a renderer buffer yet. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY ||
	    mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR)
		return state;

	/* check if we have invalid zpos values, like duplicate(s) */
	drm_output_check_zpos_plane_states(state);

	/* Check to see if this state will actually work. */
	ret = drm_pending_state_test(state->pending_state);
	if (ret != 0) {
		drm_debug(b, "\t\t[view] failing state generation: "
			     "atomic test not OK\n");
		goto err;
	}

	/* Counterpart to duplicating scanout state at the top of this
	 * function: if we have taken a renderer framebuffer and placed it in
	 * the pending state in order to incrementally test overlay planes,
	 * remove it now. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		assert(scanout_state->fb->type == BUFFER_GBM_SURFACE ||
		       scanout_state->fb->type == BUFFER_PIXMAN_DUMB);
		drm_plane_state_put_back(scanout_state);
	}
	return state;

err_region:
	pixman_region32_fini(&renderer_region);
	pixman_region32_fini(&occluded_region);
err:
	drm_output_state_free(state);
	return NULL;
}

void
drm_assign_planes(struct weston_output *output_base)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct drm_pending_state *pending_state = device->repaint_data;
	struct drm_output_state *state = NULL;
	struct drm_plane_state *plane_state;
	struct drm_writeback_state *wb_state = output->wb_state;
	struct weston_paint_node *pnode;
	struct weston_plane *primary = &output_base->primary_plane;
	enum drm_output_propose_state_mode mode = DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY;

	assert(output);

	drm_debug(b, "\t[repaint] preparing state for output %s (%lu)\n",
		  output_base->name, (unsigned long) output_base->id);

	if (!device->sprites_are_broken && !output_base->disable_planes &&
	    !output->is_virtual && b->gbm) {
		drm_debug(b, "\t[repaint] trying planes-only build state\n");
		state = drm_output_propose_state(output_base, pending_state, mode);
		if (!state) {
			drm_debug(b, "\t[repaint] could not build planes-only "
				     "state, trying mixed\n");
			mode = DRM_OUTPUT_PROPOSE_STATE_MIXED;
			state = drm_output_propose_state(output_base,
							 pending_state,
							 mode);
		}
	} else {
		drm_debug(b, "\t[state] no overlay plane support\n");
	}

	/* We can enter this block in two situations:
	 * 1. If we didn't enter the last block (for some reason we can't use planes)
	 * 2. If we entered but both the planes-only and the mixed modes didn't work */
	if (!state) {
		if (output_base->disable_planes)
			mode = DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY;
		else
			mode = DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR;

		drm_debug(b, "\t[repaint] could not build state with planes, "
			     "trying %s\n",
			     (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY) ?
			     "renderer-only" : "renderer-and-cursor");

		state = drm_output_propose_state(output_base, pending_state,
						 mode);
		/* If renderer/renderer-and-cursor mode failed and we are in a
		 * writeback screenshot, let's abort the writeback screenshot
		 * and try again. */
		if (!state && drm_output_get_writeback_state(output) != DRM_OUTPUT_WB_SCREENSHOT_OFF) {
			drm_debug(b, "\t[repaint] could not build %s "
				     "state, trying without writeback setup\n",
				     (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY) ?
				     "renderer-only" : "renderer-and-cursor");
			drm_writeback_fail_screenshot(wb_state, "drm: failed to propose state");
			state = drm_output_propose_state(output_base, pending_state,
							 mode);
		}
	}

	assert(state);
	drm_debug(b, "\t[repaint] Using %s composition\n",
		  drm_propose_state_mode_to_string(mode));

	wl_list_for_each(pnode, &output->base.paint_node_z_order_list,
			 z_order_link) {
		struct weston_view *ev = pnode->view;
		struct drm_plane *target_plane = NULL;

		assert(ev->output_mask & (1u << output->base.id));

		/* Update dmabuf-feedback if needed */
		if (ev->surface->dmabuf_feedback)
			dmabuf_feedback_maybe_update(device, ev,
						     pnode->try_view_on_plane_failure_reasons);
		pnode->try_view_on_plane_failure_reasons = FAILURE_REASONS_NONE;

		/* Test whether this buffer can ever go into a plane:
		 * non-shm, or small enough to be a cursor.  */
		ev->surface->keep_buffer = false;
		if (weston_view_has_valid_buffer(ev)) {
			struct weston_buffer *buffer =
				ev->surface->buffer_ref.buffer;
			if (buffer->type == WESTON_BUFFER_DMABUF ||
			    buffer->type == WESTON_BUFFER_RENDERER_OPAQUE)
				ev->surface->keep_buffer = true;
			else if (buffer->type == WESTON_BUFFER_SHM &&
				 (ev->surface->width <= device->cursor_width &&
		       		  ev->surface->height <= device->cursor_height))
				ev->surface->keep_buffer = true;
		}

		/* This is a bit unpleasant, but lacking a temporary place to
		 * hang a plane off the view, we have to do a nested walk.
		 * Our first-order iteration has to be planes rather than
		 * views, because otherwise we won't reset views which were
		 * previously on planes to being on the primary plane. */
		wl_list_for_each(plane_state, &state->plane_list, link) {
			if (plane_state->ev == ev) {
				plane_state->ev = NULL;
				target_plane = plane_state->plane;
				break;
			}
		}

		if (target_plane) {
			drm_debug(b, "\t[repaint] view %p on %s plane %lu\n",
				  ev, drm_output_get_plane_type_name(target_plane),
				  (unsigned long) target_plane->plane_id);
			weston_paint_node_move_to_plane(pnode, &target_plane->base);
		} else {
			drm_debug(b, "\t[repaint] view %p using renderer "
				     "composition\n", ev);
			weston_paint_node_move_to_plane(pnode, primary);
			pnode->need_hole = false;
		}

		if (!target_plane ||
		    target_plane->type == WDRM_PLANE_TYPE_CURSOR) {
			/* cursor plane & renderer involve a copy */
			pnode->psf_flags = 0;
		} else {
			/* All other planes are a direct scanout of a
			 * single client buffer.
			 */
			pnode->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
		}
	}

	/* We rely on output->cursor_view being both an accurate reflection of
	 * the cursor plane's state, but also being maintained across repaints
	 * to avoid unnecessary damage uploads, per the comment in
	 * drm_output_prepare_cursor_paint_node. In the event that we go from
	 * having a cursor view to not having a cursor view, we need to clear
	 * it. */
	if (output->cursor_view) {
		plane_state =
			drm_output_state_get_existing_plane(state,
							    output->cursor_plane);
		if (!plane_state || !plane_state->fb)
			drm_output_set_cursor_view(output, NULL);
	}

	if (drm_output_get_writeback_state(output) == DRM_OUTPUT_WB_SCREENSHOT_PREPARE_COMMIT)
		drm_writeback_reference_planes(wb_state, &state->plane_list);
}

static void
drm_output_handle_cursor_view_destroy(struct wl_listener *listener, void *data)
{
	struct drm_output *output =
		container_of(listener, struct drm_output,
			     cursor_view_destroy_listener);

	drm_output_set_cursor_view(output, NULL);
}

/** Set the current cursor view used for an output.
 *
 * Ensure the stored value will be properly cleared if the view is destroyed.
 * The stored cursor view helps avoid unnecessary uploads of cursor data to
 * cursor plane buffer objects (see drm_output_prepare_cursor_paint_node).
 */
void
drm_output_set_cursor_view(struct drm_output *output, struct weston_view *ev)
{
	if (output->cursor_view == ev)
		return;

	if (output->cursor_view)
		wl_list_remove(&output->cursor_view_destroy_listener.link);

	output->cursor_view = ev;

	if (ev) {
		output->cursor_view_destroy_listener.notify =
			drm_output_handle_cursor_view_destroy;
		wl_signal_add(&ev->destroy_signal,
			      &output->cursor_view_destroy_listener);
	}
}
