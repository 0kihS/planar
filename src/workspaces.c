#include "server.h"
#include "workspaces.h"
#include "toplevel.h"

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
        focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
        break;
    }
            struct planar_output *output;

    wl_list_for_each(output, &server->outputs, link) {
            wlr_output_schedule_frame(output->wlr_output);
        }
}


void move_toplevel_to_workspace(struct planar_toplevel *toplevel, struct planar_workspace *new_workspace) {
    if (toplevel->workspace == new_workspace) {
        return;
    }

    // Remove from old workspace
    wl_list_remove(&toplevel->link);

    // Add to new workspace
    wl_list_insert(&new_workspace->toplevels, &toplevel->link);
    toplevel->workspace = new_workspace;

    // Update visibility
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, new_workspace->visible);
}
