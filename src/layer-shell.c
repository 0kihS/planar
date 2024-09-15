#include <stdlib.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wayland-util.h>

#include "server.h"
#include "layers.h"
#include "output.h"
#include "popup.h"

void arrange_layers(struct planar_output *output) {
    struct wlr_box usable_area;
    wlr_output_effective_resolution(output->wlr_output, &usable_area.width, &usable_area.height);
    usable_area.x = usable_area.y = 0;
    const struct wlr_box full_area = usable_area;

    // Arrange each layer
    for (int i = 0; i < 4; i++) {
        struct wlr_scene_tree *layer_tree = output->server->layers[i];
        struct wlr_scene_node *node;
        wl_list_for_each(node, &layer_tree->children, link) {
            // **Retrieve planar_layer_surface from node->data**
            struct planar_layer_surface *planar_layer_surface = (struct planar_layer_surface *)node->data;
            if (!planar_layer_surface) {
                continue;  // Skip if data is NULL
            }

            struct wlr_layer_surface_v1 *layer_surface = planar_layer_surface->layer_surface;
            if (!layer_surface || !layer_surface->initialized) {
                continue;
            }

            struct wlr_scene_layer_surface_v1 *scene_layer_surface = planar_layer_surface->scene_layer_surface;

            // Configure the surface based on the available areas
            wlr_scene_layer_surface_v1_configure(scene_layer_surface, &full_area, &usable_area);

            // Adjust usable area based on exclusive zone
            if (layer_surface->current.exclusive_zone > 0) {
                switch (layer_surface->current.anchor) {
                    case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
                        usable_area.y += layer_surface->current.exclusive_zone;
                        usable_area.height -= layer_surface->current.exclusive_zone;
                        break;
                    case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
                        usable_area.height -= layer_surface->current.exclusive_zone;
                        break;
                    case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
                        usable_area.x += layer_surface->current.exclusive_zone;
                        usable_area.width -= layer_surface->current.exclusive_zone;
                        break;
                    case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
                        usable_area.width -= layer_surface->current.exclusive_zone;
                        break;
                }
            }
        }
    }

    // Update the usable area for normal windows
    output->usable_area = usable_area;
}

void server_layer_shell_surface(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(listener, server, new_layer_shell_surface);
    struct wlr_layer_surface_v1 *layer_surface = data;

    // Determine the output for the layer surface
    struct planar_output *output = NULL;
    if (layer_surface->output) {
        output = layer_surface->output->data;
    }
    if (!output) {
        // Use the first output if the client didn't specify one
        output = wl_container_of(server->outputs.next, output, link);
    }

    // Get the appropriate scene tree for this layer
    struct wlr_scene_tree *layer_tree = server->layers[layer_surface->pending.layer];

    // Create the scene layer surface
    struct wlr_scene_layer_surface_v1 *scene_layer_surface =
        wlr_scene_layer_surface_v1_create(layer_tree, layer_surface);
    if (!scene_layer_surface) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    // Create and initialize your planar_layer_surface
    struct planar_layer_surface *planar_layer_surface = calloc(1, sizeof(struct planar_layer_surface));
    if (!planar_layer_surface) {
        free(layer_surface);
        return;
    }

    planar_layer_surface->server = server;
    planar_layer_surface->scene_layer_surface = scene_layer_surface;
    planar_layer_surface->layer_surface = layer_surface;
    planar_layer_surface->output = output;

    scene_layer_surface->tree->node.data = planar_layer_surface;

    // Set up listeners
    planar_layer_surface->surface_map.notify = server_layer_shell_surface_map;
    wl_signal_add(&layer_surface->surface->events.map, &planar_layer_surface->surface_map);

    planar_layer_surface->surface_unmap.notify = server_layer_shell_surface_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &planar_layer_surface->surface_unmap);

    planar_layer_surface->surface_destroy.notify = server_layer_shell_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &planar_layer_surface->surface_destroy);

    planar_layer_surface->surface_commit.notify = server_layer_shell_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &planar_layer_surface->surface_commit);

    // Store the planar_layer_surface in the layer_surface's user data
    layer_surface->data = planar_layer_surface;
}

void server_layer_shell_surface_map(struct wl_listener *listener, void *data) {
    struct planar_layer_surface *layer_surface = wl_container_of(listener, layer_surface, surface_map);
    if (layer_surface->layer_surface->current.keyboard_interactive) {
        focus_layer_surface(layer_surface, layer_surface->layer_surface->surface);
    }

    layer_surface->mapped = true;
    arrange_layers(layer_surface->output);
}

void server_layer_shell_surface_unmap(struct wl_listener *listener, void *data) {
    struct planar_layer_surface *layer_surface = wl_container_of(listener, layer_surface, surface_unmap);

    layer_surface->mapped = false;
    arrange_layers(layer_surface->output);
}

void server_layer_shell_surface_destroy(struct wl_listener *listener, void *data) {
    struct planar_layer_surface *layer_surface = wl_container_of(listener, layer_surface, surface_destroy);

    wl_list_remove(&layer_surface->surface_map.link);
    wl_list_remove(&layer_surface->surface_unmap.link);
    wl_list_remove(&layer_surface->surface_destroy.link);
    wl_list_remove(&layer_surface->surface_commit.link);

    free(layer_surface);
}

void server_layer_shell_surface_commit(struct wl_listener *listener, void *data) {
    struct planar_layer_surface *planar_layer_surface = wl_container_of(listener, planar_layer_surface, surface_commit);
    struct planar_server *server = planar_layer_surface->server;

    struct wlr_layer_surface_v1 *layer_surface = planar_layer_surface->layer_surface;
	if (!layer_surface->initialized) {
		return;
	}

    struct planar_output *output = NULL;
    if (layer_surface->output) {
        output = layer_surface->output->data;
    }
    if (!output) {
        // Use the first output if the client didn't specify one
        output = wl_container_of(server->outputs.next, output, link);
    }

    uint32_t committed = layer_surface->current.committed;
	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		enum zwlr_layer_shell_v1_layer layer_type = layer_surface->current.layer;
		struct wlr_scene_tree *output_layer = planar_layer_get_scene(
			planar_layer_surface->output, layer_type);
		wlr_scene_node_reparent(&planar_layer_surface->scene_layer_surface->tree->node, output_layer);
	}

	if (layer_surface->initial_commit || committed || layer_surface->surface->mapped != planar_layer_surface->mapped) {
		planar_layer_surface->mapped = layer_surface->surface->mapped;
		arrange_layers(planar_layer_surface->output);
	}
}

struct planar_layer_surface *layer_surface_at(struct planar_server *server, double lx, double ly,
                                            struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }

    *surface = scene_surface->surface;

    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree->node.data;
}

void focus_layer_surface(struct planar_layer_surface *layer_surface, struct wlr_surface *surface) {
    if(!layer_surface) {
        return;
    }
    struct wlr_seat *seat = layer_surface->server->seat;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
                                       keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static struct wlr_scene_tree *planar_layer_get_scene(struct planar_output *output,
		enum zwlr_layer_shell_v1_layer type) {
        struct planar_server *server = output->server;
	switch (type) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return server->layers[0];
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->layers[1];
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->layers[2];
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return server->layers[3];
	}

	return NULL;
}
