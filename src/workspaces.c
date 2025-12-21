#include "server.h"
#include "workspaces.h"
#include "toplevel.h"
#include "output.h"

void set_workspace_offset(struct planar_server *server, int offset_x, int offset_y) {
    struct planar_workspace *active_workspace = server->active_workspace;
    
    // Update the stored offset
    active_workspace->global_offset.x = offset_x;
    active_workspace->global_offset.y = offset_y;

    // Move the workspace tree
    wlr_scene_node_set_position(&active_workspace->scene_tree->node, offset_x, offset_y);

    // Request a frame to render the changes
    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void update_workspace_offset(struct planar_server *server, int offset_x, int offset_y) {
    struct planar_workspace *active_workspace = server->active_workspace;
    
    // Update the stored offset
    active_workspace->global_offset.x += offset_x;
    active_workspace->global_offset.y += offset_y;

    // Move the workspace tree
    struct wlr_scene_node *node = &active_workspace->scene_tree->node;
    wlr_scene_node_set_position(node, node->x + offset_x, node->y + offset_y);

    // Request a frame to render the changes
    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void switch_to_workspace(struct planar_server *server, int index) {
    struct planar_workspace *new_workspace;
    wlr_scene_node_set_enabled(&server->active_workspace->scene_tree->node, false);

    wl_list_for_each(new_workspace, &server->workspaces, link) {
        if (new_workspace->index == index) {
            server->active_workspace = new_workspace;
            break;
        }
    }
    
    // Update focus
    wlr_scene_node_set_enabled(&server->active_workspace->scene_tree->node, true);
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
    wlr_scene_node_reparent(&toplevel->container->node, new_workspace->scene_tree);
}

void update_workspace_scale(struct planar_server *server, double scale, double pivot_x, double pivot_y) {
     struct planar_workspace *ws = server->active_workspace;
     double old_scale = ws->scale;
     
     if (scale < 0.1) scale = 0.1;
     if (scale > 5.0) scale = 5.0;
    
     ws->scale = scale;

     double pw_x = (pivot_x - ws->global_offset.x) / old_scale;
     double pw_y = (pivot_y - ws->global_offset.y) / old_scale;
     
     ws->global_offset.x = pivot_x - pw_x * scale;
     ws->global_offset.y = pivot_y - pw_y * scale;
    
     // Apply new offset to workspace tree
    wlr_scene_node_set_position(&ws->scene_tree->node,
        ws->global_offset.x, ws->global_offset.y);
        
    // Scale all toplevels
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &ws->toplevels, link) {
        scale_toplevel(toplevel, scale);
    }
    
    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}