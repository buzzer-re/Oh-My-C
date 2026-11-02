/* usage.h - `/usage` in the interactive UI (usage.c) and the per-turn usage row (chat.c): ports of omp
 * packages/tui/src/overlays/{usage-dashboard,usage-row,reset-usage-selector}.ts and command-controller.ts
 * renderUsageReports. */
#ifndef OMC_UI_USAGE_H
#define OMC_UI_USAGE_H

#include "core/core.h"
#include "core/json.h"

typedef struct ui ui_t;

/* `/usage [show|reset [target]]`: the dashboard (reports fetched off the UI thread), the reset picker or a reset */
void ui_usage_command(ui_t *u, const char *args);
bool ui_usage_dashboard_open(void);

/* the classic per-account report the dashboard's Details view shows (owned, "\n"-joined lines) */
char *ui_usage_detail(const json_t *reports, double now, int width, const char *active_provider, const json_t *identity,
                      const json_t *selectors, const json_t *unavailable);
/* getUsageReportingModelSelectors: sorted "provider/model" of available models the reports map to (owned) */
json_t *ui_usage_model_selectors(const json_t *reports);
/* per-local-day {day, cost, requests} of the assistant requests in the session files under sessions_dir (the
 * activity heatmap's input; owned array, oldest first) */
void ui_usage_load_activity(const char *sessions_dir, json_t **points_out);

/* usage-row.ts: the dim metric strip under a finished turn (`display.showTokenUsage`). usage is the
 * assistant message's usage; duration_ms / ttft_ms / timestamp / turn_ms are 0 when unknown. Owned. */
char *ui_usage_row(const json_t *usage, double duration_ms, double ttft_ms, double timestamp, double turn_ms);

#endif
