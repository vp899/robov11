/*
 * robocontrol/config.h - Simple TOML config parser (subset: tables, key=value, strings, ints, bools)
 */
#ifndef ROBOCONTROL_CONFIG_H
#define ROBOCONTROL_CONFIG_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_CONFIG_MAX_SECTIONS  32
#define RC_CONFIG_MAX_KEYS      64
#define RC_CONFIG_MAX_LINE      1024

typedef struct {
    char key[128];
    char value[512];
} rc_config_kv_t;

typedef struct {
    char name[128];
    rc_config_kv_t keys[RC_CONFIG_MAX_KEYS];
    int key_count;
} rc_config_section_t;

typedef struct {
    rc_config_section_t sections[RC_CONFIG_MAX_SECTIONS];
    int section_count;
} rc_config_t;

static inline char *rc_config_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) *e-- = '\0';
    return s;
}

static inline int rc_config_load(rc_config_t *cfg, const char *path) {
    memset(cfg, 0, sizeof(*cfg));

    FILE *f = fopen(path, "r");
    if (!f) return RC_ERR_IO;

    char line[RC_CONFIG_MAX_LINE];
    char current_section[128] = "";

    while (fgets(line, sizeof(line), f)) {
        char *s = rc_config_trim(line);
        if (*s == '\0' || *s == '#') continue;

        /* Section header */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                strncpy(current_section, s + 1, sizeof(current_section) - 1);
            }
            continue;
        }

        /* Key = value */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = rc_config_trim(s);
        char *val = rc_config_trim(eq + 1);

        /* Remove quotes from value */
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
            val[vlen - 1] = '\0';
            val++;
            vlen -= 2;
        }

        /* Find or create section */
        rc_config_section_t *sec = NULL;
        for (int i = 0; i < cfg->section_count; i++) {
            if (strcmp(cfg->sections[i].name, current_section) == 0) {
                sec = &cfg->sections[i];
                break;
            }
        }
        if (!sec && cfg->section_count < RC_CONFIG_MAX_SECTIONS) {
            sec = &cfg->sections[cfg->section_count++];
            strncpy(sec->name, current_section, sizeof(sec->name) - 1);
        }
        if (!sec) { fclose(f); return RC_ERR_FULL; }

        if (sec->key_count < RC_CONFIG_MAX_KEYS) {
            rc_config_kv_t *kv = &sec->keys[sec->key_count++];
            strncpy(kv->key, key, sizeof(kv->key) - 1);
            strncpy(kv->value, val, sizeof(kv->value) - 1);
        }
    }

    fclose(f);
    return RC_OK;
}

static inline const char *rc_config_get(rc_config_t *cfg, const char *section,
                                         const char *key, const char *def) {
    for (int i = 0; i < cfg->section_count; i++) {
        if (strcmp(cfg->sections[i].name, section) == 0) {
            for (int j = 0; j < cfg->sections[i].key_count; j++) {
                if (strcmp(cfg->sections[i].keys[j].key, key) == 0)
                    return cfg->sections[i].keys[j].value;
            }
        }
    }
    return def;
}

static inline int rc_config_get_int(rc_config_t *cfg, const char *section,
                                     const char *key, int def) {
    const char *v = rc_config_get(cfg, section, key, NULL);
    return v ? atoi(v) : def;
}

static inline int rc_config_get_bool(rc_config_t *cfg, const char *section,
                                      const char *key, int def) {
    const char *v = rc_config_get(cfg, section, key, NULL);
    if (!v) return def;
    return (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0);
}

static inline void rc_config_destroy(rc_config_t *cfg) {
    (void)cfg; /* No dynamic memory */
}

#ifdef __cplusplus
}
#endif

#endif /* ROBOCONTROL_CONFIG_H */
