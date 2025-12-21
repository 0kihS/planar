#include "cursor.h"
#include "server.h"
#include "toplevel.h"
#include "output.h"
#include "layers.h"
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <string.h>
#include <linux/input-event-codes.h>

static void server_cursor_motion(struct wl_listener *listener, void *data);
static void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
static void server_cursor_button(struct wl_listener *listener, void *data);
static void server_cursor_axis(struct wl_listener *listener, void *data);
static void server_cursor_frame(struct wl_listener *listener, void *data);

static struct planar_toplevel *desktop_toplevel_at(
		struct planar_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost node in the scene at the given layout coords.
	 * We only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a planar_toplevel. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Find the node corresponding to the planar_toplevel at the root of this
	 * surface tree, it is the only one for which we set the data field. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree->node.data;
}

void cursor_init(struct planar_server *server) {
    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

    server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server->cursor_mode = PLANAR_CURSOR_PASSTHROUGH;

    // Set up listeners
    server->cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);

    server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);

    server->cursor_button.notify = server_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);

    server->cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);

    server->cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
}

void cursor_destroy(struct planar_server *server) {
    wlr_xcursor_manager_destroy(server->cursor_mgr);
    wlr_cursor_destroy(server->cursor);
}

void process_cursor_motion(struct planar_server *server, double cx, double cy, uint32_t time) {
    /* If the mode is non-passthrough, delegate to those functions. */
    if (server->cursor_mode == PLANAR_CURSOR_MOVE) {
        process_cursor_move(server, time);
        return;
    } else if (server->cursor_mode == PLANAR_CURSOR_RESIZE) {
        process_cursor_resize(server, time);
        return;
    }

    /* First, check for layer surfaces using global coordinates */
    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct planar_layer_surface *layer_surface = layer_surface_at(server,
            cx, cy, &surface, &sx, &sy);

    if (layer_surface && strcmp(surface->role->name, "zwlr_layer_surface_v1") == 0) {
        if (surface) {
            wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(seat, time, sx, sy);
        }
        return;
    }

    struct planar_toplevel *toplevel = desktop_toplevel_at(server,
            cx, cy, &surface, &sx, &sy);

    if (toplevel && toplevel->server) {
        if (surface) {
            wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(seat, time, sx, sy);
        }
    } else {
        /* If there's no toplevel under the cursor, set the cursor image to a
         * default. This is what makes the cursor image appear when you move it
         * around the screen, not over any toplevels. */
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(seat);
    }
}

void process_cursor_move(struct planar_server *server, uint32_t time) {
    (void)time;
    struct planar_toplevel *toplevel = server->grabbed_toplevel;
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        server->cursor->x - server->grab_x,
        server->cursor->y - server->grab_y);
}

void process_cursor_resize(struct planar_server *server, uint32_t time) {
	(void)time;
	/*
	 * Resizing the grabbed toplevel can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the
	 * toplevel on one or two axes, but can also move the toplevel if you resize
	 * from the top or left edges (or top-left corner).
	 *
	 * Note that some shortcuts are taken here. In a more fleshed-out
	 * compositor, you'd wait for the client to prepare a buffer at the new
	 * size, then commit any movement that was prepared.
	 */
	struct planar_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct planar_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    wlr_cursor_move(server->cursor, &event->pointer->base,
            event->delta_x, event->delta_y);

    if (server->cursor_mode == PLANAR_CURSOR_PANNING) {
        // Calculate total offset from initial grab point
        double dx = server->cursor->x - server->grab_x;
        double dy = server->cursor->y - server->grab_y;
        
        // Set the workspace offset relative to the initial workspace position
        set_workspace_offset(server, 
            server->grab_workspace_x + dx,
            server->grab_workspace_y + dy);
        return;
    }

    double cx = server->cursor->x;
    double cy = server->cursor->y;
    process_cursor_motion(server, cx, cy, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct planar_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	double cx = server->cursor->x;
    double cy = server->cursor->y;
	process_cursor_motion(server, cx, cy, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct planar_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    // Handle drag mode
    if (server->cursor_mode == PLANAR_CURSOR_DRAG_PENDING && 
        event->button == BTN_LEFT && 
        event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        
        double cx = server->cursor->x;
        double cy = server->cursor->y;
        double sx, sy;
        struct wlr_surface *surface;
        
        struct planar_toplevel *toplevel = desktop_toplevel_at(server,
                cx, cy, &surface, &sx, &sy);
        
        if (toplevel) {
            // Start the drag operation
            server->cursor_mode = PLANAR_CURSOR_MOVE;
            server->grabbed_toplevel = toplevel;
            server->grab_x = cx - toplevel->scene_tree->node.x;
            server->grab_y = cy - toplevel->scene_tree->node.y;
            return;
        }
    }

    // Handle button release during drag
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->cursor_mode == PLANAR_CURSOR_MOVE) {
            server->cursor_mode = PLANAR_CURSOR_DRAG_PENDING;  // Go back to pending mode
        }
        // Don't reset cursor mode here as the key might still be held
    }

    wlr_seat_pointer_notify_button(server->seat,
            event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // Reset the cursor mode when any button is released
        reset_cursor_mode(server);
        return;
    }

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (event->button == BTN_MIDDLE) {
            server->cursor_mode = PLANAR_CURSOR_PANNING;
            // Store initial cursor position and workspace offset for panning
            server->grab_x = server->cursor->x;
            server->grab_y = server->cursor->y;
            server->grab_workspace_x = server->active_workspace->global_offset.x;
            server->grab_workspace_y = server->active_workspace->global_offset.y;
            return;
        }
    }

    double cx = server->cursor->x;
    double cy = server->cursor->y;
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct planar_layer_surface *layer_surface = layer_surface_at(server,
            cx, cy, &surface, &sx, &sy);

    if (layer_surface) {
        focus_layer_surface(layer_surface, surface);
        return;
    }

    struct planar_toplevel *toplevel = desktop_toplevel_at(server,
            cx, cy, &surface, &sx, &sy);
    
    if (toplevel) {
        focus_toplevel(toplevel, surface);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct planar_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	(void)data;
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct planar_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

void reset_cursor_mode(struct planar_server *server) {
    if (server->cursor_mode == PLANAR_CURSOR_MOVE) {
        // If we were moving a window, make sure to focus it
        if (server->grabbed_toplevel) {
            focus_toplevel(server->grabbed_toplevel, 
                server->grabbed_toplevel->xdg_toplevel->base->surface);
        }
    }
    
    server->cursor_mode = PLANAR_CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
}

void server_new_pointer(struct planar_server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}