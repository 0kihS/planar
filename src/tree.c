#include <wayland-util.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include <assert.h>
#include <stdlib.h>

#include "output.h"
#include "tree.h"
#include "server.h"

static struct planar_surface_tree_node *planar_surface_tree_create(struct planar_server *server,  struct wlr_surface *surface) {
    struct planar_surface_tree_node *node = calloc(1, sizeof(struct planar_surface_tree_node));

    wlr_log(WLR_INFO, "New node at %p", node);

    node->server = server;
    node->wlr_surface = surface;

    wl_list_init(&node->child_subsurfaces);

    node->new_subsurface.notify = handle_new_node_subsurface;
    wl_signal_add(&surface->events.new_subsurface, &node->new_subsurface);

    node->commit.notify = handle_commit;
    wl_signal_add(&surface->events.commit, &node->commit);

    node->destroy.notify = handle_node_destroy;
    wl_signal_add(&surface->events.destroy, &node->destroy);

    struct wlr_subsurface *wlr_subsurface;
    wl_list_for_each(wlr_subsurface, &surface->current.subsurfaces_below, current.link) {
        handle_new_node_subsurface(&node->new_subsurface, wlr_subsurface);
    }
    wl_list_for_each(wlr_subsurface, &surface->current.subsurfaces_above, current.link) {
        handle_new_node_subsurface(&node->new_subsurface, wlr_subsurface);
    }

    return node;
}


static struct planar_surface_tree_node *planar_surface_tree_subsurface_node_create(struct planar_server *server, struct planar_surface_tree_node *parent, struct planar_subsurface *subsurface, struct wlr_surface *surface) {
    struct planar_surface_tree_node *node = planar_surface_tree_create(server, surface);
    node->parent = parent;
    node->subsurface = subsurface;

    return node;
}

struct planar_surface_tree_node *planar_surface_tree_root_create(struct planar_server *server, struct wlr_surface *surface) {

    struct planar_surface_tree_node *node = planar_surface_tree_create(server, surface);

    return node;
}

static void handle_subsurface_map (struct wl_listener *listener, void *data) {
    struct planar_subsurface *subsurface = wl_container_of(listener, subsurface, map);
    struct wlr_subsurface *wlr_subsurface = subsurface->wlr_subsurface;


    subsurface->child = planar_surface_tree_subsurface_node_create(subsurface->server, subsurface->parent, subsurface, wlr_subsurface->surface);

    wlr_log(WLR_INFO, "Mapped subsurface at %p creates node at %p", subsurface, subsurface->child);
}

static void planar_subsurface_destroy(struct planar_subsurface *subsurface) {
    if (subsurface->child) {
        planar_surface_tree_destroy(subsurface->child);
        subsurface->child = NULL;
    }

    wl_list_remove(&subsurface->map.link);
    wl_list_remove(&subsurface->unmap.link);
    wl_list_remove(&subsurface->destroy.link);

    free(subsurface);
}

void planar_surface_tree_destroy(struct planar_surface_tree_node *node) {
    struct planar_subsurface *subsurface;
    wl_list_for_each(subsurface, &node->child_subsurfaces, node_link) {
        planar_subsurface_destroy(subsurface);
    }

    wl_list_remove(&node->new_subsurface.link);
    wl_list_remove(&node->commit.link);
    wl_list_remove(&node->destroy.link);

    free(node);
}

static void handle_subsurface_unmap (struct wl_listener *listener, void *data) {
    struct planar_subsurface *subsurface = wl_container_of(listener, subsurface, unmap);

    wlr_log(WLR_INFO, "Unmapped subsurface at %p with child %p", subsurface, subsurface->child);

    if (subsurface->child) {

        struct planar_surface_tree_node *node = subsurface->child;

        struct wlr_box surface_extents = { 0 };
        /*wlr_surface_get_extends(node->wlr_surface, &surface_extents);
        surface_extents.x += lx;
        surface_extents.y += ly; */

        struct planar_output *output;
        /* wl_list_for_each(output, &node->server->outputs, link) {
            planar_output_damage_layout_coords_box(output, &surface_extents);
        } */

        planar_surface_tree_destroy(subsurface->child);
        subsurface->child = NULL;
    }

}

static void handle_subsurface_destroy (struct wl_listener *listener, void *data) {
    struct planar_subsurface *subsurface = wl_container_of(listener, subsurface, destroy);
    wl_list_remove(&subsurface->node_link);
    wlr_log(WLR_INFO, "Destroyed subsurface at %p", subsurface);
    free(subsurface);
}

static void handle_new_node_subsurface (struct wl_listener *listener, void *data) {
    struct planar_surface_tree_node *node = wl_container_of(listener, node, new_subsurface);

    struct wlr_subsurface *wlr_subsurface = data;

    struct planar_subsurface *subsurface = calloc(1, sizeof(struct planar_subsurface));

    wlr_log(WLR_INFO, "New subsurface at %p", subsurface);

    subsurface->server = node->server;
    subsurface->wlr_subsurface = wlr_subsurface;
    subsurface->parent = node;

    subsurface->map.notify = handle_subsurface_map;
    wl_signal_add(&wlr_subsurface->surface->events.map, &subsurface->map);

    subsurface->unmap.notify = handle_subsurface_unmap;
    wl_signal_add(&wlr_subsurface->surface->events.unmap, &subsurface->unmap);

    subsurface->destroy.notify = handle_subsurface_destroy;
    wl_signal_add(&wlr_subsurface->events.destroy, &subsurface->destroy);

    struct wlr_subsurface *existing_wlr_subsurface;
    wl_list_for_each(existing_wlr_subsurface, &wlr_subsurface->surface->current.subsurfaces_below, current.link) {
        handle_new_node_subsurface(&node->new_subsurface, existing_wlr_subsurface);
    }
    wl_list_for_each(existing_wlr_subsurface, &wlr_subsurface->surface->current.subsurfaces_above, current.link) {
        handle_new_node_subsurface(&node->new_subsurface, existing_wlr_subsurface);
    }

    wl_list_insert(&node->child_subsurfaces, &subsurface->node_link);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    struct planar_surface_tree_node *node = wl_container_of(listener, node, commit);
    struct wlr_surface *surface = node->wlr_surface;
}

static void handle_node_destroy(struct wl_listener *listener, void *data) {
    struct planar_surface_tree_node *node = wl_container_of(listener, node, destroy);

    wlr_log(WLR_INFO, "Destroy node at %p", node);

    struct planar_subsurface *subsurface;
    wl_list_for_each(subsurface, &node->child_subsurfaces, node_link) {
        planar_subsurface_destroy(subsurface);
    }

    wl_list_remove(&node->new_subsurface.link);
    wl_list_remove(&node->commit.link);
    wl_list_remove(&node->destroy.link);

    free(node);
}