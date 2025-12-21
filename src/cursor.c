#include "cursor.h"
#include "server.h"
#include "toplevel.h"
#include "output.h"
#include "layers.h"
#include "decoration.h"
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <string.h>
#include <linux/input-event-codes.h>

static void server_cursor_motion(struct wl_listener *listener, void *data);
static void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
static void server_cursor_button(struct wl_listener *listener, void *data);
static void server_cursor_axis(struct wl_listener *listener, void *data);
static void server_cursor_frame(struct wl_listener *listener, void *data);

static const char *cursor_name_for_edges(uint32_t edges) {
    switch (edges) {
    case WLR_EDGE_TOP:
        return "top_side";
    case WLR_EDGE_BOTTOM:
        return "bottom_side";
    case WLR_EDGE_LEFT:
        return "left_side";
    case WLR_EDGE_RIGHT:
        return "right_side";
    case WLR_EDGE_TOP | WLR_EDGE_LEFT:
        return "top_left_corner";
    case WLR_EDGE_TOP | WLR_EDGE_RIGHT:
        return "top_right_corner";
    case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT:
        return "bottom_left_corner";
    case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT:
        return "bottom_right_corner";
    default:
        return NULL;
    }
}

// Check if cursor is over any toplevel's decoration
// Returns the toplevel if found, and sets edge_result:
//   0 = on titlebar (move)
//   1 = on border (resize, edges set)
//  -1 = not on decoration
static struct planar_toplevel *toplevel_decoration_at(
        struct planar_server *server, double lx, double ly,
        int *edge_result, uint32_t *edges) {
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->active_workspace->toplevels, link) {
        if (!toplevel->decoration) continue;

        int result = decoration_get_edge_at(toplevel->decoration, lx, ly, edges);
        if (result >= 0) {
            *edge_result = result;
            return toplevel;
        }
    }
    *edge_result = -1;
    *edges = 0;
    return NULL;
}

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

    // Check decorations first
    int edge_result;
    uint32_t edges;
    struct planar_toplevel *dec_toplevel = toplevel_decoration_at(server, cx, cy, &edge_result, &edges);
    if (dec_toplevel) {
        wlr_seat_pointer_clear_focus(seat);
        if (edge_result == 0) {
            // On titlebar
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        } else {
            // On border - show resize cursor
            const char *cursor_name = cursor_name_for_edges(edges);
            if (cursor_name) {
                wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, cursor_name);
            }
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
    double new_node_x = server->cursor->x - server->grab_x;
    double new_node_y = server->cursor->y - server->grab_y;
    double scale = toplevel->workspace ? toplevel->workspace->scale : 1.0;
    toplevel->logical_x = new_node_x / scale;
    toplevel->logical_y = new_node_y / scale;

    wlr_scene_node_set_position(&toplevel->container->node,
        toplevel->logical_x * scale,
        toplevel->logical_y * scale);
}

void process_cursor_resize(struct planar_server *server, uint32_t time) {
	(void)time;
	struct planar_toplevel *toplevel = server->grabbed_toplevel;
	double scale = toplevel->workspace ? toplevel->workspace->scale : 1.0;
	int border = DECORATION_BORDER_WIDTH;

	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;

	// grab_geobox holds the client area (inside borders)
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

	// Container position is client position minus border
	int container_x = new_left - border;
	int container_y = new_top - border;

	toplevel->logical_x = container_x / scale;
	toplevel->logical_y = container_y / scale;

	wlr_scene_node_set_position(&toplevel->container->node, container_x, container_y);

	int new_width = (new_right - new_left) / scale;
	int new_height = (new_bottom - new_top) / scale;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct planar_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    wlr_cursor_move(server->cursor, &event->pointer->base,
            event->delta_x, event->delta_y);

    if (server->cursor_mode == PLANAR_CURSOR_PANNING) {
        double dx = server->cursor->x - server->grab_x;
        double dy = server->cursor->y - server->grab_y;
        
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
            server->cursor_mode = PLANAR_CURSOR_MOVE;
            server->grabbed_toplevel = toplevel;
            server->grab_x = cx - toplevel->container->node.x;
            server->grab_y = cy - toplevel->container->node.y;
            return;
        }
    }

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->cursor_mode == PLANAR_CURSOR_MOVE) {
            server->cursor_mode = PLANAR_CURSOR_DRAG_PENDING;
        }
    }

    wlr_seat_pointer_notify_button(server->seat,
            event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_cursor_mode(server);
        return;
    }

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (event->button == BTN_MIDDLE) {
            server->cursor_mode = PLANAR_CURSOR_PANNING;
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

    // Check if clicking on a decoration
    if (event->button == BTN_LEFT) {
        int edge_result;
        uint32_t edges;
        struct planar_toplevel *dec_toplevel = toplevel_decoration_at(server, cx, cy, &edge_result, &edges);
        if (dec_toplevel) {
            focus_toplevel(dec_toplevel, dec_toplevel->xdg_toplevel->base->surface);

            // Border - begin resize
            server->cursor_mode = PLANAR_CURSOR_RESIZE;
            server->grabbed_toplevel = dec_toplevel;
            server->resize_edges = edges;

            int border = DECORATION_BORDER_WIDTH;
            struct wlr_box *geo_box = &dec_toplevel->xdg_toplevel->base->geometry;
            double scale = dec_toplevel->workspace ? dec_toplevel->workspace->scale : 1.0;

            // Client area starts at container + border
            int client_x = dec_toplevel->container->node.x + border;
            int client_y = dec_toplevel->container->node.y + border;
            int client_w = geo_box->width * scale;
            int client_h = geo_box->height * scale;

            double border_x = client_x + ((edges & WLR_EDGE_RIGHT) ? client_w : 0);
            double border_y = client_y + ((edges & WLR_EDGE_BOTTOM) ? client_h : 0);

            server->grab_x = cx - border_x;
            server->grab_y = cy - border_y;

            server->grab_geobox.x = client_x;
            server->grab_geobox.y = client_y;
            server->grab_geobox.width = client_w;
            server->grab_geobox.height = client_h;

            return;
        }
    }

    struct planar_toplevel *toplevel = desktop_toplevel_at(server,
            cx, cy, &surface, &sx, &sy);
    
    if (toplevel) {
        focus_toplevel(toplevel, surface);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct planar_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
    
    // Check for Ctrl + Scroll
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_CTRL)) {
        if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            double zoom_factor = 0.1;
            double scale = server->active_workspace->scale;
            // Scroll down (-delta) -> zoom out. Scroll up (+delta) -> zoom in?
            // Actually usually negative delta is scroll up.
            // Let's assume standard wheel: delta < 0 is scroll up.
            
            // Normalize delta
            double delta = event->delta;
            if (delta == 0) delta = event->delta_discrete * 10;
            
            if (delta < 0) {
                scale += zoom_factor;
            } else {
                scale -= zoom_factor;
            }
            
            update_workspace_scale(server, scale, server->cursor->x, server->cursor->y);
            return;
        }
    }

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