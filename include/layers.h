#ifndef PLANAR_LAYERS_H
#define PLANAR_LAYERS_H

#include "server.h"
#include <wlr/types/wlr_layer_shell_v1.h>

struct planar_layer_surface {
    struct planar_server *server;
    struct wlr_layer_surface_v1 *layer_surface;

    struct planar_surface_tree_node *surface_tree;

    struct planar_output *output;

    struct wl_listener surface_map;
    struct wl_listener surface_unmap;
    struct wl_listener surface_destroy;
    struct wl_listener new_popup;
    struct wl_listener surface_commit;

    struct wlr_scene_layer_surface_v1 *scene_layer_surface;

    struct wl_list output_link;

    bool mapped;
};

void server_layer_shell_surface(struct wl_listener *listener, void *data);
void server_layer_shell_surface_map(struct wl_listener *listener, void *data);
void server_layer_shell_surface_unmap(struct wl_listener *listener, void *data);
void server_layer_shell_surface_destroy(struct wl_listener *listener, void *data);
void server_layer_shell_surface_commit(struct wl_listener *listener, void *data);
static struct wlr_scene_tree *planar_layer_get_scene(struct planar_output *output,
		enum zwlr_layer_shell_v1_layer type);
void arrange_layers(struct planar_output *output);

#endif // PLANAR_LAYERS_H
