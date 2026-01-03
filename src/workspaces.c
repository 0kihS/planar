#include "server.h"
#include "workspaces.h"
#include "toplevel.h"
#include "output.h"
#include "ipc.h"
#include "group.h"
#include "selection.h"
#include <stdio.h>

void set_workspace_offset(struct planar_server *server, int offset_x, int offset_y) {
    struct planar_workspace *active_workspace = server->active_workspace;

    active_workspace->global_offset.x = offset_x;
    active_workspace->global_offset.y = offset_y;

    wlr_scene_node_set_position(&active_workspace->scene_tree->node, offset_x, offset_y);

    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void update_workspace_offset(struct planar_server *server, int offset_x, int offset_y) {
    struct planar_workspace *active_workspace = server->active_workspace;

    active_workspace->global_offset.x += offset_x;
    active_workspace->global_offset.y += offset_y;

    struct wlr_scene_node *node = &active_workspace->scene_tree->node;
    wlr_scene_node_set_position(node, node->x + offset_x, node->y + offset_y);

    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void switch_to_workspace(struct planar_server *server, int index) {
    struct planar_workspace *new_workspace;

    // Clear selection when switching workspaces
    selection_clear(server);

    wlr_scene_node_set_enabled(&server->active_workspace->scene_tree->node, false);

    wl_list_for_each(new_workspace, &server->workspaces, link) {
        if (new_workspace->index == index) {
            server->active_workspace = new_workspace;
            break;
        }
    }

    wlr_scene_node_set_enabled(&server->active_workspace->scene_tree->node, true);
    struct planar_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", index + 1);
    ipc_broadcast_event(server, "workspace", buf);
}

void active_toplevel_to_workspace(struct planar_server *server, int index) {
    struct planar_workspace *new_workspace = NULL;
    struct planar_toplevel *toplevel = NULL;
    bool found_toplevel = false;

    wl_list_for_each_reverse(toplevel, &server->active_workspace->toplevels, link) {
        if (toplevel->xdg_toplevel->base->surface == server->seat->keyboard_state.focused_surface) {
            found_toplevel = true;
            break;
        }
    }

    if (!found_toplevel) {
        return;
    }

    struct planar_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (ws->index == index) {
            new_workspace = ws;
            break;
        }
    }

    if (toplevel->workspace == new_workspace || !toplevel ||
        toplevel->xdg_toplevel->base->surface != server->seat->keyboard_state.focused_surface) {
        return;
    }

    if (toplevel->group) {
        group_move_to_workspace(toplevel->group, new_workspace);
    } else {
        wl_list_remove(&toplevel->link);
        wl_list_insert(&new_workspace->toplevels, &toplevel->link);
        toplevel->workspace = new_workspace;
        wlr_scene_node_reparent(&toplevel->container->node, new_workspace->scene_tree);
    }
}

void update_workspace_scale(struct planar_server *server, double scale, double pivot_x, double pivot_y) {
    struct planar_workspace *ws = server->active_workspace;
    double old_scale = ws->scale;

    if (scale < server->settings.zoom_min) scale = server->settings.zoom_min;
    if (scale > server->settings.zoom_max) scale = server->settings.zoom_max;

    ws->scale = scale;

    double pw_x = (pivot_x - ws->global_offset.x) / old_scale;
    double pw_y = (pivot_y - ws->global_offset.y) / old_scale;

    ws->global_offset.x = pivot_x - pw_x * scale;
    ws->global_offset.y = pivot_y - pw_y * scale;

    wlr_scene_node_set_position(&ws->scene_tree->node,
        ws->global_offset.x, ws->global_offset.y);
    wlr_scene_node_set_scale(&ws->scene_tree->node, scale);

    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}
