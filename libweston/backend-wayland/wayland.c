/*
 * Copyright © 2010-2011 Benjamin Franzke
 * Copyright © 2013 Jason Ekstrand
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
#include "libweston/linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "renderer-gl/gl-renderer-internal.h"
#include "tearing-control-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client-core.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <xf86drm.h>

#ifdef ENABLE_EGL
#include "renderer-gl/gl-renderer.h"
#include <wayland-egl.h>
#endif

#include "pixman-renderer.h"
#include "presentation-time-server-protocol.h"
#include "renderer-gl/gl-renderer.h"
#include "shared/cairo-util.h"
#include "shared/helpers.h"
#include "shared/os-compatibility.h"
#include "shared/timespec-util.h"
#include "shared/weston-drm-fourcc.h"
#include "xdg-shell-client-protocol.h"
#include <libweston/backend-wayland.h>
#include <libweston/libweston.h>
#include <libweston/pixel-formats.h>
#include <libweston/windowed-output-api.h>

#define WINDOW_TITLE "Weston Compositor"

#define WINDOW_MIN_WIDTH 128
#define WINDOW_MIN_HEIGHT 128

#define WINDOW_MAX_WIDTH 8192
#define WINDOW_MAX_HEIGHT 8192

static const uint32_t wayland_formats[] = {
    DRM_FORMAT_XRGB8888,
};

struct passthrough_buffer_cache {
  struct weston_buffer *client_buffer;
  struct wl_buffer *host_buffer;
};

struct wayland_backend {
  struct weston_backend base;
  struct weston_compositor *compositor;

  struct {
    struct wl_display *wl_display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_shm *shm;
    struct wp_viewporter *viewporter;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct wp_tearing_control_manager_v1 *tearing_control_manager;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf;

    struct wp_presentation *presentation;
    clockid_t presentation_clock_id;
    bool presentation_clock_id_valid;

    struct wl_list output_list;

    struct wl_event_source *wl_source;
    uint32_t event_mask;
  } parent;

  bool sprawl_across_outputs;

  struct wl_cursor_theme *cursor_theme;
  struct wl_cursor *cursor;

  struct wl_list input_list;
  /* These struct wayland_input objects are waiting for the outer
   * compositor to provide a name and initial capabilities. */
  struct wl_list pending_input_list;

  const struct pixel_format_info **formats;
  unsigned int formats_count;
};

struct wayland_output {
  struct weston_output base;
  struct wayland_backend *backend;

  struct {
    struct wl_surface *surface;
    struct wp_viewport *viewport;

    struct wl_output *output;
    uint32_t global_id;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    int configure_width, configure_height;
    bool wait_for_configure;
  } parent;

  int keyboard_count;

  char *title;

  struct {
    struct wl_egl_window *egl_window;
  } gl;

  struct wp_tearing_control_v1 *tearing_control;
  uint32_t current_tearing_presentation_hint;

  bool output_using_dma;
  struct passthrough_buffer_cache passthrough_cache;

  struct weston_mode mode;
  struct weston_mode native_mode;

  struct wl_callback *frame_cb;
};

struct wayland_parent_output {
  struct wayland_backend *backend; /**< convenience */
  struct wayland_head *head;
  struct wl_list link;

  struct wl_output *global;
  uint32_t id;

  struct {
    char *make;
    char *model;
    int32_t width, height;
    uint32_t subpixel;
  } physical;

  int32_t x, y;
  uint32_t transform;
  uint32_t scale;

  struct wl_callback *sync_cb; /**< wl_output < 2 done replacement */

  struct wl_list mode_list;
  struct weston_mode *preferred_mode;
  struct weston_mode *current_mode;
};

struct wayland_head {
  struct weston_head base;
  struct wayland_parent_output *parent_output;
};

struct wayland_input {
  struct weston_seat base;
  struct wayland_backend *backend;
  struct wl_list link;

  struct {
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;
    struct zwp_locked_pointer_v1 *locked_pointer;
    struct zwp_relative_pointer_v1 *relative_pointer;

    struct {
      struct wl_surface *surface;
      int32_t hx, hy;
    } cursor;
  } parent;

  struct weston_touch_device *touch_device;

  enum weston_key_state_update keyboard_state_update;
  uint32_t key_serial;
  uint32_t enter_serial;
  uint32_t touch_points;
  bool touch_active;
  bool has_focus;
  int seat_version;

  struct wayland_output *output;
  struct wayland_output *touch_focus;
  struct wayland_output *keyboard_focus;

  struct weston_pointer_axis_event vert, horiz;

  struct {
    struct wl_buffer *host_cursor_buffer;
    struct weston_buffer *cached_weston_cursor_buffer;
    bool host_cursor_enabled;
  } passthrough_cursor;

  bool seat_initialized;
  struct wl_callback *initial_info_cb;
  char *name;
  enum wl_seat_capability caps;
};

static struct wl_buffer *create_shm_buffer_from_weston_buffer(
    struct wayland_backend *b, struct wl_resource *client_buffer_resource) {
  struct wl_shm_buffer *client_shm_buffer;
  struct wl_shm_pool *host_pool;
  struct wl_buffer *host_buffer;
  void *client_data;
  int32_t width, height, stride;
  uint32_t format;
  int fd = -1;
  void *host_data = NULL;
  size_t size;

  client_shm_buffer = wl_shm_buffer_get(client_buffer_resource);
  if (!client_shm_buffer) {
    weston_log(
        "Error: Expected a SHM buffer for cursor, but got something else.\n");
    return NULL;
  }

  width = wl_shm_buffer_get_width(client_shm_buffer);
  height = wl_shm_buffer_get_height(client_shm_buffer);
  stride = wl_shm_buffer_get_stride(client_shm_buffer);
  format = wl_shm_buffer_get_format(client_shm_buffer);
  size = stride * height;

  fd = os_create_anonymous_file(size);
  if (fd < 0) {
    weston_log("Error: Failed to create anonymous file for cursor SHM: %s\n",
               strerror(errno));
    return NULL;
  }

  host_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (host_data == MAP_FAILED) {
    weston_log("Error: Failed to mmap new cursor SHM: %s\n", strerror(errno));
    close(fd);
    return NULL;
  }

  wl_shm_buffer_begin_access(client_shm_buffer);
  client_data = wl_shm_buffer_get_data(client_shm_buffer);
  memcpy(host_data, client_data, size);
  wl_shm_buffer_end_access(client_shm_buffer);

  host_pool = wl_shm_create_pool(b->parent.shm, fd, size);
  if (!host_pool) {
    weston_log("Error: wl_shm_create_pool failed for host cursor.\n");
    munmap(host_data, size);
    close(fd);
    return NULL;
  }

  host_buffer =
      wl_shm_pool_create_buffer(host_pool, 0, width, height, stride, format);

  wl_shm_pool_destroy(host_pool);
  munmap(host_data, size);
  close(fd);

  if (!host_buffer) {
    weston_log("Error: wl_shm_pool_create_buffer failed for host cursor.\n");
    return NULL;
  }

  return host_buffer;
}

static void wayland_input_clear_host_cursor_cache(struct wayland_input *input) {
  if (input->passthrough_cursor.host_cursor_buffer) {
    wl_buffer_destroy(input->passthrough_cursor.host_cursor_buffer);
    input->passthrough_cursor.host_cursor_buffer = NULL;
  }
  input->passthrough_cursor.cached_weston_cursor_buffer = NULL;
}

static struct wl_buffer *
wayland_input_update_host_cursor(struct wayland_input *input,
                                 struct weston_pointer *pointer) {
  struct wayland_backend *b = input->backend;
  struct weston_buffer *current_cursor_wb = NULL;

  if (pointer && pointer->sprite &&
      pointer->sprite->surface->buffer_ref.buffer) {
    current_cursor_wb = pointer->sprite->surface->buffer_ref.buffer;
  }

  if (current_cursor_wb !=
      input->passthrough_cursor.cached_weston_cursor_buffer) {
    wayland_input_clear_host_cursor_cache(input);

    if (current_cursor_wb) {
      struct wl_buffer *shm_buffer =
          create_shm_buffer_from_weston_buffer(b, current_cursor_wb->resource);
      if (shm_buffer) {
        input->passthrough_cursor.host_cursor_buffer =
            (struct wl_buffer *)shm_buffer;
      }
    }

    input->passthrough_cursor.cached_weston_cursor_buffer = current_cursor_wb;
  }

  return input->passthrough_cursor.host_cursor_buffer;
}

static void passthrough_cache_init(struct wayland_output *output) {
  output->passthrough_cache.client_buffer = NULL;
  output->passthrough_cache.host_buffer = NULL;
}

static void passthrough_cache_clear(struct wayland_output *output) {
  if (output->passthrough_cache.host_buffer) {
    wl_buffer_destroy(output->passthrough_cache.host_buffer);
  }
  passthrough_cache_init(output);
}

static struct wl_buffer *
passthrough_cache_get_host_buffer(struct wayland_output *output,
                                  struct weston_buffer *client_buffer) {
  struct wayland_backend *b = output->backend;
  struct linux_dmabuf_buffer *dmabuf;

  if (!client_buffer || client_buffer->type != WESTON_BUFFER_DMABUF)
    return NULL;

  if (output->passthrough_cache.client_buffer == client_buffer) {
    return output->passthrough_cache.host_buffer;
  }

  passthrough_cache_clear(output);

  dmabuf = linux_dmabuf_buffer_get(b->compositor, client_buffer->resource);
  if (!dmabuf)
    return NULL;

  struct zwp_linux_buffer_params_v1 *params;
  struct wl_buffer *new_host_buffer;

  params = zwp_linux_dmabuf_v1_create_params(b->parent.linux_dmabuf);
  for (int i = 0; i < dmabuf->attributes.n_planes; i++) {
    zwp_linux_buffer_params_v1_add(
        params, dmabuf->attributes.fd[i], i, dmabuf->attributes.offset[i],
        dmabuf->attributes.stride[i], dmabuf->attributes.modifier >> 32,
        dmabuf->attributes.modifier & 0xFFFFFFFF);
  }

  new_host_buffer = zwp_linux_buffer_params_v1_create_immed(
      params, dmabuf->attributes.width, dmabuf->attributes.height,
      dmabuf->attributes.format, 0);
  zwp_linux_buffer_params_v1_destroy(params);

  if (new_host_buffer) {
    output->passthrough_cache.client_buffer = client_buffer;
    output->passthrough_cache.host_buffer = new_host_buffer;
  } else {
    passthrough_cache_init(output);
    weston_log("zwp_linux_buffer_params_v1_create_immed failed!\n");
  }

  return new_host_buffer;
}

static void wayland_destroy(struct weston_backend *backend);

static inline struct wayland_head *to_wayland_head(struct weston_head *base) {
  if (base->backend->destroy != wayland_destroy)
    return NULL;
  return container_of(base, struct wayland_head, base);
}

static void wayland_output_destroy(struct weston_output *base);

static inline struct wayland_output *
to_wayland_output(struct weston_output *base) {
  if (base->destroy != wayland_output_destroy)
    return NULL;
  return container_of(base, struct wayland_output, base);
}

static inline struct wayland_backend *
to_wayland_backend(struct weston_backend *base) {
  return container_of(base, struct wayland_backend, base);
}

static void frame_done(void *data, struct wl_callback *callback,
                       uint32_t time) {
  struct wayland_output *output = data;
  struct timespec ts;

  assert(callback == output->frame_cb);
  wl_callback_destroy(callback);
  output->frame_cb = NULL;

  /*
   * This is the fallback case, where Presentation extension is not
   * available from the parent compositor. We do not know the base for
   * 'time', so we cannot feed it to finish_frame(). Do the only thing
   * we can, and pretend finish_frame time is when we process this
   * event.
   */
  weston_compositor_read_presentation_clock(output->base.compositor, &ts);
  weston_output_finish_frame(&output->base, &ts, 0);
}

static const struct wl_callback_listener frame_listener = {frame_done};

static void presentation_handle_clock_id(
    void *data, struct wp_presentation *wp_presentation, uint32_t clk_id) {
  struct wayland_backend *b = data;

  b->parent.presentation_clock_id = clk_id;
  b->parent.presentation_clock_id_valid = true;
  b->base.supported_presentation_clocks = 1 << clk_id;
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = presentation_handle_clock_id,
};

static void
feedback_handle_sync_output(void *data,
                            struct wp_presentation_feedback *feedback,
                            struct wl_output *output) {
  /* This space is intentionally left blank */
}

static void feedback_handle_presented(void *data,
                                      struct wp_presentation_feedback *feedback,
                                      uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                                      uint32_t tv_nsec, uint32_t refresh,
                                      uint32_t seq_hi, uint32_t seq_lo,
                                      uint32_t flags) {
  struct wayland_output *output = data;
  struct wayland_backend *b = output->backend;
  struct timespec ts;
  uint32_t presented_flags = 0;

  if (b->parent.presentation_clock_id_valid &&
      b->parent.presentation_clock_id == b->compositor->presentation_clock) {
    ts.tv_sec = ((uint64_t)tv_sec_hi << 32) + tv_sec_lo;
    ts.tv_nsec = tv_nsec;
  } else {
    weston_compositor_read_presentation_clock(b->compositor, &ts);
  }

  if (!(flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC)) {
    presented_flags |= WESTON_FINISH_FRAME_TEARING;
  }

  weston_output_finish_frame(&output->base, &ts, presented_flags);
}

static void
feedback_handle_discarded(void *data,
                          struct wp_presentation_feedback *feedback) {
  struct wayland_output *output = data;

  weston_output_finish_frame(&output->base, NULL,
                             WP_PRESENTATION_FEEDBACK_INVALID);
}

static const struct wp_presentation_feedback_listener
    presentation_feedback_listener = {
        .sync_output = feedback_handle_sync_output,
        .presented = feedback_handle_presented,
        .discarded = feedback_handle_discarded,
};

static int
wayland_output_start_repaint_loop(struct weston_output *output_base) {
  struct wayland_output *output = to_wayland_output(output_base);
  struct wayland_backend *wb;
  struct timespec ts;

  assert(output);

  wb = output->backend;

  weston_compositor_read_presentation_clock(wb->compositor, &ts);
  weston_output_finish_frame(output_base, &ts,
                             WP_PRESENTATION_FEEDBACK_INVALID);

  return 0;
}

static bool region_contains_box(const pixman_region32_t *region,
                                const pixman_box32_t *box) {
  pixman_region32_t intersection;
  bool is_equal;
  bool ret;

  if (!pixman_region32_not_empty(region))
    return false;

  if (box->x1 >= box->x2 || box->y1 >= box->y2)
    return true;

  pixman_region32_init(&intersection);
  ret = pixman_region32_intersect_rect(&intersection, region, box->x1, box->y1,
                                       box->x2 - box->x1, box->y2 - box->y1);
  if (!ret) {
    pixman_region32_fini(&intersection);
    return false;
  }

  pixman_region32_t box_region;
  pixman_region32_init_rect(&box_region, box->x1, box->y1, box->x2 - box->x1,
                            box->y2 - box->y1);

  is_equal = pixman_region32_equal(&intersection, &box_region);

  pixman_region32_fini(&box_region);
  pixman_region32_fini(&intersection);

  return is_equal;
}

static struct weston_view *
find_passthrough_candidate_view(struct weston_output *output_base) {
  struct weston_compositor *compositor = output_base->compositor;
  struct weston_seat *seat;
  struct weston_surface *focus_surface = NULL;
  struct weston_view *candidate_view = NULL;
  struct weston_paint_node *pnode;

  wl_list_for_each(seat, &compositor->seat_list, link) {
    struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);
    if (keyboard && keyboard->focus) {
      focus_surface = keyboard->focus;
      break;
    }
  }

  if (focus_surface) {
    wl_list_for_each(pnode, &output_base->paint_node_z_order_list,
                     z_order_link) {
      if (pnode->view->surface == focus_surface) {
        candidate_view = pnode->view;
        break;
      }
    }
  }

  if (!candidate_view) {
    int client_view_count = 0;
    struct weston_view *top_client_view = NULL;

    wl_list_for_each(pnode, &output_base->paint_node_z_order_list,
                     z_order_link) {
      struct weston_view *view = pnode->view;
      if (!view->is_mapped || !view->surface->buffer_ref.buffer ||
          view->layer_link.layer == &compositor->cursor_layer) {
        continue;
      }
      client_view_count++;
      if (!top_client_view)
        top_client_view = view;
    }

    if (client_view_count == 1) {
      candidate_view = top_client_view;
    }
  }

  if (!candidate_view)
    return NULL;

  if (!candidate_view->is_mapped || !candidate_view->surface->buffer_ref.buffer)
    return NULL;

  struct weston_surface *surface = candidate_view->surface;
  pixman_box32_t surface_box = {0, 0, surface->width, surface->height};
  if (!region_contains_box(&surface->opaque, &surface_box))
    return NULL;

  pixman_box32_t *view_bbox = &candidate_view->transform.boundingbox.extents;
  pixman_box32_t *output_bbox = &output_base->region.extents;
  if (view_bbox->x1 != output_bbox->x1 || view_bbox->y1 != output_bbox->y1 ||
      view_bbox->x2 != output_bbox->x2 || view_bbox->y2 != output_bbox->y2) {
    return NULL;
  }

  return candidate_view;
}

static void request_next_frame_callback(struct wayland_backend *b,
                                        struct wayland_output *output,
                                        bool vsync) {
  if (b->parent.presentation && vsync) {
    struct wp_presentation_feedback *feedback = wp_presentation_feedback(
        b->parent.presentation, output->parent.surface);
    wp_presentation_feedback_add_listener(
        feedback, &presentation_feedback_listener, output);
  } else {
    output->frame_cb = wl_surface_frame(output->parent.surface);
    wl_callback_add_listener(output->frame_cb, &frame_listener, output);
  }
}

static bool surface_may_tear(struct wayland_output *output) {
  struct weston_view *view;

  wl_list_for_each(view, &output->base.compositor->view_list, link) {
    if (view->surface->tear_control && view->surface->tear_control->may_tear) {
      return true;
    }
  }

  return false;
}

#ifdef ENABLE_EGL
static int wayland_output_repaint_gl(struct weston_output *output_base) {
  struct wayland_output *output = to_wayland_output(output_base);
  struct wayland_backend *b = output->backend;
  struct weston_compositor *ec = output_base->compositor;
  struct weston_view *passthrough_view = NULL;
  struct weston_paint_node *pnode;
  bool async;
  struct wayland_input *input = NULL;

  if (!wl_list_empty(&b->input_list)) {
    input = wl_container_of(b->input_list.next, input, link);
  }

  if (b->parent.linux_dmabuf)
    passthrough_view = find_passthrough_candidate_view(output_base);

  async = passthrough_view || surface_may_tear(output);

  if (output->tearing_control) {
    uint32_t hint = async ? WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC
                          : WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC;
    if (output->current_tearing_presentation_hint != hint) {
      wp_tearing_control_v1_set_presentation_hint(output->tearing_control,
                                                  hint);
      output->current_tearing_presentation_hint = hint;
    }
  }

  if (output->output_using_dma != !!passthrough_view) {
    output->output_using_dma = !!passthrough_view;
    weston_log("Using DMA output: %d\n", output->output_using_dma);
  }

  if (passthrough_view) {
    struct weston_buffer *client_buffer =
        passthrough_view->surface->buffer_ref.buffer;
    struct wl_buffer *host_buffer;

    host_buffer = passthrough_cache_get_host_buffer(output, client_buffer);

    if (host_buffer) {
      wl_surface_attach(output->parent.surface, host_buffer, 0, 0);
      wl_surface_damage_buffer(output->parent.surface, 0, 0, INT32_MAX,
                               INT32_MAX);

      if (input && input->parent.pointer) {
        struct weston_pointer *pointer = weston_seat_get_pointer(&input->base);
        struct wl_buffer *host_cursor_buffer =
            wayland_input_update_host_cursor(input, pointer);

        if (host_cursor_buffer) {
          struct weston_surface *cursor_surface = pointer->sprite->surface;

          wl_surface_attach(input->parent.cursor.surface, host_cursor_buffer, 0,
                            0);
          wl_surface_damage(input->parent.cursor.surface, 0, 0,
                            cursor_surface->width, cursor_surface->height);

          wl_surface_commit(input->parent.cursor.surface);

          wl_pointer_set_cursor(input->parent.pointer, input->enter_serial,
                                input->parent.cursor.surface,
                                pointer->hotspot.c.x, pointer->hotspot.c.y);

          input->passthrough_cursor.host_cursor_enabled = true;
        } else if (input->passthrough_cursor.host_cursor_enabled) {
          wl_pointer_set_cursor(input->parent.pointer, input->enter_serial,
                                NULL, 0, 0);
          input->passthrough_cursor.host_cursor_enabled = false;
        }
      }

      request_next_frame_callback(b, output, !async);
      wl_surface_commit(output->parent.surface);
    } else {
      passthrough_view = NULL;
    }
  } else {
    passthrough_view = NULL;
  }

  if (!passthrough_view) {
    if (output->output_using_dma) {
      passthrough_cache_clear(output);
      output->output_using_dma = false;
      weston_log("Stopping DMA output, falling back to composition.\n");
    }

    if (input && input->passthrough_cursor.host_cursor_enabled) {
      wl_pointer_set_cursor(input->parent.pointer, input->enter_serial, NULL, 0,
                            0);
      input->passthrough_cursor.host_cursor_enabled = false;
    }

    pixman_region32_t damage;
    pixman_region32_init(&damage);
    weston_output_flush_damage_for_primary_plane(output_base, &damage);

    ec->renderer->repaint_output(&output->base, &damage, NULL);

    request_next_frame_callback(b, output, !async);
    wl_surface_commit(output->parent.surface);
    pixman_region32_fini(&damage);
  }

  wl_list_for_each(pnode, &output_base->paint_node_z_order_list, z_order_link) {
    struct weston_buffer *buffer = pnode->view->surface->buffer_ref.buffer;
    if (buffer && buffer->backend_lock_count > 0) {
      weston_buffer_backend_unlock(buffer);
    }
  }

  wl_display_flush(b->parent.wl_display);
  return 0;
}
#endif

static void
wayland_backend_destroy_output_surface(struct wayland_output *output) {
  assert(output->parent.surface);

  if (output->parent.viewport) {
    wp_viewport_destroy(output->parent.viewport);
    output->parent.viewport = NULL;
  }

  if (output->parent.xdg_toplevel) {
    xdg_toplevel_destroy(output->parent.xdg_toplevel);
    output->parent.xdg_toplevel = NULL;
  }

  if (output->parent.xdg_surface) {
    xdg_surface_destroy(output->parent.xdg_surface);
    output->parent.xdg_surface = NULL;
  }

  wl_surface_destroy(output->parent.surface);
  output->parent.surface = NULL;
}

static int wayland_output_disable(struct weston_output *base) {
  const struct weston_renderer *renderer = base->compositor->renderer;
  struct wayland_output *output = to_wayland_output(base);

  assert(output);

  if (!output->base.enabled)
    return 0;

  switch (renderer->type) {
#ifdef ENABLE_EGL
  case WESTON_RENDERER_GL:
    renderer->gl->output_destroy(&output->base);
    wl_egl_window_destroy(output->gl.egl_window);
    break;
#endif
  default:
    unreachable("invalid renderer");
  }

  wayland_backend_destroy_output_surface(output);

  return 0;
}

static void wayland_output_destroy(struct weston_output *base) {
  struct wayland_output *output = to_wayland_output(base);

  assert(output);

  wayland_output_disable(&output->base);

  weston_output_release(&output->base);

  if (output->frame_cb)
    wl_callback_destroy(output->frame_cb);

  passthrough_cache_clear(output);

  free(output->title);
  free(output);
}

#ifdef ENABLE_EGL
static int wayland_output_init_gl_renderer(struct wayland_output *output) {
  const struct weston_mode *mode = output->base.current_mode;
  struct wayland_backend *b = output->backend;
  const struct weston_renderer *renderer;
  struct gl_renderer_output_options options = {
      .formats = b->formats,
      .formats_count = b->formats_count,
  };

  options.area.x = 0;
  options.area.y = 0;
  options.area.width = mode->width;
  options.area.height = mode->height;
  options.fb_size.width = mode->width;
  options.fb_size.height = mode->height;

  output->gl.egl_window = wl_egl_window_create(
      output->parent.surface, options.fb_size.width, options.fb_size.height);
  if (!output->gl.egl_window) {
    weston_log("failure to create wl_egl_window\n");
    return -1;
  }
  options.window_for_legacy = output->gl.egl_window;
  options.window_for_platform = output->gl.egl_window;

  renderer = output->base.compositor->renderer;

  if (renderer->gl->output_window_create(&output->base, &options) < 0)
    goto cleanup_window;

  return 0;

cleanup_window:
  wl_egl_window_destroy(output->gl.egl_window);
  return -1;
}
#endif

static void wayland_output_resize_surface(struct wayland_output *output) {
  struct wayland_backend *b = output->backend;
  /* Defaults for without frame: */
  struct weston_size fb_size = {.width = output->base.current_mode->width,
                                .height = output->base.current_mode->height};
  struct weston_geometry area = {
      .x = 0, .y = 0, .width = fb_size.width, .height = fb_size.height};
  struct weston_geometry inp = area;
  struct weston_geometry opa = area;
  struct wl_region *region;

  assert(b->compositor);
  assert(b->compositor->renderer);

  region = wl_compositor_create_region(b->parent.compositor);
  wl_region_add(region, inp.x, inp.y, inp.width, inp.height);
  wl_surface_set_input_region(output->parent.surface, region);
  wl_region_destroy(region);

  if (output->parent.xdg_surface) {
    xdg_surface_set_window_geometry(output->parent.xdg_surface, inp.x, inp.y,
                                    inp.width, inp.height);
  }

  region = wl_compositor_create_region(b->parent.compositor);
  wl_region_add(region, opa.x, opa.y, opa.width, opa.height);
  wl_surface_set_opaque_region(output->parent.surface, region);
  wl_region_destroy(region);

#ifdef ENABLE_EGL
  if (output->gl.egl_window) {
    wl_egl_window_resize(output->gl.egl_window, fb_size.width, fb_size.height,
                         0, 0);
    weston_renderer_resize_output(&output->base, &fb_size, &area);

  } else
#endif
  {
    struct weston_size pm_size = {.width = area.width, .height = area.height};
    weston_renderer_resize_output(&output->base, &pm_size, NULL);
  }
}

static void handle_relative_motion(
    void *data, struct zwp_relative_pointer_v1 *relative_pointer,
    uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy,
    wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
  struct wayland_input *input = data;
  struct timespec ts;
  struct weston_pointer_motion_event motion_event = {0};

  timespec_from_usec(&ts, ((uint64_t)utime_hi << 32) | utime_lo);

  motion_event.mask = WESTON_POINTER_MOTION_REL;
  motion_event.rel.x = wl_fixed_to_double(dx);
  motion_event.rel.y = wl_fixed_to_double(dy);

  notify_motion(&input->base, &ts, &motion_event);

  if (input->seat_version < WL_POINTER_FRAME_SINCE_VERSION)
    notify_pointer_frame(&input->base);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener =
    {
        .relative_motion = handle_relative_motion,
};

static void
locked_pointer_locked(void *data,
                      struct zwp_locked_pointer_v1 *locked_pointer) {}

static void
locked_pointer_unlocked(void *data,
                        struct zwp_locked_pointer_v1 *locked_pointer);

static void destroy_pointer_lock(struct wayland_input *input,
                                 bool is_manual_unlock) {
  if (input->parent.locked_pointer) {
    // If this is a manual unlock (e.g., leaving fullscreen),
    // we should not try to re-lock. Otherwise, if the server
    // unlocked us, we might want to re-establish the lock.
    // By setting user_data to NULL, we prevent the unlocked
    // handler from re-locking.
    if (is_manual_unlock) {
      zwp_locked_pointer_v1_set_user_data(input->parent.locked_pointer, NULL);
    }

    zwp_locked_pointer_v1_destroy(input->parent.locked_pointer);
    input->parent.locked_pointer = NULL;
  }
  if (input->parent.relative_pointer) {
    zwp_relative_pointer_v1_destroy(input->parent.relative_pointer);
    input->parent.relative_pointer = NULL;
  }
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
    .locked = locked_pointer_locked,
    .unlocked = locked_pointer_unlocked,
};

static void create_pointer_lock(struct wayland_input *input,
                                struct wayland_output *output) {
  struct wayland_backend *b = input->backend;

  if (!b->parent.pointer_constraints || !b->parent.relative_pointer_manager ||
      !input->parent.pointer || input->parent.locked_pointer) {
    return;
  }

  input->parent.relative_pointer =
      zwp_relative_pointer_manager_v1_get_relative_pointer(
          b->parent.relative_pointer_manager, input->parent.pointer);
  zwp_relative_pointer_v1_add_listener(input->parent.relative_pointer,
                                       &relative_pointer_listener, input);

  input->parent.locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
      b->parent.pointer_constraints, output->parent.surface,
      input->parent.pointer, NULL,
      ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);

  zwp_locked_pointer_v1_add_listener(input->parent.locked_pointer,
                                     &locked_pointer_listener, input);
}

static void
locked_pointer_unlocked(void *data,
                        struct zwp_locked_pointer_v1 *locked_pointer) {
  struct wayland_input *input = data;

  if (!input) {
    return;
  }

  destroy_pointer_lock(input, false);

  if (input->output) {
    weston_log("Pointer was unlocked by host, attempting to re-lock to focused "
               "output.\n");
    create_pointer_lock(input, input->output);
  }
}

static void wayland_output_set_fullscreen(struct wayland_output *output,
                                          uint32_t framerate,
                                          struct wl_output *target) {
  wayland_output_resize_surface(output);

  if (output->parent.xdg_toplevel) {
    xdg_toplevel_set_fullscreen(output->parent.xdg_toplevel, target);
  } else {
    abort();
  }

  struct wayland_input *input;
  wl_list_for_each(input, &output->backend->input_list, link) {
    create_pointer_lock(input, output);
  }
}

static int wayland_output_switch_mode_finish(struct wayland_output *output) {
  struct weston_renderer *renderer = output->base.compositor->renderer;

  switch (renderer->type) {
#ifdef ENABLE_EGL
  case WESTON_RENDERER_GL:
    renderer->gl->output_destroy(&output->base);
    wl_egl_window_destroy(output->gl.egl_window);
    if (wayland_output_init_gl_renderer(output) < 0)
      return -1;
    break;
#endif
  default:
    unreachable("invalid renderer");
  }

  weston_output_schedule_repaint(&output->base);

  return 0;
}

static int wayland_output_switch_mode_xdg(struct wayland_output *output,
                                          struct weston_mode *mode) {
  if (output->backend->sprawl_across_outputs)
    return -1;

  assert(&output->mode == output->base.current_mode);

  output->mode.width = mode->width;
  output->mode.height = mode->height;

  if (mode->width < WINDOW_MIN_WIDTH)
    output->mode.width = WINDOW_MIN_WIDTH;
  if (mode->width > WINDOW_MAX_WIDTH)
    output->mode.width = WINDOW_MAX_WIDTH;

  if (mode->height < WINDOW_MIN_HEIGHT)
    output->mode.height = WINDOW_MIN_HEIGHT;
  if (mode->height > WINDOW_MAX_HEIGHT)
    output->mode.height = WINDOW_MAX_HEIGHT;

  /* Blow the old buffers because we changed size/surfaces */
  wayland_output_resize_surface(output);

  return wayland_output_switch_mode_finish(output);
}

static int wayland_output_switch_mode(struct weston_output *output_base,
                                      struct weston_mode *mode) {
  struct wayland_output *output = to_wayland_output(output_base);

  assert(output);

  if (mode == NULL) {
    weston_log("mode is NULL.\n");
    return -1;
  }

  if (output->parent.xdg_surface)
    return wayland_output_switch_mode_xdg(output, mode);

  return -1;
}

static void handle_xdg_surface_configure(void *data,
                                         struct xdg_surface *surface,
                                         uint32_t serial) {
  xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    handle_xdg_surface_configure};

static void scale_wayland_input_coords(struct wayland_output *output, double *x,
                                       double *y) {
  if (!output || !output->parent.viewport || !output->base.current_mode)
    return;

  if (output->parent.configure_width <= 0 ||
      output->parent.configure_height <= 0 ||
      (output->parent.configure_width == output->base.current_mode->width &&
       output->parent.configure_height == output->base.current_mode->height))
    return;

  if (output->base.current_mode->width <= 0 ||
      output->base.current_mode->height <= 0)
    return;

  *x = *x * ((double)output->base.current_mode->width /
             (double)output->parent.configure_width);
  *y = *y * ((double)output->base.current_mode->height /
             (double)output->parent.configure_height);
}

static void handle_xdg_toplevel_configure(void *data,
                                          struct xdg_toplevel *toplevel,
                                          int32_t width, int32_t height,
                                          struct wl_array *states) {
  struct wayland_output *output = data;

  output->parent.wait_for_configure = false;

  if (!output->parent.viewport) {
    weston_log("wp_viewporter not supported by host, cannot bypass scaling.\n");
    abort();
  }

  if (width > 0 && height > 0) {
    wp_viewport_set_destination(output->parent.viewport, width, height);
    output->parent.configure_width = width;
    output->parent.configure_height = height;
  }
}

static void handle_xdg_toplevel_close(void *data,
                                      struct xdg_toplevel *xdg_toplevel) {
  struct wayland_output *output = data;
  struct weston_compositor *compositor = output->base.compositor;

  wayland_output_destroy(&output->base);

  if (wl_list_empty(&compositor->output_list))
    weston_compositor_exit(compositor);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    handle_xdg_toplevel_configure,
    handle_xdg_toplevel_close,
};

static int
wayland_backend_create_output_surface(struct wayland_output *output) {
  struct wayland_backend *b = output->backend;

  assert(!output->parent.surface);

  output->parent.surface = wl_compositor_create_surface(b->parent.compositor);
  if (!output->parent.surface)
    return -1;

  if (b->parent.viewporter) {
    output->parent.viewport = wp_viewporter_get_viewport(
        b->parent.viewporter, output->parent.surface);
  }

  wl_surface_set_user_data(output->parent.surface, output);
  wl_surface_set_buffer_scale(output->parent.surface, 1);

  if (b->parent.tearing_control_manager) {
    output->tearing_control = wp_tearing_control_manager_v1_get_tearing_control(
        b->parent.tearing_control_manager, output->parent.surface);
    output->current_tearing_presentation_hint = UINT32_MAX;
  }

  if (b->parent.xdg_wm_base) {
    output->parent.xdg_surface = xdg_wm_base_get_xdg_surface(
        b->parent.xdg_wm_base, output->parent.surface);
    xdg_surface_add_listener(output->parent.xdg_surface, &xdg_surface_listener,
                             output);

    output->parent.xdg_toplevel =
        xdg_surface_get_toplevel(output->parent.xdg_surface);
    xdg_toplevel_add_listener(output->parent.xdg_toplevel,
                              &xdg_toplevel_listener, output);

    xdg_toplevel_set_title(output->parent.xdg_toplevel, output->title);

    wl_surface_commit(output->parent.surface);

    output->parent.wait_for_configure = true;

    while (output->parent.wait_for_configure)
      wl_display_dispatch(b->parent.wl_display);

    weston_log("wayland-backend: Using xdg_wm_base\n");
  }

  if (b->parent.decoration_manager && output->parent.xdg_toplevel) {
    struct zxdg_toplevel_decoration_v1 *decoration;

    decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        b->parent.decoration_manager, output->parent.xdg_toplevel);

    if (decoration) {
      zxdg_toplevel_decoration_v1_set_mode(
          decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
  }

  return 0;
}

static int wayland_output_enable(struct weston_output *base) {
  const struct weston_renderer *renderer = base->compositor->renderer;
  struct wayland_output *output = to_wayland_output(base);
  int ret = 0;

  assert(output);

  weston_log("Creating %dx%d wayland output at (%d, %d)\n",
             output->base.current_mode->width,
             output->base.current_mode->height, (int)output->base.pos.c.x,
             (int)output->base.pos.c.y);

  if (!output->parent.surface)
    ret = wayland_backend_create_output_surface(output);

  if (ret < 0)
    return -1;

  switch (renderer->type) {
#ifdef ENABLE_EGL
  case WESTON_RENDERER_GL:
    if (wayland_output_init_gl_renderer(output) < 0)
      goto err_output;

    output->base.repaint = wayland_output_repaint_gl;
    break;
#endif
  default:
    unreachable("invalid renderer");
  }

  output->base.start_repaint_loop = wayland_output_start_repaint_loop;
  output->base.assign_planes = NULL;
  output->base.set_backlight = NULL;
  output->base.set_dpms = NULL;
  output->base.switch_mode = wayland_output_switch_mode;

  wayland_output_set_fullscreen(output, 0, NULL);

  return 0;

err_output:
  wayland_backend_destroy_output_surface(output);

  return -1;
}

static int
wayland_output_setup_for_parent_output(struct wayland_output *output,
                                       struct wayland_parent_output *poutput);

static int wayland_output_setup_fullscreen(struct wayland_output *output,
                                           struct wayland_head *head);

static int wayland_output_attach_head(struct weston_output *output_base,
                                      struct weston_head *head_base) {
  struct wayland_output *output = to_wayland_output(output_base);
  struct wayland_head *head = to_wayland_head(head_base);

  assert(output);

  if (!head)
    return -1;

  if (!wl_list_empty(&output->base.head_list))
    return -1;

  if (head->parent_output) {
    if (wayland_output_setup_for_parent_output(output, head->parent_output) < 0)
      return -1;
  } else {
    if (wayland_output_setup_fullscreen(output, head) < 0)
      return -1;
  }

  return 0;
}

static void wayland_output_detach_head(struct weston_output *output_base,
                                       struct weston_head *head) {
  struct wayland_output *output = to_wayland_output(output_base);

  assert(output);

  /* Rely on the disable hook if the output was enabled. We do not
   * support cloned heads, so detaching is guaranteed to disable the
   * output.
   */
  if (output->base.enabled)
    return;

  /* undo setup fullscreen */
  if (output->parent.surface)
    wayland_backend_destroy_output_surface(output);
}

static struct weston_output *
wayland_output_create(struct weston_backend *backend, const char *name) {
  struct wayland_backend *b =
      container_of(backend, struct wayland_backend, base);
  struct weston_compositor *compositor = b->compositor;
  struct wayland_output *output;
  char *title;

  /* name can't be NULL. */
  assert(name);

  output = zalloc(sizeof *output);
  if (output == NULL) {
    perror("zalloc");
    return NULL;
  }

  if (asprintf(&title, "%s - %s", WINDOW_TITLE, name) < 0) {
    free(output);
    return NULL;
  }
  output->title = title;
  output->base.repaint_only_on_capture = true;

  passthrough_cache_init(output);
  weston_output_init(&output->base, compositor, name);

  output->base.destroy = wayland_output_destroy;
  output->base.disable = wayland_output_disable;
  output->base.enable = wayland_output_enable;
  output->base.attach_head = wayland_output_attach_head;
  output->base.detach_head = wayland_output_detach_head;
  output->output_using_dma = false;

  output->backend = b;

  weston_compositor_add_pending_output(&output->base, compositor);

  return &output->base;
}

static struct wayland_head *wayland_head_create(struct wayland_backend *backend,
                                                const char *name) {
  struct weston_compositor *compositor = backend->compositor;
  struct wayland_head *head;

  assert(name);

  head = zalloc(sizeof *head);
  if (!head)
    return NULL;

  weston_head_init(&head->base, name);

  head->base.backend = &backend->base;

  weston_head_set_connection_status(&head->base, true);
  weston_head_set_supported_vrr_modes_mask(&head->base, WESTON_VRR_MODE_GAME);
  weston_compositor_add_head(compositor, &head->base);

  return head;
}

static int
wayland_head_create_for_parent_output(struct wayland_backend *backend,
                                      struct wayland_parent_output *poutput) {
  struct wayland_head *head;
  char name[100];
  int ret;

  ret = snprintf(name, sizeof(name), "wlparent-%d", poutput->id);
  if (ret < 1 || (unsigned)ret >= sizeof(name))
    return -1;

  head = wayland_head_create(backend, name);
  if (!head)
    return -1;

  assert(!poutput->head);
  head->parent_output = poutput;
  poutput->head = head;

  weston_head_set_monitor_strings(&head->base, poutput->physical.make,
                                  poutput->physical.model, NULL);
  weston_head_set_physical_size(&head->base, poutput->physical.width,
                                poutput->physical.height);

  return 0;
}

static void wayland_head_destroy(struct weston_head *base) {
  struct wayland_head *head = to_wayland_head(base);

  assert(head);

  if (head->parent_output)
    head->parent_output->head = NULL;

  weston_head_release(&head->base);
  free(head);
}

static int wayland_output_set_size(struct weston_output *base, int width,
                                   int height, int refresh) {
  struct wayland_output *output = to_wayland_output(base);
  struct weston_head *head;
  int output_width, output_height;

  if (!output)
    return -1;

  /* We can only be called once. */
  assert(!output->base.current_mode);

  /* Make sure we have scale set. */
  assert(output->base.current_scale);

  if (width < 1) {
    weston_log("Invalid width \"%d\" for output %s\n", width,
               output->base.name);
    return -1;
  }

  if (height < 1) {
    weston_log("Invalid height \"%d\" for output %s\n", height,
               output->base.name);
    return -1;
  }

  wl_list_for_each(head, &output->base.head_list, output_link) {
    weston_head_set_monitor_strings(head, "wayland", "none", NULL);

    /* XXX: Calculate proper size. */
    weston_head_set_physical_size(head, width, height);
  }

  output_width = width * output->base.current_scale;
  output_height = height * output->base.current_scale;

  output->mode.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

  output->mode.width = output_width;
  output->mode.height = output_height;
  output->mode.refresh = refresh;
  wl_list_insert(&output->base.mode_list, &output->mode.link);

  output->base.current_mode = &output->mode;

  return 0;
}

static int
wayland_output_setup_for_parent_output(struct wayland_output *output,
                                       struct wayland_parent_output *poutput) {
  struct weston_mode *mode;

  if (poutput->current_mode) {
    mode = poutput->current_mode;
  } else if (poutput->preferred_mode) {
    mode = poutput->preferred_mode;
  } else if (!wl_list_empty(&poutput->mode_list)) {
    mode = container_of(poutput->mode_list.next, struct weston_mode, link);
  } else {
    weston_log("No valid modes found. Skipping output.\n");
    return -1;
  }

  output->base.current_scale = 1;
  output->base.transform = WL_OUTPUT_TRANSFORM_NORMAL;

  output->parent.output = poutput->global;

  wl_list_insert_list(&output->base.mode_list, &poutput->mode_list);
  wl_list_init(&poutput->mode_list);

  /* No other mode should have CURRENT already. */
  mode->flags |= WL_OUTPUT_MODE_CURRENT;
  output->base.current_mode = mode;

  /* output->mode is unused in this path. */

  return 0;
}

static int wayland_output_setup_fullscreen(struct wayland_output *output,
                                           struct wayland_head *head) {
  struct wayland_backend *b = output->backend;
  int width = 0, height = 0, refresh = 0;

  output->base.current_scale = 1;
  output->base.transform = WL_OUTPUT_TRANSFORM_NORMAL;

  if (wayland_backend_create_output_surface(output) < 0)
    return -1;

  if (b->parent.xdg_wm_base) {
    if (output->parent.xdg_toplevel)
      xdg_toplevel_set_fullscreen(output->parent.xdg_toplevel,
                                  output->parent.output);

    wl_display_roundtrip(b->parent.wl_display);
  }

  struct wayland_parent_output *parent_output;

  wl_list_for_each(parent_output, &output->backend->parent.output_list, link) {
    if (parent_output->current_mode &&
        parent_output->current_mode->flags & WL_OUTPUT_MODE_CURRENT) {
      width = parent_output->current_mode->width;
      height = parent_output->current_mode->height;
      refresh = parent_output->current_mode->refresh;
      break;
    }
  }

  if (wayland_output_set_size(&output->base, width, height, refresh) < 0)
    goto err_set_size;

  weston_head_set_monitor_strings(&head->base, "wayland", "none", NULL);
  weston_head_set_physical_size(&head->base, width, height);

  return 0;

err_set_size:
  wayland_backend_destroy_output_surface(output);

  return -1;
}

static void input_handle_pointer_enter(void *data, struct wl_pointer *pointer,
                                       uint32_t serial,
                                       struct wl_surface *surface,
                                       wl_fixed_t fixed_x, wl_fixed_t fixed_y) {
  struct wayland_input *input = data;
  double x, y;
  struct weston_coord_global pos;

  if (!surface) {
    input->output = NULL;
    input->has_focus = false;
    clear_pointer_focus(&input->base);
    return;
  }

  x = wl_fixed_to_double(fixed_x);
  y = wl_fixed_to_double(fixed_y);

  /* XXX: If we get a modifier event immediately before the focus,
   *      we should try to keep the same serial. */
  input->enter_serial = serial;
  input->output = wl_surface_get_user_data(surface);

  scale_wayland_input_coords(input->output, &x, &y);

  pos = weston_coord_global_from_output_point(x, y, &input->output->base);

  input->has_focus = true;
  notify_pointer_focus(&input->base, &input->output->base, pos);
  wl_pointer_set_cursor(input->parent.pointer, input->enter_serial, NULL, 0, 0);
}

static void input_handle_pointer_leave(void *data, struct wl_pointer *pointer,
                                       uint32_t serial,
                                       struct wl_surface *surface) {
  struct wayland_input *input = data;

  if (!input->output)
    return;

  clear_pointer_focus(&input->base);
  input->output = NULL;
  input->has_focus = false;
}

static void input_handle_motion(void *data, struct wl_pointer *pointer,
                                uint32_t time, wl_fixed_t fixed_x,
                                wl_fixed_t fixed_y) {
  struct wayland_input *input = data;
  double x, y;
  struct weston_coord_global pos;
  struct timespec ts;

  if (!input->output)
    return;

  x = wl_fixed_to_double(fixed_x);
  y = wl_fixed_to_double(fixed_y);

  scale_wayland_input_coords(input->output, &x, &y);

  pos = weston_coord_global_from_output_point(x, y, &input->output->base);

  if (!input->has_focus) {
    wl_pointer_set_cursor(input->parent.pointer, input->enter_serial, NULL, 0,
                          0);
    notify_pointer_focus(&input->base, &input->output->base, pos);
    input->has_focus = true;
  }

  timespec_from_msec(&ts, time);
  notify_motion_absolute(&input->base, &ts, pos);

  if (input->seat_version < WL_POINTER_FRAME_SINCE_VERSION)
    notify_pointer_frame(&input->base);
}

static void input_handle_button(void *data, struct wl_pointer *pointer,
                                uint32_t serial, uint32_t time, uint32_t button,
                                enum wl_pointer_button_state state) {
  struct wayland_input *input = data;
  struct timespec ts;

  if (!input->output)
    return;

  timespec_from_msec(&ts, time);
  notify_button(&input->base, &ts, button, state);
  if (input->seat_version < WL_POINTER_FRAME_SINCE_VERSION)
    notify_pointer_frame(&input->base);
}

static void input_handle_axis(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis, wl_fixed_t value) {
  struct wayland_input *input = data;
  struct weston_pointer_axis_event weston_event;
  struct timespec ts;

  weston_event.axis = axis;
  weston_event.value = wl_fixed_to_double(value);
  weston_event.has_v120 = false;

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && input->vert.has_v120) {
    weston_event.has_v120 = true;
    weston_event.v120 = input->vert.v120;
    input->vert.has_v120 = false;
  } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL &&
             input->horiz.has_v120) {
    weston_event.has_v120 = true;
    weston_event.v120 = input->horiz.v120;
    input->horiz.has_v120 = false;
  }

  timespec_from_msec(&ts, time);

  notify_axis(&input->base, &ts, &weston_event);

  if (input->seat_version < WL_POINTER_FRAME_SINCE_VERSION)
    notify_pointer_frame(&input->base);
}

static void input_handle_frame(void *data, struct wl_pointer *pointer) {
  struct wayland_input *input = data;

  notify_pointer_frame(&input->base);
}

static void input_handle_axis_source(void *data, struct wl_pointer *pointer,
                                     uint32_t source) {
  struct wayland_input *input = data;

  notify_axis_source(&input->base, source);
}

static void input_handle_axis_stop(void *data, struct wl_pointer *pointer,
                                   uint32_t time, uint32_t axis) {
  struct wayland_input *input = data;
  struct weston_pointer_axis_event weston_event;
  struct timespec ts;

  weston_event.axis = axis;
  weston_event.value = 0;

  timespec_from_msec(&ts, time);

  notify_axis(&input->base, &ts, &weston_event);
}

static void input_handle_axis_discrete(void *data, struct wl_pointer *pointer,
                                       uint32_t axis, int32_t discrete) {
  struct wayland_input *input = data;

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    input->vert.has_v120 = true;
    input->vert.v120 = discrete * 120;
  } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
    input->horiz.has_v120 = true;
    input->horiz.v120 = discrete * 120;
  }
}

static void input_handle_axis_v120(void *data, struct wl_pointer *pointer,
                                   uint32_t axis, int32_t v120) {
  struct wayland_input *input = data;

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    input->vert.has_v120 = true;
    input->vert.v120 = v120;
  } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
    input->horiz.has_v120 = true;
    input->horiz.v120 = v120;
  }
}

static const struct wl_pointer_listener pointer_listener = {
    input_handle_pointer_enter, input_handle_pointer_leave,
    input_handle_motion,        input_handle_button,
    input_handle_axis,          input_handle_frame,
    input_handle_axis_source,   input_handle_axis_stop,
    input_handle_axis_discrete, input_handle_axis_v120};

static void input_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                uint32_t format, int fd, uint32_t size) {
  struct wayland_input *input = data;
  struct xkb_keymap *keymap;
  char *map_str;

  if (!data) {
    close(fd);
    return;
  }

  if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) {
      weston_log("mmap failed: %s\n", strerror(errno));
      goto error;
    }

    keymap = xkb_keymap_new_from_string(input->backend->compositor->xkb_context,
                                        map_str, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map_str, size);

    if (!keymap) {
      weston_log("failed to compile keymap\n");
      goto error;
    }

    input->keyboard_state_update = STATE_UPDATE_NONE;
  } else if (format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP) {
    weston_log("No keymap provided; falling back to default\n");
    keymap = NULL;
    input->keyboard_state_update = STATE_UPDATE_AUTOMATIC;
  } else {
    weston_log("Invalid keymap\n");
    goto error;
  }

  close(fd);

  if (weston_seat_get_keyboard(&input->base))
    weston_seat_update_keymap(&input->base, keymap);
  else
    weston_seat_init_keyboard(&input->base, keymap);

  xkb_keymap_unref(keymap);

  return;

error:
  wl_keyboard_release(input->parent.keyboard);
  close(fd);
}

static void input_handle_keyboard_enter(void *data,
                                        struct wl_keyboard *keyboard,
                                        uint32_t serial,
                                        struct wl_surface *surface,
                                        struct wl_array *keys) {
  struct wayland_input *input = data;
  struct wayland_output *focus;

  focus = input->keyboard_focus;
  if (focus) {
    /* This shouldn't happen */
    focus->keyboard_count--;
  }

  if (!surface) {
    input->keyboard_focus = NULL;
    return;
  }

  input->keyboard_focus = wl_surface_get_user_data(surface);
  input->keyboard_focus->keyboard_count++;

  /* XXX: If we get a modifier event immediately before the focus,
   *      we should try to keep the same serial. */
  notify_keyboard_focus_in(&input->base, keys, STATE_UPDATE_AUTOMATIC);
}

static void input_handle_keyboard_leave(void *data,
                                        struct wl_keyboard *keyboard,
                                        uint32_t serial,
                                        struct wl_surface *surface) {
  struct wayland_input *input = data;
  struct wayland_output *focus;

  notify_keyboard_focus_out(&input->base);

  focus = input->keyboard_focus;
  if (!focus)
    return;

  focus->keyboard_count--;
  input->keyboard_focus = NULL;
}

static void input_handle_key(void *data, struct wl_keyboard *keyboard,
                             uint32_t serial, uint32_t time, uint32_t key,
                             uint32_t state) {
  struct wayland_input *input = data;
  struct timespec ts;

  if (!input->keyboard_focus)
    return;

  timespec_from_msec(&ts, time);

  input->key_serial = serial;
  notify_key(&input->base, &ts, key,
             state ? WL_KEYBOARD_KEY_STATE_PRESSED
                   : WL_KEYBOARD_KEY_STATE_RELEASED,
             input->keyboard_state_update);
}

static void input_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t serial_in, uint32_t mods_depressed,
                                   uint32_t mods_latched, uint32_t mods_locked,
                                   uint32_t group) {
  struct weston_keyboard *keyboard;
  struct wayland_input *input = data;
  struct wayland_backend *b = input->backend;
  uint32_t serial_out;

  /* If we get a key event followed by a modifier event with the
   * same serial number, then we try to preserve those semantics by
   * reusing the same serial number on the way out too. */
  if (serial_in == input->key_serial)
    serial_out = wl_display_get_serial(b->compositor->wl_display);
  else
    serial_out = wl_display_next_serial(b->compositor->wl_display);

  keyboard = weston_seat_get_keyboard(&input->base);
  xkb_state_update_mask(keyboard->xkb_state.state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
  notify_modifiers(&input->base, serial_out);
}

static void input_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
                                     int32_t rate, int32_t delay) {
  struct wayland_input *input = data;
  struct wayland_backend *b = input->backend;

  b->compositor->kb_repeat_rate = rate;
  b->compositor->kb_repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    input_handle_keymap,         input_handle_keyboard_enter,
    input_handle_keyboard_leave, input_handle_key,
    input_handle_modifiers,      input_handle_repeat_info,
};

static void input_handle_touch_down(void *data, struct wl_touch *wl_touch,
                                    uint32_t serial, uint32_t time,
                                    struct wl_surface *surface, int32_t id,
                                    wl_fixed_t fixed_x, wl_fixed_t fixed_y) {
  struct wayland_input *input = data;
  struct wayland_output *output;
  bool first_touch;
  struct weston_coord_global pos;
  double x, y;
  struct timespec ts;

  x = wl_fixed_to_double(fixed_x);
  y = wl_fixed_to_double(fixed_y);

  timespec_from_msec(&ts, time);

  first_touch = (input->touch_points == 0);
  input->touch_points++;

  input->touch_focus = wl_surface_get_user_data(surface);
  output = input->touch_focus;

  scale_wayland_input_coords(output, &x, &y);

  if (!first_touch && !input->touch_active)
    return;

  pos = weston_coord_global_from_output_point(x, y, &output->base);

  notify_touch(input->touch_device, &ts, id, &pos, WL_TOUCH_DOWN);
  input->touch_active = true;
}

static void input_handle_touch_up(void *data, struct wl_touch *wl_touch,
                                  uint32_t serial, uint32_t time, int32_t id) {
  struct wayland_input *input = data;
  struct wayland_output *output = input->touch_focus;
  bool active = input->touch_active;
  struct timespec ts;

  timespec_from_msec(&ts, time);

  input->touch_points--;

  if (!output)
    return;

  if (active)
    notify_touch(input->touch_device, &ts, id, NULL, WL_TOUCH_UP);
}

static void input_handle_touch_motion(void *data, struct wl_touch *wl_touch,
                                      uint32_t time, int32_t id,
                                      wl_fixed_t fixed_x, wl_fixed_t fixed_y) {
  struct wayland_input *input = data;
  struct wayland_output *output = input->touch_focus;
  double x, y;
  struct weston_coord_global pos;
  struct timespec ts;

  x = wl_fixed_to_double(fixed_x);
  y = wl_fixed_to_double(fixed_y);
  timespec_from_msec(&ts, time);

  if (!output || !input->touch_active)
    return;

  scale_wayland_input_coords(output, &x, &y);

  pos = weston_coord_global_from_output_point(x, y, &output->base);

  notify_touch(input->touch_device, &ts, id, &pos, WL_TOUCH_MOTION);
}

static void input_handle_touch_frame(void *data, struct wl_touch *wl_touch) {
  struct wayland_input *input = data;

  if (!input->touch_focus || !input->touch_active)
    return;

  notify_touch_frame(input->touch_device);

  if (input->touch_points == 0) {
    input->touch_focus = NULL;
    input->touch_active = false;
  }
}

static void input_handle_touch_cancel(void *data, struct wl_touch *wl_touch) {
  struct wayland_input *input = data;

  if (!input->touch_focus || !input->touch_active)
    return;

  notify_touch_cancel(input->touch_device);
}

static const struct wl_touch_listener touch_listener = {
    input_handle_touch_down,   input_handle_touch_up,
    input_handle_touch_motion, input_handle_touch_frame,
    input_handle_touch_cancel,
};

static struct weston_touch_device *
create_touch_device(struct wayland_input *input) {
  struct weston_touch_device *touch_device;
  char str[128];

  /* manufacture a unique'ish name */
  snprintf(str, sizeof str, "wayland-touch[%u]",
           wl_proxy_get_id((struct wl_proxy *)input->parent.seat));

  touch_device = weston_touch_create_touch_device(input->base.touch_state, str,
                                                  NULL, NULL);

  return touch_device;
}

static void input_update_capabilities(struct wayland_input *input,
                                      enum wl_seat_capability caps) {
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->parent.pointer) {
    input->parent.pointer = wl_seat_get_pointer(input->parent.seat);
    wl_pointer_set_user_data(input->parent.pointer, input);
    wl_pointer_add_listener(input->parent.pointer, &pointer_listener, input);
    weston_seat_init_pointer(&input->base);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->parent.pointer) {
    if (input->seat_version >= WL_POINTER_RELEASE_SINCE_VERSION)
      wl_pointer_release(input->parent.pointer);
    else
      wl_pointer_destroy(input->parent.pointer);
    input->parent.pointer = NULL;
    weston_seat_release_pointer(&input->base);
  }

  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->parent.keyboard) {
    input->parent.keyboard = wl_seat_get_keyboard(input->parent.seat);
    wl_keyboard_set_user_data(input->parent.keyboard, input);
    wl_keyboard_add_listener(input->parent.keyboard, &keyboard_listener, input);
  } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->parent.keyboard) {
    if (input->seat_version >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
      wl_keyboard_release(input->parent.keyboard);
    else
      wl_keyboard_destroy(input->parent.keyboard);
    input->parent.keyboard = NULL;
    weston_seat_release_keyboard(&input->base);
  }

  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->parent.touch) {
    input->parent.touch = wl_seat_get_touch(input->parent.seat);
    wl_touch_set_user_data(input->parent.touch, input);
    wl_touch_add_listener(input->parent.touch, &touch_listener, input);
    weston_seat_init_touch(&input->base);
    input->touch_device = create_touch_device(input);
  } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->parent.touch) {
    weston_touch_device_destroy(input->touch_device);
    input->touch_device = NULL;
    if (input->seat_version >= WL_TOUCH_RELEASE_SINCE_VERSION)
      wl_touch_release(input->parent.touch);
    else
      wl_touch_destroy(input->parent.touch);
    input->parent.touch = NULL;
    weston_seat_release_touch(&input->base);
  }
}

static void input_handle_capabilities(void *data, struct wl_seat *seat,
                                      enum wl_seat_capability caps) {
  struct wayland_input *input = data;

  if (input->seat_initialized)
    input_update_capabilities(input, caps);
  else
    input->caps = caps;
}

static void input_handle_name(void *data, struct wl_seat *seat,
                              const char *name) {
  struct wayland_input *input = data;

  if (!input->seat_initialized) {
    assert(!input->name);
    input->name = strdup(name);
  }
}

static const struct wl_seat_listener seat_listener = {
    input_handle_capabilities,
    input_handle_name,
};

static void display_finish_add_seat(void *data, struct wl_callback *wl_callback,
                                    uint32_t callback_data) {
  struct wayland_input *input = data;
  char *name;

  assert(wl_callback == input->initial_info_cb);
  wl_callback_destroy(input->initial_info_cb);
  input->initial_info_cb = NULL;
  input->seat_initialized = true;

  wl_list_remove(&input->link);
  wl_list_insert(input->backend->input_list.prev, &input->link);

  name = input->name ? input->name : "default";
  weston_seat_init(&input->base, input->backend->compositor, name);
  free(input->name);
  input->name = NULL;

  input_update_capabilities(input, input->caps);

  /* Because this happens one roundtrip after wl_seat is bound,
   * wl_compositor will also have been bound by this time. */
  input->parent.cursor.surface =
      wl_compositor_create_surface(input->backend->parent.compositor);
  wl_surface_set_buffer_scale(input->parent.cursor.surface, 1);

  input->vert.axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
  input->horiz.axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
}

static const struct wl_callback_listener seat_callback_listener = {
    display_finish_add_seat};

static void display_start_add_seat(struct wayland_backend *b, uint32_t id,
                                   uint32_t available_version) {
  struct wayland_input *input;
  uint32_t version = MIN(available_version, 4);

  input = zalloc(sizeof *input);
  if (input == NULL)
    return;

  input->backend = b;
  input->parent.seat =
      wl_registry_bind(b->parent.registry, id, &wl_seat_interface, version);
  input->seat_version = version;

  wl_seat_add_listener(input->parent.seat, &seat_listener, input);
  wl_seat_set_user_data(input->parent.seat, input);

  /* Wait one roundtrip for the compositor to provide the seat name
   * and initial capabilities */
  input->initial_info_cb = wl_display_sync(b->parent.wl_display);
  wl_callback_add_listener(input->initial_info_cb, &seat_callback_listener,
                           input);

  wl_list_insert(input->backend->pending_input_list.prev, &input->link);
}

static void wayland_input_destroy(struct wayland_input *input) {
  if (input->touch_device)
    weston_touch_device_destroy(input->touch_device);

  if (input->seat_initialized)
    weston_seat_release(&input->base);

  wayland_input_clear_host_cursor_cache(input);

  if (input->parent.keyboard) {
    if (input->seat_version >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
      wl_keyboard_release(input->parent.keyboard);
    else
      wl_keyboard_destroy(input->parent.keyboard);
  }
  if (input->parent.pointer) {
    if (input->seat_version >= WL_POINTER_RELEASE_SINCE_VERSION)
      wl_pointer_release(input->parent.pointer);
    else
      wl_pointer_destroy(input->parent.pointer);
  }
  if (input->parent.touch) {
    if (input->seat_version >= WL_TOUCH_RELEASE_SINCE_VERSION)
      wl_touch_release(input->parent.touch);
    else
      wl_touch_destroy(input->parent.touch);
  }
  if (input->parent.seat) {
    if (input->seat_version >= WL_SEAT_RELEASE_SINCE_VERSION)
      wl_seat_release(input->parent.seat);
    else
      wl_seat_destroy(input->parent.seat);
  }
  if (input->initial_info_cb)
    wl_callback_destroy(input->initial_info_cb);
  if (input->parent.cursor.surface)
    wl_surface_destroy(input->parent.cursor.surface);
  if (input->name)
    free(input->name);

  free(input);
}

static void wayland_parent_output_geometry(
    void *data, struct wl_output *output_proxy, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel,
    const char *make, const char *model, int32_t transform) {
  struct wayland_parent_output *output = data;

  output->x = x;
  output->y = y;
  output->physical.width = physical_width;
  output->physical.height = physical_height;
  output->physical.subpixel = subpixel;

  free(output->physical.make);
  output->physical.make = strdup(make);
  free(output->physical.model);
  output->physical.model = strdup(model);

  output->transform = transform;
}

static struct weston_mode *find_mode(struct wl_list *list, int32_t width,
                                     int32_t height, uint32_t refresh) {
  struct weston_mode *mode;

  wl_list_for_each(mode, list, link) {
    if (mode->width == width && mode->height == height &&
        mode->refresh == refresh)
      return mode;
  }

  mode = zalloc(sizeof *mode);
  if (!mode)
    return NULL;

  mode->width = width;
  mode->height = height;
  mode->refresh = refresh;
  wl_list_insert(list, &mode->link);

  return mode;
}

static struct weston_output *wayland_parent_output_get_enabled_output(
    struct wayland_parent_output *poutput) {
  struct wayland_head *head = poutput->head;

  if (!head)
    return NULL;

  if (!weston_head_is_enabled(&head->base))
    return NULL;

  return weston_head_get_output(&head->base);
}

static void wayland_parent_output_mode(void *data,
                                       struct wl_output *wl_output_proxy,
                                       uint32_t flags, int32_t width,
                                       int32_t height, int32_t refresh) {
  struct wayland_parent_output *output = data;
  struct weston_output *enabled_output;
  struct weston_mode *mode;

  enabled_output = wayland_parent_output_get_enabled_output(output);
  if (enabled_output) {
    mode = find_mode(&enabled_output->mode_list, width, height, refresh);
    if (!mode)
      return;
    mode->flags = flags;
    /* Do a mode-switch on current mode change? */
  } else {
    mode = find_mode(&output->mode_list, width, height, refresh);
    if (!mode)
      return;
    mode->flags = flags;
    if (flags & WL_OUTPUT_MODE_CURRENT)
      output->current_mode = mode;
    if (flags & WL_OUTPUT_MODE_PREFERRED)
      output->preferred_mode = mode;
  }
}

static const struct wl_output_listener output_listener = {
    wayland_parent_output_geometry, wayland_parent_output_mode};

static void output_sync_callback(void *data, struct wl_callback *callback,
                                 uint32_t unused) {
  struct wayland_parent_output *output = data;

  assert(output->sync_cb == callback);
  wl_callback_destroy(callback);
  output->sync_cb = NULL;

  assert(output->backend->sprawl_across_outputs);

  wayland_head_create_for_parent_output(output->backend, output);
}

static const struct wl_callback_listener output_sync_listener = {
    output_sync_callback};

static void wayland_backend_register_output(struct wayland_backend *b,
                                            uint32_t id) {
  struct wayland_parent_output *output;

  output = zalloc(sizeof *output);
  if (!output)
    return;

  output->backend = b;
  output->id = id;
  output->global =
      wl_registry_bind(b->parent.registry, id, &wl_output_interface, 1);
  if (!output->global) {
    free(output);
    return;
  }

  wl_output_add_listener(output->global, &output_listener, output);

  output->scale = 0;
  output->transform = WL_OUTPUT_TRANSFORM_NORMAL;
  output->physical.subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
  wl_list_init(&output->mode_list);
  wl_list_insert(&b->parent.output_list, &output->link);

  if (b->sprawl_across_outputs) {
    output->sync_cb = wl_display_sync(b->parent.wl_display);
    wl_callback_add_listener(output->sync_cb, &output_sync_listener, output);
  }

  wl_display_roundtrip(output->backend->parent.wl_display);
}

static void
wayland_parent_output_destroy(struct wayland_parent_output *output) {
  struct weston_mode *mode, *next;

  if (output->sync_cb)
    wl_callback_destroy(output->sync_cb);

  if (output->head)
    wayland_head_destroy(&output->head->base);

  wl_output_destroy(output->global);
  free(output->physical.make);
  free(output->physical.model);

  wl_list_for_each_safe(mode, next, &output->mode_list, link) {
    wl_list_remove(&mode->link);
    free(mode);
  }

  wl_list_remove(&output->link);
  free(output);
}

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell,
                             uint32_t serial) {
  xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    xdg_wm_base_ping,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  struct wayland_backend *b = data;

  if (strcmp(interface, "wl_compositor") == 0) {
    b->parent.compositor = wl_registry_bind(
        registry, name, &wl_compositor_interface, MIN(version, 4));
  } else if (strcmp(interface, "xdg_wm_base") == 0) {
    b->parent.xdg_wm_base =
        wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(b->parent.xdg_wm_base, &wm_base_listener, b);
  } else if (strcmp(interface, "wl_seat") == 0) {
    display_start_add_seat(b, name, version);
  } else if (strcmp(interface, "wl_output") == 0) {
    wayland_backend_register_output(b, name);
  } else if (strcmp(interface, "wl_shm") == 0) {
    b->parent.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, "wp_viewporter") == 0) {
    b->parent.viewporter =
        wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
  } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
    b->parent.pointer_constraints = wl_registry_bind(
        registry, name, &zwp_pointer_constraints_v1_interface, 1);
  } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
    b->parent.relative_pointer_manager = wl_registry_bind(
        registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
  } else if (strcmp(interface, "wp_tearing_control_manager_v1") == 0) {
    b->parent.tearing_control_manager = wl_registry_bind(
        registry, name, &wp_tearing_control_manager_v1_interface, 1);
  } else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0) {
    b->parent.decoration_manager = wl_registry_bind(
        registry, name, &zxdg_decoration_manager_v1_interface, 1);
  } else if (strcmp(interface, "wp_presentation") == 0) {
    b->parent.presentation =
        wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    wp_presentation_add_listener(b->parent.presentation, &presentation_listener,
                                 b);
  } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0 && version >= 4) {
    b->parent.linux_dmabuf =
        wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4);
  }
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t name) {
  struct wayland_backend *b = data;
  struct wayland_parent_output *output, *next;

  wl_list_for_each_safe(output, next, &b->parent.output_list,
                        link) if (output->id == name)
      wayland_parent_output_destroy(output);

  // todo: handle wl_seat removal
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global, registry_handle_global_remove};

static int wayland_backend_handle_event(int fd, uint32_t mask, void *data) {
  struct wayland_backend *b = data;
  int count = 0;

  if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
    weston_compositor_exit(b->compositor);
    return 0;
  }

  if (mask & WL_EVENT_READABLE)
    count = wl_display_dispatch(b->parent.wl_display);
  if (mask & WL_EVENT_WRITABLE)
    wl_display_flush(b->parent.wl_display);

  if (mask == 0) {
    count = wl_display_dispatch_pending(b->parent.wl_display);
    wl_display_flush(b->parent.wl_display);
  }

  if (count < 0) {
    weston_compositor_exit(b->compositor);
    return 0;
  }

  return count;
}

static void wayland_shutdown(struct weston_backend *backend) {
  struct wayland_backend *b = to_wayland_backend(backend);

  wl_event_source_remove(b->parent.wl_source);
}

static void wayland_destroy(struct weston_backend *backend) {
  struct wayland_backend *b = to_wayland_backend(backend);
  struct weston_compositor *ec = b->compositor;
  struct weston_head *base, *next;
  struct wayland_parent_output *output, *next_output;
  struct wayland_input *input, *next_input;

  wl_list_remove(&b->base.link);

  wl_list_for_each_safe(base, next, &ec->head_list, compositor_link) {
    if (to_wayland_head(base))
      wayland_head_destroy(base);
  }

  wl_list_for_each_safe(output, next_output, &b->parent.output_list, link)
      wayland_parent_output_destroy(output);

  wl_list_for_each_safe(input, next_input, &b->input_list, link)
      wayland_input_destroy(input);

  wl_list_for_each_safe(input, next_input, &b->pending_input_list, link)
      wayland_input_destroy(input);

  if (b->parent.presentation)
    wp_presentation_destroy(b->parent.presentation);

  if (b->parent.decoration_manager)
    zxdg_decoration_manager_v1_destroy(b->parent.decoration_manager);

  if (b->parent.tearing_control_manager)
    wp_tearing_control_manager_v1_destroy(b->parent.tearing_control_manager);

  if (b->parent.relative_pointer_manager)
    zwp_relative_pointer_manager_v1_destroy(b->parent.relative_pointer_manager);

  if (b->parent.pointer_constraints)
    zwp_pointer_constraints_v1_destroy(b->parent.pointer_constraints);

  if (b->parent.viewporter)
    wp_viewporter_destroy(b->parent.viewporter);

  if (b->parent.shm)
    wl_shm_destroy(b->parent.shm);

  if (b->parent.xdg_wm_base)
    xdg_wm_base_destroy(b->parent.xdg_wm_base);

  if (b->parent.compositor)
    wl_compositor_destroy(b->parent.compositor);

  wl_cursor_theme_destroy(b->cursor_theme);

  free(b->formats);

  wl_registry_destroy(b->parent.registry);
  wl_display_flush(b->parent.wl_display);
  wl_display_disconnect(b->parent.wl_display);

  cleanup_after_cairo();

  free(b);
}

static const char *left_ptrs[] = {"left_ptr", "default", "top_left_arrow",
                                  "left-arrow"};

static void create_cursor(struct wayland_backend *b,
                          struct weston_wayland_backend_config *config) {
  unsigned int i;

  b->cursor_theme = wl_cursor_theme_load(config->cursor_theme,
                                         config->cursor_size, b->parent.shm);
  if (!b->cursor_theme) {
    fprintf(stderr, "could not load cursor theme\n");
    return;
  }

  b->cursor = NULL;
  for (i = 0; !b->cursor && i < ARRAY_LENGTH(left_ptrs); ++i)
    b->cursor = wl_cursor_theme_get_cursor(b->cursor_theme, left_ptrs[i]);
  if (!b->cursor) {
    fprintf(stderr, "could not load left cursor\n");
    return;
  }
}

static void
wayland_passthrough_attach_buffer(struct weston_backend *backend_base,
                                  struct weston_surface *surface,
                                  struct weston_buffer *buffer) {
  if (buffer->type == WESTON_BUFFER_DMABUF)
    weston_buffer_backend_lock(buffer);
}

static struct wayland_backend *
wayland_backend_create(struct weston_compositor *compositor,
                       struct weston_wayland_backend_config *new_config) {
  struct wayland_backend *b;
  struct wl_event_loop *loop;
  enum weston_renderer_type renderer = new_config->renderer;
  int fd;

  b = zalloc(sizeof *b);
  if (b == NULL)
    return NULL;

  b->compositor = compositor;
  b->parent.presentation_clock_id_valid = false;
  wl_list_insert(&compositor->backend_list, &b->base.link);

  b->base.supported_presentation_clocks = WESTON_PRESENTATION_CLOCKS_SOFTWARE;

  b->parent.wl_display = wl_display_connect(new_config->display_name);
  if (b->parent.wl_display == NULL) {
    weston_log("Error: Failed to connect to parent Wayland compositor: %s\n",
               strerror(errno));
    weston_log_continue(STAMP_SPACE "display option: %s, WAYLAND_DISPLAY=%s\n",
                        new_config->display_name ?: "(none)",
                        getenv("WAYLAND_DISPLAY") ?: "(not set)");
    goto err_compositor;
  }

  wl_list_init(&b->parent.output_list);
  wl_list_init(&b->input_list);
  wl_list_init(&b->pending_input_list);
  b->parent.registry = wl_display_get_registry(b->parent.wl_display);
  wl_registry_add_listener(b->parent.registry, &registry_listener, b);
  wl_display_roundtrip(b->parent.wl_display);

  if (b->parent.shm == NULL) {
    weston_log(
        "Error: Failed to retrieve wl_shm from parent Wayland compositor\n");
    goto err_display;
  }

  create_cursor(b, new_config);

  b->formats_count = ARRAY_LENGTH(wayland_formats);
  b->formats = pixel_format_get_array(wayland_formats, b->formats_count);

  switch (renderer) {
  case WESTON_RENDERER_AUTO:
  case WESTON_RENDERER_GL: {
    const struct gl_renderer_display_options options = {
        .egl_platform = EGL_PLATFORM_WAYLAND_KHR,
        .egl_native_display = b->parent.wl_display,
        .egl_surface_type = EGL_WINDOW_BIT,
        .formats = b->formats,
        .formats_count = b->formats_count,
    };

    if (weston_compositor_init_renderer(compositor, WESTON_RENDERER_GL,
                                        &options.base) < 0) {
      weston_log("Failed to initialize the GL renderer\n");
    } else {
      break;
    }
    FALLTHROUGH;
  }
  default:
    weston_log("Unsupported renderer requested\n");
    goto err_display;
  }

  if (compositor->renderer->type == WESTON_RENDERER_GL) {
    b->base.passthrough_attach_buffer = wayland_passthrough_attach_buffer;
  }

  b->base.shutdown = wayland_shutdown;
  b->base.destroy = wayland_destroy;
  b->base.create_output = wayland_output_create;

  loop = wl_display_get_event_loop(compositor->wl_display);

  fd = wl_display_get_fd(b->parent.wl_display);
  b->parent.wl_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
                                             wayland_backend_handle_event, b);
  if (b->parent.wl_source == NULL)
    goto err_renderer;

  wl_event_source_check(b->parent.wl_source);

  return b;
err_renderer:
  compositor->renderer->destroy(compositor);
err_display:
  wl_display_disconnect(b->parent.wl_display);
err_compositor:
  wl_list_remove(&b->base.link);
  free(b->formats);
  free(b);
  return NULL;
}

static void wayland_backend_destroy_backend(struct wayland_backend *b) {
  wl_display_disconnect(b->parent.wl_display);

  wl_cursor_theme_destroy(b->cursor_theme);

  wl_list_remove(&b->base.link);
  cleanup_after_cairo();
  free(b->formats);
  free(b);
}

static void
config_init_to_defaults(struct weston_wayland_backend_config *config) {}

WL_EXPORT int weston_backend_init(struct weston_compositor *compositor,
                                  struct weston_backend_config *config_base) {
  struct wayland_backend *b;
  struct weston_wayland_backend_config new_config;

  if (config_base == NULL ||
      config_base->struct_version != WESTON_WAYLAND_BACKEND_CONFIG_VERSION ||
      config_base->struct_size > sizeof(struct weston_wayland_backend_config)) {
    weston_log("wayland backend config structure is invalid\n");
    return -1;
  }

  if (compositor->renderer) {
    weston_log("wayland backend must be the primary backend\n");
    return -1;
  }

  config_init_to_defaults(&new_config);
  memcpy(&new_config, config_base, config_base->struct_size);

  b = wayland_backend_create(compositor, &new_config);

  if (!b)
    return -1;

  if (!wayland_head_create(b, "wayland-fullscreen")) {
    weston_log("Unable to create a fullscreen head.\n");
    goto err_outputs;
  }

  return 0;

err_outputs:
  wayland_backend_destroy_backend(b);
  return -1;
}
