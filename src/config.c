#include "config.h"
#include "toml.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <strings.h>
#include <wlr/util/log.h>

static uint32_t parse_modifiers(const char *mod_str) {
    uint32_t mods = 0;
    char *str = strdup(mod_str);
    char *token = strtok(str, "+");

    while (token) {
        char *trimmed = token;
        while (isspace(*trimmed)) trimmed++;

        if (strcasecmp(trimmed, "shift") == 0) mods |= WLR_MODIFIER_SHIFT;
        else if (strcasecmp(trimmed, "caps") == 0) mods |= WLR_MODIFIER_CAPS;
        else if (strcasecmp(trimmed, "ctrl") == 0) mods |= WLR_MODIFIER_CTRL;
        else if (strcasecmp(trimmed, "alt") == 0) mods |= WLR_MODIFIER_ALT;
        else if (strcasecmp(trimmed, "mod2") == 0) mods |= WLR_MODIFIER_MOD2;
        else if (strcasecmp(trimmed, "mod3") == 0) mods |= WLR_MODIFIER_MOD3;
        else if (strcasecmp(trimmed, "super") == 0 || strcasecmp(trimmed, "logo") == 0)
            mods |= WLR_MODIFIER_LOGO;
        else if (strcasecmp(trimmed, "mod5") == 0) mods |= WLR_MODIFIER_MOD5;

        token = strtok(NULL, "+");
    }

    free(str);
    return mods;
}

static void parse_keybind(struct config *config, const char *key_combo, const char *command) {
    if (config->num_keybindings >= MAX_KEYBINDINGS) {
        wlr_log(WLR_ERROR, "Maximum number of keybindings reached");
        return;
    }

    const char *last_plus = strrchr(key_combo, '+');
    if (!last_plus) {
        wlr_log(WLR_ERROR, "Invalid key binding format: %s", key_combo);
        return;
    }
    
    char *mods = strndup(key_combo, last_plus - key_combo);
    const char *key = last_plus + 1;
    
    struct keybinding *bind = &config->keybindings[config->num_keybindings];
    bind->modifiers = parse_modifiers(mods);
    
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names rules = {0};
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    
    if (keymap) {
        struct xkb_state *state = xkb_state_new(keymap);
        if (state) {
            bind->key = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
            
            if (bind->key == XKB_KEY_NoSymbol) {
                char xkb_key[64] = "XKB_KEY_";
                strcat(xkb_key, key);
                bind->key = xkb_keysym_from_name(xkb_key, XKB_KEYSYM_NO_FLAGS);
            }
            
            xkb_state_unref(state);
        }
        xkb_keymap_unref(keymap);
    }
    xkb_context_unref(ctx);
    
    bind->command = strdup(command);
    bind->is_internal = (command[0] == '@');
    
    free(mods);
    config->num_keybindings++;
}

struct config *config_load(const char *path) {
    struct config *config = calloc(1, sizeof(struct config));
    if (!config) {
        return NULL;
    }

    const char *config_path = path;
    char default_path[256];

    if (!config_path) {
        const char *home_dir = getenv("HOME");
        if (!home_dir) {
            wlr_log(WLR_ERROR, "Could not determine home directory");
            free(config);
            return NULL;
        }
        snprintf(default_path, sizeof(default_path), "%s/.config/planar/config.toml", home_dir);
        config_path = default_path;
    }

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        wlr_log(WLR_ERROR, "Could not open config file: %s", config_path);
        free(config);
        return NULL;
    }

    char errbuf[200];
    toml_table_t *conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);

    if (!conf) {
        wlr_log(WLR_ERROR, "Error parsing TOML: %s", errbuf);
        free(config);
        return NULL;
    }

    toml_array_t *startup = toml_array_in(conf, "on_startup");
    if (startup && toml_array_nelem(startup) > 0) {
        toml_datum_t elem = toml_string_at(startup, 0);
        if (elem.ok) {
            config->startup_cmd = strdup(elem.u.s);
            free(elem.u.s);
        }
    }

    toml_table_t *keybinds = toml_table_in(conf, "keybinds");
    if (keybinds) {
        const char *key;
        for (int i = 0; 0 != (key = toml_key_in(keybinds, i)); i++) {
            toml_datum_t val = toml_string_in(keybinds, key);
            if (val.ok) {
                parse_keybind(config, key, val.u.s);
                free(val.u.s);
            }
        }
    }

    toml_table_t *rules = toml_table_in(conf, "rules");
    if (rules) {
        toml_array_t *nodec = toml_array_in(rules, "nodecoration");
        if (nodec) {
            int n = toml_array_nelem(nodec);
            if (n > 0) {
                config->nodecoration = calloc(n, sizeof(char *));
                for (int i = 0; i < n; i++) {
                    toml_datum_t elem = toml_string_at(nodec, i);
                    if (elem.ok) {
                        config->nodecoration[config->nodecoration_count++] = strdup(elem.u.s);
                        free(elem.u.s);
                    }
                }
            }
        }

        toml_array_t *ontop = toml_array_in(rules, "ontop");
        if (ontop) {
            int n = toml_array_nelem(ontop);
            if (n > 0) {
                config->ontop = calloc(n, sizeof(char *));
                for (int i = 0; i < n; i++) {
                    toml_datum_t elem = toml_string_at(ontop, i);
                    if (elem.ok) {
                        config->ontop[config->ontop_count++] = strdup(elem.u.s);
                        free(elem.u.s);
                    }
                }
            }
        }
    }

    toml_free(conf);
    return config;
}

void config_destroy(struct config *config) {
    if (!config) return;

    free(config->startup_cmd);
    for (int i = 0; i < config->num_keybindings; i++) {
        free(config->keybindings[i].command);
    }
    for (size_t i = 0; i < config->nodecoration_count; i++) {
        free(config->nodecoration[i]);
    }
    free(config->nodecoration);
    for (size_t i = 0; i < config->ontop_count; i++) {
        free(config->ontop[i]);
    }
    free(config->ontop);
    free(config);
}

bool handle_internal_command(struct planar_server *server, const char *cmd) {
    /* Strip the '@' prefix and dispatch via IPC handler */
    if (cmd[0] == '@') {
        return ipc_dispatch_command(server, cmd + 1);
    }
    return false;
}

bool handle_external_command(const char *cmd) {
    if (fork() == 0) {
        execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
        exit(0);
    }
    return true;
}