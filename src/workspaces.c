#include "server.h"
#include "workspaces.h"
#include "toplevel.h"
#include "output.h"
#include "ipc.h"
#include "group.h"
#include "selection.h"
#include "cursor.h"
#include <stdio.h>

static struct planar_workspace *workspace_by_index(struct planar_server *server, int index) {
    if (index < 0 || index >= WORKSPACE_COUNT) {
        return NULL;
    }

    struct planar_workspace *workspace;
    wl_list_for_each(workspace, &server->workspaces, link) {
        if (workspace->index == index) {
            return workspace;
        }
    }

    return NULL;
}

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

bool switch_to_workspace(struct planar_server *server, int index) {
    struct planar_workspace *new_workspace = workspace_by_index(server, index);
    if (!new_workspace) {
        return false;
    }

    // Clear selection when switching workspaces
    selection_clear(server);

    wlr_scene_node_set_enabled(&server->active_workspace->scene_tree->node, false);

    server->active_workspace = new_workspace;
    wlr_scene_node_set_enabled(&server->active_workspace->scene_tree->node, true);

    if (server->seat) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        wlr_seat_pointer_clear_focus(server->seat);
    }

    if (server->seat && server->cursor &&
            server->cursor_mode == PLANAR_CURSOR_PASSTHROUGH) {
        process_cursor_motion(server, server->cursor->x, server->cursor->y, 0);
    }

    struct planar_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", index + 1);
    ipc_broadcast_event(server, "workspace", buf);
    return true;
}

bool active_toplevel_to_workspace(struct planar_server *server, int index) {
    struct planar_workspace *new_workspace = workspace_by_index(server, index);
    if (!new_workspace) {
        return false;
    }

    struct planar_toplevel *toplevel =
        find_toplevel_by_surface(server, server->seat->keyboard_state.focused_surface);

    if (!toplevel) {
        return false;
    }

    if (toplevel->workspace == new_workspace) {
        return true;
    }

    if (toplevel->group) {
        group_move_to_workspace(toplevel->group, new_workspace);
    } else {
        wl_list_remove(&toplevel->link);
        wl_list_insert(&new_workspace->toplevels, &toplevel->link);
        toplevel->workspace = new_workspace;
        wlr_scene_node_reparent(&toplevel->container->node, new_workspace->scene_tree);
        scale_toplevel(toplevel);
    }
    return true;
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
