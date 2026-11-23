/* commands.c - the top-level command table (port of omp cli-commands.ts + cli/command-help.ts + each command's
 * static args/flags/examples). Only commands omc implements are registered; omp's other commands are refused by
 * resolve_cli_argv (args.c) instead of reaching the model as a prompt. */
#include "cli/cli_int.h"

#define NOOPT NULL

/* ---------- launch (commands/launch-help.ts) ---------- */

static const char *const MODE_OPTS[] = {"text", "json", "rpc", "acp", "rpc-ui", NULL};
static const char *const THINKING_OPTS[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max", "auto", NULL};
static const char *const APPROVAL_OPTS[] = {"always-ask", "always-ask-cached", "write", "yolo", NULL};
static const char *const SERVICE_TIER_OPTS[] = {"none", "auto", "default", "flex", "scale", "priority", "ultrafast", NULL};

const cflag_t LAUNCH_FLAGS[] = {
    {.name = "model", .kind = CF_STR, .ch = 0, .desc = "Model to use (fuzzy match: \"opus\", \"gpt-5.2\"; exact with a provider: \"openai/gpt-5.2\")"},
    {.name = "smol", .kind = CF_STR, .ch = 0, .desc = "Smol/fast model for lightweight tasks (or PI_SMOL_MODEL env)"},
    {.name = "slow", .kind = CF_STR, .ch = 0, .desc = "Slow/reasoning model for thorough analysis (or PI_SLOW_MODEL env)"},
    {.name = "plan", .kind = CF_STR, .ch = 0, .desc = "Plan model for architectural planning (or PI_PLAN_MODEL env)"},
    {.name = "goal", .kind = CF_STR, .ch = 0,
     .desc = "Start fresh in goal mode (interactive only; bypasses autoResume; no positional prompt, startup plan mode, or "
     "explicit resume)"},
    {.name = "prewalk", .kind = CF_BOOL, .ch = 0,
     .desc = "Switch from the active model to a fast/cheap model at the first edit/write after the plan's todo list exists "
     "(default off; see prewalk.enabled)"},
    {.name = "no-prewalk", .kind = CF_BOOL, .ch = 0,
     .desc = "Disable prewalk, including prewalk.auto, even if prewalk.enabled is set"},
    {.name = "prewalk-into", .kind = CF_STR, .ch = 0, .desc = "Target model for prewalk (default the \"smol\" role)"},
    {.name = "plan-yolo", .kind = CF_BOOL, .ch = 0,
     .desc = "Force read-only plan mode at start, auto-approve the plan on the model's first resolve call, then switch to "
     "--plan-yolo-into to implement it"},
    {.name = "plan-yolo-into", .kind = CF_STR, .ch = 0, .desc = "Target model for plan-yolo execution (default the \"smol\" role)"},
    {.name = "provider", .kind = CF_STR, .ch = 0, .desc = "Provider to use (legacy; prefer --model)"},
    {.name = "api-key", .kind = CF_STR, .ch = 0, .desc = "API key (defaults to env vars)"},
    {.name = "system-prompt", .kind = CF_STR, .ch = 0, .desc = "System prompt (default: coding assistant prompt)"},
    {.name = "system-prompt-template", .kind = CF_STR, .ch = 0, .desc = "Handlebars system prompt template (mutually exclusive with --system-prompt)"},
    {.name = "append-system-prompt", .kind = CF_STR, .ch = 0, .desc = "Append text or file contents to the system prompt"},
    {.name = "cwd", .kind = CF_STR, .ch = 0, .desc = "Directory to start in (overrides the launch cwd)"},
    {.name = "mode", .kind = CF_STR, .ch = 0, .desc = "Output mode: text (default), json, rpc, or rpc-ui", .options = MODE_OPTS},
    {.name = "config", .kind = CF_STR, .ch = 0, .desc = "Load an extra config.yml-style overlay for this run (repeatable)", .options = NOOPT, .multiple = true},
    {.name = "add-dir", .kind = CF_STR, .ch = 0, .desc = "Add a workspace directory beyond the working directory (repeatable)", .options = NOOPT, .multiple = true},
    {.name = "print", .kind = CF_BOOL, .ch = 'p', .desc = "Non-interactive mode: process prompt and exit"},
    {.name = "continue", .kind = CF_BOOL, .ch = 'c', .desc = "Continue previous session"},
    {.name = "resume", .kind = CF_STR, .ch = 'r', .desc = "Resume a session (by ID prefix, path, or picker if omitted)"},
    {.name = "restore", .kind = CF_STR, .ch = 0,
     .desc = "Resume a session as --resume does, then go on with what its last process left unfinished (the interrupted turn)"},
    {.name = "session-dir", .kind = CF_STR, .ch = 0, .desc = "Directory for session storage and lookup"},
    {.name = "no-session", .kind = CF_BOOL, .ch = 0, .desc = "Don't save session (ephemeral)"},
    {.name = "models", .kind = CF_STR, .ch = 0, .desc = "Comma-separated model patterns for Ctrl+P cycling"},
    {.name = "no-tools", .kind = CF_BOOL, .ch = 0, .desc = "Disable all built-in tools"},
    {.name = "no-judge", .kind = CF_BOOL, .ch = 0, .desc = "Restrict judge calls to the active model's provider"},
    {.name = "no-spotlight", .kind = CF_BOOL, .ch = 0, .desc = "Restrict Spotlight calls to the active model's provider"},
    {.name = "offline", .kind = CF_BOOL, .ch = 0, .desc = "Restrict auxiliary services to the active model's provider (not a network firewall)"},
    {.name = "no-lsp", .kind = CF_BOOL, .ch = 0, .desc = "Disable LSP tools, formatting, and diagnostics"},
    {.name = "tools", .kind = CF_STR, .ch = 0, .desc = "Comma-separated built-in tool names to enable (default: tools enabled in settings; see Built-in Tools)"},
    {.name = "thinking", .kind = CF_STR, .ch = 0, .desc = "Set thinking level: off, minimal, low, medium, high, xhigh, max, auto", .options = THINKING_OPTS},
    {.name = "service-tier", .kind = CF_STR, .ch = 0, .desc = "OpenAI service tier for this session (none omits service_tier)",
     .options = SERVICE_TIER_OPTS},
    {.name = "hide-thinking", .kind = CF_BOOL, .ch = 0, .desc = "Hide thinking blocks in TUI output (display only, does not disable model thinking)"},
    {.name = "advisor", .kind = CF_BOOL, .ch = 0, .desc = "Enable the advisor runtime (passively reviews each turn and injects notes)"},
    {.name = "external-thinking", .kind = CF_BOOL, .ch = 0,
     .desc = "Use a private scratchpad while disabling supported GPT, Claude, and Gemini reasoning (at your own risk: providers "
             "have flagged this request shape as abuse)"},
    {.name = "no-skills", .kind = CF_BOOL, .ch = 0, .desc = "Disable skills discovery and loading"},
    {.name = "skills", .kind = CF_STR, .ch = 0, .desc = "Comma-separated glob patterns to filter skills (e.g., git-*,docker)"},
    {.name = "no-rules", .kind = CF_BOOL, .ch = 0, .desc = "Disable rules discovery and loading"},
    {.name = "no-context-files", .kind = CF_BOOL, .ch = 0, .desc = "Disable context files (AGENTS.md, CLAUDE.md, ...)"},
    {.name = "export", .kind = CF_STR, .ch = 0, .desc = "Export session file to HTML and exit"},
    {.name = "no-title", .kind = CF_BOOL, .ch = 0, .desc = "Disable title auto-generation"},
    {.name = "spinner-interval", .kind = CF_STR, .ch = 0,
     .desc = "Milliseconds between spinner frames (default 80; 0 = static). Overrides tui.spinnerInterval and PI_SPINNER_INTERVAL for this run"},
    {.name = "max-fps", .kind = CF_STR, .ch = 0,
     .desc = "Repaint ceiling in frames per second (default 30; 1-120); keystroke echo keeps 30 fps. Overrides tui.maxFps and PI_MAX_FPS for this run"},
    {.name = "motion", .kind = CF_STR, .ch = 0,
     .desc = "Animation preset: full (default), reduced (250 ms spinners, no shimmer or blink), or none (static). Overrides tui.motion and PI_MOTION for this run"},
    {.name = "print-thoughts", .kind = CF_BOOL, .ch = 0, .desc = "Include thinking blocks in print mode text output"},
    {.name = "max-time", .kind = CF_STR, .ch = 0, .desc = "Stop the session after this duration (e.g., 600, 10m, 1h)"},
    {.name = "warn-cost", .kind = CF_STR, .ch = 0, .desc = "Warn once when the session's spend, subagents included, reaches this many USD"},
    {.name = "max-cost", .kind = CF_STR, .ch = 0,
     .desc = "Stop sending model requests once the session's spend reaches this many USD; requests already in flight can "
     "finish past it (print mode exits 3)"},
    {.name = "auto-approve", .kind = CF_BOOL, .ch = 0, .desc = "Auto-approve all tool calls (skip approval prompts)"},
    {.name = "approval-mode", .kind = CF_STR, .ch = 0, .desc = "Override tools.approvalMode for this session (always-ask|always-ask-cached|write|yolo)",
     .options = APPROVAL_OPTS},
    {0}};

static const carg_t LAUNCH_ARGS[] = {{.name = "messages", .desc = "Messages to send (prefix files with @)", .required = false, .multiple = true}, {0}};

static const char *const LAUNCH_EXAMPLES[] = {
    "# Interactive mode\n  omc",
    "# Interactive mode with initial prompt\n  omc \"List all .ts files in src/\"",
    "# Start working on a goal immediately\n  omc --goal \"Investigate the importer\"",
    "# Include files in initial message\n  omc @prompt.md @image.png \"What color is the sky?\"",
    "# Non-interactive mode (process and exit)\n  omc -p \"List all .ts files in src/\"",
    "# Continue previous session\n  omc --continue \"What did we discuss?\"",
    "# Pick a session back up after a crash\n  omc --restore ~/.omc/agent/sessions/--path--/session.jsonl",
    "# Use different model (fuzzy matching)\n  omc --model opus \"Help me refactor this code\"",
    "# Limit model cycling to specific models\n  omc --models claude-sonnet,claude-haiku,gpt-4o",
    "# Export a session file to HTML\n  omc --export ~/.omc/agent/sessions/--path--/session.jsonl",
    NULL};

/* ---------- subcommand metadata ---------- */

extern const cflag_t PLAY_FLAGS[];
extern const carg_t PLAY_ARGS[];
extern const char *const PLAY_EXAMPLES[];

static const char *const AGENTS_ACTIONS[] = {"unpack", NULL};
static const carg_t AGENTS_ARGS[] = {{"action", "Agents action", false, false, AGENTS_ACTIONS}, {0}};
static const cflag_t AGENTS_FLAGS[] = {{.name = "force", .kind = CF_BOOL, .ch = 'f', .desc = "Overwrite existing agent files"},
                                       {.name = "json", .kind = CF_BOOL, .ch = 0, .desc = "Output JSON"},
                                       {.name = "dir", .kind = CF_STR, .ch = 0, .desc = "Output directory (overrides --user/--project)"},
                                       {.name = "user", .kind = CF_BOOL, .ch = 0, .desc = "Write to ~/.omc/agent/agents (default)"},
                                       {.name = "project", .kind = CF_BOOL, .ch = 0, .desc = "Write to ./.omc/agents"},
                                       {0}};
static const char *const AGENTS_EXAMPLES[] = {
    "# Export bundled agents into user config (default)\n  omc agents unpack",
    "# Export bundled agents into project config\n  omc agents unpack --project",
    "# Overwrite existing local agent files\n  omc agents unpack --project --force",
    "# Export into a custom directory\n  omc agents unpack --dir ./tmp/agents --json", NULL};

static const char *const AUTH_ACTIONS[] = {"export", "import", NULL};
static const carg_t AUTH_ARGS[] = {{"action", "export or import", true, false, AUTH_ACTIONS},
                                   {.name = "file", .desc = "File written by `omc auth export`, or - for stdin (import)"},
                                   {0}};
static const char *const AUTH_EXAMPLES[] = {
    "# Write every stored credential to a file (keep it secret)\n  omc auth export > credentials.json",
    "# Merge them into this machine's credential store\n  omc auth import credentials.json",
    "# Copy credentials to another machine over SSH\n  omc auth export | ssh host omc auth import -", NULL};

static const char *const SHELLS[] = {"bash", "zsh", "fish", NULL};
static const carg_t COMPLETIONS_ARGS[] = {{"shell", "Target shell", true, false, SHELLS}, {0}};
static const char *const COMPLETIONS_EXAMPLES[] = {
    "# zsh — eval at startup, or write to a file in $fpath\n  eval \"$(omc completions zsh)\"",
    "# bash\n  eval \"$(omc completions bash)\"",
    "# fish\n  omc completions fish > ~/.config/fish/completions/omc.fish", NULL};

static const char *const CONFIG_ACTIONS[] = {"list", "get", "set", "reset", "path", NULL};
static const char *const CONFIG_SCOPES[] = {"global", "project", NULL};
static const carg_t CONFIG_ARGS[] = {{"action", "Config action", false, false, CONFIG_ACTIONS},
                                     {.name = "key", .desc = "Setting key"},
                                     {.name = "value", .desc = "Value (for set/reset)", .required = false, .multiple = true},
                                     {0}};
static const cflag_t CONFIG_FLAGS[] = {
    {.name = "json", .kind = CF_BOOL, .ch = 0, .desc = "Output JSON"},
    {.name = "scope", .kind = CF_STR, .ch = 0,
     .desc = "With list: only the keys the \"global\" (config.yml in the agent dir) or \"project\" (<cwd>/.omc) layer sets, "
     "as written",
     .options = CONFIG_SCOPES},
    {0}};

static const carg_t GREP_ARGS[] = {{.name = "pattern", .desc = "Regex pattern to search for"}, {.name = "path", .desc = "Directory or file to search"}, {0}};
static const cflag_t GREP_FLAGS[] = {{.name = "glob", .kind = CF_STR, .ch = 'g', .desc = "Filter files by glob pattern"},
                                     {.name = "limit", .kind = CF_INT, .ch = 'l', .desc = "Max matches", .options = NOOPT, .multiple = false, .def = "20"},
                                     {.name = "context", .kind = CF_INT, .ch = 'C', .desc = "Context lines", .options = NOOPT, .multiple = false, .def = "2"},
                                     {.name = "files", .kind = CF_BOOL, .ch = 'f', .desc = "Output file names only"},
                                     {.name = "count", .kind = CF_BOOL, .ch = 'c', .desc = "Output match counts per file"},
                                     {.name = "no-gitignore", .kind = CF_BOOL, .ch = 0, .desc = "Include files excluded by .gitignore"},
                                     {0}};

static const carg_t LOGIN_ARGS[] = {
    {.name = "provider", .desc = "OAuth provider id (e.g. anthropic, openai-codex); omit to pick interactively"}, {0}};
static const char *const LOGIN_EXAMPLES[] = {"# Pick a provider interactively\n  omc login",
                                             "# Log in to a specific provider\n  omc login anthropic", NULL};

static const char *const MODEL_KINDS[] = {"chat",   "tiny",      "image",  "tts",   "stt", "search",
                                          "judge", "embedding", "rerank", "video", "all", NULL};
static const carg_t MODELS_ARGS[] = {{.name = "action", .desc = "ls (default) | find | refresh | <provider>"},
                                     {.name = "pattern", .desc = "Filter/search substring, or provider name (required for find)"},
                                     {0}};
static const cflag_t MODELS_FLAGS[] = {
    {.name = "json", .kind = CF_BOOL, .ch = 0, .desc = "Output JSON"},
    {.name = "kind", .kind = CF_STR, .ch = 0, .desc = "Catalog kind to list", .options = MODEL_KINDS, .multiple = false, .def = "chat"},
    {.name = "config", .kind = CF_STR, .ch = 0, .desc = "Load an extra config.yml-style overlay for this run (repeatable)", .options = NOOPT, .multiple = true},
    {0}};
static const char *const MODELS_EXAMPLES[] = {
    "# List available chat models, grouped by provider\n  omc models",
    "# List models of every catalog kind\n  omc models --kind all",
    "# List one provider's models (any provider name works)\n  omc models openai-codex",
    "# Find models by substring\n  omc models find minimax",
    "# Force a fresh catalog fetch\n  omc models refresh",
    "# Machine-readable output\n  omc models --json", NULL};

static const carg_t READ_ARGS[] = {
    {.name = "path", .desc = "Path, URL, or internal URI to read (append :sel for line ranges or raw mode, e.g. src/foo.ts:50-100)", .required = true},
    {0}};
static const char *const READ_EXAMPLES[] = {"omc read src/foo.ts", "omc read src/foo.ts:50-100", "omc read src/foo.ts:raw",
                                            "omc read https://example.com", "omc read omp://",
                                            "omc read path/to/archive.zip:dir/file.ts", NULL};

static const char *const RECENCY[] = {"day", "week", "month", "year", NULL};
static const carg_t SEARCH_ARGS[] = {{.name = "query", .desc = "Search query text", .required = false, .multiple = true}, {0}};
static const cflag_t SEARCH_FLAGS[] = {{.name = "model", .kind = CF_STR, .ch = 0, .desc = "Catalog model selector"},
                                       {.name = "recency", .kind = CF_STR, .ch = 0, .desc = "Recency filter", .options = RECENCY},
                                       {.name = "limit", .kind = CF_INT, .ch = 'l', .desc = "Max results to return"},
                                       {.name = "compact", .kind = CF_BOOL, .ch = 0, .desc = "Render condensed output"},
                                       {0}};

#ifdef OMC_CLI_SESSION
static const char *const SESSION_ACTIONS[] = {"pack", "unpack", "moved", "lease", NULL};
static const carg_t SESSION_ARGS[] = {
    {"action", "pack, unpack, moved, or lease", true, false, SESSION_ACTIONS},
    {.name = "source", .desc = "Session .jsonl path or id (pack, moved); archive path (unpack); session ids (lease)"},
    {.name = "output", .desc = "Archive file to write (pack)"},
    {0}};
static const cflag_t SESSION_FLAGS[] = {
    {.name = "cwd", .kind = CF_STR, .ch = 0, .desc = "Directory the session continues in on this machine (unpack)"},
    {.name = "to", .kind = CF_STR, .ch = 0, .desc = "Where the session went, e.g. a machine name (moved)"},
    {.name = "json", .kind = CF_BOOL, .ch = 0, .desc = "Print {held, unknown} session id lists as JSON (lease)"},
    {0}};
static const char *const SESSION_EXAMPLES[] = {
    "# Pack a session with its artifacts and image blobs\n  omc session pack ~/.omc/agent/sessions/<project>/<file>.jsonl "
    "/tmp/session.tar",
    "# Unpack it on another machine; prints the session file path\n  omc session unpack /tmp/session.tar --cwd "
    "~/src/project",
    "# Mark the local copy stale; resuming it later forks it\n  omc session moved <file>.jsonl --to cloud",
    "# List which of these sessions a running omc owns, without taking ownership (`--` keeps ids starting with `-` "
    "from parsing as options)\n  omc session lease --json -- <id> <id>",
    NULL};
#endif

static const char *const SETUP_COMPONENTS[] = {"python", NULL};
static const carg_t SETUP_ARGS[] = {{"component", "Optional component to install", false, false, SETUP_COMPONENTS}, {0}};
static const cflag_t SETUP_FLAGS[] = {{.name = "check", .kind = CF_BOOL, .ch = 'c', .desc = "Check if dependencies are installed"},
                                      {.name = "json", .kind = CF_BOOL, .ch = 0, .desc = "Output status as JSON"},
                                      {0}};

static const char *const SSH_ACTIONS[] = {"add", "remove", "list", NULL};
static const char *const SSH_SCOPES[] = {"project", "user", NULL};
static const carg_t SSH_ARGS[] = {{"action", "SSH action", false, false, SSH_ACTIONS},
                                  {.name = "targets", .desc = "Host name or arguments", .required = false, .multiple = true},
                                  {0}};
static const cflag_t SSH_FLAGS[] = {{.name = "json", .kind = CF_BOOL, .ch = 0, .desc = "Output JSON"},
                                    {.name = "host", .kind = CF_STR, .ch = 0, .desc = "Host address"},
                                    {.name = "user", .kind = CF_STR, .ch = 0, .desc = "Username"},
                                    {.name = "port", .kind = CF_STR, .ch = 0, .desc = "Port number"},
                                    {.name = "key", .kind = CF_STR, .ch = 0, .desc = "Identity key path"},
                                    {.name = "desc", .kind = CF_STR, .ch = 0, .desc = "Host description"},
                                    {.name = "compat", .kind = CF_BOOL, .ch = 0, .desc = "Enable compatibility mode"},
                                    {.name = "scope", .kind = CF_STR, .ch = 0, .desc = "Config scope (project|user)", .options = SSH_SCOPES},
                                    {0}};

static const carg_t TOKEN_ARGS[] = {{.name = "provider", .desc = "Provider ID (e.g. anthropic, openai)", .required = true}, {0}};
static const cflag_t TOKEN_FLAGS[] = {
    {.name = "raw", .kind = CF_BOOL, .ch = 0, .desc = "Output the raw credential value without parsing nested JSON structures", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "force-refresh", .kind = CF_BOOL, .ch = 0, .desc = "Force refresh the OAuth token even if it has not expired", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "account", .kind = CF_INT, .ch = 'a', .desc = "Select the Nth OAuth account (1-based) in stored order instead of the round-robin default"},
    {.name = "list", .kind = CF_BOOL, .ch = 'l', .desc = "List the provider's OAuth accounts (index + identity) and exit", .options = NOOPT, .multiple = false, .def = "false"},
    {0}};
static const char *const TOKEN_EXAMPLES[] = {
    "# Get API key for Anthropic\n  omc token anthropic",
    "# Get raw Copilot credential JSON\n  omc token github-copilot --raw",
    "# Force refresh and get an Antigravity token\n  omc token google-antigravity --force-refresh",
    "# List Anthropic OAuth accounts\n  omc token anthropic --list",
    "# Get the 2nd Anthropic OAuth account's token\n  omc token anthropic --account 2", NULL};

/* commands/update.ts: --plugins, --canary and --stable are left out (omc has no plugin manager and one channel), so
 * the strict parse rejects them as unknown options */
static const cflag_t UPDATE_FLAGS[] = {
    {.name = "force", .kind = CF_BOOL, .ch = 'f', .desc = "Force update", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "check", .kind = CF_BOOL, .ch = 'c', .desc = "Check for updates without installing", .options = NOOPT, .multiple = false, .def = "false"},
    {0}};
static const char *const UPDATE_EXAMPLES[] = {"omc update", "omc update --check", NULL};

static const char *const USAGE_ACTIONS[] = {"invalidate", "clients", "accounts", "reset", NULL};
static const carg_t USAGE_ARGS[] = {
    {"action", "Optional subcommand to execute", false, false, USAGE_ACTIONS},
    {.name = "target", .desc = "Saved reset to spend with `reset`: <provider>/<credential id> as `omc usage reset` lists it"},
    {0}};
static const cflag_t USAGE_FLAGS[] = {
    {.name = "json", .kind = CF_BOOL, .ch = 'j', .desc = "Output usage reports as JSON", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "provider", .kind = CF_STR, .ch = 'p', .desc = "Only show usage for this provider id (e.g. anthropic)"},
    {.name = "redact", .kind = CF_BOOL, .ch = 'r', .desc = "Redact account emails/ids (shortest unique prefix) for sharing screenshots", .options = NOOPT,
     .multiple = false, .def = "false"},
    {.name = "history", .kind = CF_BOOL, .ch = 0, .desc = "Show recorded usage-limit history (hourly snapshots) instead of a live snapshot", .options = NOOPT,
     .multiple = false, .def = "false"},
    {.name = "days", .kind = CF_INT, .ch = 'd', .desc = "History window in days (with --history or clients)", .def = "7"},
    {0}};
static const char *const USAGE_EXAMPLES[] = {
    "# Detailed per-account usage breakdown across all providers\n  omc usage",
    "# Only Anthropic accounts\n  omc usage --provider anthropic",
    "# Redact account identifiers for screenshots\n  omc usage --redact",
    "# Machine-readable output\n  omc usage --json",
    "# Usage-limit trend over the last 30 days\n  omc usage --history --days 30",
    "# Per-client token burn (which machine/app spent what) over the last 30 days\n  omc usage clients --days 30",
    "# OAuth identity keys for task.agentAccountPools (no tokens)\n  omc usage accounts",
    "# Saved Codex and Claude rate-limit resets per account, with each account's credential id\n  omc usage reset",
    "# Spend one saved reset on a listed account\n  omc usage reset codex/3",
    "# Invalidate cached usage reports for all providers\n  omc usage invalidate",
    "# Invalidate cached usage reports for a specific provider\n  omc usage invalidate --provider anthropic",
    NULL};

#ifdef OMC_CLI_TTSR
static const char *const TTSR_ACTIONS[] = {"test", "list", "scan", NULL};
static const char *const TTSR_SOURCES[] = {"text", "thinking", "tool", NULL};
static const carg_t TTSR_ARGS[] = {
    {"action", "TTSR action", false, false, TTSR_ACTIONS},
    {.name = "snippet", .desc = "Inline snippet text to test (ttsr test) or directory to scan (ttsr scan)"},
    {0}};
static const cflag_t TTSR_FLAGS[] = {
    {.name = "file", .kind = CF_STR, .ch = 0, .desc = "Snippet file path, or - for stdin (ttsr test)"},
    {.name = "rule", .kind = CF_STR, .ch = 'r', .desc = "Rule markdown file to test in isolation (skips project rule loading)"},
    {.name = "source", .kind = CF_STR, .ch = 0, .desc = "Match source: text, thinking, or tool (inferred from --file when omitted)", .options = TTSR_SOURCES},
    {.name = "tool", .kind = CF_STR, .ch = 0, .desc = "Tool name when source is tool (e.g. edit, write); defaults to edit"},
    {.name = "path", .kind = CF_STR, .ch = 'p', .desc = "Candidate file path for scope/glob matching and AST language inference"},
    {.name = "agent", .kind = CF_STR, .ch = 0, .desc = "Agent name to evaluate rule `agents` scoping as (ttsr test); defaults to main"},
    {.name = "verbose", .kind = CF_BOOL, .ch = 'v', .desc = "Show every evaluated rule, not just triggered ones"},
    {.name = "json", .kind = CF_BOOL, .ch = 0, .desc = "Output JSON"},
    {.name = "no-gitignore", .kind = CF_BOOL, .ch = 0, .desc = "Include files excluded by .gitignore (ttsr scan)"},
    {.name = "max-bytes", .kind = CF_INT, .ch = 0, .desc = "Maximum file size to scan in bytes; 0 disables the limit (ttsr scan)"},
    {0}};
static const char *const TTSR_EXAMPLES[] = {
    "omc ttsr list",
    "omc ttsr test 'const x: any = 1'",
    "omc ttsr test src/foo.ts",
    "omc ttsr test --file src/foo.ts",
    "omc ttsr test --file src/foo.ts --source text",
    "omc ttsr test --rule .omc/rules/no-any.md --source tool --path src/foo.ts 'const x: any = 1'",
    "omc ttsr test --agent scout 'const x: any = 1'",
    "echo 'Box::leak(&mut v)' | omc ttsr test --file - --path src/lib.rs",
    "omc ttsr test --source tool --tool edit --path src/foo.ts 'const x: any = 1'",
    "omc ttsr scan",
    "omc ttsr scan src/",
    "omc ttsr scan -r .omc/rules/no-any.md src/",
    NULL};
#endif

#ifdef OMC_CLI_WORKTREE
static const char *const WT_ACTIONS[] = {"list", "clear", "add", NULL};
static const carg_t WT_ARGS[] = {{"action", "list (default), clear, or add", false, false, WT_ACTIONS},
                                 {.name = "path", .desc = "Worktree path (add)"},
                                 {.name = "commit", .desc = "Commit-ish to check out (add)"},
                                 {0}};
static const cflag_t WT_FLAGS[] = {
    {.name = "cwd", .kind = CF_STR, .ch = 'C', .desc = "Repository/base directory (add)"},
    {.name = "branch", .kind = CF_STR, .ch = 'b', .desc = "Create a new branch (add)"},
    {.name = "force-branch", .kind = CF_STR, .ch = 'B', .desc = "Create or reset a branch (add)"},
    {.name = "detach", .kind = CF_BOOL, .ch = 'd', .desc = "Detach HEAD (add)", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "quiet", .kind = CF_BOOL, .ch = 'q', .desc = "Suppress add output", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "all", .kind = CF_BOOL, .ch = 0, .desc = "Clear every entry, including live PR-checkout worktrees (clear)", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "dry-run", .kind = CF_BOOL, .ch = 'n', .desc = "Print what would be removed without touching the filesystem (clear)", .options = NOOPT, .multiple = false, .def = "false"},
    {.name = "json", .kind = CF_BOOL, .ch = 'j', .desc = "Emit machine-readable JSON", .options = NOOPT, .multiple = false, .def = "false"},
    {0}};
static const char *const WT_EXAMPLES[] = {"omc worktree", "omc worktree list --json", "omc worktree add ../feature",
                                          "omc worktree add -b feature ../feature origin/main",
                                          "omc worktree add --detach ../review HEAD~2", "omc worktree clear",
                                          "omc worktree clear --dry-run", "omc worktree clear --all", NULL};
static const char *const WT_ALIASES[] = {"wt", NULL};
#endif

static const char *const SEARCH_ALIASES[] = {"q", "web-search", NULL};

/* omc extras keep their own argv handling and stay out of the listing (omp has no such commands), except migrate */
static const cli_cmd_t COMMANDS[] = {
    {"launch", "AI coding assistant", NULL, true, LAUNCH_FLAGS, LAUNCH_ARGS, LAUNCH_EXAMPLES, NULL, cli_launch},
    {"acp", "Run omc as an ACP (Agent Client Protocol) server over stdio", NULL, false, NULL, NULL, NULL, NULL, cli_acp},
    {.name = "auth", .desc = "Export or import stored provider credentials (API keys and OAuth logins) as JSON", .aliases = NULL, .hidden = false, .flags = NULL,
     .args = AUTH_ARGS, .examples = AUTH_EXAMPLES, .run = cmd_auth},
    {.name = "agents", .desc = "Manage bundled task agents", .aliases = NULL, .hidden = false, .flags = AGENTS_FLAGS, .args = AGENTS_ARGS, .examples = AGENTS_EXAMPLES, .run = cmd_agents},
    {"completions", "Print a shell completion script (bash, zsh, or fish)", NULL, false, NULL, COMPLETIONS_ARGS,
     COMPLETIONS_EXAMPLES, NULL, cmd_completions_raw},
    {"__complete", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_complete},
    {.name = "config", .desc = "Manage configuration settings", .aliases = NULL, .hidden = false, .flags = CONFIG_FLAGS, .args = CONFIG_ARGS, .examples = NULL, .run = cmd_config},
    {.name = "grep", .desc = "Test grep tool", .aliases = NULL, .hidden = false, .flags = GREP_FLAGS, .args = GREP_ARGS, .examples = NULL, .run = cmd_grep},
    {.name = "login", .desc = "Log in to a model provider (terminal counterpart of /login)", .aliases = NULL, .hidden = false, .flags = NULL, .args = LOGIN_ARGS,
     .examples = LOGIN_EXAMPLES, .run = cmd_login},
    {.name = "models", .desc = "List, search, and refresh available models", .aliases = NULL, .hidden = false, .flags = MODELS_FLAGS, .args = MODELS_ARGS, .examples = MODELS_EXAMPLES,
     .run = cmd_models},
#ifdef OMC_CLI_SESSION
    {.name = "session",
     .desc = "Hand a session to another machine (pack, unpack, moved) or check which sessions a running omc owns (lease)", .aliases = NULL,
     .hidden = false, .flags = SESSION_FLAGS, .args = SESSION_ARGS, .examples = SESSION_EXAMPLES, .run = cmd_session},
#endif
    {.name = "setup", .desc = "Run onboarding setup or install dependencies for optional features", .aliases = NULL, .hidden = false, .flags = SETUP_FLAGS,
     .args = SETUP_ARGS, .examples = NULL, .run = cmd_setup},
    {.name = "play", .desc = "Replay a /record session recording in the terminal (space pauses, q quits)", .aliases = NULL,
     .hidden = false, .flags = PLAY_FLAGS, .args = PLAY_ARGS, .examples = PLAY_EXAMPLES, .run = cmd_play},
    {.name = "read", .desc = "Show what the read tool will return for a path, URL, or internal URI", .aliases = NULL, .hidden = false, .flags = NULL, .args = READ_ARGS,
     .examples = READ_EXAMPLES, .run = cmd_read},
    {.name = "ssh", .desc = "Manage SSH host configurations", .aliases = NULL, .hidden = false, .flags = SSH_FLAGS, .args = SSH_ARGS, .examples = NULL, .run = cmd_ssh},
    {.name = "update", .desc = "Check for and install updates", .aliases = NULL, .hidden = false, .flags = UPDATE_FLAGS,
     .args = NULL, .examples = UPDATE_EXAMPLES, .run = cmd_update},
    {.name = "usage", .desc = "Show provider usage limits for every authenticated account", .aliases = NULL, .hidden = false, .flags = USAGE_FLAGS,
     .args = USAGE_ARGS, .examples = USAGE_EXAMPLES, .run = cmd_usage},
    {.name = "token", .desc = "Get the API key or OAuth token for a provider", .aliases = NULL, .hidden = false, .flags = TOKEN_FLAGS, .args = TOKEN_ARGS, .examples = TOKEN_EXAMPLES,
     .run = cmd_token},
#ifdef OMC_CLI_TTSR
    {.name = "ttsr", .desc = "Inspect and test Time-Traveling Stream Rules (TTSR)", .aliases = NULL, .hidden = false, .flags = TTSR_FLAGS, .args = TTSR_ARGS, .examples = TTSR_EXAMPLES,
     .run = cmd_ttsr},
#endif
#ifdef OMC_CLI_WORKTREE
    {.name = "worktree", .desc = "Add, list, or clear git worktrees (clone-first when enabled)", .aliases = WT_ALIASES, .hidden = false, .flags = WT_FLAGS, .args = WT_ARGS,
     .examples = WT_EXAMPLES, .run = cmd_worktree},
#endif
    {.name = "search", .desc = "Test web search providers", .aliases = SEARCH_ALIASES, .hidden = false, .flags = SEARCH_FLAGS, .args = SEARCH_ARGS, .examples = NULL, .run = cmd_search},
    /* omc */
    {"migrate", "Copy an omp configuration (~/.omp/agent) into omc", NULL, false, NULL, NULL, NULL, NULL,
     cmd_migrate_raw},
    {"mcp", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_mcp_raw},
    {"export", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_export_raw},
    {"sessions", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_sessions_raw},
    {"logout", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_logout_raw},
    {"glob", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_glob_raw},
    {"bash", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_bash_raw},
    {"version", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_version_raw},
    {"__omc_worker_auto_update", NULL, NULL, true, NULL, NULL, NULL, NULL, cmd_auto_update_worker_raw}, /* update.auto */
};

const cli_cmd_t *cli_commands(size_t *n) {
    *n = ARRAY_LEN(COMMANDS);
    return COMMANDS;
}

const cli_cmd_t *cli_launch_command(void) { return &COMMANDS[0]; }

const cli_cmd_t *cli_find_command(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < ARRAY_LEN(COMMANDS); i++)
        if (!strcmp(COMMANDS[i].name, name)) return &COMMANDS[i];
    for (size_t i = 0; i < ARRAY_LEN(COMMANDS); i++)
        for (int k = 0; COMMANDS[i].aliases && COMMANDS[i].aliases[k]; k++)
            if (!strcmp(COMMANDS[i].aliases[k], name)) return &COMMANDS[i];
    return NULL;
}

/* ---------- help-extra.ts ---------- */

char *cli_extra_help(void) {
    char *env = cli_bold("Environment Variables:"), *tools = cli_bold("Built-in Tools (names accepted by --tools):");
    char *ton = cli_dim("# Enabled by default"), *toff = cli_dim("# Optional (off by default; enable in settings)");
    char *useful = cli_bold("Useful Commands:");
    char *core = cli_dim("# Core Providers"), *more = cli_dim("# Additional LLM Providers"),
         *cloud = cli_dim("# Cloud Providers"), *search = cli_dim("# Search & Tools"), *cfg = cli_dim("# Configuration");
    char *s = xasprintf(
        "%s\n"
        "  %s\n"
        "  ANTHROPIC_API_KEY          - Anthropic Claude models\n"
        "  ANTHROPIC_OAUTH_TOKEN      - Anthropic OAuth (takes precedence over API key)\n"
        "  OPENAI_API_KEY             - OpenAI GPT models\n"
        "  GEMINI_API_KEY             - Google Gemini models\n"
        "  COPILOT_GITHUB_TOKEN      - GitHub Copilot\n"
        "\n"
        "  %s\n"
        "  AZURE_OPENAI_API_KEY       - Azure OpenAI models\n"
        "  GROQ_API_KEY               - Groq models\n"
        "  CEREBRAS_API_KEY           - Cerebras models\n"
        "  XAI_API_KEY                - xAI Grok models\n"
        "  OPENROUTER_API_KEY         - OpenRouter aggregated models\n"
        "  KILO_API_KEY               - Kilo Gateway models\n"
        "  MISTRAL_API_KEY            - Mistral models\n"
        "  ZAI_API_KEY                - z.ai models (ZhipuAI/GLM)\n"
        "  UMANS_AI_CODING_PLAN_API_KEY - Umans AI Coding Plan models\n"
        "  ABLITERATION_API_KEY       - Abliteration uncensored GLM models\n"
        "  UMANS_WEBSEARCH_PROVIDER    - Umans gateway web search backend (native or exa)\n"
        "  MINIMAX_API_KEY            - MiniMax models\n"
        "  OPENCODE_API_KEY           - OpenCode Zen/OpenCode Go models\n"
        "  CURSOR_ACCESS_TOKEN        - Cursor AI models\n"
        "  CLINE_API_KEY              - ClinePass subscription models\n"
        "  COMMAND_CODE_API_KEY       - Command Code Provider API models\n"
        "  CHARM_HYPER_API_KEY        - Charm Hyper inference gateway models\n"
        "  AI_GATEWAY_API_KEY         - Vercel AI Gateway\n"
        "  STEPFUN_API_KEY            - StepFun Step models\n"
        "  HELMCODE_API_KEY           - Helmcode private EU inference\n"
        "  WAFER_SERVERLESS_API_KEY   - Wafer Serverless (pay-as-you-go)\n"
        "  YOLO_AUTO_API_KEY          - Yolo-Auto flat-rate Qwen models\n"
        "  SINGULARITYAPI_DEV_API_KEY - SingularityAPI universal gateway (300+ models)\n"
        "  SINGULARITYAPI_TECH_API_KEY - SingularityAPI reserved DeepSeek lanes\n"
        "  SCW_SECRET_KEY             - Scaleway Generative APIs models\n"
        "  OTARI_API_KEY              - Otari AI gateway (workspace models)\n"
        "  OTARI_BASE_URL             - Otari self-hosted API root (e.g. https://host/api/v1)\n"
        "\n"
        "  %s\n"
        "  AWS_PROFILE                - AWS Bedrock (or AWS_ACCESS_KEY_ID + AWS_SECRET_ACCESS_KEY)\n"
        "  GOOGLE_CLOUD_PROJECT       - Google Vertex AI (requires GOOGLE_CLOUD_LOCATION)\n"
        "  GOOGLE_APPLICATION_CREDENTIALS - Service account for Vertex AI\n"
        "\n"
        "  %s\n"
        "  EXA_API_KEY                - Exa web search\n"
        "  BRAVE_API_KEY              - Brave web search\n"
        "  TAVILY_API_KEY             - Tavily web search\n"
        "  KEENABLE_API_KEY           - Keenable web search (optional; keyless public fallback)\n"
        "  SOFYA_API_KEY              - Sofya web search\n"
        "\n"
        "  %s\n"
        "  OMC_CODING_AGENT_DIR       - Session storage directory (default: ~/.omc/agent)\n"
        "  PI_SMOL_MODEL              - Override smol/fast model (see --smol)\n"
        "  PI_SLOW_MODEL              - Override slow/reasoning model (see --slow)\n"
        "  PI_PLAN_MODEL              - Override planning model (see --plan)\n"
        "%s\n"
        "  %s\n"
        "  read          - Read files, directories, URLs, archives, and databases\n"
        "  edit          - Edit files in place\n"
        "  write         - Create or overwrite files\n"
        "  bash          - Execute shell commands\n"
        "  grep          - Search file contents by regex\n"
        "  glob          - Find files by glob pattern\n"
        "  find          - Find code by describing what it does (when a native judge model is available)\n"
        "  eval          - Run Python/JS cells\n"
        "  task          - Launch subagents for parallel work\n"
        "  wait          - Wait for background jobs\n"
        "  todo          - Track a todo list\n"
        "  ask           - Ask the user questions (interactive mode only)\n"
        "  web_search    - Search the web\n"
        "  lsp           - Language server queries (code intelligence)\n"
        "  ast_edit      - Rewrite code with AST patterns\n"
        "  debug         - Debug a running process over DAP\n"
        "\n"
        "  %s\n"
        "  ast_grep      - Search code with AST patterns\n"
        "  github        - Query and act on GitHub repositories, issues, and PRs\n"
        "  checkpoint    - Save a git-based checkpoint of session state\n"
        "  rewind        - Rewind to a saved checkpoint\n"
        "  learn         - Capture reusable lessons in long-term memory\n"
        "  manage_skill  - Create, update, or delete managed skills\n"
        "\n"
        "%s\n"
        "  omc agents unpack           - Export bundled subagents to ~/.omc/agent/agents (default)\n"
        "  omc agents unpack --project - Export bundled subagents to ./.omc/agents",
        env, core, more, cloud, search, cfg, tools, ton, toff, useful);
    free(env);
    free(tools);
    free(ton);
    free(toff);
    free(useful);
    free(core);
    free(more);
    free(cloud);
    free(search);
    free(cfg);
    return s;
}
