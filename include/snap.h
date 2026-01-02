#ifndef PLANAR_SNAP_H
#define PLANAR_SNAP_H

#include <wayland-server-core.h>
#include <stdbool.h>

struct planar_server;
struct planar_toplevel;
struct wlr_scene_tree;
struct wlr_scene_rect;

struct planar_snap_guide {
    struct wl_list link;
    struct wlr_scene_rect *rect;
    bool is_vertical;
};

struct planar_snap_state {
    struct planar_server *server;
    struct wlr_scene_tree *tree;
    struct wl_list guides;
    bool active;
};

// Lifecycle
void snap_init(struct planar_server *server);
void snap_finish(struct planar_server *server);

// Called when starting/ending a move operation
void snap_start(struct planar_server *server);
void snap_end(struct planar_server *server);

// Called during move to detect snaps and update guides
// Modifies logical_x/logical_y if snap is applied
void snap_update(struct planar_server *server, struct planar_toplevel *moving,
                 double *logical_x, double *logical_y, int moving_width, int moving_height);

#endif // PLANAR_SNAP_H
