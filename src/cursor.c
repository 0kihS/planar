#include "cursor.h"
#include "server.h"
#include "toplevel.h"
#include "output.h"
#include "layers.h"
#include "decoration.h"
#include "group.h"
#include "selection.h"
#include "snap.h"
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
	if (tree == NULL) {
		return NULL;
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
            // Reset cursor to default when entering a surface from decoration
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
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
    
    // Find parent total scale
    float total_scale = 1.0;
    struct wlr_scene_node *it = toplevel->container->node.parent ? &toplevel->container->node.parent->node : NULL;
    while (it) {
        total_scale *= it->scale;
        it = it->parent ? &it->parent->node : NULL;
    }

    double new_logical_x = (server->cursor->x - server->grab_x) / total_scale;
    double new_logical_y = (server->cursor->y - server->grab_y) / total_scale;

    // Determine what's being moved and get bounding box for snap
    bool is_selection_move = selection_count(server) > 1 && selection_contains(server, toplevel);
    bool is_group_move = toplevel->group != NULL;

    // Apply snap detection
    if (server->settings.snap_enabled) {
        if (is_selection_move) {
            // Get selection bounding box
            double bounds_x, bounds_y;
            int bounds_w, bounds_h;
            selection_get_bounds(server, &bounds_x, &bounds_y, &bounds_w, &bounds_h);

            // Calculate where the bounding box would move to
            double delta_x = new_logical_x - toplevel->logical_x;
            double delta_y = new_logical_y - toplevel->logical_y;
            double new_bounds_x = bounds_x + delta_x;
            double new_bounds_y = bounds_y + delta_y;

            // Snap using the bounding box (pass toplevel for skip logic)
            snap_update(server, toplevel, &new_bounds_x, &new_bounds_y, bounds_w, bounds_h);

            // Convert back to toplevel position
            new_logical_x = toplevel->logical_x + (new_bounds_x - bounds_x);
            new_logical_y = toplevel->logical_y + (new_bounds_y - bounds_y);
        } else if (is_group_move) {
            // Get group bounding box
            double bounds_x, bounds_y;
            int bounds_w, bounds_h;
            group_get_bounds(toplevel->group, &bounds_x, &bounds_y, &bounds_w, &bounds_h);

            // Calculate where the bounding box would move to
            double delta_x = new_logical_x - toplevel->logical_x;
            double delta_y = new_logical_y - toplevel->logical_y;
            double new_bounds_x = bounds_x + delta_x;
            double new_bounds_y = bounds_y + delta_y;

            // Snap using the bounding box
            snap_update(server, toplevel, &new_bounds_x, &new_bounds_y, bounds_w, bounds_h);

            // Convert back to toplevel position
            new_logical_x = toplevel->logical_x + (new_bounds_x - bounds_x);
            new_logical_y = toplevel->logical_y + (new_bounds_y - bounds_y);
        } else if (toplevel->decoration) {
            // Single window snap
            int border = server->settings.border_width;
            int moving_width = toplevel->decoration->width + 2 * border;
            int moving_height = toplevel->decoration->height + 2 * border;
            snap_update(server, toplevel, &new_logical_x, &new_logical_y,
                        moving_width, moving_height);
        }
    }

    double delta_x = new_logical_x - toplevel->logical_x;
    double delta_y = new_logical_y - toplevel->logical_y;

    if (is_selection_move) {
        selection_move_by(server, delta_x, delta_y);
    } else if (is_group_move) {
        group_move_by(toplevel->group, delta_x, delta_y);
    } else {
        toplevel->logical_x = new_logical_x;
        toplevel->logical_y = new_logical_y;

        wlr_scene_node_set_position(&toplevel->container->node,
            toplevel->logical_x,
            toplevel->logical_y);
    }
}

void process_cursor_resize(struct planar_server *server, uint32_t time) {
	(void)time;
	struct planar_toplevel *toplevel = server->grabbed_toplevel;

	// Find parent total scale
	float total_scale = 1.0;
	struct wlr_scene_node *it = toplevel->container->node.parent ? &toplevel->container->node.parent->node : NULL;
	while (it) {
		total_scale *= it->scale;
		it = it->parent ? &it->parent->node : NULL;
	}

	double border_x = (server->cursor->x - server->grab_x) / total_scale;
	double border_y = (server->cursor->y - server->grab_y) / total_scale;

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

	int new_width = (new_right - new_left);
	int new_height = (new_bottom - new_top);
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

    if (server->cursor_mode == PLANAR_CURSOR_BOX_SELECT) {
        selection_update_box(server, server->cursor->x, server->cursor->y);
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

    double cx = server->cursor->x;
    double cy = server->cursor->y;

    // Check for Shift modifier
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    bool shift_held = keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_SHIFT);

    // Handle box-select release
    if (server->cursor_mode == PLANAR_CURSOR_BOX_SELECT &&
        event->button == BTN_LEFT &&
        event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        selection_finish_box(server);
        server->cursor_mode = PLANAR_CURSOR_PASSTHROUGH;
        return;
    }

    // Handle drag mode
    if (server->cursor_mode == PLANAR_CURSOR_DRAG_PENDING &&
        event->button == BTN_LEFT &&
        event->state == WL_POINTER_BUTTON_STATE_PRESSED) {

        double sx, sy;
        struct wlr_surface *surface;

        struct planar_toplevel *toplevel = desktop_toplevel_at(server,
                cx, cy, &surface, &sx, &sy);

        if (toplevel) {
            server->cursor_mode = PLANAR_CURSOR_MOVE;
            server->grabbed_toplevel = toplevel;

            float total_scale = 1.0;
            struct wlr_scene_node *it = toplevel->container->node.parent ? &toplevel->container->node.parent->node : NULL;
            while (it) {
                total_scale *= it->scale;
                it = it->parent ? &it->parent->node : NULL;
            }

            server->grab_x = cx - (toplevel->container->node.x * total_scale);
            server->grab_y = cy - (toplevel->container->node.y * total_scale);
            snap_start(server);
            return;
        }
    }

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->cursor_mode == PLANAR_CURSOR_MOVE) {
            // End snap mode before changing cursor mode
            snap_end(server);
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

            int border = server->settings.border_width;
            struct wlr_box *geo_box = &dec_toplevel->xdg_toplevel->base->geometry;

            // Client area starts at container + border
            int client_x = dec_toplevel->container->node.x + border;
            int client_y = dec_toplevel->container->node.y + border;
            int client_w = geo_box->width;
            int client_h = geo_box->height;

            double border_x = client_x + ((edges & WLR_EDGE_RIGHT) ? client_w : 0);
            double border_y = client_y + ((edges & WLR_EDGE_BOTTOM) ? client_h : 0);

            float total_scale = 1.0;
            struct wlr_scene_node *it = dec_toplevel->container->node.parent ? &dec_toplevel->container->node.parent->node : NULL;
            while (it) {
                total_scale *= it->scale;
                it = it->parent ? &it->parent->node : NULL;
            }

            server->grab_x = cx - (border_x * total_scale);
            server->grab_y = cy - (border_y * total_scale);

            server->grab_geobox.x = client_x;
            server->grab_geobox.y = client_y;
            server->grab_geobox.width = client_w;
            server->grab_geobox.height = client_h;

            return;
        }
    }

    struct planar_toplevel *toplevel = desktop_toplevel_at(server,
            cx, cy, &surface, &sx, &sy);

    if (event->button == BTN_LEFT && event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (toplevel) {
            if (shift_held) {
                selection_toggle(server, toplevel);
                return;
            }

            if (!selection_contains(server, toplevel)) {
                selection_clear(server);
            }
            focus_toplevel(toplevel, surface);
        } else {
            if (!shift_held) {
                selection_clear(server);
            }
            server->cursor_mode = PLANAR_CURSOR_BOX_SELECT;
            selection_start_box(server, cx, cy);
        }
    } else if (toplevel) {
        focus_toplevel(toplevel, surface);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct planar_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
    
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_CTRL)) {
        if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            double zoom_step = server->settings.zoom_step;
            double scale = server->active_workspace->scale;
            double delta = event->delta;
            if (delta == 0) delta = event->delta_discrete * 10;

            if (delta < 0) {
                scale += zoom_step;
            } else {
                scale -= zoom_step;
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
        // End snap mode and cleanup guides
        snap_end(server);

        // If we were moving a window, make sure to focus it
        if (server->grabbed_toplevel) {
            focus_toplevel(server->grabbed_toplevel,
                server->grabbed_toplevel->xdg_toplevel->base->surface);
        }
    }

    if (server->cursor_mode == PLANAR_CURSOR_BOX_SELECT) {
        selection_cancel_box(server);
    }

    server->cursor_mode = PLANAR_CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
}

void server_new_pointer(struct planar_server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}