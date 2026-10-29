/*
 * ss_parser.c — the settings script parser for GnuChanScreenShoot.
 *
 * Reads the Python-style settings script and walks it into statement trees.
 * This is the same parser family as GnuChanWM's wm_config_parser.c and
 * GnuChanSS's ss_parser.c: a tiny hand-written recursive descent parser for
 * the call/assign syntax the settings scripts use.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screenshot_config.h"

static const char *s_cursor;
static const char *s_end;
static char s_error[SS_TEXT_LENGTH * 2];
static struct SsStatement *s_statements;
static int s_statement_count;
static int s_statement_capacity;

static void set_error(const char *message) {
    snprintf(s_error, sizeof(s_error), "%s", message);
}

const char *ss_parse_last_error(void) {
    return s_error;
}

static int is_name_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-';
}

static char peek(void) {
    return (s_cursor < s_end) ? *s_cursor : '\0';
}

static void skip_ws(void) {
    while (s_cursor < s_end && isspace((unsigned char)*s_cursor)) {
        s_cursor++;
    }
}

static int read_name(char *out, unsigned int size) {
    skip_ws();
    unsigned int n = 0;
    while (s_cursor < s_end && is_name_char(*s_cursor) && n + 1 < size) {
        out[n++] = *s_cursor++;
    }
    out[n] = '\0';
    return n > 0;
}

static int read_string(char *out, unsigned int size) {
    skip_ws();
    if (s_cursor >= s_end || *s_cursor != '"') {
        return 0;
    }
    s_cursor++; /* opening quote */
    unsigned int n = 0;
    while (s_cursor < s_end && *s_cursor != '"' && n + 1 < size) {
        if (*s_cursor == '\\' && s_cursor + 1 < s_end) {
            s_cursor++;
            switch (*s_cursor) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case '"': out[n++] = '"'; break;
            case '\\': out[n++] = '\\'; break;
            default: out[n++] = *s_cursor; break;
            }
            s_cursor++;
        } else {
            out[n++] = *s_cursor++;
        }
    }
    if (s_cursor >= s_end) {
        set_error("unterminated string");
        return 0;
    }
    s_cursor++; /* closing quote */
    out[n] = '\0';
    return 1;
}

static int read_number(int *out) {
    skip_ws();
    char buf[32];
    unsigned int n = 0;
    while (s_cursor < s_end && (isdigit((unsigned char)*s_cursor) ||
                                *s_cursor == '-' || *s_cursor == '+') &&
           n + 1 < sizeof(buf)) {
        buf[n++] = *s_cursor++;
    }
    if (n == 0) {
        return 0;
    }
    buf[n] = '\0';
    *out = atoi(buf);
    return 1;
}

static int read_bool(int *out) {
    skip_ws();
    if (s_cursor + 4 <= s_end && strncmp(s_cursor, "True", 4) == 0 &&
        !is_name_char(s_cursor[4])) {
        s_cursor += 4;
        *out = 1;
        return 1;
    }
    if (s_cursor + 5 <= s_end && strncmp(s_cursor, "False", 5) == 0 &&
        !is_name_char(s_cursor[5])) {
        s_cursor += 5;
        *out = 0;
        return 1;
    }
    return 0;
}

/* statement list grows like a small vector. */
static int statement_push(struct SsStatement *statement) {
    if (s_statement_count >= s_statement_capacity) {
        int new_cap = s_statement_capacity ? s_statement_capacity * 2 : 16;
        struct SsStatement *tmp = realloc(s_statements,
                                          (size_t)new_cap * sizeof(*tmp));
        if (!tmp) {
            return -1;
        }
        s_statements = tmp;
        s_statement_capacity = new_cap;
    }
    s_statements[s_statement_count++] = *statement;
    return 0;
}

int ss_parse_file(const char *path, struct SsStatement **out,
                  int *out_count) {
    s_error[0] = '\0';
    s_statements = NULL;
    s_statement_count = 0;
    s_statement_capacity = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        set_error("could not open settings file");
        *out = NULL;
        *out_count = 0;
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *text = malloc((size_t)sz + 1);
    if (!text) {
        fclose(f);
        set_error("out of memory");
        *out = NULL;
        *out_count = 0;
        return -1;
    }
    size_t rd = fread(text, 1, (size_t)sz, f);
    fclose(f);
    text[rd] = '\0';

    s_cursor = text;
    s_end = text + rd;

    while (s_cursor < s_end) {
        skip_ws();
        if (s_cursor >= s_end) {
            break;
        }
        if (*s_cursor == '#') {
            /* comment to end of line */
            while (s_cursor < s_end && *s_cursor != '\n') {
                s_cursor++;
            }
            continue;
        }

        struct SsStatement stmt;
        memset(&stmt, 0, sizeof(stmt));

        if (!read_name(stmt.target, sizeof(stmt.target))) {
            set_error("expected a name");
            free(text);
            return -1;
        }
        skip_ws();

        if (peek() == '=') {
            s_cursor++; /* '=' */
            stmt.kind = SS_STMT_ASSIGN;
            SsValue value;
            memset(&value, 0, sizeof(value));
            if (read_string(value.text, sizeof(value.text))) {
                value.kind = 0; /* string */
            } else if (read_number(&value.number)) {
                value.kind = 1; /* number */
            } else if (read_bool(&value.boolean)) {
                value.kind = 2; /* bool */
            } else {
                set_error("expected a value");
                free(text);
                return -1;
            }
            stmt.value = value;
        } else if (peek() == '.') {
            s_cursor++; /* '.' */
            char method[SS_TEXT_LENGTH];
            if (!read_name(method, sizeof(method))) {
                set_error("expected a method name");
                free(text);
                return -1;
            }
            char target[sizeof(stmt.target)];
            int written = snprintf(target, sizeof(target), "%s.%s",
                                   stmt.target, method);
            if (written < 0 || (size_t)written >= sizeof(target)) {
                set_error("setting name is too long");
                free(text);
                return -1;
            }
            memcpy(stmt.target, target, (size_t)written + 1);
            skip_ws();
            if (peek() != '(') {
                set_error("expected '(' after method name");
                free(text);
                return -1;
            }
            s_cursor++; /* '(' */
            stmt.kind = SS_STMT_CALL;
            /* read named arguments: name=value, ... */
            while (s_cursor < s_end && peek() != ')') {
                skip_ws();
                if (peek() == ')') {
                    break;
                }
                if (stmt.arg_count >= SS_MAX_ARGS) {
                    set_error("too many arguments");
                    free(text);
                    return -1;
                }
                SsArgument arg;
                memset(&arg, 0, sizeof(arg));
                if (!read_name(arg.name, sizeof(arg.name))) {
                    set_error("expected an argument name");
                    free(text);
                    return -1;
                }
                skip_ws();
                if (peek() != '=') {
                    set_error("expected '=' after argument name");
                    free(text);
                    return -1;
                }
                s_cursor++; /* '=' */
                SsValue value;
                memset(&value, 0, sizeof(value));
                if (read_string(value.text, sizeof(value.text))) {
                    value.kind = 0;
                } else if (read_number(&value.number)) {
                    value.kind = 1;
                } else if (read_bool(&value.boolean)) {
                    value.kind = 2;
                } else {
                    set_error("expected an argument value");
                    free(text);
                    return -1;
                }
                arg.value = value;
                stmt.args[stmt.arg_count++] = arg;
                skip_ws();
                if (peek() == ',') {
                    s_cursor++;
                }
            }
            if (s_cursor >= s_end || peek() != ')') {
                set_error("expected ')' to close call");
                free(text);
                return -1;
            }
            s_cursor++; /* ')' */
        } else {
            set_error("expected '=' or '.' after name");
            free(text);
            return -1;
        }

        if (statement_push(&stmt) != 0) {
            set_error("out of memory");
            free(text);
            return -1;
        }
        skip_ws();
    }

    free(text);
    *out = s_statements;
    *out_count = s_statement_count;
    return 0;
}

void ss_parse_free(struct SsStatement *statements, int count) {
    (void)statements;
    (void)count;
    free(s_statements);
    s_statements = NULL;
    s_statement_count = 0;
    s_statement_capacity = 0;
}

const SsValue *ss_argument(const struct SsStatement *statement,
                           const char *name) {
    for (int i = 0; i < statement->arg_count; i++) {
        if (strcmp(statement->args[i].name, name) == 0) {
            return &statement->args[i].value;
        }
    }
    return NULL;
}

void ss_value_text(const SsValue *value, char *out, unsigned int size) {
    if (!value) {
        if (size > 0) out[0] = '\0';
        return;
    }
    if (value->kind == 0) {
        snprintf(out, size, "%s", value->text);
    } else if (value->kind == 1) {
        snprintf(out, size, "%d", value->number);
    } else if (value->kind == 2) {
        snprintf(out, size, "%s", value->boolean ? "True" : "False");
    } else {
        if (size > 0) out[0] = '\0';
    }
}

int ss_value_number(const SsValue *value, int fallback) {
    if (!value) {
        return fallback;
    }
    if (value->kind == 1) {
        return value->number;
    }
    if (value->kind == 0) {
        return atoi(value->text);
    }
    return fallback;
}

int ss_value_bool(const SsValue *value, int fallback) {
    if (!value) {
        return fallback;
    }
    if (value->kind == 2) {
        return value->boolean;
    }
    if (value->kind == 0) {
        return strcmp(value->text, "True") == 0 ||
               strcmp(value->text, "true") == 0 ||
               strcmp(value->text, "1") == 0;
    }
    return value->number != 0;
}
