#ifndef PLANAR_DECORATION_H
#define PLANAR_DECORATION_H

#include <scenefx/types/wlr_scene.h>
#include <wlr/util/edges.h>
#include <stdbool.h>

#define DECORATION_BORDER_WIDTH 4

struct planar_toplevel;
struct planar_server;

struct planar_decoration {
    struct planar_toplevel *toplevel;
    struct wlr_scene_tree *tree;

    struct wlr_scene_rect *border_top;
    struct wlr_scene_rect *border_bottom;
    struct wlr_scene_rect *border_left;
    struct wlr_scene_rect *border_right;

    int width;
    int height;
};

struct planar_decoration *decoration_create(struct planar_toplevel *toplevel);
void decoration_destroy(struct planar_decoration *decoration);
void decoration_update_geometry(struct planar_decoration *decoration);

// Returns which edge the point is on, or -1 if not on decoration
int decoration_get_edge_at(struct planar_decoration *decoration, double lx, double ly, uint32_t *edges);

#endif // PLANAR_DECORATION_H
