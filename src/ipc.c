#include "ipc.h"
#include "server.h"
#include "toplevel.h"
#include "workspaces.h"
#include "decoration.h"
#include "group.h"
#include "selection.h"
#include "cursor.h"

#include <errno.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#define IPC_BUFFER_SIZE 4096

static bool write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t written = write(fd, buf, len);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    buf += written;
    len -= written;
  }
  return true;
}

static void ipc_client_destroy(struct ipc_client *client) {
  wl_list_remove(&client->link);
  wl_event_source_remove(client->event_source);
  close(client->fd);
  free(client);
}

static void send_json_response(int fd, bool ok, json_object *data,
                               const char *error) {
  json_object *response = json_object_new_object();
  if (!response) {
    return;
  }

  json_object_object_add(response, "ok", json_object_new_boolean(ok));
  if (ok) {
    if (data) {
      json_object_object_add(response, "data", data);
    }
  } else {
    json_object_object_add(response, "error",
        json_object_new_string(error ? error : "unknown error"));
  }

  const char *json = json_object_to_json_string_ext(response,
      JSON_C_TO_STRING_PLAIN);
  write_all(fd, json, strlen(json));
  write_all(fd, "\n", 1);

  json_object_put(response);
}

static void send_ok(int fd) {
  send_json_response(fd, true, NULL, NULL);
}

static void send_error(int fd, const char *error) {
  send_json_response(fd, false, NULL, error);
}

static void send_json_data(int fd, json_object *data) {
  if (!data) {
    send_error(fd, "out of memory");
    return;
  }
  send_json_response(fd, true, data, NULL);
}

static json_object *json_string_or_empty(const char *value) {
  return json_object_new_string(value ? value : "");
}

static json_object *json_geometry(double x, double y, int width, int height) {
  json_object *geometry = json_object_new_object();
  if (!geometry) {
    return NULL;
  }

  json_object_object_add(geometry, "x", json_object_new_int((int)lround(x)));
  json_object_object_add(geometry, "y", json_object_new_int((int)lround(y)));
  json_object_object_add(geometry, "width", json_object_new_int(width));
  json_object_object_add(geometry, "height", json_object_new_int(height));
  return geometry;
}

static json_object *json_toplevel(struct planar_server *server,
                                  struct planar_toplevel *toplevel,
                                  bool include_focused,
                                  bool include_group) {
  json_object *window = json_object_new_object();
  if (!window) {
    return NULL;
  }

  struct wlr_box geo;
  toplevel_get_geometry(toplevel, &geo);
  int width = toplevel->decoration ? toplevel->decoration->width : geo.width;
  int height = toplevel->decoration ? toplevel->decoration->height : geo.height;

  json_object_object_add(window, "id",
      json_string_or_empty(toplevel->window_id));
  json_object_object_add(window, "app_id",
      json_string_or_empty(toplevel_get_app_id(toplevel)));
  json_object_object_add(window, "title",
      json_string_or_empty(toplevel_get_title(toplevel)));
  json_object_object_add(window, "workspace",
      json_object_new_int(toplevel->workspace ? toplevel->workspace->index + 1 : 0));
  json_object_object_add(window, "geometry",
      json_geometry(toplevel->logical_x, toplevel->logical_y, width, height));

  if (include_focused) {
    struct wlr_surface *focused_surface =
        server->seat->keyboard_state.focused_surface;
    json_object_object_add(window, "focused",
        json_object_new_boolean(toplevel_get_surface(toplevel) == focused_surface));
  }

  if (include_group) {
    if (toplevel->group) {
      json_object_object_add(window, "group",
          json_string_or_empty(toplevel->group->group_id));
    } else {
      json_object_object_add(window, "group", json_object_new_null());
    }
  }

  return window;
}

static json_object *json_group(struct planar_group *group) {
  json_object *group_json = json_object_new_object();
  json_object *color = json_object_new_array();
  json_object *members = json_object_new_array();
  if (!group_json || !color || !members) {
    json_object_put(group_json);
    json_object_put(color);
    json_object_put(members);
    return NULL;
  }

  json_object_object_add(group_json, "id", json_string_or_empty(group->group_id));
  json_object_object_add(group_json, "member_count",
      json_object_new_int(group_member_count(group)));

  for (size_t i = 0; i < 4; i++) {
    json_object_array_add(color, json_object_new_double(group->border_color[i]));
  }
  json_object_object_add(group_json, "color", color);

  struct planar_group_member *member;
  wl_list_for_each(member, &group->members, link) {
    if (member->toplevel && member->toplevel->window_id) {
      json_object_array_add(members,
          json_string_or_empty(member->toplevel->window_id));
    }
  }
  json_object_object_add(group_json, "members", members);

  return group_json;
}

static json_object *json_string_array(char **values, size_t count) {
  json_object *array = json_object_new_array();
  if (!array) {
    return NULL;
  }

  for (size_t i = 0; i < count; i++) {
    json_object_array_add(array, json_string_or_empty(values[i]));
  }
  return array;
}

static void handle_get_workspaces(struct planar_server *server, int client_fd) {
  json_object *workspaces = json_object_new_array();
  if (!workspaces) {
    send_error(client_fd, "out of memory");
    return;
  }

  struct planar_workspace *ws;
  wl_list_for_each(ws, &server->workspaces, link) {
    json_object *workspace = json_object_new_object();
    json_object *offset = json_object_new_object();
    if (!workspace || !offset) {
      json_object_put(workspace);
      json_object_put(offset);
      json_object_put(workspaces);
      send_error(client_fd, "out of memory");
      return;
    }

    json_object_object_add(workspace, "index", json_object_new_int(ws->index + 1));
    json_object_object_add(workspace, "active",
        json_object_new_boolean(ws == server->active_workspace));
    json_object_object_add(workspace, "scale", json_object_new_double(ws->scale));
    json_object_object_add(offset, "x", json_object_new_int(ws->global_offset.x));
    json_object_object_add(offset, "y", json_object_new_int(ws->global_offset.y));
    json_object_object_add(workspace, "offset", offset);
    json_object_array_add(workspaces, workspace);
  }

  send_json_data(client_fd, workspaces);
}

static void handle_get_windows(struct planar_server *server, int client_fd) {
  json_object *windows = json_object_new_array();
  if (!windows) {
    send_error(client_fd, "out of memory");
    return;
  }

  struct planar_workspace *ws;
  wl_list_for_each(ws, &server->workspaces, link) {
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &ws->toplevels, link) {
      if (!toplevel_is_mapped(toplevel)) continue;

      json_object *window = json_toplevel(server, toplevel, true, true);
      if (!window) {
        json_object_put(windows);
        send_error(client_fd, "out of memory");
        return;
      }
      json_object_array_add(windows, window);
    }
  }

  send_json_data(client_fd, windows);
}

static void handle_get_window(struct planar_server *server, int client_fd, const char *window_id) {
  struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
  if (!toplevel) {
    send_error(client_fd, "window not found");
    return;
  }

  send_json_data(client_fd, json_toplevel(server, toplevel, true, true));
}

static void handle_get_focused(struct planar_server *server, int client_fd) {
  struct planar_toplevel *toplevel =
      find_toplevel_by_surface(server, server->seat->keyboard_state.focused_surface);
  if (!toplevel) {
    send_json_data(client_fd, json_object_new_null());
    return;
  }

  send_json_data(client_fd, json_toplevel(server, toplevel, false, false));
}

static void handle_get_groups(struct planar_server *server, int client_fd) {
  json_object *groups = json_object_new_array();
  if (!groups) {
    send_error(client_fd, "out of memory");
    return;
  }

  struct planar_group *group;
  wl_list_for_each(group, &server->groups, link) {
    json_object *group_json = json_group(group);
    if (!group_json) {
      json_object_put(groups);
      send_error(client_fd, "out of memory");
      return;
    }
    json_object_array_add(groups, group_json);
  }

  send_json_data(client_fd, groups);
}

static void handle_get_group(struct planar_server *server, int client_fd, const char *group_id) {
  struct planar_group *group = find_group_by_id(server, group_id);
  if (!group) {
    send_error(client_fd, "group not found");
    return;
  }

  send_json_data(client_fd, json_group(group));
}

static void handle_get_selection(struct planar_server *server, int client_fd) {
  json_object *selection = json_object_new_array();
  if (!selection) {
    send_error(client_fd, "out of memory");
    return;
  }

  struct planar_selection_entry *entry;
  wl_list_for_each(entry, &server->selected_toplevels, link) {
    if (entry->toplevel && entry->toplevel->window_id) {
      json_object_array_add(selection,
          json_string_or_empty(entry->toplevel->window_id));
    }
  }

  send_json_data(client_fd, selection);
}

static void handle_get_command(struct planar_server *server, int client_fd,
                               const char *args) {
  char setting[64];
  if (sscanf(args, "%63s", setting) != 1) {
    send_error(client_fd, "missing setting name");
    return;
  }

  if (strcmp(setting, "border_width") == 0) {
    send_json_data(client_fd, json_object_new_int(server->settings.border_width));
  } else if (strcmp(setting, "border_color") == 0) {
    json_object *color = json_object_new_array();
    if (!color) {
      send_error(client_fd, "out of memory");
      return;
    }
    for (size_t i = 0; i < 4; i++) {
      json_object_array_add(color,
          json_object_new_double(server->settings.border_color[i]));
    }
    send_json_data(client_fd, color);
  } else if (strcmp(setting, "zoom_min") == 0) {
    send_json_data(client_fd, json_object_new_double(server->settings.zoom_min));
  } else if (strcmp(setting, "zoom_max") == 0) {
    send_json_data(client_fd, json_object_new_double(server->settings.zoom_max));
  } else if (strcmp(setting, "zoom_step") == 0) {
    send_json_data(client_fd, json_object_new_double(server->settings.zoom_step));
  } else if (strcmp(setting, "cursor_size") == 0) {
    send_json_data(client_fd, json_object_new_int(server->settings.cursor_size));
  } else if (strcmp(setting, "snap_enabled") == 0) {
    send_json_data(client_fd, json_object_new_boolean(server->settings.snap_enabled));
  } else if (strcmp(setting, "snap_threshold") == 0) {
    send_json_data(client_fd, json_object_new_int(server->settings.snap_threshold));
  } else if (strcmp(setting, "workspaces") == 0) {
    handle_get_workspaces(server, client_fd);
  } else if (strcmp(setting, "focused") == 0) {
    handle_get_focused(server, client_fd);
  } else if (strcmp(setting, "windows") == 0) {
    handle_get_windows(server, client_fd);
  } else if (strncmp(setting, "window", 6) == 0) {
    const char *window_id = args + 7;
    while (*window_id == ' ') window_id++;
    if (*window_id) {
      handle_get_window(server, client_fd, window_id);
    } else {
      send_error(client_fd, "missing window id");
    }
  } else if (strcmp(setting, "groups") == 0) {
    handle_get_groups(server, client_fd);
  } else if (strncmp(setting, "group", 5) == 0) {
    const char *group_id = args + 6;
    while (*group_id == ' ') group_id++;
    if (*group_id) {
      handle_get_group(server, client_fd, group_id);
    } else {
      send_error(client_fd, "missing group id");
    }
  } else if (strcmp(setting, "selection") == 0) {
    handle_get_selection(server, client_fd);
  } else if (strcmp(setting, "nodecoration") == 0) {
    send_json_data(client_fd, json_string_array(
        server->window_rules.nodecoration,
        server->window_rules.nodecoration_count));
  } else if (strcmp(setting, "ontop") == 0) {
    send_json_data(client_fd, json_string_array(
        server->window_rules.ontop,
        server->window_rules.ontop_count));
  } else {
    send_error(client_fd, "unknown setting");
  }
}

static uint32_t get_current_time_msec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Find a keycode that produces the given keysym (returns 0 if not found)
static xkb_keycode_t find_keycode_for_keysym(struct xkb_keymap *keymap,
                                              struct xkb_state *state,
                                              xkb_keysym_t keysym,
                                              bool *needs_shift) {
  *needs_shift = false;

  // Iterate through all possible keycodes (typically 8-255 for evdev)
  xkb_keycode_t min_keycode = xkb_keymap_min_keycode(keymap);
  xkb_keycode_t max_keycode = xkb_keymap_max_keycode(keymap);

  for (xkb_keycode_t keycode = min_keycode; keycode <= max_keycode; keycode++) {
    // Try without shift first
    xkb_keysym_t sym = xkb_state_key_get_one_sym(state, keycode);
    if (sym == keysym) {
      return keycode;
    }
  }

  // Try with shift modifier
  for (xkb_keycode_t keycode = min_keycode; keycode <= max_keycode; keycode++) {
    // Get keysyms at level 1 (shifted)
    const xkb_keysym_t *syms;
    xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
    int num_syms =
        xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 1, &syms);
    for (int i = 0; i < num_syms; i++) {
      if (syms[i] == keysym) {
        *needs_shift = true;
        return keycode;
      }
    }
  }

  return 0;
}

static bool handle_send_keys(struct planar_server *server, const char *args) {
  char window_id[256];
  char text[1024];

  // Parse: window_id followed by text (rest of line)
  const char *space = strchr(args, ' ');
  if (!space) {
    wlr_log(WLR_ERROR, "send_keys: missing text argument");
    return false;
  }

  size_t id_len = space - args;
  if (id_len >= sizeof(window_id)) {
    id_len = sizeof(window_id) - 1;
  }
  strncpy(window_id, args, id_len);
  window_id[id_len] = '\0';

  // Skip space and get the rest as text
  const char *text_start = space + 1;
  strncpy(text, text_start, sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';

  // Find target window
  struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
  if (!toplevel) {
    wlr_log(WLR_ERROR, "send_keys: window '%s' not found", window_id);
    return false;
  }

  // Ensure window has focus
  if (toplevel->workspace != server->active_workspace) {
    switch_to_workspace(server, toplevel->workspace->index);
  }
  focus_toplevel(toplevel, toplevel_get_surface(toplevel));

  // Get keyboard
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
  if (!keyboard || !keyboard->xkb_state) {
    wlr_log(WLR_ERROR, "send_keys: no keyboard available");
    return false;
  }

  struct xkb_keymap *keymap = xkb_state_get_keymap(keyboard->xkb_state);
  uint32_t time_msec = get_current_time_msec();

  wlr_log(WLR_DEBUG, "send_keys: sending '%s' to window '%s'", text, window_id);

  // Find shift keycode for when we need it
  xkb_keysym_t shift_sym = XKB_KEY_Shift_L;
  bool dummy;
  xkb_keycode_t shift_keycode =
      find_keycode_for_keysym(keymap, keyboard->xkb_state, shift_sym, &dummy);

  // Process each character
  for (const char *c = text; *c; c++) {
    xkb_keysym_t keysym;

    // Handle special escape sequences
    if (*c == '\\' && *(c + 1)) {
      c++;
      switch (*c) {
      case 'n':
        keysym = XKB_KEY_Return;
        break;
      case 't':
        keysym = XKB_KEY_Tab;
        break;
      case 'e':
        keysym = XKB_KEY_Escape;
        break;
      case '\\':
        keysym = XKB_KEY_backslash;
        break;
      default:
        keysym = xkb_utf32_to_keysym((uint32_t)*c);
      }
    } else {
      // Convert UTF-8 character to keysym
      keysym = xkb_utf32_to_keysym((uint32_t)(unsigned char)*c);
    }

    if (keysym == XKB_KEY_NoSymbol) {
      wlr_log(WLR_DEBUG, "send_keys: no keysym for character '%c'", *c);
      continue;
    }

    bool needs_shift = false;
    xkb_keycode_t keycode =
        find_keycode_for_keysym(keymap, keyboard->xkb_state, keysym, &needs_shift);

    if (keycode == 0) {
      wlr_log(WLR_DEBUG, "send_keys: no keycode for keysym 0x%x", keysym);
      continue;
    }

    // keycode for wlr_seat_keyboard_notify_key needs to be evdev keycode
    // (XKB keycode - 8)
    uint32_t evdev_keycode = keycode - 8;

    // Press shift if needed
    if (needs_shift && shift_keycode != 0) {
      wlr_seat_keyboard_notify_key(server->seat, time_msec++, shift_keycode - 8,
                                   WL_KEYBOARD_KEY_STATE_PRESSED);
    }

    // Press the key
    wlr_seat_keyboard_notify_key(server->seat, time_msec++, evdev_keycode,
                                 WL_KEYBOARD_KEY_STATE_PRESSED);

    // Release the key
    wlr_seat_keyboard_notify_key(server->seat, time_msec++, evdev_keycode,
                                 WL_KEYBOARD_KEY_STATE_RELEASED);

    // Release shift if needed
    if (needs_shift && shift_keycode != 0) {
      wlr_seat_keyboard_notify_key(server->seat, time_msec++, shift_keycode - 8,
                                   WL_KEYBOARD_KEY_STATE_RELEASED);
    }
  }

  return true;
}

bool ipc_dispatch_command(struct planar_server *server, const char *cmd) {
  if (strncmp(cmd, "workspace ", 10) == 0) {
    int ws;
    if (sscanf(cmd + 10, "%d", &ws) == 1) {
      switch_to_workspace(server, ws - 1);
      return true;
    }
    return false;
  }

  if (strcmp(cmd, "killactive") == 0) {
    kill_active_toplevel(server);
    return true;
  }

  if (strncmp(cmd, "move_to_workspace ", 18) == 0) {
    int ws;
    if (sscanf(cmd + 18, "%d", &ws) == 1) {
      active_toplevel_to_workspace(server, ws - 1);
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "move_workspace ", 15) == 0) {
    int x, y;
    if (sscanf(cmd + 15, "%d %d", &x, &y) == 2) {
      update_workspace_offset(server, x, y);
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "set ", 4) == 0) {
    const char *args = cmd + 4;
    char setting[64];
    if (sscanf(args, "%63s", setting) != 1)
      return false;
    const char *value = args + strlen(setting);
    while (*value == ' ')
      value++;

    if (strcmp(setting, "border_width") == 0) {
      int width;
      if (sscanf(value, "%d", &width) == 1 && width >= 0 && width <= 32) {
        server->settings.border_width = width;
        return true;
      }
    } else if (strcmp(setting, "border_color") == 0) {
      float r, g, b, a;
      if (sscanf(value, "%f %f %f %f", &r, &g, &b, &a) == 4) {
        server->settings.border_color[0] = r;
        server->settings.border_color[1] = g;
        server->settings.border_color[2] = b;
        server->settings.border_color[3] = a;
        return true;
      }
    } else if (strcmp(setting, "zoom_min") == 0) {
      double val;
      if (sscanf(value, "%lf", &val) == 1 && val > 0 &&
          val < server->settings.zoom_max) {
        server->settings.zoom_min = val;
        return true;
      }
    } else if (strcmp(setting, "zoom_max") == 0) {
      double val;
      if (sscanf(value, "%lf", &val) == 1 && val > server->settings.zoom_min) {
        server->settings.zoom_max = val;
        return true;
      }
    } else if (strcmp(setting, "zoom_step") == 0) {
      double val;
      if (sscanf(value, "%lf", &val) == 1 && val > 0) {
        server->settings.zoom_step = val;
        return true;
      }
    } else if (strcmp(setting, "cursor_size") == 0) {
      int size;
      if (sscanf(value, "%d", &size) == 1 && size >= 8 && size <= 128) {
        server->settings.cursor_size = size;
        return cursor_reload_theme(server);
      }
    } else if (strcmp(setting, "snap_enabled") == 0) {
      if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        server->settings.snap_enabled = true;
        return true;
      } else if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        server->settings.snap_enabled = false;
        return true;
      }
    } else if (strcmp(setting, "snap_threshold") == 0) {
      int threshold;
      if (sscanf(value, "%d", &threshold) == 1 && threshold >= 0 && threshold <= 100) {
        server->settings.snap_threshold = threshold;
        return true;
      }
    }
    return false;
  }

  if (strncmp(cmd, "focus_window ", 13) == 0) {
    const char *window_id = cmd + 13;
    struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
    if (toplevel) {
      if (toplevel->workspace != server->active_workspace) {
        switch_to_workspace(server, toplevel->workspace->index);
      }
      focus_toplevel(toplevel, toplevel_get_surface(toplevel));
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "close_window ", 13) == 0) {
    const char *window_id = cmd + 13;
    struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
    if (toplevel) {
      toplevel_close(toplevel);
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "move_window ", 12) == 0) {
    char window_id[256];
    double x, y;
    if (sscanf(cmd + 12, "%255s %lf %lf", window_id, &x, &y) == 3) {
      struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
      if (toplevel) {
        move_toplevel(toplevel, x, y);
        return true;
      }
    }
    return false;
  }

  if (strncmp(cmd, "resize_window ", 14) == 0) {
    char window_id[256];
    int width, height;
    if (sscanf(cmd + 14, "%255s %d %d", window_id, &width, &height) == 3) {
      struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
      if (toplevel && width > 0 && height > 0) {
        toplevel_set_size(toplevel, width, height);
        return true;
      }
    }
    return false;
  }

  if (strncmp(cmd, "move_window_to_workspace ", 25) == 0) {
    char window_id[256];
    int workspace_idx;
    if (sscanf(cmd + 25, "%255s %d", window_id, &workspace_idx) == 2) {
      struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
      if (toplevel && workspace_idx >= 1 && workspace_idx <= 9) {
        struct planar_workspace *target_ws = NULL;
        struct planar_workspace *ws;
        wl_list_for_each(ws, &server->workspaces, link) {
          if (ws->index == workspace_idx - 1) {
            target_ws = ws;
            break;
          }
        }
        if (target_ws && target_ws != toplevel->workspace) {
          if (toplevel->group) {
            group_move_to_workspace(toplevel->group, target_ws);
          } else {
            wl_list_remove(&toplevel->link);
            wl_list_insert(&target_ws->toplevels, &toplevel->link);
            wlr_scene_node_reparent(&toplevel->container->node, target_ws->scene_tree);
            toplevel->workspace = target_ws;
            scale_toplevel(toplevel);
          }
          return true;
        }
      }
    }
    return false;
  }

  if (strncmp(cmd, "goto_window ", 12) == 0) {
    const char *window_id = cmd + 12;
    struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
    if (toplevel) {
      // Switch workspace if needed
      if (toplevel->workspace != server->active_workspace) {
        switch_to_workspace(server, toplevel->workspace->index);
      }

      // Get window center in logical coordinates
      int win_width = toplevel->decoration ? toplevel->decoration->width : 100;
      int win_height = toplevel->decoration ? toplevel->decoration->height : 100;
      double win_center_x = toplevel->logical_x + win_width / 2.0;
      double win_center_y = toplevel->logical_y + win_height / 2.0;

      // Get screen center (use first output)
      int screen_width = 1920, screen_height = 1080; // fallback
      struct planar_output *output;
      wl_list_for_each(output, &server->outputs, link) {
        screen_width = output->wlr_output->width;
        screen_height = output->wlr_output->height;
        break;
      }
      double screen_center_x = screen_width / 2.0;
      double screen_center_y = screen_height / 2.0;

      // Calculate offset to center the window
      // screen_pos = logical_pos * scale + offset
      // We want: screen_center = win_center * scale + new_offset
      // So: new_offset = screen_center - win_center * scale
      struct planar_workspace *ws = toplevel->workspace;
      int new_offset_x = (int)(screen_center_x - win_center_x * ws->scale);
      int new_offset_y = (int)(screen_center_y - win_center_y * ws->scale);

      set_workspace_offset(server, new_offset_x, new_offset_y);
      focus_toplevel(toplevel, toplevel_get_surface(toplevel));
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "spawn ", 6) == 0) {
    const char *command = cmd + 6;
    pid_t pid = fork();
    if (pid == 0) {
      setsid();
      char *cmd_copy = strdup(command);
      char *argv[64] = {0};
      int argc = 0;
      char *token = strtok(cmd_copy, " ");
      while (token && argc < 63) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
      }
      argv[argc] = NULL;
      execvp(argv[0], argv);
      _exit(1);
    } else if (pid > 0) {
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "zoom ", 5) == 0) {
    double scale;
    if (sscanf(cmd + 5, "%lf", &scale) == 1) {
      update_workspace_scale(server, scale, server->cursor->x, server->cursor->y);
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "send_keys ", 10) == 0) {
    return handle_send_keys(server, cmd + 10);
  }

  if (strcmp(cmd, "select_focused") == 0) {
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    if (!focused) return false;
    struct planar_toplevel *toplevel = find_toplevel_by_surface(server, focused);
    if (toplevel) {
      selection_toggle(server, toplevel);
      return true;
    }
    return false;
  }

  if (strcmp(cmd, "tile_focused_group") == 0) {
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    if (!focused) return false;

    struct planar_toplevel *toplevel = find_toplevel_by_surface(server, focused);
    if (toplevel && toplevel->group) {
      struct planar_group *group = toplevel->group;
      double base_x = toplevel->logical_x;
      double base_y = toplevel->logical_y;

      struct planar_group_member *member;
      double x = base_x;
      wl_list_for_each(member, &group->members, link) {
        struct planar_toplevel *t = member->toplevel;
        double win_w = t->decoration ? t->decoration->width : 100;

        move_toplevel(t, x, base_y);
        x += win_w + 10;
      }

      group_recalculate_offsets(group);
      group_update_decorations(group);
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "group ", 6) == 0) {
    const char *subcmd = cmd + 6;

    if (strncmp(subcmd, "create", 6) == 0 && (subcmd[6] == ' ' || subcmd[6] == '\0')) {
      const char *name = subcmd + 6;
      while (*name == ' ') name++;
      struct planar_group *group = group_create(server, *name ? name : NULL);
      return group != NULL;
    }

    if (strncmp(subcmd, "delete ", 7) == 0) {
      const char *group_id = subcmd + 7;
      struct planar_group *group = find_group_by_id(server, group_id);
      if (group) {
        group_destroy(group);
        return true;
      }
      return false;
    }

    if (strncmp(subcmd, "add ", 4) == 0) {
      char group_id[256], window_id[256];
      if (sscanf(subcmd + 4, "%255s %255s", group_id, window_id) == 2) {
        struct planar_group *group = find_group_by_id(server, group_id);
        struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
        if (group && toplevel) {
          return group_add_toplevel(group, toplevel);
        }
      }
      return false;
    }

    if (strncmp(subcmd, "remove ", 7) == 0) {
      char group_id[256], window_id[256];
      if (sscanf(subcmd + 7, "%255s %255s", group_id, window_id) == 2) {
        struct planar_group *group = find_group_by_id(server, group_id);
        struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
        if (group && toplevel) {
          bool result = group_remove_toplevel(group, toplevel);
          if (group_is_empty(group)) {
            group_destroy(group);
          }
          return result;
        }
      }
      return false;
    }

    if (strncmp(subcmd, "move ", 5) == 0) {
      char group_id[256];
      double x, y;
      if (sscanf(subcmd + 5, "%255s %lf %lf", group_id, &x, &y) == 3) {
        struct planar_group *group = find_group_by_id(server, group_id);
        if (group) {
          group_move_to(group, x, y);
          return true;
        }
      }
      return false;
    }

    if (strncmp(subcmd, "color ", 6) == 0) {
      char group_id[256];
      float r, g, b, a;
      if (sscanf(subcmd + 6, "%255s %f %f %f %f", group_id, &r, &g, &b, &a) == 5) {
        struct planar_group *group = find_group_by_id(server, group_id);
        if (group) {
          group->border_color[0] = r;
          group->border_color[1] = g;
          group->border_color[2] = b;
          group->border_color[3] = a;
          group_update_decorations(group);
          return true;
        }
      }
      return false;
    }

    if (strcmp(subcmd, "create_from_selection") == 0) {
      struct planar_group *group = selection_create_group(server);
      return group != NULL;
    }

    if (strncmp(subcmd, "tile ", 5) == 0) {
      const char *group_id = subcmd + 5;
      while (*group_id == ' ') group_id++;
      struct planar_group *group = find_group_by_id(server, group_id);
      if (!group || group_member_count(group) == 0) return false;

      /* Get the anchor position */
      struct planar_toplevel *anchor = group_get_anchor(group);
      if (!anchor) return false;
      double base_x = anchor->logical_x;
      double base_y = anchor->logical_y;

      /* Tile members horizontally from anchor */
      struct planar_group_member *member;
      double x = base_x;
      wl_list_for_each(member, &group->members, link) {
        struct planar_toplevel *t = member->toplevel;
        double win_w = t->decoration ? t->decoration->width : 100;
        
        t->logical_x = x;
        t->logical_y = base_y;
        scale_toplevel(t);
        
        x += win_w + 10; /* 10px gap */
      }
      
      group_recalculate_offsets(group);
      group_update_decorations(group);
      return true;
    }

    return false;
  }

  if (strncmp(cmd, "select ", 7) == 0) {
    const char *subcmd = cmd + 7;

    if (strcmp(subcmd, "clear") == 0) {
      selection_clear(server);
      return true;
    }

    if (strncmp(subcmd, "add ", 4) == 0) {
      const char *window_id = subcmd + 4;
      struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
      if (toplevel) {
        return selection_add(server, toplevel);
      }
      return false;
    }

    if (strncmp(subcmd, "remove ", 7) == 0) {
      const char *window_id = subcmd + 7;
      struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
      if (toplevel) {
        return selection_remove(server, toplevel);
      }
      return false;
    }

    if (strncmp(subcmd, "toggle ", 7) == 0) {
      const char *window_id = subcmd + 7;
      struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
      if (toplevel) {
        selection_toggle(server, toplevel);
        return true;
      }
      return false;
    }

    if (strcmp(subcmd, "all") == 0) {
      struct planar_toplevel *toplevel;
      wl_list_for_each(toplevel, &server->active_workspace->toplevels, link) {
        selection_add(server, toplevel);
      }
      return true;
    }

    return false;
  }

  if (strcmp(cmd, "group_selection") == 0) {
    struct planar_group *group = selection_create_group(server);
    return group != NULL;
  }

  if (strcmp(cmd, "clear_selection") == 0) {
    selection_clear(server);
    return true;
  }

  if (strncmp(cmd, "rule ", 5) == 0) {
    const char *subcmd = cmd + 5;

    if (strncmp(subcmd, "nodecoration add ", 17) == 0) {
      const char *app_id = subcmd + 17;
      while (*app_id == ' ') app_id++;
      return window_rules_add_nodecoration(server, app_id);
    }

    if (strncmp(subcmd, "nodecoration remove ", 20) == 0) {
      const char *app_id = subcmd + 20;
      while (*app_id == ' ') app_id++;
      return window_rules_remove_nodecoration(server, app_id);
    }

    if (strncmp(subcmd, "ontop add ", 10) == 0) {
      const char *app_id = subcmd + 10;
      while (*app_id == ' ') app_id++;
      return window_rules_add_ontop(server, app_id);
    }

    if (strncmp(subcmd, "ontop remove ", 13) == 0) {
      const char *app_id = subcmd + 13;
      while (*app_id == ' ') app_id++;
      return window_rules_remove_ontop(server, app_id);
    }

    return false;
  }

  return false;
}

static void handle_command(struct planar_server *server, int client_fd,
                           char *cmd) {
  while (*cmd == ' ' || *cmd == '\t')
    cmd++;
  char *end = cmd + strlen(cmd) - 1;
  while (end > cmd &&
         (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
    *end-- = '\0';
  }

  if (strlen(cmd) == 0) {
    send_error(client_fd, "empty command");
    return;
  }

  wlr_log(WLR_DEBUG, "IPC command: %s", cmd);

  if (strncmp(cmd, "get ", 4) == 0) {
    handle_get_command(server, client_fd, cmd + 4);
    return;
  }

  if (ipc_dispatch_command(server, cmd)) {
    send_ok(client_fd);
  } else {
    send_error(client_fd, "unknown or invalid command");
  }
}

static int ipc_client_handler(int fd, uint32_t mask, void *data) {
  struct ipc_client *client = data;

  if (mask & WL_EVENT_ERROR || mask & WL_EVENT_HANGUP) {
    ipc_client_destroy(client);
    return 0;
  }

  if (mask & WL_EVENT_READABLE) {
    char buf[IPC_BUFFER_SIZE];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    if (len <= 0) {
      ipc_client_destroy(client);
      return 0;
    }
    buf[len] = '\0';
    handle_command(client->server, fd, buf);
  }

  return 0;
}

static int ipc_event_client_handler(int fd, uint32_t mask, void *data) {
  struct ipc_client *client = data;
  (void)fd;

  if (mask & WL_EVENT_ERROR || mask & WL_EVENT_HANGUP) {
    wlr_log(WLR_DEBUG, "Event client disconnected");
    ipc_client_destroy(client);
    return 0;
  }

  return 0;
}

static int ipc_socket_handler(int fd, uint32_t mask, void *data) {
  struct planar_server *server = data;

  if (mask & WL_EVENT_ERROR || mask & WL_EVENT_HANGUP) {
    wlr_log(WLR_ERROR, "IPC socket error");
    return 0;
  }

  if (mask & WL_EVENT_READABLE) {
    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
      wlr_log(WLR_ERROR, "Failed to accept IPC connection: %s",
              strerror(errno));
      return 0;
    }

    struct ipc_client *client = calloc(1, sizeof(*client));
    client->fd = client_fd;
    client->server = server;

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    client->event_source = wl_event_loop_add_fd(
        loop, client_fd, WL_EVENT_READABLE, ipc_client_handler, client);

    wl_list_insert(&server->ipc_clients, &client->link);
  }

  return 0;
}

static int ipc_event_socket_handler(int fd, uint32_t mask, void *data) {
  struct planar_server *server = data;

  if (mask & WL_EVENT_ERROR || mask & WL_EVENT_HANGUP) {
    wlr_log(WLR_ERROR, "IPC event socket error");
    return 0;
  }

  if (mask & WL_EVENT_READABLE) {
    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
      wlr_log(WLR_ERROR, "Failed to accept IPC event connection: %s",
              strerror(errno));
      return 0;
    }

    struct ipc_client *client = calloc(1, sizeof(*client));
    client->fd = client_fd;
    client->server = server;

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    client->event_source = wl_event_loop_add_fd(
        loop, client_fd, 0, ipc_event_client_handler, client);

    wl_list_insert(&server->ipc_event_clients, &client->link);
    wlr_log(WLR_DEBUG, "New event subscriber connected");
  }

  return 0;
}

static int create_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    wlr_log(WLR_ERROR, "Failed to create IPC socket: %s", strerror(errno));
    return -1;
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  unlink(path);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    wlr_log(WLR_ERROR, "Failed to bind IPC socket: %s", strerror(errno));
    close(fd);
    return -1;
  }

  if (listen(fd, 10) < 0) {
    wlr_log(WLR_ERROR, "Failed to listen on IPC socket: %s", strerror(errno));
    close(fd);
    return -1;
  }

  return fd;
}

bool ipc_init(struct planar_server *server) {
  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (!runtime_dir) {
    wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR not set");
    return false;
  }

  char socket_path[256];
  snprintf(socket_path, sizeof(socket_path), "%s/planar.%s.sock", runtime_dir,
           server->socket);

  server->ipc_socket = create_socket(socket_path);
  if (server->ipc_socket < 0) {
    return false;
  }

  setenv("PLANAR_SOCKET", socket_path, 1);
  wlr_log(WLR_INFO, "IPC socket: %s", socket_path);

  char event_socket_path[256];
  snprintf(event_socket_path, sizeof(event_socket_path),
           "%s/planar.%s.event.sock", runtime_dir, server->socket);

  server->ipc_event_socket = create_socket(event_socket_path);
  if (server->ipc_event_socket < 0) {
    close(server->ipc_socket);
    unlink(socket_path);
    return false;
  }

  setenv("PLANAR_EVENT_SOCKET", event_socket_path, 1);
  wlr_log(WLR_INFO, "IPC event socket: %s", event_socket_path);

  struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);

  server->ipc_event_source = wl_event_loop_add_fd(
      loop, server->ipc_socket, WL_EVENT_READABLE, ipc_socket_handler, server);

  server->ipc_event_socket_source =
      wl_event_loop_add_fd(loop, server->ipc_event_socket, WL_EVENT_READABLE,
                           ipc_event_socket_handler, server);

  return true;
}

void ipc_finish(struct planar_server *server) {
  struct ipc_client *client, *tmp;

  wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
    ipc_client_destroy(client);
  }

  wl_list_for_each_safe(client, tmp, &server->ipc_event_clients, link) {
    ipc_client_destroy(client);
  }

  if (server->ipc_event_source) {
    wl_event_source_remove(server->ipc_event_source);
  }
  if (server->ipc_event_socket_source) {
    wl_event_source_remove(server->ipc_event_socket_source);
  }

  if (server->ipc_socket >= 0) {
    close(server->ipc_socket);
  }
  if (server->ipc_event_socket >= 0) {
    close(server->ipc_event_socket);
  }

  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (runtime_dir && server->socket) {
    char path[256];
    snprintf(path, sizeof(path), "%s/planar.%s.sock", runtime_dir,
             server->socket);
    unlink(path);
    snprintf(path, sizeof(path), "%s/planar.%s.event.sock", runtime_dir,
             server->socket);
    unlink(path);
  }
}

void ipc_broadcast_event(struct planar_server *server, const char *event_type,
                         const char *data) {
  char buf[IPC_BUFFER_SIZE];
  int len;
  if (data) {
    len = snprintf(buf, sizeof(buf), "%s %s\n", event_type, data);
  } else {
    len = snprintf(buf, sizeof(buf), "%s\n", event_type);
  }
  if (len < 0) {
    return;
  }
  if ((size_t)len >= sizeof(buf)) {
    len = sizeof(buf) - 1;
    buf[len] = '\0';
  }

  struct ipc_client *client, *tmp;
  wl_list_for_each_safe(client, tmp, &server->ipc_event_clients, link) {
    if (!write_all(client->fd, buf, len)) {
      wlr_log(WLR_DEBUG, "Event client disconnected");
    }
  }
}

void ipc_broadcast_json_event(struct planar_server *server, const char *event_type,
                              json_object *data) {
  if (!data) {
    return;
  }

  const char *json = json_object_to_json_string_ext(data,
      JSON_C_TO_STRING_PLAIN);
  ipc_broadcast_event(server, event_type, json);
  json_object_put(data);
}
