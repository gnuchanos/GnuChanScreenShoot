/*
 * screenshot_config.c — the walk that gives the settings script its meaning.
 *
 * The parser reads the file as a name and a value, or a name and a call. This
 * is the other half: it knows which call a screenshot tool understands and
 * what each of its arguments means.
 *
 * The one call it understands, whole:
 *
 *     gcl_Screenshot.Settings(
 *         SaveDir="~/Pictures",     where screenshots are written
 *         Format="png",             "png" or "jpeg"
 *         Quality=90,               jpeg quality, 1..100
 *         CaptureFocused=True,      Alt+P grabs the focused window
 *         SettingsWindow=True,      Print/SysRq opens the settings window
 *     )
 *
 * A name that is not one of these is not a mistake worth refusing the file
 * for: a script is written while it is being lived with, and a value the
 * program does not use is a value it does not use. That is different from a
 * statement that cannot be READ — an unclosed bracket, a missing quote —
 * which the parser refuses, and this module turns that refusal into the
 * message the program prints.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screenshot_config.h"

/* --- the defaults a machine with no settings file gets --------------------- */

void ss_config_defaults(SsConfig *config) {
    memset(config, 0, sizeof(*config));

    snprintf(config->save_dir, sizeof(config->save_dir), "%s", SS_SAVE_DIR_DEFAULT);
    config->format = SS_FORMAT_PNG;
    config->quality = 90;
    config->capture_focused = 1;
    config->settings_window = 1;
    config->error[0] = '\0';
}

/* --- reading one call ----------------------------------------------------- */

/* If the call passed an argument called `name`, copy its text into out.
   Returns 1 when it did. A caller that already has a value keeps it when this
   returns 0, so a script that names one setting and not another gets what it
   expects for the one it left out. */
static int argument_text(const struct SsStatement *statement, const char *name,
                         char *out, unsigned int size) {
    const SsValue *value = ss_argument(statement, name);
    if (!value) {
        return 0;
    }
    ss_value_text(value, out, size);
    return out[0] != '\0';
}

/* gcl_Screenshot.Settings(...). The only call this program has. */
static void read_settings(SsConfig *config, const struct SsStatement *statement) {
    char text[SS_TEXT_LENGTH];

    if (argument_text(statement, "SaveDir", text, sizeof(text))) {
        snprintf(config->save_dir, sizeof(config->save_dir), "%s", text);
    }

    if (argument_text(statement, "Format", text, sizeof(text))) {
        if (strcmp(text, "jpeg") == 0 || strcmp(text, "jpg") == 0) {
            config->format = SS_FORMAT_JPEG;
        } else {
            config->format = SS_FORMAT_PNG;
        }
    }

    config->quality = ss_value_number(
        ss_argument(statement, "Quality"), config->quality);
    if (config->quality < 1) {
        config->quality = 1;
    }
    if (config->quality > 100) {
        config->quality = 100;
    }

    config->capture_focused = ss_value_bool(
        ss_argument(statement, "CaptureFocused"), config->capture_focused);
    config->settings_window = ss_value_bool(
        ss_argument(statement, "SettingsWindow"), config->settings_window);
}

/* --- the public entry points ---------------------------------------------- */

char *ss_config_path(char *buffer, unsigned int size) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(buffer, size,
                 "%s/GnuChanScreenShoot/GnuChanScreenShoot.py", xdg);
        return buffer;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buffer, size,
                 "%s/.config/GnuChanScreenShoot/GnuChanScreenShoot.py", home);
        return buffer;
    }
    buffer[0] = '\0';
    return buffer;
}

int ss_config_load(SsConfig *config, const char *path) {
    /* The defaults are laid down first, and the walk only ever changes what
       the file names. That is what makes a settings file able to name one
       path and get the rest of the program it expects. */
    ss_config_defaults(config);

    if (!path || !path[0]) {
        snprintf(config->error, sizeof(config->error),
                 "no settings file was found");
        return -1;
    }

    struct SsStatement *statements = NULL;
    int count = 0;
    if (ss_parse_file(path, &statements, &count) != 0) {
        snprintf(config->error, sizeof(config->error), "%s",
                 ss_parse_last_error());
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (statements[i].kind == SS_STMT_CALL &&
            strcmp(statements[i].target, "gcl_Screenshot.Settings") == 0) {
            read_settings(config, &statements[i]);
        }
    }
    ss_parse_free(statements, count);

    if (config->quality < 1) {
        config->quality = 1;
    }
    if (config->quality > 100) {
        config->quality = 100;
    }
    return 0;
}

int ss_config_load_default(SsConfig *config) {
    char path[SS_TEXT_LENGTH];
    ss_config_path(path, sizeof(path));
    if (!path[0]) {
        ss_config_defaults(config);
        return -1;
    }
    if (ss_config_load(config, path) != 0) {
        return -1;
    }
    return 0;
}
