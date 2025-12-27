#include "selection.h"
#include "server.h"
#include "toplevel.h"
#include "group.h"
#include "decoration.h"
#include "workspaces.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>

// Selection highlight color (cyan - distinct from group colors)
static const float selection_color[4] = {0.0f, 0.85f, 0.85f, 1.0f};

const float *selection_get_color(void) {
    return selection_color;
}

bool selection_add(struct planar_server *server, struct planar_toplevel *toplevel) {
    if (!server || !toplevel) return false;

    if (selection_contains(server, toplevel)) return false;

    struct planar_selection_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) return false;

    entry->toplevel = toplevel;
    wl_list_insert(&server->selected_toplevels, &entry->link);

    if (toplevel->decoration) {
        decoration_update_geometry(toplevel->decoration);
    }

    return true;
}

bool selection_remove(struct planar_server *server, struct planar_toplevel *toplevel) {
    if (!server || !toplevel) return false;

    struct planar_selection_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &server->selected_toplevels, link) {
        if (entry->toplevel == toplevel) {
            wl_list_remove(&entry->link);
            free(entry);

            if (toplevel->decoration) {
                decoration_update_geometry(toplevel->decoration);
            }

            return true;
        }
    }

    return false;
}

void selection_toggle(struct planar_server *server, struct planar_toplevel *toplevel) {
    if (!server || !toplevel) return;

    if (selection_contains(server, toplevel)) {
        selection_remove(server, toplevel);
    } else {
        selection_add(server, toplevel);
    }
}

void selection_clear(struct planar_server *server) {
    if (!server) return;

    struct planar_selection_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &server->selected_toplevels, link) {
        struct planar_toplevel *toplevel = entry->toplevel;
        wl_list_remove(&entry->link);
        free(entry);

        if (toplevel && toplevel->decoration) {
            decoration_update_geometry(toplevel->decoration);
        }
    }
}

bool selection_contains(struct planar_server *server, struct planar_toplevel *toplevel) {
    if (!server || !toplevel) return false;

    struct planar_selection_entry *entry;
    wl_list_for_each(entry, &server->selected_toplevels, link) {
        if (entry->toplevel == toplevel) {
            return true;
        }
    }

    return false;
}

int selection_count(struct planar_server *server) {
    if (!server) return 0;

    int count = 0;
    struct planar_selection_entry *entry;
    wl_list_for_each(entry, &server->selected_toplevels, link) {
        count++;
    }

    return count;
}

void selection_move_by(struct planar_server *server, double delta_x, double delta_y) {
    if (!server) return;

    struct planar_selection_entry *entry;
    wl_list_for_each(entry, &server->selected_toplevels, link) {
        struct planar_toplevel *toplevel = entry->toplevel;
        if (!toplevel) continue;

        toplevel->logical_x += delta_x;
        toplevel->logical_y += delta_y;

        double scale = toplevel->workspace ? toplevel->workspace->scale : 1.0;
        wlr_scene_node_set_position(&toplevel->container->node,
            toplevel->logical_x * scale,
            toplevel->logical_y * scale);
    }
}

struct planar_group *selection_create_group(struct planar_server *server) {
    if (!server || selection_count(server) < 2) return NULL;

    struct planar_group *group = group_create(server, NULL);
    if (!group) return NULL;

    struct planar_selection_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &server->selected_toplevels, link) {
        if (entry->toplevel) {
            group_add_toplevel(group, entry->toplevel);
        }
    }

    selection_clear(server);

    return group;
}

void selection_start_box(struct planar_server *server, double x, double y) {
    if (!server) return;

    server->selection_box_start_x = x;
    server->selection_box_start_y = y;

    if (!server->selection_box && server->active_workspace) {
        // Semi-transparent cyan to match selection color
        static const float box_color[4] = {0.0f, 0.85f, 0.85f, 0.3f};
        server->selection_box = wlr_scene_rect_create(
            server->active_workspace->scene_tree, 1, 1, box_color);
        if (server->selection_box) {
            wlr_scene_node_set_position(&server->selection_box->node, (int)x, (int)y);
        }
    }
}

void selection_update_box(struct planar_server *server, double x, double y) {
    if (!server || !server->selection_box) return;

    double start_x = server->selection_box_start_x;
    double start_y = server->selection_box_start_y;

    double left = start_x < x ? start_x : x;
    double top = start_y < y ? start_y : y;
    double width = start_x < x ? x - start_x : start_x - x;
    double height = start_y < y ? y - start_y : start_y - y;

    struct planar_workspace *ws = server->active_workspace;
    if (ws) {
        left -= ws->global_offset.x;
        top -= ws->global_offset.y;
    }

    wlr_scene_node_set_position(&server->selection_box->node, (int)left, (int)top);
    wlr_scene_rect_set_size(server->selection_box, (int)width, (int)height);
}

static bool box_intersects_toplevel(struct planar_server *server,
                                    double box_x1, double box_y1,
                                    double box_x2, double box_y2,
                                    struct planar_toplevel *toplevel) {
    if (!toplevel || !toplevel->decoration) return false;

    struct planar_workspace *ws = toplevel->workspace;
    if (!ws || ws != server->active_workspace) return false;

    double scale = ws->scale;

    double tl_x1 = toplevel->logical_x * scale;
    double tl_y1 = toplevel->logical_y * scale;
    double tl_x2 = tl_x1 + toplevel->decoration->width * scale;
    double tl_y2 = tl_y1 + toplevel->decoration->height * scale;

    double bl_x1 = box_x1 - ws->global_offset.x;
    double bl_y1 = box_y1 - ws->global_offset.y;
    double bl_x2 = box_x2 - ws->global_offset.x;
    double bl_y2 = box_y2 - ws->global_offset.y;

    if (bl_x1 > bl_x2) { double t = bl_x1; bl_x1 = bl_x2; bl_x2 = t; }
    if (bl_y1 > bl_y2) { double t = bl_y1; bl_y1 = bl_y2; bl_y2 = t; }

    return !(tl_x2 < bl_x1 || tl_x1 > bl_x2 || tl_y2 < bl_y1 || tl_y1 > bl_y2);
}

void selection_finish_box(struct planar_server *server) {
    if (!server) return;

    double x1 = server->selection_box_start_x;
    double y1 = server->selection_box_start_y;
    double x2 = server->cursor->x;
    double y2 = server->cursor->y;

    if (server->active_workspace) {
        struct planar_toplevel *toplevel;
        wl_list_for_each(toplevel, &server->active_workspace->toplevels, link) {
            if (box_intersects_toplevel(server, x1, y1, x2, y2, toplevel)) {
                selection_add(server, toplevel);
            }
        }
    }

    if (server->selection_box) {
        wlr_scene_node_destroy(&server->selection_box->node);
        server->selection_box = NULL;
    }
}

void selection_cancel_box(struct planar_server *server) {
    if (!server) return;

    if (server->selection_box) {
        wlr_scene_node_destroy(&server->selection_box->node);
        server->selection_box = NULL;
    }
}
