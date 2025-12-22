#include "ipc.h"
#include "config.h"
#include "server.h"
#include "toplevel.h"
#include "workspaces.h"
#include "decoration.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>

#define IPC_BUFFER_SIZE 4096

static void ipc_client_destroy(struct ipc_client *client) {
  wl_list_remove(&client->link);
  wl_event_source_remove(client->event_source);
  close(client->fd);
  free(client);
}

static void send_response(int fd, bool ok, const char *data) {
  char buf[IPC_BUFFER_SIZE];
  int len;
  if (ok) {
    if (data) {
      len = snprintf(buf, sizeof(buf), "{\"ok\":true,\"data\":%s}\n", data);
    } else {
      len = snprintf(buf, sizeof(buf), "{\"ok\":true}\n");
    }
  } else {
    len = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}\n",
                   data ? data : "unknown error");
  }
  write(fd, buf, len);
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

      written = snprintf(ptr, remaining,
          "%s{\"id\":\"%s\",\"app_id\":\"%s\",\"title\":\"%s\","
          "\"workspace\":%d,\"geometry\":{\"x\":%.0f,\"y\":%.0f,"
          "\"width\":%d,\"height\":%d},\"focused\":%s}",
          first ? "" : ",",
          toplevel->window_id ? toplevel->window_id : "",
          app_id, title,
          ws->index + 1,
          toplevel->logical_x, toplevel->logical_y,
          width, height,
          is_focused ? "true" : "false");

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

  char buf[IPC_BUFFER_SIZE];
  snprintf(buf, sizeof(buf),
      "{\"id\":\"%s\",\"app_id\":\"%s\",\"title\":\"%s\","
      "\"workspace\":%d,\"geometry\":{\"x\":%.0f,\"y\":%.0f,"
      "\"width\":%d,\"height\":%d},\"focused\":%s}",
      toplevel->window_id ? toplevel->window_id : "",
      app_id, title,
      toplevel->workspace->index + 1,
      toplevel->logical_x, toplevel->logical_y,
      width, height,
      is_focused ? "true" : "false");

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
  } else if (strcmp(setting, "workspaces") == 0) {
    handle_get_workspaces(server, client_fd);
  } else if (strcmp(setting, "focused") == 0) {
    handle_get_focused(server, client_fd);
  } else {
    send_response(client_fd, false, "unknown setting");
  }
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
    client->event_source = NULL;

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
    wl_list_remove(&client->link);
    close(client->fd);
    free(client);
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
    ssize_t written = write(client->fd, buf, len);
    if (written < 0) {
      wlr_log(WLR_DEBUG, "Event client disconnected");
      wl_list_remove(&client->link);
      close(client->fd);
      free(client);
    }
  }
}
