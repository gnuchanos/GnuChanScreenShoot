/*
 * screenshot_config.h — the settings GnuChanScreenShoot is built from.
 *
 * GnuChanScreenShoot is configured by a script, not by a file of key = value
 * pairs, the same way GnuChanWM, GnuChanTerm and GnuChanSS are: the same file
 * a person would write to pick the save directory, the format, and the hotkeys.
 * That script is Python, and this header is the shape of what running it
 * produces — the small, fixed set of things a screenshot tool can be asked for.
 *
 * Everything here is plain data. The script is read by a parser, walked by
 * screenshot_config.c, and drawn by GnuChanScreenShoot.c. Nothing in this
 * header knows about X.
 */
#ifndef GNUCHANSCREENSHOOT_CONFIG_H
#define GNUCHANSCREENSHOOT_CONFIG_H

/* The most arguments one call may pass, and the most statements one file may
   contain. Both are far above what screenshot settings need, and both exist so
   a malformed file cannot make the parser allocate without end. */
#define SS_MAX_ARGS              32
#define SS_PARSER_MAX_STATEMENTS 128

/* The largest path a settings file may name. */
#define SS_TEXT_LENGTH 256

/* Where screenshots are saved. Alt+P writes here; Print/SysRq writes here too
   unless the settings window changes it. */
#define SS_SAVE_DIR_DEFAULT "~/Pictures"

/* The hotkeys: Alt+P grabs the focused window and saves it. Print/SysRq opens
   the settings window. Both are read from the script so a person can rebind
   them if they want. */
#define SS_HOTKEY_CAPTURE "Alt+P"
#define SS_HOTKEY_SETTINGS "Print"

/* The formats the program knows how to write. */
typedef enum SsFormat {
    SS_FORMAT_PNG,   /* lossless, the default                     */
    SS_FORMAT_JPEG,  /* lossy, smaller, quality from the script   */
} SsFormat;

/* One written value: a path, a format name, a key combination, a number. */
typedef struct SsValue {
    int kind;          /* SS_VALUE_STRING / NUMBER / BOOL / NAME    */
    char text[SS_TEXT_LENGTH];
    int number;
    int boolean;
} SsValue;

/* One named argument of a call. */
typedef struct SsArgument {
    char name[SS_TEXT_LENGTH];
    SsValue value;
} SsArgument;

typedef enum SsStatementKind {
    SS_STMT_ASSIGN,   /* name = value                   */
    SS_STMT_CALL,     /* object.method(name=value, ...) */
} SsStatementKind;

struct SsStatement {
    SsStatementKind kind;
    char target[SS_TEXT_LENGTH];
    SsValue value;
    SsArgument args[SS_MAX_ARGS];
    int arg_count;
};

/* The settings a script may name. Adding a setting is adding one field here,
   one branch in the config walker, and one row in the settings window. */
typedef struct SsConfig {
    /* Where screenshots land. $HOME/Pictures by default; ~ is expanded. */
    char save_dir[SS_TEXT_LENGTH];

    /* The format: PNG or JPEG. */
    SsFormat format;

    /* JPEG quality, 1..100. Ignored for PNG. */
    int quality;

    /* Whether Alt+P captures the focused window (1) or the whole screen (0). */
    int capture_focused;

    /* Whether the settings window opens on Print/SysRq. */
    int settings_window;

    /* The reason the last read refused, in the words to show a person. Empty
       when the file parsed. */
    char error[SS_TEXT_LENGTH * 2];
} SsConfig;

/* The settings a machine with no settings file gets: PNG, focused window,
   ~/Pictures, settings window on. */
void ss_config_defaults(SsConfig *config);

/* Read the script at path into config. Returns 0 when the file was read and
   walked, -1 when it could not be opened or could not be parsed — in which
   case config is left at its defaults and `error` says why. */
int ss_config_load(SsConfig *config, const char *path);

/* Read the settings from the standard place, or the defaults when there is no
   file. Always leaves a usable config. Returns 0 when a file was read. */
int ss_config_load_default(SsConfig *config);

/* Where the script is looked for: $XDG_CONFIG_HOME/GnuChanScreenShoot/
   GnuChanScreenShoot.py, or ~/.config/GnuChanScreenShoot/GnuChanScreenShoot.py.
   Written into buffer, which is returned. */
char *ss_config_path(char *buffer, unsigned int size);

/* Parse one script file into a statement list. Reused by the settings window
   when it wants to show what the file says. */
int ss_parse_file(const char *path, struct SsStatement **out, int *out_count);
void ss_parse_free(struct SsStatement *statements, int count);

/* The argument called name, or NULL when the call did not pass it. */
const SsValue *ss_argument(const struct SsStatement *statement, const char *name);

/* A value as text: a string is itself, a number is written out, a name is the
   name, and anything else leaves out empty. */
void ss_value_text(const SsValue *value, char *out, unsigned int size);

/* A value as a number or a flag, with the answer to use when the value is
   missing or is not of that kind. */
int ss_value_number(const SsValue *value, int fallback);
int ss_value_bool(const SsValue *value, int fallback);

/* The message for the first statement the last read refused, or an empty
   string when that read parsed. */
const char *ss_parse_last_error(void);

#endif /* GNUCHANSCREENSHOOT_CONFIG_H */
