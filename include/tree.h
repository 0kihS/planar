#ifndef PLANAR_TREE_H
#define PLANAR_TREE_H

#include "server.h"

struct planar_surface_tree_node {
    struct planar_server *server;
    struct wlr_surface *wlr_surface;

    struct wl_listener new_subsurface;
    struct wl_listener commit;
    struct wl_listener destroy;

    struct planar_surface_tree_node *parent;
    struct planar_subsurface *subsurface;

    struct wl_list child_subsurfaces;

    void (*apply_global_offset)(void *, int *, int *);
    void *global_offset_data;
};

struct planar_subsurface {
    struct planar_server *server;
    struct wlr_subsurface *wlr_subsurface;
    struct planar_surface_tree_node *parent;
    struct planar_surface_tree_node *child;

    struct wl_list node_link;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
};


// Create a surface tree from the input surface. The surface tree will automatically wrap
// all of the subsurfaces (existing or later-created) and handle all surface commit
// events.  Commit events will be used to damage every output, with offsets calculated
// including the global offset passed here.
struct planar_surface_tree_node *planar_surface_tree_root_create(struct planar_server *server, struct wlr_surface *surface);

/// Clean up the node's state (bound events etc.) and free it
void planar_surface_tree_destroy(struct planar_surface_tree_node *node);

static void handle_new_node_subsurface (struct wl_listener *listener, void *data);
static void handle_commit(struct wl_listener *listener, void *data);
static void handle_node_destroy(struct wl_listener *listener, void *data);
static void handle_new_node_subsurface (struct wl_listener *listener, void *data);

#endif
