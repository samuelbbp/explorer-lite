// tasks.h: the Task manager tab. Every process grouped by app (an app and the processes it started), in three
// sections: Apps, Agents (Claude Code, Codex and what they run) and Windows. CPU and memory (the private working
// set, Task Manager's «Memory» column) are sampled every 2 s on a thread, only while a Task manager tab is open.
// End task ends the whole tree. Also the services and what starts with Windows.
#pragma once
#include "util.h"
#include <d2d1.h>
#include <dwrite.h>
#include <string>

struct TasksView;
typedef void (*IconFn)(void *ctx, int index, float x, float y, float size);

TasksView *tasks_create(HWND wnd, IconFn icon, void *ctx); // WM_APP_TASKS reaches wnd on each sample
void tasks_destroy(TasksView *t);
void tasks_on_sample(TasksView *t);
void tasks_refresh(TasksView *t);
void tasks_toolbar(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, IDWriteTextFormat *f, IDWriteTextFormat *icons, D2D1_RECT_F r);
void tasks_draw(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, IDWriteTextFormat *f, IDWriteTextFormat *small,
                IDWriteTextFormat *icons, D2D1_RECT_F r, int hover);
std::wstring tasks_status(TasksView *t);
int tasks_row_at(TasksView *t, float x, float y);   // what is under the mouse (-1: nothing), for the hover
void tasks_click(TasksView *t, HWND wnd, float x, float y, bool dbl, bool right);
bool tasks_key(TasksView *t, HWND wnd, WPARAM key);
void tasks_wheel(TasksView *t, int delta);
