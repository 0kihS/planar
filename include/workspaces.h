#ifndef PLANAR_WORKSPACES_H
#define PLANAR_WORKSPACES_H

#include "output.h"
#include <wayland-server-core.h>

#define WORKSPACE_COUNT 9

struct planar_workspace {
    struct wl_list link;
    struct wl_list toplevels;
    struct wlr_scene_tree *scene_tree;
    struct planar_output *output;
    int index;
    bool visible;

    struct {
        double x;
        double y;
    } global_offset;
};

void switch_to_workspace(struct planar_server *server, int index);
void move_toplevel_to_workspace(struct planar_toplevel *toplevel, struct planar_workspace *new_workspace);

#endif
