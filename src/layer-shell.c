#include <stdlib.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wayland-util.h>

#include "server.h"
#include "layers.h"
#include "output.h"
#include "popup.h"
#include "tree.h"

void arrange_layers(struct planar_output *output) {
    struct wlr_box usable_area;
    struct wlr_output_layout_output *output_layout_output = wlr_output_layout_get(output->server->output_layout, output->wlr_output);
    wlr_output_effective_resolution(output->wlr_output,
        &usable_area.width, &usable_area.height);
    usable_area.x = usable_area.y = 0;

    // Arrange each layer from bottom to top
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
         layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; layer++) {
        struct wlr_scene_tree *layer_tree = output->server->layers[layer];
        struct wlr_scene_node *node;
        wl_list_for_each(node, &layer_tree->children, link) {
            if (node->type != WLR_SCENE_NODE_TREE) {
                continue;
            }
            struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
            struct planar_layer_surface *planar_layer_surface = tree->node.data;
            struct wlr_layer_surface_v1 *layer_surface = planar_layer_surface->layer_surface;

            wlr_scene_node_set_position(&tree->node, usable_area.x, usable_area.y);
            wlr_layer_surface_v1_configure(layer_surface, usable_area.width, usable_area.height);

            // Update usable area based on exclusive zone
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
    struct planar_layer_surface *planar_layer_surface = calloc(1, sizeof(struct planar_layer_surface));

    planar_layer_surface->server = server;
    planar_layer_surface->layer_surface = layer_surface;
    planar_layer_surface->mapped = false;

    struct planar_output *output = NULL;
    if (layer_surface->output) {
        struct planar_output *o;
        wl_list_for_each(o, &server->outputs, link) {
            if (o->wlr_output == layer_surface->output) {
                output = o;
                break;
            }
        }
    }
    if (!output) {
        // Use the first output if the client didn't specify one
        output = wl_container_of(server->outputs.next, output, link);
    }
    planar_layer_surface->output = output;

    planar_layer_surface->surface_map.notify = server_layer_shell_surface_map;
    wl_signal_add(&layer_surface->surface->events.map, &planar_layer_surface->surface_map);

    planar_layer_surface->surface_unmap.notify = server_layer_shell_surface_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &planar_layer_surface->surface_unmap);

    planar_layer_surface->surface_destroy.notify = server_layer_shell_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &planar_layer_surface->surface_destroy);

    planar_layer_surface->surface_commit.notify = server_layer_shell_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &planar_layer_surface->surface_commit);

    wl_list_insert(&output->layer_views, &planar_layer_surface->output_link);
    planar_layer_surface->output = output;
}

void server_layer_shell_surface_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct planar_layer_surface *layer_view = wl_container_of(listener, layer_view, surface_map);

    layer_view->surface_tree = planar_surface_tree_root_create(layer_view->server, layer_view->layer_surface->surface);

    /*if (layer_view->layer_surface->current.keyboard_interactive) {
        struct viv_seat *seat = viv_server_get_default_seat(layer_view->server);
        viv_seat_focus_layer_view(seat, layer_view);
    } */
}

void server_layer_shell_surface_unmap(struct wl_listener *listener, void *data) {
    struct planar_layer_surface *planar_layer_surface = wl_container_of(listener, planar_layer_surface, surface_unmap);
    struct wlr_output *wlr_output = planar_layer_surface->layer_surface->output;
    struct planar_output *output = wlr_output->data;

	planar_layer_surface->mapped = false;

    if (planar_layer_surface->surface_tree) {
        planar_surface_tree_destroy(planar_layer_surface->surface_tree);
        planar_layer_surface->surface_tree = NULL;
    } else {
    }

    // Rearrange the layers
    arrange_layers(planar_layer_surface->output);
}

void server_layer_shell_surface_destroy(struct wl_listener *listener, void *data) {
    struct planar_layer_surface *planar_layer_surface = wl_container_of(listener, planar_layer_surface, surface_destroy);

    wl_list_remove(&planar_layer_surface->surface_map.link);
    wl_list_remove(&planar_layer_surface->surface_unmap.link);
    wl_list_remove(&planar_layer_surface->surface_destroy.link);
    // wl_list_remove(&planar_layer_surface->new_popup.link);
    wl_list_remove(&planar_layer_surface->surface_commit.link);

    free(planar_layer_surface);
}

void server_layer_shell_surface_commit(struct wl_listener *listener, void *data) {
    struct planar_layer_surface *planar_layer_surface =
		wl_container_of(listener, planar_layer_surface, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = planar_layer_surface->layer_surface;

	struct wlr_output *wlr_output = planar_layer_surface->layer_surface->output;
	struct planar_output *output = wlr_output->data;

	if (!layer_surface->current.committed && layer_surface->surface->mapped == planar_layer_surface->mapped) {
        return;
    }

    planar_layer_surface->mapped = layer_surface->surface->mapped;

    arrange_layers(planar_layer_surface->output);
}
