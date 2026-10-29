/*
 * ss_parser.h — declarations for the settings script parser.
 */
#ifndef SS_PARSER_H
#define SS_PARSER_H

#include "screenshot_config.h"

int ss_parse_file(const char *path, struct SsStatement **out,
                  int *out_count);
void ss_parse_free(struct SsStatement *statements, int count);
const SsValue *ss_argument(const struct SsStatement *statement,
                           const char *name);
void ss_value_text(const SsValue *value, char *out, unsigned int size);
int ss_value_number(const SsValue *value, int fallback);
int ss_value_bool(const SsValue *value, int fallback);
const char *ss_parse_last_error(void);

#endif /* SS_PARSER_H */
