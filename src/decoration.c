#include "decoration.h"
#include "toplevel.h"
#include "server.h"
#include "group.h"
#include "selection.h"

#include <stdlib.h>
#include <wlr/types/wlr_xdg_shell.h>

static int get_border_width(struct planar_toplevel *toplevel) {
    if (toplevel && toplevel->server) {
        return toplevel->server->settings.border_width;
    }
    return 4;
}

static const float *get_border_color(struct planar_toplevel *toplevel) {
    static const float fallback[4] = {0.3f, 0.3f, 0.3f, 1.0f};
    if (toplevel) {
        if (toplevel->server && selection_contains(toplevel->server, toplevel)) {
            return selection_get_color();
        }
        if (toplevel->group) {
            return toplevel->group->border_color;
        }
        if (toplevel->server) {
            return toplevel->server->settings.border_color;
        }
    }
    return fallback;
}

struct planar_decoration *decoration_create(struct planar_toplevel *toplevel) {
    struct planar_decoration *decoration = calloc(1, sizeof(*decoration));
    if (!decoration) {
        return NULL;
    }

    decoration->toplevel = toplevel;

    decoration->tree = wlr_scene_tree_create(toplevel->container);
    if (!decoration->tree) {
        free(decoration);
        return NULL;
    }

    int border = get_border_width(toplevel);
    const float *color = get_border_color(toplevel);

    decoration->border_top = wlr_scene_rect_create(decoration->tree,
        0, border, color);
    decoration->border_bottom = wlr_scene_rect_create(decoration->tree,
        0, border, color);
    decoration->border_left = wlr_scene_rect_create(decoration->tree,
        border, 0, color);
    decoration->border_right = wlr_scene_rect_create(decoration->tree,
        border, 0, color);

    wlr_scene_node_place_below(&decoration->tree->node, &toplevel->scene_tree->node);

    return decoration;
}

void decoration_destroy(struct planar_decoration *decoration) {
    if (!decoration) return;

    if (decoration->tree) {
        wlr_scene_node_destroy(&decoration->tree->node);
        decoration->tree = NULL;
    }
    free(decoration);
}

void decoration_update_geometry(struct planar_decoration *decoration) {
    if (!decoration || !decoration->toplevel || !decoration->tree) return;

    struct planar_toplevel *toplevel = decoration->toplevel;
    struct wlr_box *geo = &toplevel->xdg_toplevel->base->geometry;

    int width = geo->width;
    int height = geo->height;

    if (width <= 0 || height <= 0) {
        return;
    }

    decoration->width = width;
    decoration->height = height;

    int border = get_border_width(toplevel);
    const float *color = get_border_color(toplevel);
    int total_width = width + 2 * border;
    int total_height = height + 2 * border;

    wlr_scene_rect_set_color(decoration->border_top, color);
    wlr_scene_rect_set_color(decoration->border_bottom, color);
    wlr_scene_rect_set_color(decoration->border_left, color);
    wlr_scene_rect_set_color(decoration->border_right, color);

    wlr_scene_node_set_position(&decoration->tree->node, 0, 0);

    wlr_scene_rect_set_size(decoration->border_top, total_width, border);
    wlr_scene_node_set_position(&decoration->border_top->node, 0, 0);

    wlr_scene_rect_set_size(decoration->border_bottom, total_width, border);
    wlr_scene_node_set_position(&decoration->border_bottom->node, 0, total_height - border);

    wlr_scene_rect_set_size(decoration->border_left, border, height);
    wlr_scene_node_set_position(&decoration->border_left->node, 0, border);

    wlr_scene_rect_set_size(decoration->border_right, border, height);
    wlr_scene_node_set_position(&decoration->border_right->node, total_width - border, border);
}

int decoration_get_edge_at(struct planar_decoration *decoration, double lx, double ly, uint32_t *edges) {
    if (!decoration || !decoration->toplevel || !decoration->tree) return -1;

    *edges = 0;

    struct planar_toplevel *toplevel = decoration->toplevel;
    
    int container_lx, container_ly;
    if (!wlr_scene_node_coords(&toplevel->container->node, &container_lx, &container_ly)) {
        return -1;
    }

    float total_scale = 1.0;
    struct wlr_scene_node *it = &toplevel->container->node;
    while (it) {
        total_scale *= it->scale;
        it = it->parent ? &it->parent->node : NULL;
    }

    int border = get_border_width(toplevel) * total_scale;
    int width = decoration->width * total_scale;
    int height = decoration->height * total_scale;

    int dec_left = container_lx;
    int dec_top = container_ly;
    int dec_right = container_lx + width + 2 * border;
    int dec_bottom = container_ly + height + 2 * border;

    int client_left = container_lx + border;
    int client_top = container_ly + border;
    int client_right = container_lx + border + width;
    int client_bottom = container_ly + border + height;

    if (lx < dec_left || lx >= dec_right || ly < dec_top || ly >= dec_bottom) {
        return -1;
    }

    if (lx >= client_left && lx < client_right &&
        ly >= client_top && ly < client_bottom) {
        return -1;
    }

    if (lx < client_left) {
        *edges |= WLR_EDGE_LEFT;
    } else if (lx >= client_right) {
        *edges |= WLR_EDGE_RIGHT;
    }

    if (ly < client_top) {
        *edges |= WLR_EDGE_TOP;
    } else if (ly >= client_bottom) {
        *edges |= WLR_EDGE_BOTTOM;
    }

    return (*edges != 0) ? 1 : -1;
}
