/*
 * Copyright © 2011 Intel Corporation
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

#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <assert.h>
#include <X11/Xcursor/Xcursor.h>
#include <linux/input.h>

#include <libweston/libweston.h>
#include "xwayland.h"
#include "xwayland-internal-interface.h"

#include "shared/cairo-util.h"
#include "shared/hash.h"
#include "shared/helpers.h"
#include "shared/xcb-xwayland.h"
#include "xwayland-shell-v1-server-protocol.h"

struct wm_size_hints {
	uint32_t flags;
	int32_t x, y;
	int32_t width, height;	/* should set so old wm's don't mess up */
	int32_t min_width, min_height;
	int32_t max_width, max_height;
	int32_t width_inc, height_inc;
	struct {
		int32_t x;
		int32_t y;
	} min_aspect, max_aspect;
	int32_t base_width, base_height;
	int32_t win_gravity;
};

#define USPosition	(1L << 0)
#define USSize		(1L << 1)
#define PPosition	(1L << 2)
#define PSize		(1L << 3)
#define PMinSize	(1L << 4)
#define PMaxSize	(1L << 5)
#define PResizeInc	(1L << 6)
#define PAspect		(1L << 7)
#define PBaseSize	(1L << 8)
#define PWinGravity	(1L << 9)

struct motif_wm_hints {
	uint32_t flags;
	uint32_t functions;
	uint32_t decorations;
	int32_t input_mode;
	uint32_t status;
};

#define MWM_HINTS_FUNCTIONS     (1L << 0)
#define MWM_HINTS_DECORATIONS   (1L << 1)
#define MWM_HINTS_INPUT_MODE    (1L << 2)
#define MWM_HINTS_STATUS        (1L << 3)

#define MWM_FUNC_ALL            (1L << 0)
#define MWM_FUNC_RESIZE         (1L << 1)
#define MWM_FUNC_MOVE           (1L << 2)
#define MWM_FUNC_MINIMIZE       (1L << 3)
#define MWM_FUNC_MAXIMIZE       (1L << 4)
#define MWM_FUNC_CLOSE          (1L << 5)

#define MWM_DECOR_ALL           (1L << 0)
#define MWM_DECOR_BORDER        (1L << 1)
#define MWM_DECOR_RESIZEH       (1L << 2)
#define MWM_DECOR_TITLE         (1L << 3)
#define MWM_DECOR_MENU          (1L << 4)
#define MWM_DECOR_MINIMIZE      (1L << 5)
#define MWM_DECOR_MAXIMIZE      (1L << 6)

#define MWM_DECOR_EVERYTHING \
	(MWM_DECOR_BORDER | MWM_DECOR_RESIZEH | MWM_DECOR_TITLE | \
	 MWM_DECOR_MENU | MWM_DECOR_MINIMIZE | MWM_DECOR_MAXIMIZE)

#define MWM_INPUT_MODELESS 0
#define MWM_INPUT_PRIMARY_APPLICATION_MODAL 1
#define MWM_INPUT_SYSTEM_MODAL 2
#define MWM_INPUT_FULL_APPLICATION_MODAL 3
#define MWM_INPUT_APPLICATION_MODAL MWM_INPUT_PRIMARY_APPLICATION_MODAL

#define MWM_TEAROFF_WINDOW      (1L<<0)

#define _NET_WM_MOVERESIZE_SIZE_TOPLEFT      0
#define _NET_WM_MOVERESIZE_SIZE_TOP          1
#define _NET_WM_MOVERESIZE_SIZE_TOPRIGHT     2
#define _NET_WM_MOVERESIZE_SIZE_RIGHT        3
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT  4
#define _NET_WM_MOVERESIZE_SIZE_BOTTOM       5
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT   6
#define _NET_WM_MOVERESIZE_SIZE_LEFT         7
#define _NET_WM_MOVERESIZE_MOVE              8   /* movement only */
#define _NET_WM_MOVERESIZE_SIZE_KEYBOARD     9   /* size via keyboard */
#define _NET_WM_MOVERESIZE_MOVE_KEYBOARD    10   /* move via keyboard */
#define _NET_WM_MOVERESIZE_CANCEL           11   /* cancel operation */

static const char *xwayland_surface_role = "xwayland";

struct weston_output_weak_ref {
	struct weston_output *output;
	struct wl_listener destroy_listener;
};

struct weston_wm_window {
	struct weston_wm *wm;
	xcb_window_t id;
	xcb_window_t frame_id;
	struct frame *frame;
	cairo_surface_t *cairo_surface;
	uint32_t surface_id;
	uint64_t surface_serial;
	struct weston_surface *surface;
	struct weston_desktop_xwayland_surface *shsurf;
	struct wl_listener surface_destroy_listener;
	struct wl_event_source *repaint_source;
	struct wl_event_source *configure_source;
	int properties_dirty;
	int pid;
	char *machine;
	char *class;
	char *name;
	struct weston_wm_window *transient_for;
	uint32_t protocols;
	xcb_atom_t type;
	int width, height;
	struct weston_coord_global pos;
	bool pos_dirty;
	bool map_request_valid;
	struct weston_coord_global map_request;
	struct weston_output_weak_ref legacy_fullscreen_output;
	int saved_width, saved_height;
	int decorate;
	uint32_t last_button_time;
	int did_double;
	int override_redirect;
	int fullscreen;
	int has_alpha;
	int delete_window;
	int maximized_vert;
	int maximized_horz;
	int take_focus;
	struct wm_size_hints size_hints;
	struct motif_wm_hints motif_hints;
	struct wl_list link;
	int decor_top;
	int decor_bottom;
	int decor_left;
	int decor_right;
};

struct xwl_surface {
	struct wl_resource *resource;
	struct weston_wm *wm;
	struct weston_surface *weston_surface;
	uint64_t serial;
	struct wl_listener surface_commit_listener;
	struct wl_list link;
};

static void
weston_wm_window_set_allow_commits(struct weston_wm_window *window, bool allow);

static struct weston_wm_window *
get_wm_window(struct weston_surface *surface);

static void
weston_wm_set_net_active_window(struct weston_wm *wm, xcb_window_t window);

static void
weston_wm_window_schedule_repaint(struct weston_wm_window *window);

static int
legacy_fullscreen(struct weston_wm *wm,
		  struct weston_wm_window *window,
		  struct weston_output **output_ret);

static void
xserver_map_shell_surface(struct weston_wm_window *window,
			  struct weston_surface *surface);

static inline bool
weston_wm_window_is_maximized(struct weston_wm_window *window)
{
	return window->maximized_horz && window->maximized_vert;
}

static bool
wm_debug_is_enabled(struct weston_wm *wm)
{
	return weston_log_scope_is_enabled(wm->server->wm_debug);
}

static void __attribute__ ((format (printf, 2, 3)))
wm_printf(struct weston_wm *wm, const char *fmt, ...)
{
	va_list ap;
	char timestr[128];

	if (wm_debug_is_enabled(wm))
		weston_log_scope_printf(wm->server->wm_debug, "%s ",
				weston_log_scope_timestamp(wm->server->wm_debug,
				timestr, sizeof timestr));

	va_start(ap, fmt);
	weston_log_scope_vprintf(wm->server->wm_debug, fmt, ap);
	va_end(ap);
}
static void
weston_output_weak_ref_init(struct weston_output_weak_ref *ref)
{
	ref->output = NULL;
}

static void
weston_output_weak_ref_clear(struct weston_output_weak_ref *ref)
{
	if (!ref->output)
		return;

	wl_list_remove(&ref->destroy_listener.link);
	ref->output = NULL;
}

static void
weston_output_weak_ref_handle_destroy(struct wl_listener *listener, void *data)
{
	struct weston_output_weak_ref *ref;

	ref = wl_container_of(listener, ref, destroy_listener);
	assert(ref->output == data);

	weston_output_weak_ref_clear(ref);
}

static void
weston_output_weak_ref_set(struct weston_output_weak_ref *ref,
			   struct weston_output *output)
{
	weston_output_weak_ref_clear(ref);

	if (!output)
		return;

	ref->destroy_listener.notify = weston_output_weak_ref_handle_destroy;
	wl_signal_add(&output->destroy_signal, &ref->destroy_listener);
	ref->output = output;
}

static bool __attribute__ ((warn_unused_result))
wm_lookup_window(struct weston_wm *wm, xcb_window_t hash,
		 struct weston_wm_window **window)
{
	*window = hash_table_lookup(wm->window_hash, hash);
	if (*window)
		return true;
	return false;
}


static xcb_cursor_t
xcb_cursor_image_load_cursor(struct weston_wm *wm, const XcursorImage *img)
{
	xcb_connection_t *c = wm->conn;
	xcb_screen_iterator_t s = xcb_setup_roots_iterator(xcb_get_setup(c));
	xcb_screen_t *screen = s.data;
	xcb_gcontext_t gc;
	xcb_pixmap_t pix;
	xcb_render_picture_t pic;
	xcb_cursor_t cursor;
	int stride = img->width * 4;

	pix = xcb_generate_id(c);
	xcb_create_pixmap(c, 32, pix, screen->root, img->width, img->height);

	pic = xcb_generate_id(c);
	xcb_render_create_picture(c, pic, pix, wm->format_rgba.id, 0, 0);

	gc = xcb_generate_id(c);
	xcb_create_gc(c, gc, pix, 0, 0);

	xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc,
		      img->width, img->height, 0, 0, 0, 32,
		      stride * img->height, (uint8_t *) img->pixels);
	xcb_free_gc(c, gc);

	cursor = xcb_generate_id(c);
	xcb_render_create_cursor(c, cursor, pic, img->xhot, img->yhot);

	xcb_render_free_picture(c, pic);
	xcb_free_pixmap(c, pix);

	return cursor;
}

static xcb_cursor_t
xcb_cursor_images_load_cursor(struct weston_wm *wm, const XcursorImages *images)
{
	/* TODO: treat animated cursors as well */
	if (images->nimage != 1)
		return -1;

	return xcb_cursor_image_load_cursor(wm, images->images[0]);
}

static xcb_cursor_t
xcb_cursor_library_load_cursor(struct weston_wm *wm, const char *file)
{
	xcb_cursor_t cursor;
	XcursorImages *images;
	char *v = NULL;
	char *theme;
	int size = 0;

	if (!file)
		return 0;

	v = getenv ("XCURSOR_SIZE");
	if (v)
		size = atoi(v);

	if (!size)
		size = 32;

	theme = getenv("XCURSOR_THEME");

	images = XcursorLibraryLoadImages(file, theme, size);
	if (!images)
		return -1;

	cursor = xcb_cursor_images_load_cursor (wm, images);
	XcursorImagesDestroy (images);

	return cursor;
}

static unsigned
dump_cardinal_array_elem(FILE *fp, unsigned format,
			 void *arr, unsigned len, unsigned ind)
{
	const char *comma;

	/* If more than 16 elements, print 0-14, ..., last */
	if (ind > 14 && ind < len - 1) {
		fprintf(fp, ", ...");
		return len - 1;
	}

	comma = ind ? ", " : "";

	switch (format) {
	case 32:
		fprintf(fp, "%s%" PRIu32, comma, ((uint32_t *)arr)[ind]);
		break;
	case 16:
		fprintf(fp, "%s%" PRIu16, comma, ((uint16_t *)arr)[ind]);
		break;
	case 8:
		fprintf(fp, "%s%" PRIu8, comma, ((uint8_t *)arr)[ind]);
		break;
	default:
		fprintf(fp, "%s???", comma);
	}

	return ind + 1;
}

static void
dump_cardinal_array(FILE *fp, xcb_get_property_reply_t *reply)
{
	unsigned i = 0;
	void *arr;

	assert(reply->type == XCB_ATOM_CARDINAL);

	arr = xcb_get_property_value(reply);

	fprintf(fp, "[");
	while (i < reply->value_len)
		i = dump_cardinal_array_elem(fp, reply->format,
					     arr, reply->value_len, i);
	fprintf(fp, "]");
}

void
dump_property(FILE *fp, struct weston_wm *wm,
	      xcb_atom_t property, xcb_get_property_reply_t *reply)
{
	int32_t *incr_value;
	const char *text_value, *name;
	xcb_atom_t *atom_value;
	xcb_window_t *window_value;
	int width, len;
	uint32_t i;

	width = fprintf(fp, "%s: ", get_atom_name(wm->conn, property));
	if (reply == NULL) {
		fprintf(fp, "(no reply)\n");
		return;
	}

	width += fprintf(fp, "%s/%d, length %d (value_len %d): ",
			 get_atom_name(wm->conn, reply->type),
			 reply->format,
			 xcb_get_property_value_length(reply),
			 reply->value_len);

	if (reply->type == wm->atom.incr) {
		incr_value = xcb_get_property_value(reply);
		fprintf(fp, "%d\n", *incr_value);
	} else if (reply->type == wm->atom.utf8_string ||
	           reply->type == wm->atom.string) {
		text_value = xcb_get_property_value(reply);
		if (reply->value_len > 40)
			len = 40;
		else
			len = reply->value_len;
		fprintf(fp, "\"%.*s\"\n", len, text_value);
	} else if (reply->type == XCB_ATOM_ATOM) {
		atom_value = xcb_get_property_value(reply);
		for (i = 0; i < reply->value_len; i++) {
			name = get_atom_name(wm->conn, atom_value[i]);
			if (width + strlen(name) + 2 > 78) {
				fprintf(fp, "\n    ");
				width = 4;
			} else if (i > 0) {
				width +=  fprintf(fp, ", ");
			}

			width +=  fprintf(fp, "%s", name);
		}
		fprintf(fp, "\n");
	} else if (reply->type == XCB_ATOM_CARDINAL) {
		dump_cardinal_array(fp, reply);
		fprintf(fp, "\n");
	} else if (reply->type == XCB_ATOM_WINDOW && reply->format == 32) {
		window_value = xcb_get_property_value(reply);
		fprintf(fp, "win %u\n", *window_value);
	} else {
		fprintf(fp, "huh?\n");
	}
}

static void
read_and_dump_property(FILE *fp, struct weston_wm *wm,
		       xcb_window_t window, xcb_atom_t property)
{
	xcb_get_property_reply_t *reply;
	xcb_get_property_cookie_t cookie;

	cookie = xcb_get_property(wm->conn, 0, window,
				  property, XCB_ATOM_ANY, 0, 2048);
	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	dump_property(fp, wm, property, reply);

	free(reply);
}

/* We reuse some predefined, but otherwise useles atoms
 * as local type placeholders that never touch the X11 server,
 * to make weston_wm_window_read_properties() less exceptional.
 */
#define TYPE_WM_PROTOCOLS	XCB_ATOM_CUT_BUFFER0
#define TYPE_MOTIF_WM_HINTS	XCB_ATOM_CUT_BUFFER1
#define TYPE_NET_WM_STATE	XCB_ATOM_CUT_BUFFER2
#define TYPE_WM_NORMAL_HINTS	XCB_ATOM_CUT_BUFFER3

static void
weston_wm_window_read_properties(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;

#define F(field) (&window->field)
	const struct {
		xcb_atom_t atom;
		xcb_atom_t type;
		void *ptr;
	} props[] = {
		{ XCB_ATOM_WM_CLASS,           XCB_ATOM_STRING,            F(class) },
		{ XCB_ATOM_WM_NAME,            XCB_ATOM_STRING,            F(name) },
		{ XCB_ATOM_WM_TRANSIENT_FOR,   XCB_ATOM_WINDOW,            F(transient_for) },
		{ wm->atom.wm_protocols,       TYPE_WM_PROTOCOLS,          NULL },
		{ wm->atom.wm_normal_hints,    TYPE_WM_NORMAL_HINTS,       NULL },
		{ wm->atom.net_wm_state,       TYPE_NET_WM_STATE,          NULL },
		{ wm->atom.net_wm_window_type, XCB_ATOM_ATOM,              F(type) },
		{ wm->atom.net_wm_name,        XCB_ATOM_STRING,            F(name) },
		{ wm->atom.net_wm_pid,         XCB_ATOM_CARDINAL,          F(pid) },
		{ wm->atom.motif_wm_hints,     TYPE_MOTIF_WM_HINTS,        NULL },
		{ wm->atom.wm_client_machine,  XCB_ATOM_WM_CLIENT_MACHINE, F(machine) },
	};
#undef F

	xcb_get_property_cookie_t cookie[ARRAY_LENGTH(props)];
	xcb_get_property_reply_t *reply;
	void *p;
	uint32_t *xid;
	xcb_atom_t *atom;
	uint32_t i;
	char name[1024];

	if (!window->properties_dirty)
		return;
	window->properties_dirty = 0;

	for (i = 0; i < ARRAY_LENGTH(props); i++)
		cookie[i] = xcb_get_property(wm->conn,
					     0, /* delete */
					     window->id,
					     props[i].atom,
					     XCB_ATOM_ANY, 0, 2048);

	window->decorate = window->override_redirect ? 0 : MWM_DECOR_EVERYTHING;
	window->size_hints.flags = 0;
	window->motif_hints.flags = 0;
	window->delete_window = 0;
	window->take_focus = 0;

	for (i = 0; i < ARRAY_LENGTH(props); i++)  {
		reply = xcb_get_property_reply(wm->conn, cookie[i], NULL);
		if (!reply)
			/* Bad window, typically */
			continue;
		if (reply->type == XCB_ATOM_NONE) {
			/* No such property */
			free(reply);
			continue;
		}

		p = props[i].ptr;

		switch (props[i].type) {
		case XCB_ATOM_WM_CLIENT_MACHINE:
		case XCB_ATOM_STRING:
			/* FIXME: We're using this for both string and
			   utf8_string */
			if (*(char **) p)
				free(*(char **) p);

			*(char **) p =
				strndup(xcb_get_property_value(reply),
					xcb_get_property_value_length(reply));
			break;
		case XCB_ATOM_WINDOW:
			xid = xcb_get_property_value(reply);
			if (!wm_lookup_window(wm, *xid, p))
				weston_log("XCB_ATOM_WINDOW contains window"
					   " id not found in hash table.\n");
			break;
		case XCB_ATOM_CARDINAL:
		case XCB_ATOM_ATOM:
			atom = xcb_get_property_value(reply);
			*(xcb_atom_t *) p = *atom;
			break;
		case TYPE_WM_PROTOCOLS:
			atom = xcb_get_property_value(reply);
			for (i = 0; i < reply->value_len; i++)
				if (atom[i] == wm->atom.wm_delete_window) {
					window->delete_window = 1;
				} else if (atom[i] == wm->atom.wm_take_focus) {
					window->take_focus = 1;
				}
			break;
		case TYPE_WM_NORMAL_HINTS:
			/* WM_NORMAL_HINTS can be either 15 or 18 CARD32s */
			memset(&window->size_hints, 0,
			       sizeof(window->size_hints));
			memcpy(&window->size_hints,
			       xcb_get_property_value(reply),
			       MIN(sizeof(window->size_hints),
			           reply->value_len * 4));
			break;
		case TYPE_NET_WM_STATE:
			window->fullscreen = 0;
			atom = xcb_get_property_value(reply);
			for (i = 0; i < reply->value_len; i++) {
				if (atom[i] == wm->atom.net_wm_state_fullscreen)
					window->fullscreen = 1;
				if (atom[i] == wm->atom.net_wm_state_maximized_vert)
					window->maximized_vert = 1;
				if (atom[i] == wm->atom.net_wm_state_maximized_horz)
					window->maximized_horz = 1;
			}
			break;
		case TYPE_MOTIF_WM_HINTS:
			memcpy(&window->motif_hints,
			       xcb_get_property_value(reply),
			       sizeof window->motif_hints);
			if (window->motif_hints.flags & MWM_HINTS_DECORATIONS) {
				if (window->motif_hints.decorations & MWM_DECOR_ALL)
					/* MWM_DECOR_ALL means all except the other values listed. */
					window->decorate =
						MWM_DECOR_EVERYTHING & (~window->motif_hints.decorations);
				else
					window->decorate =
						window->motif_hints.decorations;
			}
			break;
		default:
			break;
		}
		free(reply);
	}

	if (window->pid > 0) {
		gethostname(name, sizeof(name));
		for (i = 0; i < sizeof(name); i++) {
			if (name[i] == '\0')
				break;
		}
		if (i == sizeof(name))
			name[0] = '\0'; /* ignore stupid hostnames */

		/* this is only one heuristic to guess the PID of a client is
		* valid, assuming it's compliant with icccm and ewmh.
		* Non-compliants and remote applications of course fail. */
		if (!window->machine || strcmp(window->machine, name))
			window->pid = 0;
	}
}

#undef TYPE_WM_PROTOCOLS
#undef TYPE_MOTIF_WM_HINTS
#undef TYPE_NET_WM_STATE
#undef TYPE_WM_NORMAL_HINTS

static void
weston_wm_window_get_frame_size(struct weston_wm_window *window,
				int *width, int *height)
{
	struct theme *t = window->wm->theme;

	if (window->fullscreen) {
		*width = window->width;
		*height = window->height;
	} else if (window->decorate && window->frame) {
		*width = frame_width(window->frame);
		*height = frame_height(window->frame);
	} else {
		*width = window->width + t->margin * 2;
		*height = window->height + t->margin * 2;
	}
}

static void
weston_wm_window_get_child_position(struct weston_wm_window *window,
				    int *x, int *y)
{
	struct theme *t = window->wm->theme;

	if (window->fullscreen) {
		*x = 0;
		*y = 0;
	} else if (window->decorate && window->frame) {
		frame_interior(window->frame, x, y, NULL, NULL);
	} else {
		*x = t->margin;
		*y = t->margin;
	}
}

static void
weston_wm_window_send_configure_notify(struct weston_wm_window *window)
{
	xcb_configure_notify_event_t configure_notify;
	struct weston_wm *wm = window->wm;
	int x, y;
	int32_t dx = 0, dy = 0;
	const struct weston_desktop_xwayland_interface *xwayland_api =
		wm->server->compositor->xwayland_interface;

	if (window->override_redirect) {
		/* Some clever application has changed the override redirect
		 * flag on an existing window. We didn't see it at map time,
		 * so have no idea what to do with it now. Log and leave.
		 */
		wm_printf(wm, "XWM warning: Can't send XCB_CONFIGURE_NOTIFY to"
			  " window %d which was mapped override redirect\n",
			  window->id);
		return;
	}

	weston_wm_window_get_child_position(window, &x, &y);
	/* Synthetic ConfigureNotify events must be relative to the root
	 * window, so get our offset if we're mapped. */
	if (window->shsurf)
		xwayland_api->get_position(window->shsurf, &dx, &dy);
	configure_notify.response_type = XCB_CONFIGURE_NOTIFY;
	configure_notify.pad0 = 0;
	configure_notify.event = window->id;
	configure_notify.window = window->id;
	configure_notify.above_sibling = XCB_WINDOW_NONE;
	configure_notify.x = x + dx;
	configure_notify.y = y + dy;
	configure_notify.width = window->width;
	configure_notify.height = window->height;
	configure_notify.border_width = 0;
	configure_notify.override_redirect = 0;
	configure_notify.pad1 = 0;

	xcb_send_event(wm->conn, 0, window->id,
		       XCB_EVENT_MASK_STRUCTURE_NOTIFY,
		       (char *) &configure_notify);
}

static void
weston_wm_configure_window(struct weston_wm *wm, xcb_window_t window_id,
			   uint16_t mask, const uint32_t *values)
{
	static const struct {
		xcb_config_window_t bitmask;
		const char *name;
	} names[] = {
		{ XCB_CONFIG_WINDOW_X, "x" },
		{ XCB_CONFIG_WINDOW_Y, "y" },
		{ XCB_CONFIG_WINDOW_WIDTH, "width" },
		{ XCB_CONFIG_WINDOW_HEIGHT, "height" },
		{ XCB_CONFIG_WINDOW_BORDER_WIDTH, "border_width" },
		{ XCB_CONFIG_WINDOW_SIBLING, "sibling" },
		{ XCB_CONFIG_WINDOW_STACK_MODE, "stack_mode" },
	};
	char *buf = NULL;
	size_t sz = 0;
	FILE *fp;
	unsigned i, v;

	xcb_configure_window(wm->conn, window_id, mask, values);

	if (!wm_debug_is_enabled(wm))
		return;

	fp = open_memstream(&buf, &sz);
	if (!fp)
		return;

	fprintf(fp, "XWM: configure window %d:", window_id);
	for (i = 0, v = 0; i < ARRAY_LENGTH(names); i++) {
		if (mask & names[i].bitmask)
			fprintf(fp, " %s=%d", names[i].name, values[v++]);
	}
	fclose(fp);

	wm_printf(wm, "%s\n", buf);
	free(buf);
}

static void
weston_wm_window_configure_frame(struct weston_wm_window *window)
{
	uint16_t mask;
	uint32_t values[2];
	int width, height;

	if (!window->frame_id)
		return;

	weston_wm_window_get_frame_size(window, &width, &height);
	values[0] = width;
	values[1] = height;
	mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	weston_wm_configure_window(window->wm, window->frame_id, mask, values);
}

static void
weston_wm_handle_configure_request(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_configure_request_event_t *configure_request =
		(xcb_configure_request_event_t *) event;
	struct weston_wm_window *window;
	uint32_t values[16];
	uint16_t mask;
	int x, y;
	int i = 0;

	wm_printf(wm, "XCB_CONFIGURE_REQUEST (window %d) %d,%d @ %dx%d\n",
		  configure_request->window,
		  configure_request->x, configure_request->y,
		  configure_request->width, configure_request->height);

	if (!wm_lookup_window(wm, configure_request->window, &window))
		return;

	/* If we see this, a window's override_redirect state has changed
	 * after it was mapped, and we don't really know what to do about
	 * that.
	 */
	if (window->override_redirect)
		return;

	if (window->fullscreen) {
		weston_wm_window_send_configure_notify(window);
		return;
	}

	if (configure_request->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
		window->width = configure_request->width;
		if (!weston_wm_window_is_maximized(window))
			window->saved_width = window->width;
	}
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
		window->height = configure_request->height;
		if (!weston_wm_window_is_maximized(window))
			window->saved_height = window->height;
	}

	if (window->frame) {
		weston_wm_window_set_allow_commits(window, false);
		frame_resize_inside(window->frame, window->width, window->height);
	}

	weston_wm_window_get_child_position(window, &x, &y);
	values[i++] = x;
	values[i++] = y;
	values[i++] = window->width;
	values[i++] = window->height;
	values[i++] = 0;
	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		XCB_CONFIG_WINDOW_BORDER_WIDTH;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
		values[i++] = configure_request->sibling;
		mask |= XCB_CONFIG_WINDOW_SIBLING;
	}
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
		values[i++] = configure_request->stack_mode;
		mask |= XCB_CONFIG_WINDOW_STACK_MODE;
	}

	weston_wm_configure_window(wm, window->id, mask, values);
	weston_wm_window_configure_frame(window);
	weston_wm_window_send_configure_notify(window);
	weston_wm_window_schedule_repaint(window);
}

static int
our_resource(struct weston_wm *wm, uint32_t id)
{
	const xcb_setup_t *setup;

	setup = xcb_get_setup(wm->conn);

	return (id & ~setup->resource_id_mask) == setup->resource_id_base;
}

static void
weston_wm_handle_configure_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_configure_notify_event_t *configure_notify =
		(xcb_configure_notify_event_t *) event;
	const struct weston_desktop_xwayland_interface *xwayland_api =
		wm->server->compositor->xwayland_interface;
	struct weston_wm_window *window;

	wm_printf(wm, "XCB_CONFIGURE_NOTIFY (window %d) %d,%d @ %dx%d%s\n",
		  configure_notify->window,
		  configure_notify->x, configure_notify->y,
		  configure_notify->width, configure_notify->height,
		  configure_notify->override_redirect ? ", override" : "");

	if (!wm_lookup_window(wm, configure_notify->window, &window))
		return;

	window->pos.c = weston_coord(configure_notify->x,
				     configure_notify->y);
	window->pos_dirty = false;

	if (window->override_redirect) {
		window->width = configure_notify->width;
		window->height = configure_notify->height;
		if (window->frame)
			frame_resize_inside(window->frame,
					    window->width, window->height);

		/* We should check if shsurf has been created because sometimes
		 * there are races
		 * (configure_notify is sent before xserver_map_surface) */
		if (window->shsurf)
			xwayland_api->set_xwayland(window->shsurf,
						   window->pos);
	}
}

static void
weston_wm_kill_client(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct weston_wm_window *window = get_wm_window(surface);
	if (!window)
		return;

	if (window->pid > 0)
		kill(window->pid, SIGKILL);
}

static void
weston_wm_create_surface(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct weston_wm *wm =
		container_of(listener,
			     struct weston_wm, create_surface_listener);
	struct weston_wm_window *window;

	if (wm->shell_bound)
		return;

	if (wl_resource_get_client(surface->resource) != wm->server->client)
		return;

	wm_printf(wm, "XWM: create weston_surface %p\n", surface);

	wl_list_for_each(window, &wm->unpaired_window_list, link)
		if (window->surface_id ==
		    wl_resource_get_id(surface->resource)) {
			xserver_map_shell_surface(window, surface);
			window->surface_id = 0;
			wl_list_remove(&window->link);
			wl_list_init(&window->link);
			break;
		}
}

static void
weston_wm_send_focus_window(struct weston_wm *wm,
			    struct weston_wm_window *window)
{
	if (window) {
		uint32_t values[1];

		if (window->override_redirect)
			return;

		if (window->take_focus) {
			/* Set a property to get a roundtrip
			 * with a timestamp for WM_TAKE_FOCUS */
			xcb_change_property(wm->conn,
					    XCB_PROP_MODE_REPLACE,
					    window->id,
					    wm->atom.weston_focus_ping,
					    XCB_ATOM_STRING,
					    8, /* format */
					    0, NULL);
		}

		xcb_set_input_focus (wm->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
				     window->id, XCB_TIME_CURRENT_TIME);

		values[0] = XCB_STACK_MODE_ABOVE;
		weston_wm_configure_window(wm, window->frame_id,
					   XCB_CONFIG_WINDOW_STACK_MODE,
					   values);
	} else {
		xcb_set_input_focus (wm->conn,
				     XCB_INPUT_FOCUS_POINTER_ROOT,
				     wm->no_focus_window,
				     XCB_TIME_CURRENT_TIME);
	}
}

static void
weston_wm_window_activate(struct wl_listener *listener, void *data)
{
	struct weston_surface_activation_data *activation_data = data;
	struct weston_surface *surface = activation_data->view->surface;
	struct weston_wm_window *window = NULL;
	struct weston_wm *wm =
		container_of(listener, struct weston_wm, activate_listener);

	if (surface) {
		window = get_wm_window(surface);
	}

	if (wm->focus_window == window)
		return;

	if (window) {
		weston_wm_set_net_active_window(wm, window->id);
	} else {
		weston_wm_set_net_active_window(wm, wm->no_focus_window);
	}

	weston_wm_send_focus_window(wm, window);

	if (wm->focus_window) {
		if (wm->focus_window->frame)
			frame_unset_flag(wm->focus_window->frame, FRAME_FLAG_ACTIVE);
		weston_wm_window_schedule_repaint(wm->focus_window);
	}
	wm->focus_window = window;
	if (wm->focus_window) {
		if (wm->focus_window->frame)
			frame_set_flag(wm->focus_window->frame, FRAME_FLAG_ACTIVE);
		weston_wm_window_schedule_repaint(wm->focus_window);
	}

	xcb_flush(wm->conn);

}

/** Control Xwayland wl_surface.commit behaviour
 *
 * This function sets the "_XWAYLAND_ALLOW_COMMITS" property of the frame window
 * (not the content window!) to \p allow.
 *
 * If the property is set to \c true, Xwayland will commit whenever it likes.
 * If the property is set to \c false, Xwayland will not commit.
 * If the property is not set at all, Xwayland assumes it is \c true.
 *
 * \param window The XWM window to control.
 * \param allow Whether Xwayland is allowed to wl_surface.commit for the window.
 */
static void
weston_wm_window_set_allow_commits(struct weston_wm_window *window, bool allow)
{
	struct weston_wm *wm = window->wm;
	uint32_t property[1];

	assert(window->frame_id != XCB_WINDOW_NONE);

	wm_printf(wm, "XWM: window %d set _XWAYLAND_ALLOW_COMMITS = %s\n",
		  window->id, allow ? "true" : "false");

	property[0] = allow ? 1 : 0;

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    window->frame_id,
			    wm->atom.allow_commits,
			    XCB_ATOM_CARDINAL,
			    32, /* format */
			    1, property);
	xcb_flush(wm->conn);
}

#define ICCCM_WITHDRAWN_STATE	0
#define ICCCM_NORMAL_STATE	1
#define ICCCM_ICONIC_STATE	3

static void
weston_wm_window_set_wm_state(struct weston_wm_window *window, int32_t state)
{
	struct weston_wm *wm = window->wm;
	uint32_t property[2];

	property[0] = state;
	property[1] = XCB_WINDOW_NONE;

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    window->id,
			    wm->atom.wm_state,
			    wm->atom.wm_state,
			    32, /* format */
			    2, property);
}

static void
weston_wm_window_set_net_frame_extents(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;
	uint32_t property[4];
	int top = 0, bottom = 0, left = 0, right = 0;

	if (!window->fullscreen)
		frame_decoration_sizes(window->frame, &top, &bottom, &left, &right);

	if (window->decor_top == top && window->decor_bottom == bottom &&
	    window->decor_left == left && window->decor_right == right)
		return;

	window->decor_top = top;
	window->decor_bottom = bottom;
	window->decor_left = left;
	window->decor_right = right;

	property[0] = left;
	property[1] = right;
	property[2] = top;
	property[3] = bottom;
	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    window->id,
			    wm->atom.net_frame_extents,
			    XCB_ATOM_CARDINAL,
			    32, /* format */
			    4, property);
}

static void
weston_wm_window_set_net_wm_state(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;
	uint32_t property[3];
	int i;

	i = 0;
	if (window->fullscreen)
		property[i++] = wm->atom.net_wm_state_fullscreen;
	if (window->maximized_vert)
		property[i++] = wm->atom.net_wm_state_maximized_vert;
	if (window->maximized_horz)
		property[i++] = wm->atom.net_wm_state_maximized_horz;

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    window->id,
			    wm->atom.net_wm_state,
			    XCB_ATOM_ATOM,
			    32, /* format */
			    i, property);
}

static void
weston_wm_window_create_frame(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;
	uint32_t values[3];
	int x, y, width, height;
	int buttons = FRAME_BUTTON_CLOSE;

	if (window->decorate & MWM_DECOR_MAXIMIZE)
		buttons |= FRAME_BUTTON_MAXIMIZE;

	if (window->decorate & MWM_DECOR_MINIMIZE)
		buttons |= FRAME_BUTTON_MINIMIZE;

	window->frame = frame_create(window->wm->theme,
				     window->width, window->height,
				     buttons, window->name, NULL);

	if (!window->frame)
		return;

	frame_resize_inside(window->frame, window->width, window->height);

	weston_wm_window_get_frame_size(window, &width, &height);
	weston_wm_window_get_child_position(window, &x, &y);

	values[0] = wm->screen->black_pixel;
	values[1] =
		XCB_EVENT_MASK_KEY_PRESS |
		XCB_EVENT_MASK_KEY_RELEASE |
		XCB_EVENT_MASK_BUTTON_PRESS |
		XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_POINTER_MOTION |
		XCB_EVENT_MASK_ENTER_WINDOW |
		XCB_EVENT_MASK_LEAVE_WINDOW |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
	values[2] = wm->colormap;

	window->frame_id = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  32,
			  window->frame_id,
			  wm->screen->root,
			  0, 0,
			  width, height,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  wm->visual_id,
			  XCB_CW_BORDER_PIXEL |
			  XCB_CW_EVENT_MASK |
			  XCB_CW_COLORMAP, values);

	xcb_reparent_window(wm->conn, window->id, window->frame_id, x, y);

	values[0] = 0;
	weston_wm_configure_window(wm, window->id,
				   XCB_CONFIG_WINDOW_BORDER_WIDTH, values);

	window->cairo_surface =
		cairo_xcb_surface_create_with_xrender_format(wm->conn,
							     wm->screen,
							     window->frame_id,
							     &wm->format_rgba,
							     width, height);

	hash_table_insert(wm->window_hash, window->frame_id, window);
	weston_wm_window_send_configure_notify(window);
}

/*
 * Sets the _NET_WM_DESKTOP property for the window to 'desktop'.
 * Passing a <0 desktop value deletes the property.
 */
static void
weston_wm_window_set_virtual_desktop(struct weston_wm_window *window,
				     int desktop)
{
	if (desktop >= 0) {
		xcb_change_property(window->wm->conn,
		                    XCB_PROP_MODE_REPLACE,
		                    window->id,
		                    window->wm->atom.net_wm_desktop,
		                    XCB_ATOM_CARDINAL,
		                    32, /* format */
		                    1, &desktop);
	} else {
		xcb_delete_property(window->wm->conn,
		                    window->id,
		                    window->wm->atom.net_wm_desktop);
	}
}

static void
weston_wm_handle_map_request(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_map_request_event_t *map_request =
		(xcb_map_request_event_t *) event;
	struct weston_wm_window *window;
	struct weston_output *output;

	if (our_resource(wm, map_request->window)) {
		wm_printf(wm, "XCB_MAP_REQUEST (window %d, ours)\n",
			  map_request->window);
		return;
	}

	if (!wm_lookup_window(wm, map_request->window, &window))
		return;

	weston_wm_window_read_properties(window);

	/* For a new Window, MapRequest happens before the Window is realized
	 * in Xwayland. We do the real xcb_map_window() here as a response to
	 * MapRequest. The Window will get realized (wl_surface created in
	 * Wayland and WL_SURFACE_ID sent in X11) when it has been mapped for
	 * real.
	 *
	 * MapRequest only happens for (X11) unmapped Windows. On UnmapNotify,
	 * we reset shsurf to NULL, so even if X11 connection races far ahead
	 * of the Wayland connection and the X11 client is repeatedly mapping
	 * and unmapping, we will never have shsurf set on MapRequest.
	 */
	assert(!window->shsurf);

	window->map_request_valid = true;
	window->map_request = window->pos;

	if (window->frame_id == XCB_WINDOW_NONE)
		weston_wm_window_create_frame(window); /* sets frame_id */
	assert(window->frame_id != XCB_WINDOW_NONE);

	wm_printf(wm, "XCB_MAP_REQUEST (window %d, %p, frame %d, %dx%d @ %d,%d)\n",
		  window->id, window, window->frame_id,
		  window->width, window->height,
		  (int)window->map_request.c.x, (int)window->map_request.c.y);

	weston_wm_window_set_allow_commits(window, false);
	weston_wm_window_set_wm_state(window, ICCCM_NORMAL_STATE);
	weston_wm_window_set_net_wm_state(window);
	weston_wm_window_set_virtual_desktop(window, 0);

	if (legacy_fullscreen(wm, window, &output)) {
		window->fullscreen = 1;
		weston_output_weak_ref_set(&window->legacy_fullscreen_output,
					   output);
	}

	xcb_map_window(wm->conn, map_request->window);
	xcb_map_window(wm->conn, window->frame_id);

	/* Mapped in the X server, we can draw immediately.
	 * Cannot set pending state though, no weston_surface until
	 * xserver_map_shell_surface() time. */
	weston_wm_window_schedule_repaint(window);
}

static void
weston_wm_handle_map_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_map_notify_event_t *map_notify = (xcb_map_notify_event_t *) event;

	if (our_resource(wm, map_notify->window)) {
		wm_printf(wm, "XCB_MAP_NOTIFY (window %d, ours)\n",
			  map_notify->window);
			return;
	}

	wm_printf(wm, "XCB_MAP_NOTIFY (window %d%s)\n", map_notify->window,
		  map_notify->override_redirect ? ", override" : "");
}

static void
weston_wm_handle_unmap_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_unmap_notify_event_t *unmap_notify =
		(xcb_unmap_notify_event_t *) event;
	struct weston_wm_window *window;

	wm_printf(wm, "XCB_UNMAP_NOTIFY (window %d, event %d%s)\n",
		  unmap_notify->window,
		  unmap_notify->event,
		  our_resource(wm, unmap_notify->window) ? ", ours" : "");

	if (our_resource(wm, unmap_notify->window))
		return;

	if (unmap_notify->response_type & SEND_EVENT_MASK)
		/* We just ignore the ICCCM 4.1.4 synthetic unmap notify
		 * as it may come in after we've destroyed the window. */
		return;

	if (!wm_lookup_window(wm, unmap_notify->window, &window))
		return;

	if (window->surface_id) {
		/* Make sure we're not on the unpaired surface list or we
		 * could be assigned a surface during surface creation that
		 * was mapped before this unmap request.
		 */
		wl_list_remove(&window->link);
		wl_list_init(&window->link);
		window->surface_id = 0;
	}
	if (wm->focus_window == window)
		wm->focus_window = NULL;
	if (window->surface)
		wl_list_remove(&window->surface_destroy_listener.link);
	window->surface = NULL;
	window->shsurf = NULL;

	weston_wm_window_set_wm_state(window, ICCCM_WITHDRAWN_STATE);
	weston_wm_window_set_virtual_desktop(window, -1);

	xcb_unmap_window(wm->conn, window->frame_id);
}

static void
weston_wm_window_draw_decoration(struct weston_wm_window *window)
{
	cairo_t *cr;
	int width, height;
	const char *how;

	weston_wm_window_get_frame_size(window, &width, &height);

	cairo_xcb_surface_set_size(window->cairo_surface, width, height);
	cr = cairo_create(window->cairo_surface);

	if (window->fullscreen) {
		how = "fullscreen";
		/* nothing */
	} else if (window->decorate) {
		how = "decorate";
		frame_set_title(window->frame, window->name);
		frame_repaint(window->frame, cr);
	} else {
		how = "shadow";
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_rgba(cr, 0, 0, 0, 0);
		cairo_paint(cr);

		render_shadow(cr, window->wm->theme->shadow,
			      2, 2, width + 8, height + 8, 64, 64);
	}

	wm_printf(window->wm, "XWM: draw decoration, win %d, %s\n",
		  window->id, how);

	cairo_destroy(cr);
	cairo_surface_flush(window->cairo_surface);
	xcb_flush(window->wm->conn);
}

static void
weston_wm_window_set_pending_state(struct weston_wm_window *window)
{
	int x, y, width, height;
	int32_t input_x, input_y, input_w, input_h;
	const struct weston_desktop_xwayland_interface *xwayland_interface =
		window->wm->server->compositor->xwayland_interface;

	if (!window->surface)
		return;

	weston_wm_window_get_frame_size(window, &width, &height);
	weston_wm_window_get_child_position(window, &x, &y);

	pixman_region32_fini(&window->surface->pending.opaque);
	if (window->has_alpha) {
		pixman_region32_init(&window->surface->pending.opaque);
	} else {
		/* We leave an extra pixel around the X window area to
		 * make sure we don't sample from the undefined alpha
		 * channel when filtering. */
		pixman_region32_init_rect(&window->surface->pending.opaque,
					  x - 1, y - 1,
					  window->width + 2,
					  window->height + 2);
	}

	if (window->decorate && !window->fullscreen) {
		frame_input_rect(window->frame, &input_x, &input_y,
				 &input_w, &input_h);
	} else {
		input_x = x;
		input_y = y;
		input_w = width;
		input_h = height;
	}

	wm_printf(window->wm, "XWM: win %d geometry: %d,%d %dx%d\n",
		  window->id, input_x, input_y, input_w, input_h);

	pixman_region32_fini(&window->surface->pending.input);
	pixman_region32_init_rect(&window->surface->pending.input,
				  input_x, input_y, input_w, input_h);
	window->surface->pending.status |= WESTON_SURFACE_DIRTY_INPUT;

	xwayland_interface->set_window_geometry(window->shsurf,
						input_x, input_y,
						input_w, input_h);
	if (window->name)
		xwayland_interface->set_title(window->shsurf, window->name);
	if (window->pid > 0)
		xwayland_interface->set_pid(window->shsurf, window->pid);
}

static void
weston_wm_window_do_repaint(void *data)
{
	struct weston_wm_window *window = data;

	window->repaint_source = NULL;

	weston_wm_window_set_allow_commits(window, false);
	weston_wm_window_read_properties(window);

	weston_wm_window_draw_decoration(window);
	weston_wm_window_set_net_frame_extents(window);
	weston_wm_window_set_pending_state(window);
	weston_wm_window_set_allow_commits(window, true);
}

static void
weston_wm_window_set_pending_state_OR(struct weston_wm_window *window)
{
	int width, height;

	/* for override-redirect windows */
	assert(window->frame_id == XCB_WINDOW_NONE);

	if (!window->surface)
		return;

	weston_wm_window_get_frame_size(window, &width, &height);
	pixman_region32_fini(&window->surface->pending.opaque);
	if (window->has_alpha) {
		pixman_region32_init(&window->surface->pending.opaque);
	} else {
		pixman_region32_init_rect(&window->surface->pending.opaque, 0, 0,
					  width, height);
	}
}

static void
weston_wm_window_schedule_repaint(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;

	if (window->frame_id == XCB_WINDOW_NONE) {
		/* Override-redirect windows go through here, but we
		 * cannot assert(window->override_redirect); because
		 * we do not deal with changing OR flag yet.
		 * XXX: handle OR flag changes in message handlers
		 */
		weston_wm_window_set_pending_state_OR(window);
		return;
	}

	if (window->repaint_source)
		return;

	wm_printf(wm, "XWM: schedule repaint, win %d\n", window->id);

	window->repaint_source =
		wl_event_loop_add_idle(wm->server->loop,
				       weston_wm_window_do_repaint, window);
}

static void
weston_wm_handle_property_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_property_notify_event_t *property_notify =
		(xcb_property_notify_event_t *) event;
	struct weston_wm_window *window;
	FILE *fp = NULL;
	char *logstr;
	size_t logsize;
	char timestr[128];

	if (!wm_lookup_window(wm, property_notify->window, &window))
		return;

	/* We set the weston_focus_ping property on this window to
	 * get a timestamp to send a WM_TAKE_FOCUS... send it now,
	 * or just return if this is confirming we deleted the
	 * property.
	 */
	if (property_notify->atom == wm->atom.weston_focus_ping) {
		xcb_client_message_event_t client_message;

		if (property_notify->state == XCB_PROPERTY_DELETE)
			return;

		/* delete our ping property */
		xcb_delete_property(window->wm->conn,
		                    window->id,
		                    window->wm->atom.weston_focus_ping);

		client_message.response_type = XCB_CLIENT_MESSAGE;
		client_message.format = 32;
		client_message.window = window->id;
		client_message.type = wm->atom.wm_protocols;
		client_message.data.data32[0] = wm->atom.wm_take_focus;
		client_message.data.data32[1] = property_notify->time;
		xcb_send_event(wm->conn, 0, window->id,
			       XCB_EVENT_MASK_NO_EVENT,
			       (char *) &client_message);

		return;
	}

	window->properties_dirty = 1;

	if (wm_debug_is_enabled(wm))
		fp = open_memstream(&logstr, &logsize);

	if (fp) {
		fprintf(fp, "%s XCB_PROPERTY_NOTIFY: window %d, ",
			weston_log_scope_timestamp(wm->server->wm_debug,
			timestr, sizeof timestr),
			property_notify->window);
		if (property_notify->state == XCB_PROPERTY_DELETE)
			fprintf(fp, "deleted %s\n",
					get_atom_name(wm->conn, property_notify->atom));
		else
			read_and_dump_property(fp, wm, property_notify->window,
					       property_notify->atom);

		if (fclose(fp) == 0)
			weston_log_scope_write(wm->server->wm_debug,
						 logstr, logsize);
		free(logstr);
	} else {
		/* read_and_dump_property() is a X11 roundtrip.
		 * Mimic it to maintain ordering semantics between debug
		 * and non-debug paths.
		 */
		get_atom_name(wm->conn, property_notify->atom);
	}

	if (property_notify->atom == wm->atom.net_wm_name ||
	    property_notify->atom == XCB_ATOM_WM_NAME)
		weston_wm_window_schedule_repaint(window);
}

static void
weston_wm_window_create(struct weston_wm *wm,
			xcb_window_t id, int width, int height,
			struct weston_coord_global initial_pos, int override)
{
	struct weston_wm_window *window;
	uint32_t values[1];
	xcb_get_geometry_cookie_t geometry_cookie;
	xcb_get_geometry_reply_t *geometry_reply;

	window = zalloc(sizeof *window);
	if (window == NULL) {
		wm_printf(wm, "failed to allocate window\n");
		return;
	}

	geometry_cookie = xcb_get_geometry(wm->conn, id);

	values[0] = XCB_EVENT_MASK_PROPERTY_CHANGE |
	            XCB_EVENT_MASK_FOCUS_CHANGE;
	xcb_change_window_attributes(wm->conn, id, XCB_CW_EVENT_MASK, values);

	window->wm = wm;
	window->id = id;
	window->properties_dirty = 1;
	window->override_redirect = override;
	window->width = width;
	window->height = height;
	/* Completely arbitrary defaults in case something starts
	 * maximized and we unmaximize it later - at which point 0 x 0
	 * would not be the most useful size.
	 */
	window->saved_width = 512;
	window->saved_height = 512;
	window->pos = initial_pos;
	window->pos_dirty = false;
	window->map_request_valid = false;
	window->decor_top = -1;
	window->decor_bottom = -1;
	window->decor_left = -1;
	window->decor_right = -1;
	wl_list_init(&window->link);
	weston_output_weak_ref_init(&window->legacy_fullscreen_output);

	geometry_reply = xcb_get_geometry_reply(wm->conn, geometry_cookie, NULL);
	/* technically we should use XRender and check the visual format's
	alpha_mask, but checking depth is simpler and works in all known cases */
	if (geometry_reply != NULL)
		window->has_alpha = geometry_reply->depth == 32;
	free(geometry_reply);

	hash_table_insert(wm->window_hash, id, window);
}

static void
weston_wm_window_destroy(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;

	weston_output_weak_ref_clear(&window->legacy_fullscreen_output);

	if (window->configure_source)
		wl_event_source_remove(window->configure_source);
	if (window->repaint_source)
		wl_event_source_remove(window->repaint_source);
	if (window->cairo_surface)
		cairo_surface_destroy(window->cairo_surface);

	if (window->frame_id) {
		xcb_reparent_window(wm->conn, window->id, wm->wm_window, 0, 0);
		xcb_destroy_window(wm->conn, window->frame_id);
		weston_wm_window_set_wm_state(window, ICCCM_WITHDRAWN_STATE);
		weston_wm_window_set_virtual_desktop(window, -1);
		hash_table_remove(wm->window_hash, window->frame_id);
		window->frame_id = XCB_WINDOW_NONE;
	}

	if (window->frame)
		frame_destroy(window->frame);

	wl_list_remove(&window->link);

	if (window->surface)
		wl_list_remove(&window->surface_destroy_listener.link);

	free(window->class);
	free(window->name);
	free(window->machine);

	hash_table_remove(window->wm->window_hash, window->id);
	free(window);
}

static void
weston_wm_handle_create_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_create_notify_event_t *create_notify =
		(xcb_create_notify_event_t *) event;
	struct weston_coord_global pos;

	wm_printf(wm, "XCB_CREATE_NOTIFY (window %d, at (%d, %d), width %d, height %d%s%s)\n",
		  create_notify->window,
		  create_notify->x, create_notify->y,
		  create_notify->width, create_notify->height,
		  create_notify->override_redirect ? ", override" : "",
		  our_resource(wm, create_notify->window) ? ", ours" : "");

	if (our_resource(wm, create_notify->window))
		return;

	pos.c = weston_coord(create_notify->x, create_notify->y);
	weston_wm_window_create(wm, create_notify->window,
				create_notify->width, create_notify->height,
				pos, create_notify->override_redirect);
}

static void
weston_wm_handle_destroy_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_destroy_notify_event_t *destroy_notify =
		(xcb_destroy_notify_event_t *) event;
	struct weston_wm_window *window;

	wm_printf(wm, "XCB_DESTROY_NOTIFY, win %d, event %d%s\n",
		  destroy_notify->window,
		  destroy_notify->event,
		  our_resource(wm, destroy_notify->window) ? ", ours" : "");

	if (our_resource(wm, destroy_notify->window))
		return;

	if (!wm_lookup_window(wm, destroy_notify->window, &window))
		return;

	weston_wm_window_destroy(window);
}

static void
weston_wm_handle_reparent_notify(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_reparent_notify_event_t *reparent_notify =
		(xcb_reparent_notify_event_t *) event;
	struct weston_wm_window *window;

	wm_printf(wm, "XCB_REPARENT_NOTIFY (window %d, parent %d, event %d%s)\n",
		  reparent_notify->window,
		  reparent_notify->parent,
		  reparent_notify->event,
		  reparent_notify->override_redirect ? ", override" : "");

	if (reparent_notify->parent == wm->screen->root) {
		struct weston_coord_global c;

		c.c = weston_coord(reparent_notify->x, reparent_notify->y);
		weston_wm_window_create(wm, reparent_notify->window, 10, 10,
					c, reparent_notify->override_redirect);
	} else if (!our_resource(wm, reparent_notify->parent)) {
		if (!wm_lookup_window(wm, reparent_notify->window, &window))
			return;
		weston_wm_window_destroy(window);
	}
}

struct weston_seat *
weston_wm_pick_seat(struct weston_wm *wm)
{
	struct wl_list *seats = wm->server->compositor->seat_list.next;
	if (wl_list_empty(seats))
		return NULL;
	return container_of(seats, struct weston_seat, link);
}

static struct weston_seat *
weston_wm_pick_seat_for_window(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;
	struct weston_seat *seat, *s;

	seat = NULL;
	wl_list_for_each(s, &wm->server->compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(s);
		struct weston_pointer *old_pointer =
			weston_seat_get_pointer(seat);

		if (pointer && pointer->focus &&
		    pointer->focus->surface == window->surface &&
		    pointer->button_count > 0 &&
		    (!seat ||
		     pointer->grab_serial -
		     old_pointer->grab_serial < (1 << 30)))
			seat = s;
	}

	return seat;
}

static void
weston_wm_window_handle_moveresize(struct weston_wm_window *window,
				   xcb_client_message_event_t *client_message)
{
	static const int map[] = {
		THEME_LOCATION_RESIZING_TOP_LEFT,
		THEME_LOCATION_RESIZING_TOP,
		THEME_LOCATION_RESIZING_TOP_RIGHT,
		THEME_LOCATION_RESIZING_RIGHT,
		THEME_LOCATION_RESIZING_BOTTOM_RIGHT,
		THEME_LOCATION_RESIZING_BOTTOM,
		THEME_LOCATION_RESIZING_BOTTOM_LEFT,
		THEME_LOCATION_RESIZING_LEFT
	};

	struct weston_wm *wm = window->wm;
	struct weston_seat *seat = weston_wm_pick_seat_for_window(window);
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	int detail;
	const struct weston_desktop_xwayland_interface *xwayland_interface =
		wm->server->compositor->xwayland_interface;

	if (!pointer || pointer->button_count != 1
	    || !pointer->focus
	    || pointer->focus->surface != window->surface)
		return;

	detail = client_message->data.data32[2];
	switch (detail) {
	case _NET_WM_MOVERESIZE_MOVE:
		xwayland_interface->move(window->shsurf, pointer);
		break;
	case _NET_WM_MOVERESIZE_SIZE_TOPLEFT:
	case _NET_WM_MOVERESIZE_SIZE_TOP:
	case _NET_WM_MOVERESIZE_SIZE_TOPRIGHT:
	case _NET_WM_MOVERESIZE_SIZE_RIGHT:
	case _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT:
	case _NET_WM_MOVERESIZE_SIZE_BOTTOM:
	case _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT:
	case _NET_WM_MOVERESIZE_SIZE_LEFT:
		xwayland_interface->resize(window->shsurf, pointer, map[detail]);
		break;
	case _NET_WM_MOVERESIZE_CANCEL:
		break;
	}
}

#define _NET_WM_STATE_REMOVE	0
#define _NET_WM_STATE_ADD	1
#define _NET_WM_STATE_TOGGLE	2

static int
update_state(int action, int *state)
{
	int new_state, changed;

	switch (action) {
	case _NET_WM_STATE_REMOVE:
		new_state = 0;
		break;
	case _NET_WM_STATE_ADD:
		new_state = 1;
		break;
	case _NET_WM_STATE_TOGGLE:
		new_state = !*state;
		break;
	default:
		return 0;
	}

	changed = (*state != new_state);
	*state = new_state;

	return changed;
}

static void
weston_wm_window_configure(void *data);

static void
weston_wm_window_set_toplevel(struct weston_wm_window *window)
{
	const struct weston_desktop_xwayland_interface *xwayland_interface =
		window->wm->server->compositor->xwayland_interface;

	xwayland_interface->set_toplevel(window->shsurf);
	window->width = window->saved_width;
	window->height = window->saved_height;
	if (window->frame) {
		frame_unset_flag(window->frame, FRAME_FLAG_MAXIMIZED);
		frame_resize_inside(window->frame,
					window->width,
					window->height);
	}
	weston_wm_window_configure(window);
}

static void
weston_wm_window_handle_state(struct weston_wm_window *window,
			      xcb_client_message_event_t *client_message)
{
	struct weston_wm *wm = window->wm;
	const struct weston_desktop_xwayland_interface *xwayland_interface =
		wm->server->compositor->xwayland_interface;
	uint32_t action, property1, property2;
	int maximized = weston_wm_window_is_maximized(window);

	action = client_message->data.data32[0];
	property1 = client_message->data.data32[1];
	property2 = client_message->data.data32[2];

	if ((property1 == wm->atom.net_wm_state_fullscreen ||
	     property2 == wm->atom.net_wm_state_fullscreen) &&
	    update_state(action, &window->fullscreen)) {
		weston_wm_window_set_net_wm_state(window);
		if (window->fullscreen) {
			window->saved_width = window->width;
			window->saved_height = window->height;

			if (window->shsurf)
				xwayland_interface->set_fullscreen(window->shsurf,
								   NULL);
		} else {
			if (window->shsurf)
				weston_wm_window_set_toplevel(window);
		}
	} else {
		if ((property1 == wm->atom.net_wm_state_maximized_vert ||
		     property2 == wm->atom.net_wm_state_maximized_vert) &&
		    update_state(action, &window->maximized_vert))
			weston_wm_window_set_net_wm_state(window);
		if ((property1 == wm->atom.net_wm_state_maximized_horz ||
		     property2 == wm->atom.net_wm_state_maximized_horz) &&
		    update_state(action, &window->maximized_horz))
			weston_wm_window_set_net_wm_state(window);

		if (maximized != weston_wm_window_is_maximized(window)) {
			if (weston_wm_window_is_maximized(window)) {
				window->saved_width = window->width;
				window->saved_height = window->height;

				if (window->shsurf)
					xwayland_interface->set_maximized(window->shsurf);
			} else if (window->shsurf) {
				weston_wm_window_set_toplevel(window);
			}
		}
	}
}

static void
weston_wm_window_handle_iconic_state(struct weston_wm_window *window,
				     xcb_client_message_event_t *client_message)
{
	struct weston_wm *wm = window->wm;
	const struct weston_desktop_xwayland_interface *xwayland_interface =
		wm->server->compositor->xwayland_interface;
	uint32_t iconic_state;

	if (!window->shsurf)
		return;

	iconic_state = client_message->data.data32[0];

	if (iconic_state == ICCCM_ICONIC_STATE) {
		/* If window is currently in maximized or fullscreen state,
		 * don't override saved size.
		 */
		if (!weston_wm_window_is_maximized(window) &&
		    !window->fullscreen) {
			window->saved_height = window->height;
			window->saved_width = window->width;
		}
		xwayland_interface->set_minimized(window->shsurf);
	}
}

static void
surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_wm_window *window =
		container_of(listener,
			     struct weston_wm_window, surface_destroy_listener);

	wm_printf(window->wm, "surface for xid %d destroyed\n", window->id);

	/* This should have been freed by the shell.
	 * Don't try to use it later. */
	window->shsurf = NULL;
	window->surface = NULL;
}

static void
weston_wm_window_handle_surface_id(struct weston_wm_window *window,
				   xcb_client_message_event_t *client_message)
{
	struct weston_wm *wm = window->wm;
	struct wl_resource *resource;

	assert(!wm->shell_bound);

	if (window->surface_id != 0) {
		wm_printf(wm, "already have surface id for window %d\n",
			  window->id);
		return;
	}

	/* Xwayland will send the wayland requests to create the
	 * wl_surface before sending this client message.  Even so, we
	 * can end up handling the X event before the wayland requests
	 * and thus when we try to look up the surface ID, the surface
	 * hasn't been created yet.  In that case put the window on
	 * the unpaired window list and continue when the surface gets
	 * created. */
	uint32_t id = client_message->data.data32[0];
	resource = wl_client_get_object(wm->server->client, id);
	if (resource) {
		window->surface_id = 0;
		xserver_map_shell_surface(window,
					  wl_resource_get_user_data(resource));
	}
	else {
		window->surface_id = id;
		wl_list_insert(&wm->unpaired_window_list, &window->link);
	}
}

static void
weston_wm_window_handle_surface_serial(struct weston_wm_window *window,
				       xcb_client_message_event_t *client_message)
{
	struct xwl_surface *xsurf, *next;
	struct weston_wm *wm = window->wm;
	uint64_t serial = u64_from_u32s(client_message->data.data32[1],
					client_message->data.data32[0]);

	window->surface_serial = serial;
	wl_list_remove(&window->link);
	wl_list_init(&window->link);

	wl_list_for_each_safe(xsurf, next, &wm->unpaired_surface_list, link) {
		if (window->surface_serial == xsurf->serial) {
			xserver_map_shell_surface(window, xsurf->weston_surface);
			wl_list_remove(&xsurf->link);
			wl_list_init(&xsurf->link);
			return;
		}
	}
	wl_list_insert(&wm->unpaired_window_list, &window->link);
}

static void
weston_wm_handle_client_message(struct weston_wm *wm,
				xcb_generic_event_t *event)
{
	xcb_client_message_event_t *client_message =
		(xcb_client_message_event_t *) event;
	struct weston_wm_window *window;

	wm_printf(wm, "XCB_CLIENT_MESSAGE (%s %d %d %d %d %d win %d)\n",
		  get_atom_name(wm->conn, client_message->type),
		  client_message->data.data32[0],
		  client_message->data.data32[1],
		  client_message->data.data32[2],
		  client_message->data.data32[3],
		  client_message->data.data32[4],
		  client_message->window);

	/* The window may get created and destroyed before we actually
	 * handle the message.  If it doesn't exist, bail.
	 */
	if (!wm_lookup_window(wm, client_message->window, &window))
		return;

	if (client_message->type == wm->atom.net_wm_moveresize)
		weston_wm_window_handle_moveresize(window, client_message);
	else if (client_message->type == wm->atom.net_wm_state)
		weston_wm_window_handle_state(window, client_message);
	else if (client_message->type == wm->atom.wl_surface_id &&
		 !wm->shell_bound)
		weston_wm_window_handle_surface_id(window, client_message);
	else if (client_message->type == wm->atom.wm_change_state)
		weston_wm_window_handle_iconic_state(window, client_message);
	else if (client_message->type == wm->atom.wl_surface_serial)
		weston_wm_window_handle_surface_serial(window, client_message);
}

enum cursor_type {
	XWM_CURSOR_TOP,
	XWM_CURSOR_BOTTOM,
	XWM_CURSOR_LEFT,
	XWM_CURSOR_RIGHT,
	XWM_CURSOR_TOP_LEFT,
	XWM_CURSOR_TOP_RIGHT,
	XWM_CURSOR_BOTTOM_LEFT,
	XWM_CURSOR_BOTTOM_RIGHT,
	XWM_CURSOR_LEFT_PTR,
};

/*
 * The following correspondences between file names and cursors was copied
 * from: https://bugs.kde.org/attachment.cgi?id=67313
 */

static const char *bottom_left_corners[] = {
	"bottom_left_corner",
	"sw-resize",
	"size_bdiag"
};

static const char *bottom_right_corners[] = {
	"bottom_right_corner",
	"se-resize",
	"size_fdiag"
};

static const char *bottom_sides[] = {
	"bottom_side",
	"s-resize",
	"size_ver"
};

static const char *left_ptrs[] = {
	"left_ptr",
	"default",
	"top_left_arrow",
	"left-arrow"
};

static const char *left_sides[] = {
	"left_side",
	"w-resize",
	"size_hor"
};

static const char *right_sides[] = {
	"right_side",
	"e-resize",
	"size_hor"
};

static const char *top_left_corners[] = {
	"top_left_corner",
	"nw-resize",
	"size_fdiag"
};

static const char *top_right_corners[] = {
	"top_right_corner",
	"ne-resize",
	"size_bdiag"
};

static const char *top_sides[] = {
	"top_side",
	"n-resize",
	"size_ver"
};

struct cursor_alternatives {
	const char **names;
	size_t count;
};

static const struct cursor_alternatives cursors[] = {
	{top_sides, ARRAY_LENGTH(top_sides)},
	{bottom_sides, ARRAY_LENGTH(bottom_sides)},
	{left_sides, ARRAY_LENGTH(left_sides)},
	{right_sides, ARRAY_LENGTH(right_sides)},
	{top_left_corners, ARRAY_LENGTH(top_left_corners)},
	{top_right_corners, ARRAY_LENGTH(top_right_corners)},
	{bottom_left_corners, ARRAY_LENGTH(bottom_left_corners)},
	{bottom_right_corners, ARRAY_LENGTH(bottom_right_corners)},
	{left_ptrs, ARRAY_LENGTH(left_ptrs)},
};

static void
weston_wm_create_cursors(struct weston_wm *wm)
{
	const char *name;
	int i, count = ARRAY_LENGTH(cursors);
	size_t j;

	wm->cursors = malloc(count * sizeof(xcb_cursor_t));
	for (i = 0; i < count; i++) {
		for (j = 0; j < cursors[i].count; j++) {
			name = cursors[i].names[j];
			wm->cursors[i] =
				xcb_cursor_library_load_cursor(wm, name);
			if (wm->cursors[i] != (xcb_cursor_t)-1)
				break;
		}
	}

	wm->last_cursor = -1;
}

static void
weston_wm_destroy_cursors(struct weston_wm *wm)
{
	uint8_t i;

	for (i = 0; i < ARRAY_LENGTH(cursors); i++)
		xcb_free_cursor(wm->conn, wm->cursors[i]);

	free(wm->cursors);
}

static int
get_cursor_for_location(enum theme_location location)
{
	switch (location) {
		case THEME_LOCATION_RESIZING_TOP:
			return XWM_CURSOR_TOP;
		case THEME_LOCATION_RESIZING_BOTTOM:
			return XWM_CURSOR_BOTTOM;
		case THEME_LOCATION_RESIZING_LEFT:
			return XWM_CURSOR_LEFT;
		case THEME_LOCATION_RESIZING_RIGHT:
			return XWM_CURSOR_RIGHT;
		case THEME_LOCATION_RESIZING_TOP_LEFT:
			return XWM_CURSOR_TOP_LEFT;
		case THEME_LOCATION_RESIZING_TOP_RIGHT:
			return XWM_CURSOR_TOP_RIGHT;
		case THEME_LOCATION_RESIZING_BOTTOM_LEFT:
			return XWM_CURSOR_BOTTOM_LEFT;
		case THEME_LOCATION_RESIZING_BOTTOM_RIGHT:
			return XWM_CURSOR_BOTTOM_RIGHT;
		case THEME_LOCATION_EXTERIOR:
		case THEME_LOCATION_TITLEBAR:
		default:
			return XWM_CURSOR_LEFT_PTR;
	}
}

static void
weston_wm_window_set_cursor(struct weston_wm *wm, xcb_window_t window_id,
			    int cursor)
{
	uint32_t cursor_value_list;

	if (wm->last_cursor == cursor)
		return;

	wm->last_cursor = cursor;

	cursor_value_list = wm->cursors[cursor];
	xcb_change_window_attributes (wm->conn, window_id,
				      XCB_CW_CURSOR, &cursor_value_list);
	xcb_flush(wm->conn);
}

static void
weston_wm_window_close(struct weston_wm_window *window, xcb_timestamp_t time)
{
	xcb_client_message_event_t client_message;

	if (window->delete_window) {
		client_message.response_type = XCB_CLIENT_MESSAGE;
		client_message.format = 32;
		client_message.window = window->id;
		client_message.type = window->wm->atom.wm_protocols;
		client_message.data.data32[0] =
			window->wm->atom.wm_delete_window;
		client_message.data.data32[1] = time;

		xcb_send_event(window->wm->conn, 0, window->id,
			       XCB_EVENT_MASK_NO_EVENT,
			       (char *) &client_message);
	} else {
		xcb_kill_client(window->wm->conn, window->id);
	}
}

#define DOUBLE_CLICK_PERIOD 250
static void
weston_wm_handle_button(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_button_press_event_t *button = (xcb_button_press_event_t *) event;
	const struct weston_desktop_xwayland_interface *xwayland_interface =
		wm->server->compositor->xwayland_interface;
	struct weston_seat *seat;
	struct weston_pointer *pointer;
	struct weston_wm_window *window;
	enum theme_location location;
	enum wl_pointer_button_state button_state;
	uint32_t button_id;
	uint32_t double_click = 0;

	wm_printf(wm, "XCB_BUTTON_%s (detail %d)\n",
		  button->response_type == XCB_BUTTON_PRESS ?
		  "PRESS" : "RELEASE", button->detail);

	if (!wm_lookup_window(wm, button->event, &window) ||
	    !window->decorate)
		return;

	if (button->detail != 1 && button->detail != 2)
		return;

	seat = weston_wm_pick_seat_for_window(window);
	pointer = weston_seat_get_pointer(seat);

	button_state = button->response_type == XCB_BUTTON_PRESS ?
		WL_POINTER_BUTTON_STATE_PRESSED :
		WL_POINTER_BUTTON_STATE_RELEASED;
	button_id = button->detail == 1 ? BTN_LEFT : BTN_RIGHT;

	if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (button->time - window->last_button_time <= DOUBLE_CLICK_PERIOD) {
			double_click = 1;
			window->did_double = 1;
		} else
			window->did_double = 0;

		window->last_button_time = button->time;
	} else if (window->did_double == 1) {
		double_click = 1;
		window->did_double = 0;
	}

	/* Make sure we're looking at the right location.  The frame
	 * could have received a motion event from a pointer from a
	 * different wl_seat, but under X it looks like our core
	 * pointer moved.  Move the frame pointer to the button press
	 * location before deciding what to do. */
	location = frame_pointer_motion(window->frame, NULL,
					button->event_x, button->event_y);
	if (double_click)
		location = frame_double_click(window->frame, NULL,
					      button_id, button_state);
	else
		location = frame_pointer_button(window->frame, NULL,
						button_id, button_state);

	if (frame_status(window->frame) & FRAME_STATUS_REPAINT)
		weston_wm_window_schedule_repaint(window);

	if (frame_status(window->frame) & FRAME_STATUS_MOVE) {
		if (pointer)
			xwayland_interface->move(window->shsurf, pointer);
		frame_status_clear(window->frame, FRAME_STATUS_MOVE);
	}

	if (frame_status(window->frame) & FRAME_STATUS_RESIZE) {
		if (pointer)
			xwayland_interface->resize(window->shsurf, pointer, location);
		frame_status_clear(window->frame, FRAME_STATUS_RESIZE);
	}

	if (frame_status(window->frame) & FRAME_STATUS_CLOSE) {
		weston_wm_window_close(window, button->time);
		frame_status_clear(window->frame, FRAME_STATUS_CLOSE);
	}

	if (frame_status(window->frame) & FRAME_STATUS_MAXIMIZE) {
		window->maximized_horz = !window->maximized_horz;
		window->maximized_vert = !window->maximized_vert;
		weston_wm_window_set_net_wm_state(window);
		if (weston_wm_window_is_maximized(window)) {
			window->saved_width = window->width;
			window->saved_height = window->height;
			xwayland_interface->set_maximized(window->shsurf);
		} else {
			weston_wm_window_set_toplevel(window);
		}
		frame_status_clear(window->frame, FRAME_STATUS_MAXIMIZE);
	}

	if (frame_status(window->frame) & FRAME_STATUS_MINIMIZE) {
		/* If window is currently in maximized or fullscreen state,
		 * don't override saved size.
		 */
		if (!weston_wm_window_is_maximized(window) &&
		    !window->fullscreen) {
			window->saved_width = window->width;
			window->saved_height = window->height;
		}
		xwayland_interface->set_minimized(window->shsurf);
		frame_status_clear(window->frame, FRAME_STATUS_MINIMIZE);
	}
}

static void
weston_wm_handle_motion(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *) event;
	struct weston_wm_window *window;
	enum theme_location location;
	int cursor;

	if (!wm_lookup_window(wm, motion->event, &window) ||
	    !window->decorate)
		return;

	location = frame_pointer_motion(window->frame, NULL,
					motion->event_x, motion->event_y);
	if (frame_status(window->frame) & FRAME_STATUS_REPAINT)
		weston_wm_window_schedule_repaint(window);

	cursor = get_cursor_for_location(location);
	weston_wm_window_set_cursor(wm, window->frame_id, cursor);
}

static void
weston_wm_handle_enter(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_enter_notify_event_t *enter = (xcb_enter_notify_event_t *) event;
	struct weston_wm_window *window;
	enum theme_location location;
	int cursor;

	if (!wm_lookup_window(wm, enter->event, &window) ||
	    !window->decorate)
		return;

	location = frame_pointer_enter(window->frame, NULL,
				       enter->event_x, enter->event_y);
	if (frame_status(window->frame) & FRAME_STATUS_REPAINT)
		weston_wm_window_schedule_repaint(window);

	cursor = get_cursor_for_location(location);
	weston_wm_window_set_cursor(wm, window->frame_id, cursor);
}

static void
weston_wm_handle_leave(struct weston_wm *wm, xcb_generic_event_t *event)
{
	xcb_leave_notify_event_t *leave = (xcb_leave_notify_event_t *) event;
	struct weston_wm_window *window;

	if (!wm_lookup_window(wm, leave->event, &window) ||
	    !window->decorate)
		return;

	frame_pointer_leave(window->frame, NULL);
	if (frame_status(window->frame) & FRAME_STATUS_REPAINT)
		weston_wm_window_schedule_repaint(window);

	weston_wm_window_set_cursor(wm, window->frame_id, XWM_CURSOR_LEFT_PTR);
}

static void
weston_wm_handle_focus_in(struct weston_wm *wm, xcb_generic_event_t *event)
{
	struct weston_wm_window *window;
	xcb_focus_in_event_t *focus = (xcb_focus_in_event_t *) event;

	/* Do not interfere with grabs */
	if (focus->mode == XCB_NOTIFY_MODE_GRAB ||
	    focus->mode == XCB_NOTIFY_MODE_UNGRAB)
		return;

	if (!wm_lookup_window(wm, focus->event, &window))
		return;

	/* Sometimes apps like to focus their own windows, and we don't
	 * want to prevent that - but we'd like to at least prevent any
	 * attempt to focus a toplevel that isn't the currently activated
	 * toplevel.
	 */
	if (!window->frame)
		return;

	/* Do not let X clients change the focus behind the compositor's
	 * back. Reset the focus to the old one if it changed. */
	if (!wm->focus_window || focus->event != wm->focus_window->id)
		weston_wm_send_focus_window(wm, wm->focus_window);
}

static int
weston_wm_handle_event(int fd, uint32_t mask, void *data)
{
	struct weston_wm *wm = data;
	xcb_generic_event_t *event;
	int count = 0;

	while (event = xcb_poll_for_event(wm->conn), event != NULL) {
		if (weston_wm_handle_selection_event(wm, event)) {
			free(event);
			count++;
			continue;
		}

		if (weston_wm_handle_dnd_event(wm, event)) {
			free(event);
			count++;
			continue;
		}

		switch (EVENT_TYPE(event)) {
		case XCB_BUTTON_PRESS:
		case XCB_BUTTON_RELEASE:
			weston_wm_handle_button(wm, event);
			break;
		case XCB_ENTER_NOTIFY:
			weston_wm_handle_enter(wm, event);
			break;
		case XCB_LEAVE_NOTIFY:
			weston_wm_handle_leave(wm, event);
			break;
		case XCB_MOTION_NOTIFY:
			weston_wm_handle_motion(wm, event);
			break;
		case XCB_CREATE_NOTIFY:
			weston_wm_handle_create_notify(wm, event);
			break;
		case XCB_MAP_REQUEST:
			weston_wm_handle_map_request(wm, event);
			break;
		case XCB_MAP_NOTIFY:
			weston_wm_handle_map_notify(wm, event);
			break;
		case XCB_UNMAP_NOTIFY:
			weston_wm_handle_unmap_notify(wm, event);
			break;
		case XCB_REPARENT_NOTIFY:
			weston_wm_handle_reparent_notify(wm, event);
			break;
		case XCB_CONFIGURE_REQUEST:
			weston_wm_handle_configure_request(wm, event);
			break;
		case XCB_CONFIGURE_NOTIFY:
			weston_wm_handle_configure_notify(wm, event);
			break;
		case XCB_DESTROY_NOTIFY:
			weston_wm_handle_destroy_notify(wm, event);
			break;
		case XCB_MAPPING_NOTIFY:
			wm_printf(wm, "XCB_MAPPING_NOTIFY\n");
			break;
		case XCB_PROPERTY_NOTIFY:
			weston_wm_handle_property_notify(wm, event);
			break;
		case XCB_CLIENT_MESSAGE:
			weston_wm_handle_client_message(wm, event);
			break;
		case XCB_FOCUS_IN:
			weston_wm_handle_focus_in(wm, event);
			break;
		}

		free(event);
		count++;
	}

	if (count != 0)
		xcb_flush(wm->conn);

	return count;
}

static void
weston_wm_set_net_active_window(struct weston_wm *wm, xcb_window_t window) {
	xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE,
			wm->screen->root, wm->atom.net_active_window,
			wm->atom.window, 32, 1, &window);
}

static void
weston_wm_get_visual_and_colormap(struct weston_wm *wm)
{
	xcb_depth_iterator_t d_iter;
	xcb_visualtype_iterator_t vt_iter;
	xcb_visualtype_t *visualtype;

	d_iter = xcb_screen_allowed_depths_iterator(wm->screen);
	visualtype = NULL;
	while (d_iter.rem > 0) {
		if (d_iter.data->depth == 32) {
			vt_iter = xcb_depth_visuals_iterator(d_iter.data);
			visualtype = vt_iter.data;
			break;
		}

		xcb_depth_next(&d_iter);
	}

	if (visualtype == NULL) {
		weston_log("no 32 bit visualtype\n");
		return;
	}

	wm->visual_id = visualtype->visual_id;
	wm->colormap = xcb_generate_id(wm->conn);
	xcb_create_colormap(wm->conn, XCB_COLORMAP_ALLOC_NONE,
			    wm->colormap, wm->screen->root, wm->visual_id);
}

static void
weston_wm_get_resources(struct weston_wm *wm)
{
	xcb_xfixes_query_version_cookie_t xfixes_cookie;
	xcb_xfixes_query_version_reply_t *xfixes_reply;
	xcb_render_query_pict_formats_reply_t *formats_reply;
	xcb_render_query_pict_formats_cookie_t formats_cookie;
	xcb_render_pictforminfo_t *formats;
	uint32_t i;

	xcb_prefetch_extension_data (wm->conn, &xcb_xfixes_id);
	xcb_prefetch_extension_data (wm->conn, &xcb_composite_id);

	formats_cookie = xcb_render_query_pict_formats(wm->conn);

	x11_get_atoms(wm->conn, &wm->atom);
	wm->xfixes = xcb_get_extension_data(wm->conn, &xcb_xfixes_id);
	if (!wm->xfixes || !wm->xfixes->present)
		weston_log("xfixes not available\n");

	xfixes_cookie = xcb_xfixes_query_version(wm->conn,
						 XCB_XFIXES_MAJOR_VERSION,
						 XCB_XFIXES_MINOR_VERSION);
	xfixes_reply = xcb_xfixes_query_version_reply(wm->conn,
						      xfixes_cookie, NULL);

	weston_log("xfixes version: %d.%d\n",
	       xfixes_reply->major_version, xfixes_reply->minor_version);

	free(xfixes_reply);

	formats_reply = xcb_render_query_pict_formats_reply(wm->conn,
							    formats_cookie, 0);
	if (formats_reply == NULL)
		return;

	formats = xcb_render_query_pict_formats_formats(formats_reply);
	for (i = 0; i < formats_reply->num_formats; i++) {
		if (formats[i].direct.red_mask != 0xff &&
		    formats[i].direct.red_shift != 16)
			continue;
		if (formats[i].type == XCB_RENDER_PICT_TYPE_DIRECT &&
		    formats[i].depth == 24)
			wm->format_rgb = formats[i];
		if (formats[i].type == XCB_RENDER_PICT_TYPE_DIRECT &&
		    formats[i].depth == 32 &&
		    formats[i].direct.alpha_mask == 0xff &&
		    formats[i].direct.alpha_shift == 24)
			wm->format_rgba = formats[i];
	}

	free(formats_reply);
}

static void
weston_wm_create_wm_window(struct weston_wm *wm)
{
	static const char name[] = "Weston WM";

	wm->wm_window = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  XCB_COPY_FROM_PARENT,
			  wm->wm_window,
			  wm->screen->root,
			  0, 0,
			  10, 10,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  wm->screen->root_visual,
			  0, NULL);

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->wm_window,
			    wm->atom.net_supporting_wm_check,
			    XCB_ATOM_WINDOW,
			    32, /* format */
			    1, &wm->wm_window);

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->wm_window,
			    wm->atom.net_wm_name,
			    wm->atom.utf8_string,
			    8, /* format */
			    strlen(name), name);

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->screen->root,
			    wm->atom.net_supporting_wm_check,
			    XCB_ATOM_WINDOW,
			    32, /* format */
			    1, &wm->wm_window);

	/* Claim the WM_S0 selection even though we don't support
	 * the --replace functionality. */
	xcb_set_selection_owner(wm->conn,
				wm->wm_window,
				wm->atom.wm_s0,
				XCB_TIME_CURRENT_TIME);

	xcb_set_selection_owner(wm->conn,
				wm->wm_window,
				wm->atom.net_wm_cm_s0,
				XCB_TIME_CURRENT_TIME);
}

static void
weston_wm_create_no_focus_window(struct weston_wm *wm)
{
	uint32_t values[2];

	values[0] = 1;
	values[1] =
		XCB_EVENT_MASK_KEY_PRESS |
		XCB_EVENT_MASK_KEY_RELEASE |
		XCB_EVENT_MASK_FOCUS_CHANGE;

	wm->no_focus_window = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  XCB_COPY_FROM_PARENT,
			  wm->no_focus_window,
			  wm->screen->root,
			  -100, -100, 1, 1,
			  0,
			  XCB_WINDOW_CLASS_COPY_FROM_PARENT,
			  XCB_COPY_FROM_PARENT,
			  XCB_CW_OVERRIDE_REDIRECT |
			  XCB_CW_EVENT_MASK,
			  values);
	xcb_map_window(wm->conn, wm->no_focus_window);
}

static void
free_xwl_surface(struct wl_resource *resource)
{
	struct xwl_surface *xsurf = wl_resource_get_user_data(resource);

	wl_list_remove(&xsurf->surface_commit_listener.link);
	wl_list_remove(&xsurf->link);
	free(xsurf);
}

static void
xwl_surface_set_serial(struct wl_client *client,
		       struct wl_resource *resource,
		       uint32_t serial_lo,
		       uint32_t serial_hi)
{
	struct xwl_surface *xsurf = wl_resource_get_user_data(resource);
	uint64_t serial = u64_from_u32s(serial_hi, serial_lo);

	if (serial == 0) {
		wl_resource_post_error(resource,
				       XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL,
				       "Invalid serial for xwayland surface");
		return;
	}

	if (xsurf->serial != 0) {
		wl_resource_post_error(resource,
				       XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED,
				       "Surface already has a serial");
		return;
	}
	xsurf->serial = serial;
}

static void
xwl_surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct xwayland_surface_v1_interface xwl_surface_interface = {
	.set_serial = xwl_surface_set_serial,
	.destroy = xwl_surface_destroy,
};

static void
xwl_surface_committed(struct wl_listener *listener, void *data)
{
	struct weston_wm_window *window, *next;
	struct xwl_surface *xsurf = wl_container_of(listener, xsurf,
						    surface_commit_listener);

	/* We haven't set a serial yet */
	if (xsurf->serial == 0)
		return;

	window = get_wm_window(xsurf->weston_surface);
	wl_list_remove(&xsurf->surface_commit_listener.link);
	wl_list_init(&xsurf->surface_commit_listener.link);

	wl_list_for_each_safe(window, next, &xsurf->wm->unpaired_window_list, link) {
		if (window->surface_serial == xsurf->serial) {
			xserver_map_shell_surface(window, xsurf->weston_surface);
			wl_list_remove(&window->link);
			wl_list_init(&window->link);
			return;
		}
	}

	wl_list_insert(&xsurf->wm->unpaired_surface_list, &xsurf->link);
}

static void
get_xwl_surface(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource)
{
	struct weston_wm *wm = wl_resource_get_user_data(resource);
	struct weston_surface *surf;
	struct xwl_surface *xsurf;
	uint32_t version;

	surf = wl_resource_get_user_data(surface_resource);
	if (weston_surface_set_role(surf, xwayland_surface_role, resource,
				    XWAYLAND_SHELL_V1_ERROR_ROLE) < 0)
		return;

	xsurf = zalloc(sizeof *xsurf);
	if (!xsurf)
		goto fail;

	version = wl_resource_get_version(resource);
	xsurf->resource = wl_resource_create(client,
					     &xwayland_surface_v1_interface,
					     version, id);
	if (!xsurf->resource)
		goto fail;

	wl_list_init(&xsurf->link);
	xsurf->wm = wm;
	xsurf->weston_surface = surf;

	wl_resource_set_implementation(xsurf->resource, &xwl_surface_interface,
				       xsurf, free_xwl_surface);
	xsurf->surface_commit_listener.notify = xwl_surface_committed;
	wl_signal_add(&surf->commit_signal, &xsurf->surface_commit_listener);

	return;

fail:
	wl_client_post_no_memory(client);
}

static void
xwl_shell_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct xwayland_shell_v1_interface xwayland_shell_implementation = {
	.get_xwayland_surface = get_xwl_surface,
	.destroy = xwl_shell_destroy,
};

static void
bind_xwayland_shell(struct wl_client *client,
		    void *data,
		    uint32_t version,
		    uint32_t id)
{
	struct weston_wm *wm = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &xwayland_shell_v1_interface,
				      version, id);
	if (client != wm->server->client) {
		wl_resource_post_error(resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "permission to bind xwayland_shell "
				       "denied");
		return;
	}

	wm->shell_bound = true;

	wl_resource_set_implementation(resource, &xwayland_shell_implementation,
				       wm, NULL);
}

struct weston_wm *
weston_wm_create(struct weston_xserver *wxs, int fd)
{
	struct weston_wm *wm;
	struct wl_event_loop *loop;
	xcb_screen_iterator_t s;
	uint32_t values[1];
	xcb_atom_t supported[7];

	wm = zalloc(sizeof *wm);
	if (wm == NULL)
		return NULL;

	wm->server = wxs;
	wm->window_hash = hash_table_create();
	if (wm->window_hash == NULL) {
		free(wm);
		return NULL;
	}

	/* xcb_connect_to_fd takes ownership of the fd. */
	wm->conn = xcb_connect_to_fd(fd, NULL);
	if (xcb_connection_has_error(wm->conn)) {
		weston_log("xcb_connect_to_fd failed\n");
		close(fd);
		hash_table_destroy(wm->window_hash);
		free(wm);
		return NULL;
	}

	s = xcb_setup_roots_iterator(xcb_get_setup(wm->conn));
	wm->screen = s.data;

	loop = wl_display_get_event_loop(wxs->wl_display);
	wm->source =
		wl_event_loop_add_fd(loop, fd,
				     WL_EVENT_READABLE,
				     weston_wm_handle_event, wm);
	wl_event_source_check(wm->source);

	weston_wm_get_resources(wm);
	weston_wm_get_visual_and_colormap(wm);

	values[0] =
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(wm->conn, wm->screen->root,
				     XCB_CW_EVENT_MASK, values);

	xcb_composite_redirect_subwindows(wm->conn, wm->screen->root,
					  XCB_COMPOSITE_REDIRECT_MANUAL);

	wm->theme = theme_create();

	supported[0] = wm->atom.net_wm_moveresize;
	supported[1] = wm->atom.net_wm_state;
	supported[2] = wm->atom.net_wm_state_fullscreen;
	supported[3] = wm->atom.net_wm_state_maximized_vert;
	supported[4] = wm->atom.net_wm_state_maximized_horz;
	supported[5] = wm->atom.net_active_window;
	supported[6] = wm->atom.net_frame_extents;
	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->screen->root,
			    wm->atom.net_supported,
			    XCB_ATOM_ATOM,
			    32, /* format */
			    ARRAY_LENGTH(supported), supported);

	weston_wm_set_net_active_window(wm, XCB_WINDOW_NONE);

	weston_wm_selection_init(wm);

	weston_wm_dnd_init(wm);

	xcb_flush(wm->conn);

	wm->create_surface_listener.notify = weston_wm_create_surface;
	wl_signal_add(&wxs->compositor->create_surface_signal,
		      &wm->create_surface_listener);
	wm->activate_listener.notify = weston_wm_window_activate;
	wl_signal_add(&wxs->compositor->activate_signal,
		      &wm->activate_listener);
	wm->kill_listener.notify = weston_wm_kill_client;
	wl_signal_add(&wxs->compositor->kill_signal,
		      &wm->kill_listener);
	wl_list_init(&wm->unpaired_window_list);
	wl_list_init(&wm->unpaired_surface_list);

	weston_wm_create_cursors(wm);
	weston_wm_window_set_cursor(wm, wm->screen->root, XWM_CURSOR_LEFT_PTR);

	wm->xwayland_shell_global = wl_global_create(wxs->compositor->wl_display,
						     &xwayland_shell_v1_interface,
						     1, wm, bind_xwayland_shell);

	/* Create wm window and take WM_S0 selection last, which
	 * signals to Xwayland that we're done with setup. */
	weston_wm_create_wm_window(wm);

	/* Create a dummy no_focus_window to use when focus changes
	 * to a non-X window. */
	weston_wm_create_no_focus_window(wm);

	weston_log("created wm, root %d\n", wm->screen->root);

	return wm;
}

void
weston_wm_destroy(struct weston_wm *wm)
{
	wl_global_destroy(wm->xwayland_shell_global);
	/* FIXME: Free windows in hash. */
	hash_table_destroy(wm->window_hash);
	weston_wm_destroy_cursors(wm);
	theme_destroy(wm->theme);
	xcb_disconnect(wm->conn);
	wl_event_source_remove(wm->source);
	wl_list_remove(&wm->seat_create_listener.link);
	wl_list_remove(&wm->seat_destroy_listener.link);
	wl_list_remove(&wm->selection_listener.link);
	wl_list_remove(&wm->activate_listener.link);
	wl_list_remove(&wm->kill_listener.link);
	wl_list_remove(&wm->create_surface_listener.link);

	/*
	 * No, you cannot call cleanup_after_cairo() here, because Weston
	 * on wayland-backend would crash in an assert inside Cairo.
	 * Just rely on headless and wayland backends calling it.
	 *
	 * XXX: fix this for other backends.
	 */

	free(wm);
}

static struct weston_wm_window *
get_wm_window(struct weston_surface *surface)
{
	struct wl_listener *listener;

	listener = wl_signal_get(&surface->destroy_signal, surface_destroy);
	if (listener)
		return container_of(listener, struct weston_wm_window,
				    surface_destroy_listener);

	return NULL;
}

static bool
is_wm_window(struct weston_surface *surface)
{
	return get_wm_window(surface) != NULL;
}

static const char *
get_xwayland_window_name(struct weston_surface *surface, enum window_atom_type atype)
{
	struct weston_wm_window *window = get_wm_window(surface);

	switch (atype) {
	case WM_NAME:
		return window->name;
	break;
	case WM_CLASS:
		return window->class;
	break;
	default:
		break;
	}

	return NULL;
}

static void
weston_wm_window_configure(void *data)
{
	struct weston_wm_window *window = data;
	struct weston_wm *wm = window->wm;
	uint32_t values[4];
	int x, y;

	if (window->configure_source) {
		wl_event_source_remove(window->configure_source);
		window->configure_source = NULL;
	}

	weston_wm_window_set_allow_commits(window, false);

	weston_wm_window_get_child_position(window, &x, &y);
	values[0] = x;
	values[1] = y;
	values[2] = window->width;
	values[3] = window->height;
	weston_wm_configure_window(wm, window->id,
				   XCB_CONFIG_WINDOW_X |
				   XCB_CONFIG_WINDOW_Y |
				   XCB_CONFIG_WINDOW_WIDTH |
				   XCB_CONFIG_WINDOW_HEIGHT,
				   values);

	weston_wm_window_configure_frame(window);
	weston_wm_window_send_configure_notify(window);
	weston_wm_window_schedule_repaint(window);
}

static void
send_configure(struct weston_surface *surface, int32_t width, int32_t height)
{
	struct weston_wm_window *window = get_wm_window(surface);
	struct weston_wm *wm;
	struct theme *t;
	int new_width, new_height;
	int vborder, hborder;
	bool use_saved_dimensions = false;
	bool use_current_dimensions = false;

	if (!window || !window->wm)
		return;
	wm = window->wm;
	t = wm->theme;

	if (window->decorate && !window->fullscreen) {
		hborder = 2 * t->width;
		vborder = t->titlebar_height + t->width;
	} else {
		hborder = 0;
		vborder = 0;
	}

	/* A config event with width == 0 or height == 0 is a hint to the client
	 * to choose its own dimensions. Since X11 clients don't support such
	 * hints we make a best guess here by trying to use the last saved
	 * dimensions or, as a fallback, the current dimensions. */
	if (width == 0 || height == 0) {
		use_saved_dimensions = window->saved_width > 0 &&
				       window->saved_height > 0;
		use_current_dimensions = !use_saved_dimensions &&
					 window->width > 0 &&
					 window->height > 0;
	}

	/* The saved or current dimensions are the plain window content
	 * dimensions without the borders, so we can use them directly for
	 * new_width and new_height below. */
	if (use_current_dimensions) {
		new_width = window->width;
		new_height = window->height;
	} else if (use_saved_dimensions) {
		new_width = window->saved_width;
		new_height = window->saved_height;
	} else {
		new_width = (width > hborder) ? (width - hborder) : 1;
		new_height = (height > vborder) ? (height - vborder) : 1;
	}

	if (window->width != new_width || window->height != new_height) {
		window->width = new_width;
		window->height = new_height;

		/* Save the toplevel size so that we can pick up a reasonable
		 * value when the compositor tell us to choose a size. We are
		 * already saving the size before going fullscreen/maximized,
		 * but this covers the case in which our size is changed but we
		 * continue on a normal state. */
		if (!weston_wm_window_is_maximized(window) && !window->fullscreen) {
			window->saved_width = new_width;
			window->saved_height = new_height;
		}

		if (window->frame) {
			if (weston_wm_window_is_maximized(window))
				frame_set_flag(window->frame, FRAME_FLAG_MAXIMIZED);

			frame_resize_inside(window->frame,
					    window->width, window->height);
		}
	}

	if (window->configure_source)
		return;

	window->configure_source =
		wl_event_loop_add_idle(wm->server->loop,
				       weston_wm_window_configure, window);
}

static void
send_close(struct weston_surface *surface)
{
	struct weston_wm_window *window = get_wm_window(surface);
	if (!window || !window->wm)
		return;
	weston_wm_window_close(window, XCB_CURRENT_TIME);
	xcb_flush(window->wm->conn);
}

static void
send_position(struct weston_surface *surface, int32_t x, int32_t y)
{
	struct weston_wm_window *window = get_wm_window(surface);
	struct weston_wm *wm;
	uint32_t values[2];
	uint16_t mask;
	struct weston_coord_global pos;

	pos.c = weston_coord(x, y);
	if (!window || !window->wm)
		return;

	wm = window->wm;
	/* We use pos_dirty to tell whether a configure message is in flight.
	 * This is needed in case we send two configure events in a very
	 * short time, since window->x/y is set in after a roundtrip, hence
	 * we cannot just check if the current x and y are different. */
	if (window->pos.c.x != pos.c.x || window->pos.c.y != pos.c.y ||
	    window->pos_dirty) {
		window->pos_dirty = true;
		values[0] = x;
		values[1] = y;
		mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;

		weston_wm_configure_window(wm, window->frame_id, mask, values);
		weston_wm_window_send_configure_notify(window);
		xcb_flush(wm->conn);
	}
}

static void
send_fullscreen(struct weston_surface *surface, bool fullscreen)
{
	struct weston_wm_window *window = get_wm_window(surface);

	if (!window || !window->wm)
		return;

	if (window->fullscreen == fullscreen)
		return;

	window->fullscreen = fullscreen;
	weston_wm_window_set_net_wm_state(window);

	if (window->fullscreen) {
		window->saved_width = window->width;
		window->saved_height = window->height;
	}
}

static const struct weston_xwayland_client_interface shell_client = {
	send_configure,
	send_close,
	send_fullscreen,
};

static int
legacy_fullscreen(struct weston_wm *wm,
		  struct weston_wm_window *window,
		  struct weston_output **output_ret)
{
	struct weston_compositor *compositor = wm->server->compositor;
	struct weston_output *output;
	uint32_t minmax = PMinSize | PMaxSize;
	int matching_size;

	/* Heuristics for detecting legacy fullscreen windows... */

	wl_list_for_each(output, &compositor->output_list, link) {
		if (output->pos.c.x == window->pos.c.x &&
		    output->pos.c.y == window->pos.c.y &&
		    output->width == window->width &&
		    output->height == window->height &&
		    window->override_redirect) {
			*output_ret = output;
			return 1;
		}

		matching_size = 0;
		if ((window->size_hints.flags & (USSize |PSize)) &&
		    window->size_hints.width == output->width &&
		    window->size_hints.height == output->height)
			matching_size = 1;
		if ((window->size_hints.flags & minmax) == minmax &&
		    window->size_hints.min_width == output->width &&
		    window->size_hints.min_height == output->height &&
		    window->size_hints.max_width == output->width &&
		    window->size_hints.max_height == output->height)
			matching_size = 1;

		if (matching_size && !window->decorate &&
		    (window->size_hints.flags & (USPosition | PPosition)) &&
		    window->size_hints.x == (int)output->pos.c.x &&
		    window->size_hints.y == (int)output->pos.c.y) {
			*output_ret = output;
			return 1;
		}
	}

	return 0;
}

static bool
weston_wm_window_is_positioned(struct weston_wm_window *window)
{
	if (!window->map_request_valid) {
		weston_log("XWM warning: win %d did not see map request\n",
			   window->id);

		/* Before map_request_valid existed, we used a sentinel
		 * value for the map_request coordinates. This return
		 * preserves the behaviour this function had at that
		 * time.
		 */
		return true;
	}

	if (window->size_hints.flags & (USPosition | PPosition))
		return true;

	return window->map_request.c.x != 0 || window->map_request.c.y != 0;
}

static bool
weston_wm_window_type_inactive(struct weston_wm_window *window)
{
	struct weston_wm *wm = window->wm;

	return window->type == wm->atom.net_wm_window_type_tooltip ||
	       window->type == wm->atom.net_wm_window_type_dropdown ||
	       window->type == wm->atom.net_wm_window_type_dnd ||
	       window->type == wm->atom.net_wm_window_type_combo ||
	       window->type == wm->atom.net_wm_window_type_popup ||
	       window->type == wm->atom.net_wm_window_type_utility;
}

static void
xserver_map_shell_surface(struct weston_wm_window *window,
			  struct weston_surface *surface)
{
	struct weston_wm *wm = window->wm;
	struct weston_desktop_xwayland *xwayland =
		wm->server->compositor->xwayland;
	const struct weston_desktop_xwayland_interface *xwayland_interface =
		wm->server->compositor->xwayland_interface;
	struct weston_wm_window *parent;

	/* This should be necessary only for override-redirected windows,
	 * because otherwise MapRequest handler would have already updated
	 * the properties. However, if X11 clients set properties after
	 * sending MapWindow, here we can still process them. The decorations
	 * have already been drawn once with the old property values, so if the
	 * app changes something affecting decor after MapWindow, we glitch.
	 * We only hit xserver_map_shell_surface() once per MapWindow and
	 * wl_surface, so better ensure we get the window type right.
	 */
	weston_wm_window_read_properties(window);

	/* A weston_wm_window may have many different surfaces assigned
	 * throughout its life, so we must make sure to remove the listener
	 * from the old surface signal list. */
	if (window->surface)
		wl_list_remove(&window->surface_destroy_listener.link);

	window->surface = surface;
	window->surface_destroy_listener.notify = surface_destroy;
	wl_signal_add(&window->surface->destroy_signal,
		      &window->surface_destroy_listener);

	if (!xwayland_interface)
		return;

	if (window->surface->committed) {
		weston_log("warning, unexpected in %s: "
			   "surface's configure hook is already set.\n",
			   __func__);
		return;
	}

	window->shsurf =
		xwayland_interface->create_surface(xwayland,
						   window->surface,
						   &shell_client);

	wm_printf(wm, "XWM: map shell surface, win %d, weston_surface %p, xwayland surface %p\n",
		  window->id, window->surface, window->shsurf);

	if (window->name)
		xwayland_interface->set_title(window->shsurf, window->name);
	if (window->pid > 0)
		xwayland_interface->set_pid(window->shsurf, window->pid);

	if (window->fullscreen) {
		window->saved_width = window->width;
		window->saved_height = window->height;
		xwayland_interface->set_fullscreen(window->shsurf,
						   window->legacy_fullscreen_output.output);
	} else if (window->override_redirect) {
		xwayland_interface->set_xwayland(window->shsurf,
						 window->pos);
	} else if (window->transient_for &&
		   !window->transient_for->override_redirect &&
		   window->transient_for->surface) {
		parent = window->transient_for;
		if (weston_wm_window_type_inactive(window)) {
			struct weston_coord_surface offset;

			offset.c = weston_coord_sub(window->pos.c, parent->pos.c);
			offset.coordinate_space_id = parent->surface;
			xwayland_interface->set_transient(window->shsurf,
							  parent->surface,
							  offset);
		} else {
			xwayland_interface->set_toplevel(window->shsurf);
			xwayland_interface->set_parent(window->shsurf,
						       parent->surface);
		}
	} else if (weston_wm_window_is_maximized(window)) {
		window->saved_width = window->width;
		window->saved_height = window->height;
		xwayland_interface->set_maximized(window->shsurf);
	} else {
		if (weston_wm_window_type_inactive(window)) {
			xwayland_interface->set_xwayland(window->shsurf, window->pos);
		} else if (weston_wm_window_is_positioned(window)) {
			xwayland_interface->set_toplevel_with_position(window->shsurf,
								       window->map_request);
		} else {
			xwayland_interface->set_toplevel(window->shsurf);
		}
	}

	if (window->frame_id == XCB_WINDOW_NONE) {
		weston_wm_window_set_pending_state_OR(window);
	} else {
		weston_wm_window_set_pending_state(window);
		weston_wm_window_set_allow_commits(window, true);
		xcb_flush(wm->conn);
	}
}

const struct weston_xwayland_surface_api surface_api = {
	is_wm_window,
	send_position,
	get_xwayland_window_name,
};
