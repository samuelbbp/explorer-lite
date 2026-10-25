// conf.h: explorer-lite.conf (in data_dir()), one option per line: key=value. Read once and
// saved when something changes, without touching the other lines (nor those set by idioma.cpp).
#pragma once
#include <string>

std::string conf_get(const char *key, const char *def = "");
int conf_int(const char *key, int def);
void conf_set(const char *key, const std::string &value);
void conf_reload(); // another process wrote it (the resident's taskbar settings): read again on the next conf_get
