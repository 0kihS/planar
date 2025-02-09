#include "server.h"
#include "workspaces.h"
#include "toplevel.h"
#include "output.h"

void set_workspace_offset(struct planar_server *server, int offset_x, int offset_y) {
    struct planar_workspace *active_workspace = server->active_workspace;
    
    // Calculate the change in offset
    double dx = offset_x - active_workspace->global_offset.x;
    double dy = offset_y - active_workspace->global_offset.y;
    
    // Update the stored offset
    active_workspace->global_offset.x = offset_x;
    active_workspace->global_offset.y = offset_y;

    // Move all windows in the workspace
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &active_workspace->toplevels, link) {
        struct wlr_scene_node *node = &toplevel->scene_tree->node;
        wlr_scene_node_set_position(node, node->x + dx, node->y + dy);
    }

    // Request a frame to render the changes
    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void update_workspace_offset(struct planar_server *server, int offset_x, int offset_y) {
    struct planar_workspace *active_workspace = server->active_workspace;
    
    // Move all windows in the workspace
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &active_workspace->toplevels, link) {
        struct wlr_scene_node *node = &toplevel->scene_tree->node;
        wlr_scene_node_set_position(node, 
            node->x + offset_x,
            node->y + offset_y);
    }

    // Update the stored offset
    active_workspace->global_offset.x += offset_x;
    active_workspace->global_offset.y += offset_y;

    // Request a frame to render the changes
    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void switch_to_workspace(struct planar_server *server, int index) {
    struct planar_workspace *new_workspace;
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->active_workspace->toplevels, link) {
        wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    }

    wl_list_for_each(new_workspace, &server->workspaces, link) {
        if (new_workspace->index == index) {
            server->active_workspace = new_workspace;
            break;
        }
    }
    
    // Update focus

    wl_list_for_each_reverse(toplevel, &server->active_workspace->toplevels, link) {
        wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
        focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
        break;
    }
            struct planar_output *output;

    wl_list_for_each(output, &server->outputs, link) {
            wlr_output_schedule_frame(output->wlr_output);
        }
}


void active_toplevel_to_workspace(struct planar_server *server, int index) {
    struct planar_workspace *new_workspace;
    struct planar_toplevel *toplevel = NULL;
    bool found_toplevel = 0;

    wl_list_for_each_reverse(toplevel, &server->active_workspace->toplevels, link) {
        if (toplevel->xdg_toplevel->base->surface == server->seat->keyboard_state.focused_surface) {
            found_toplevel = 1;
            break;
        }
    }

    if(!found_toplevel) {
        return;
    }

    struct planar_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (ws->index == index) {
            new_workspace = ws;
            break;
        }
    }

    if (toplevel->workspace == new_workspace || !toplevel || toplevel->xdg_toplevel->base->surface != server->seat->keyboard_state.focused_surface) {
        return;
    }

    // Remove from old workspace
    wl_list_remove(&toplevel->link);

    // Add to new workspace
    wl_list_insert(&new_workspace->toplevels, &toplevel->link);
    toplevel->workspace = new_workspace;

    wlr_scene_node_set_position(&toplevel->scene_tree->node,
                round(new_workspace->global_offset.x * -1),
                round(new_workspace->global_offset.y * -1));

    // Update visibility
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, new_workspace->visible);
}