#ifndef PLANAR_WORKSPACES_H
#define PLANAR_WORKSPACES_H

#include "output.h"
#include <wayland-server-core.h>

#define WORKSPACE_COUNT 9

struct planar_workspace {
    struct wl_list link;
    struct wl_list toplevels;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener output_destroy;
    int index;

    struct {
        int x, y;
    } global_offset;
    double scale;
};

void switch_to_workspace(struct planar_server *server, int index);
void active_toplevel_to_workspace(struct planar_server *server, int index);
void update_workspace_offset(struct planar_server *server, int offset_x, int offset_y);
void set_workspace_offset(struct planar_server *server, int offset_x, int offset_y);
void update_workspace_scale(struct planar_server *server, double scale, double pivot_x, double pivot_y);

#endif
