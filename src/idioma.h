// The UI language (see idioma.cpp; copied from wa-lite and Zas). UI strings are written in English and passed
// through T(): it returns their translation into the UI language.
#pragma once
#include <initializer_list>
#include <string>

// The UI language: "es", "en", "fr", "pt-BR"... The one from explorer-lite.conf (language=...), Windows' if it
// says auto, and English if it says nothing.
const char *ui_lang();

// true if the language follows Windows' (language=auto)
bool ui_lang_auto();

// Changes the language while explorer-lite is open ("auto": Windows') and saves it in explorer-lite.conf.
void set_ui_lang(const char *code);

// The text in the UI language. What is not translated comes out as is.
const wchar_t *T(const wchar_t *src);

// A template with {0}, {1}... holes translated and filled in: Tf(L"Until {0}", { time }).
std::wstring Tf(const wchar_t *src, std::initializer_list<std::wstring> args);

// The Windows font for the language's script (Thai, Hindi, Chinese, Japanese, Korean):
// with Segoe UI, Windows replaces it with another, smaller one. NULL if Segoe UI is enough.
const wchar_t *ui_font();

// The languages explorer-lite has, to choose from: code and name in its own language.
struct UiLanguage { const char *code; const wchar_t *name; };
const UiLanguage *ui_languages(int *count);
