#include "config.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wlr/util/log.h>
#include <unistd.h>

static uint32_t parse_modifiers(const char *mod_str) {
    uint32_t mods = 0;
    if (strstr(mod_str, "shift")) mods |= WLR_MODIFIER_SHIFT;
    if (strstr(mod_str, "caps")) mods |= WLR_MODIFIER_CAPS;
    if (strstr(mod_str, "ctrl")) mods |= WLR_MODIFIER_CTRL;
    if (strstr(mod_str, "alt")) mods |= WLR_MODIFIER_ALT;
    if (strstr(mod_str, "mod2")) mods |= WLR_MODIFIER_MOD2;
    if (strstr(mod_str, "mod3")) mods |= WLR_MODIFIER_MOD3;
    if (strstr(mod_str, "logo")) mods |= WLR_MODIFIER_LOGO;
    if (strstr(mod_str, "mod5")) mods |= WLR_MODIFIER_MOD5;
    return mods;
}

static xkb_keysym_t parse_key(const char *key_str) {
    // Convert key string to uppercase for consistency
    char *upper_key = strdup(key_str);
    for (int i = 0; upper_key[i]; i++) {
        upper_key[i] = toupper(upper_key[i]);
    }

    // Add XKB_KEY_ prefix
    char xkb_key[64] = "XKB_KEY_";
    strcat(xkb_key, upper_key);
    free(upper_key);

    // Get keysym from string
    xkb_keysym_t sym = xkb_keysym_from_name(xkb_key, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol) {
        // Try without prefix
        sym = xkb_keysym_from_name(key_str, XKB_KEYSYM_CASE_INSENSITIVE);
    }
    return sym;
}

struct config *config_load(const char *path) {
    struct config *config = calloc(1, sizeof(struct config));
    if (!config) {
        return NULL;
    }

    const char *config_path = path;
    char default_path[256];
    
    // Properly resolve the home directory
    if (!config_path) {
        const char *home_dir = getenv("HOME");
        if (!home_dir) {
            wlr_log(WLR_ERROR, "Could not determine home directory");
            free(config);
            return NULL;
        }

        snprintf(default_path, sizeof(default_path), "%s/.config/planar/config.json", home_dir);
        config_path = default_path;
    }

    // Read and parse JSON file
    json_object *root = json_object_from_file(config_path);
    if (!root) {
        wlr_log(WLR_ERROR, "Failed to load config file: %s", config_path);
        free(config);
        return NULL;
    }

    // Parse startup command
    json_object *startup_obj;
    if (json_object_object_get_ex(root, "startup_command", &startup_obj)) {
        const char *startup_cmd = json_object_get_string(startup_obj);
        if (startup_cmd) {
            config->startup_cmd = strdup(startup_cmd);
        }
    }

    // Parse keybindings
    json_object *keybindings_obj;
    if (json_object_object_get_ex(root, "keybindings", &keybindings_obj)) {
        int len = json_object_array_length(keybindings_obj);
        for (int i = 0; i < len && i < MAX_KEYBINDINGS; i++) {
            json_object *binding = json_object_array_get_idx(keybindings_obj, i);

            json_object *modifiers_obj, *key_obj, *command_obj;
            if (json_object_object_get_ex(binding, "modifiers", &modifiers_obj) &&
                json_object_object_get_ex(binding, "key", &key_obj) &&
                json_object_object_get_ex(binding, "command", &command_obj)) {

                const char *mod_str = json_object_get_string(modifiers_obj);
                const char *key_str = json_object_get_string(key_obj);
                const char *cmd_str = json_object_get_string(command_obj);

                config->keybindings[config->num_keybindings].modifiers = parse_modifiers(mod_str);
                config->keybindings[config->num_keybindings].key = parse_key(key_str);
                config->keybindings[config->num_keybindings].command = strdup(cmd_str);
                config->num_keybindings++;
            }
        }
    }

    json_object_put(root);
    return config;
}

void config_destroy(struct config *config) {
    if (!config) return;

    free(config->startup_cmd);
    for (int i = 0; i < config->num_keybindings; i++) {
        free(config->keybindings[i].command);
    }
    free(config);
}

bool handle_keybinding_from_config(struct planar_server *server, uint32_t modifiers, xkb_keysym_t sym) {
    if (!server->config) return false;

    for (int i = 0; i < server->config->num_keybindings; i++) {
        struct keybinding *bind = &server->config->keybindings[i];
        if (bind->modifiers == modifiers && bind->key == sym) {
            if (fork() == 0) {
                execl("/bin/sh", "/bin/sh", "-c", bind->command, (void *)NULL);
                exit(0);
            }
            return true;
        }
    }
    return false;
}
