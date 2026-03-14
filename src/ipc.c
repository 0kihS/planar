#include "ipc.h"
#include "server.h"
#include "toplevel.h"
#include "workspaces.h"
#include "decoration.h"
#include "group.h"
#include "selection.h"

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

static void ipc_client_destroy(struct ipc_client *client) {
  wl_list_remove(&client->link);
  wl_event_source_remove(client->event_source);
  close(client->fd);
  free(client);
}

static void send_response(int fd, bool ok, const char *data) {
  int data_len = data ? (int)strlen(data) : 0;
  /* JSON wrapping adds ~40 bytes overhead */
  int buf_size = data_len + 64;
  char *buf = malloc(buf_size);
  if (!buf) return;

  int len;
  if (ok) {
    if (data) {
      len = snprintf(buf, buf_size, "{\"ok\":true,\"data\":%s}\n", data);
    } else {
      len = snprintf(buf, buf_size, "{\"ok\":true}\n");
    }
  } else {
    len = snprintf(buf, buf_size, "{\"ok\":false,\"error\":\"%s\"}\n",
                   data ? data : "unknown error");
  }
  if (len > 0) {
    write(fd, buf, len);
  }
  free(buf);
}

static void handle_get_workspaces(struct planar_server *server, int client_fd) {
  char buf[IPC_BUFFER_SIZE];
  char *ptr = buf;
  int remaining = sizeof(buf);
  int written;

  written = snprintf(ptr, remaining, "[");
  ptr += written;
  remaining -= written;

  bool first = true;
  struct planar_workspace *ws;
  wl_list_for_each(ws, &server->workspaces, link) {
    written = snprintf(ptr, remaining,
                       "%s{\"index\":%d,\"active\":%s,\"scale\":%.2f,"
                       "\"offset\":{\"x\":%d,\"y\":%d}}",
                       first ? "" : ",", ws->index + 1,
                       ws == server->active_workspace ? "true" : "false",
                       ws->scale, ws->global_offset.x, ws->global_offset.y);
    ptr += written;
    remaining -= written;
    first = false;
  }

  snprintf(ptr, remaining, "]");
  send_response(client_fd, true, buf);
}

static void handle_get_windows(struct planar_server *server, int client_fd) {
  char buf[IPC_BUFFER_SIZE * 4];
  char *ptr = buf;
  int remaining = sizeof(buf);
  int written;

  struct wlr_surface *focused_surface = server->seat->keyboard_state.focused_surface;

  written = snprintf(ptr, remaining, "[");
  ptr += written;
  remaining -= written;

  bool first = true;
  struct planar_workspace *ws;
  wl_list_for_each(ws, &server->workspaces, link) {
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &ws->toplevels, link) {
      if (!toplevel->xdg_toplevel->base->surface->mapped) continue;

      const char *app_id = toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "";
      const char *title = toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "";
      int width = toplevel->decoration ? toplevel->decoration->width : 0;
      int height = toplevel->decoration ? toplevel->decoration->height : 0;
      bool is_focused = (toplevel->xdg_toplevel->base->surface == focused_surface);

      const char *group_id = toplevel->group ? toplevel->group->group_id : NULL;
      written = snprintf(ptr, remaining,
          "%s{\"id\":\"%s\",\"app_id\":\"%s\",\"title\":\"%s\","
          "\"workspace\":%d,\"geometry\":{\"x\":%.0f,\"y\":%.0f,"
          "\"width\":%d,\"height\":%d},\"focused\":%s,\"group\":%s%s%s}",
          first ? "" : ",",
          toplevel->window_id ? toplevel->window_id : "",
          app_id, title,
          ws->index + 1,
          toplevel->logical_x, toplevel->logical_y,
          width, height,
          is_focused ? "true" : "false",
          group_id ? "\"" : "",
          group_id ? group_id : "null",
          group_id ? "\"" : "");

      ptr += written;
      remaining -= written;
      first = false;
    }
  }

  snprintf(ptr, remaining, "]");
  send_response(client_fd, true, buf);
}

static void handle_get_window(struct planar_server *server, int client_fd, const char *window_id) {
  struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
  if (!toplevel) {
    send_response(client_fd, false, "window not found");
    return;
  }

  struct wlr_surface *focused_surface = server->seat->keyboard_state.focused_surface;
  const char *app_id = toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "";
  const char *title = toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "";
  int width = toplevel->decoration ? toplevel->decoration->width : 0;
  int height = toplevel->decoration ? toplevel->decoration->height : 0;
  bool is_focused = (toplevel->xdg_toplevel->base->surface == focused_surface);
  const char *group_id = toplevel->group ? toplevel->group->group_id : NULL;

  char buf[IPC_BUFFER_SIZE];
  snprintf(buf, sizeof(buf),
      "{\"id\":\"%s\",\"app_id\":\"%s\",\"title\":\"%s\","
      "\"workspace\":%d,\"geometry\":{\"x\":%.0f,\"y\":%.0f,"
      "\"width\":%d,\"height\":%d},\"focused\":%s,\"group\":%s%s%s}",
      toplevel->window_id ? toplevel->window_id : "",
      app_id, title,
      toplevel->workspace->index + 1,
      toplevel->logical_x, toplevel->logical_y,
      width, height,
      is_focused ? "true" : "false",
      group_id ? "\"" : "",
      group_id ? group_id : "null",
      group_id ? "\"" : "");

  send_response(client_fd, true, buf);
}

static void handle_get_focused(struct planar_server *server, int client_fd) {
  struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
  if (!focused) {
    send_response(client_fd, true, "null");
    return;
  }

  struct planar_workspace *ws;
  wl_list_for_each(ws, &server->workspaces, link) {
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &ws->toplevels, link) {
      if (toplevel->xdg_toplevel->base->surface == focused) {
        const char *app_id = toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "";
        const char *title = toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "";
        int width = toplevel->decoration ? toplevel->decoration->width : 0;
        int height = toplevel->decoration ? toplevel->decoration->height : 0;

        char buf[IPC_BUFFER_SIZE];
        snprintf(buf, sizeof(buf),
            "{\"id\":\"%s\",\"app_id\":\"%s\",\"title\":\"%s\","
            "\"workspace\":%d,\"geometry\":{\"x\":%.0f,\"y\":%.0f,"
            "\"width\":%d,\"height\":%d}}",
            toplevel->window_id ? toplevel->window_id : "",
            app_id, title,
            ws->index + 1,
            toplevel->logical_x, toplevel->logical_y,
            width, height);

        send_response(client_fd, true, buf);
        return;
      }
    }
  }

  send_response(client_fd, true, "null");
}

static void handle_get_groups(struct planar_server *server, int client_fd) {
  char buf[IPC_BUFFER_SIZE * 2];
  char *ptr = buf;
  int remaining = sizeof(buf);
  int written;

  written = snprintf(ptr, remaining, "[");
  ptr += written;
  remaining -= written;

  bool first = true;
  struct planar_group *group;
  wl_list_for_each(group, &server->groups, link) {
    written = snprintf(ptr, remaining,
        "%s{\"id\":\"%s\",\"member_count\":%d,\"color\":[%.2f,%.2f,%.2f,%.2f],\"members\":[",
        first ? "" : ",",
        group->group_id,
        group_member_count(group),
        group->border_color[0], group->border_color[1],
        group->border_color[2], group->border_color[3]);
    ptr += written;
    remaining -= written;

    bool first_member = true;
    struct planar_group_member *member;
    wl_list_for_each(member, &group->members, link) {
      if (member->toplevel && member->toplevel->window_id) {
        written = snprintf(ptr, remaining, "%s\"%s\"",
            first_member ? "" : ",",
            member->toplevel->window_id);
        ptr += written;
        remaining -= written;
        first_member = false;
      }
    }

    written = snprintf(ptr, remaining, "]}");
    ptr += written;
    remaining -= written;
    first = false;
  }

  snprintf(ptr, remaining, "]");
  send_response(client_fd, true, buf);
}

static void handle_get_group(struct planar_server *server, int client_fd, const char *group_id) {
  struct planar_group *group = find_group_by_id(server, group_id);
  if (!group) {
    send_response(client_fd, false, "group not found");
    return;
  }

  char buf[IPC_BUFFER_SIZE];
  char *ptr = buf;
  int remaining = sizeof(buf);
  int written;

  written = snprintf(ptr, remaining,
      "{\"id\":\"%s\",\"member_count\":%d,\"color\":[%.2f,%.2f,%.2f,%.2f],\"members\":[",
      group->group_id,
      group_member_count(group),
      group->border_color[0], group->border_color[1],
      group->border_color[2], group->border_color[3]);
  ptr += written;
  remaining -= written;

  bool first = true;
  struct planar_group_member *member;
  wl_list_for_each(member, &group->members, link) {
    if (member->toplevel && member->toplevel->window_id) {
      written = snprintf(ptr, remaining, "%s\"%s\"",
          first ? "" : ",",
          member->toplevel->window_id);
      ptr += written;
      remaining -= written;
      first = false;
    }
  }

  snprintf(ptr, remaining, "]}");
  send_response(client_fd, true, buf);
}

static void handle_get_selection(struct planar_server *server, int client_fd) {
  char buf[IPC_BUFFER_SIZE];
  char *ptr = buf;
  int remaining = sizeof(buf);
  int written;

  written = snprintf(ptr, remaining, "[");
  ptr += written;
  remaining -= written;

  bool first = true;
  struct planar_selection_entry *entry;
  wl_list_for_each(entry, &server->selected_toplevels, link) {
    if (entry->toplevel && entry->toplevel->window_id) {
      written = snprintf(ptr, remaining, "%s\"%s\"",
          first ? "" : ",",
          entry->toplevel->window_id);
      ptr += written;
      remaining -= written;
      first = false;
    }
  }

  snprintf(ptr, remaining, "]");
  send_response(client_fd, true, buf);
}

static void handle_get_command(struct planar_server *server, int client_fd,
                               const char *args) {
  char setting[64];
  if (sscanf(args, "%63s", setting) != 1) {
    send_response(client_fd, false, "missing setting name");
    return;
  }

  char buf[256];
  if (strcmp(setting, "border_width") == 0) {
    snprintf(buf, sizeof(buf), "%d", server->settings.border_width);
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "border_color") == 0) {
    snprintf(buf, sizeof(buf), "[%.2f,%.2f,%.2f,%.2f]",
             server->settings.border_color[0], server->settings.border_color[1],
             server->settings.border_color[2],
             server->settings.border_color[3]);
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "zoom_min") == 0) {
    snprintf(buf, sizeof(buf), "%.2f", server->settings.zoom_min);
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "zoom_max") == 0) {
    snprintf(buf, sizeof(buf), "%.2f", server->settings.zoom_max);
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "zoom_step") == 0) {
    snprintf(buf, sizeof(buf), "%.2f", server->settings.zoom_step);
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "cursor_size") == 0) {
    snprintf(buf, sizeof(buf), "%d", server->settings.cursor_size);
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "snap_enabled") == 0) {
    snprintf(buf, sizeof(buf), "%s", server->settings.snap_enabled ? "true" : "false");
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "snap_threshold") == 0) {
    snprintf(buf, sizeof(buf), "%d", server->settings.snap_threshold);
    send_response(client_fd, true, buf);
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
      send_response(client_fd, false, "missing window id");
    }
  } else if (strcmp(setting, "groups") == 0) {
    handle_get_groups(server, client_fd);
  } else if (strncmp(setting, "group", 5) == 0) {
    const char *group_id = args + 6;
    while (*group_id == ' ') group_id++;
    if (*group_id) {
      handle_get_group(server, client_fd, group_id);
    } else {
      send_response(client_fd, false, "missing group id");
    }
  } else if (strcmp(setting, "selection") == 0) {
    handle_get_selection(server, client_fd);
  } else if (strcmp(setting, "nodecoration") == 0) {
    char buf[IPC_BUFFER_SIZE];
    char *ptr = buf;
    int remaining = sizeof(buf);
    int written;

    written = snprintf(ptr, remaining, "[");
    ptr += written;
    remaining -= written;

    for (size_t i = 0; i < server->window_rules.nodecoration_count; i++) {
      written = snprintf(ptr, remaining, "%s\"%s\"",
          i == 0 ? "" : ",",
          server->window_rules.nodecoration[i]);
      ptr += written;
      remaining -= written;
    }
    snprintf(ptr, remaining, "]");
    send_response(client_fd, true, buf);
  } else if (strcmp(setting, "ontop") == 0) {
    char buf[IPC_BUFFER_SIZE];
    char *ptr = buf;
    int remaining = sizeof(buf);
    int written;

    written = snprintf(ptr, remaining, "[");
    ptr += written;
    remaining -= written;

    for (size_t i = 0; i < server->window_rules.ontop_count; i++) {
      written = snprintf(ptr, remaining, "%s\"%s\"",
          i == 0 ? "" : ",",
          server->window_rules.ontop[i]);
      ptr += written;
      remaining -= written;
    }
    snprintf(ptr, remaining, "]");
    send_response(client_fd, true, buf);
  } else {
    send_response(client_fd, false, "unknown setting");
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
  focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);

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
        return true;
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
      focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
      return true;
    }
    return false;
  }

  if (strncmp(cmd, "close_window ", 13) == 0) {
    const char *window_id = cmd + 13;
    struct planar_toplevel *toplevel = find_toplevel_by_id(server, window_id);
    if (toplevel) {
      wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
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
        toplevel->logical_x = x;
        toplevel->logical_y = y;
        wlr_scene_node_set_position(&toplevel->container->node,
            (int)x, (int)y);
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
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
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
      focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
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
    struct planar_workspace *ws = server->active_workspace;
    struct planar_toplevel *t;
    wl_list_for_each(t, &ws->toplevels, link) {
      if (t->xdg_toplevel->base->surface == focused) {
        selection_toggle(server, t);
        return true;
      }
    }
    return false;
  }

  if (strcmp(cmd, "tile_focused_group") == 0) {
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    if (!focused) return false;
    
    struct wlr_xdg_toplevel *xdg = wlr_xdg_toplevel_try_from_wlr_surface(focused);
    if (!xdg) return false;
    
    /* container->data points to toplevel */
    /* Need to find the planar_toplevel from the surface */
    struct planar_workspace *ws = server->active_workspace;
    struct planar_toplevel *toplevel;
    wl_list_for_each(toplevel, &ws->toplevels, link) {
      if (toplevel->xdg_toplevel->base->surface == focused) {
        if (toplevel->group) {
          /* Tile the group */
          struct planar_group *group = toplevel->group;
          double base_x = toplevel->logical_x;
          double base_y = toplevel->logical_y;

          struct planar_group_member *member;
          double x = base_x;
          wl_list_for_each(member, &group->members, link) {
            struct planar_toplevel *t = member->toplevel;
            double win_w = t->decoration ? t->decoration->width : 100;
            
            t->logical_x = x;
            t->logical_y = base_y;
            scale_toplevel(t);
            
            x += win_w + 10;
          }
          
          group_recalculate_offsets(group);
          group_update_decorations(group);
          return true;
        }
        return false;
      }
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
    send_response(client_fd, false, "empty command");
    return;
  }

  wlr_log(WLR_DEBUG, "IPC command: %s", cmd);

  if (strncmp(cmd, "get ", 4) == 0) {
    handle_get_command(server, client_fd, cmd + 4);
    return;
  }

  if (ipc_dispatch_command(server, cmd)) {
    send_response(client_fd, true, NULL);
  } else {
    send_response(client_fd, false, "unknown or invalid command");
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

  struct ipc_client *client, *tmp;
  wl_list_for_each_safe(client, tmp, &server->ipc_event_clients, link) {
    if (write(client->fd, buf, len) < 0) {
      wlr_log(WLR_DEBUG, "Event client disconnected");
    }
  }
}
