#include "output.h"
#include "layers.h"
#include "toplevel.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

void output_frame(struct wl_listener *listener, void *data) {
    /* This function is called every time an output is ready to display a frame,
     * generally at the output's refresh rate (e.g. 60Hz). */
    struct planar_output *output = wl_container_of(listener, output, frame);
    struct planar_server *server = output->server;
    struct wlr_scene *scene = server->scene;
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
        scene, output->wlr_output);

    // Apply the global offset to the scene output
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        // Adjust the position by global_offset
        wlr_scene_node_set_position(&toplevel->scene_tree->node,
                                    toplevel->scene_tree->node.x + round(server->global_offset.x),
                                    toplevel->scene_tree->node.y + round(server->global_offset.y));
    }

    arrange_layers(output);

    /* Render the scene if needed and commit the output */
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

        wl_list_for_each(toplevel, &server->toplevels, link) {
    // Reset the position back to original
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
                                toplevel->scene_tree->node.x - round(server->global_offset.x),
                                toplevel->scene_tree->node.y - round(server->global_offset.y));
}

    wlr_scene_output_send_frame_done(scene_output, &now);
}

void output_request_state(struct wl_listener *listener, void *data) {
    struct planar_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

void output_destroy(struct wl_listener *listener, void *data) {
    struct planar_output *output = wl_container_of(listener, output, destroy);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);

    struct planar_layer_surface *layer_view;
    wl_list_for_each(layer_view, &output->layer_views, output_link){
        layer_view->output = NULL;
        wlr_layer_surface_v1_destroy(layer_view->layer_surface);
    }
    free(output);
}

void output_create(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct planar_output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;
    output->server = server;

    wl_list_init(&output->layer_views);

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout,
        wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}