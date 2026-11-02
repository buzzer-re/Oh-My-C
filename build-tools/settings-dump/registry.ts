// Prints omp's full settings registry (config/registry.ts `all()`, every registered setting incl. the
// config-file-only ones) as JSON for `cfg://`. Run by build-tools/extract_settings.sh --registry; dev-time only.
const PKG = process.env.UPSTREAM_PACKAGES!;
// Key-hint descriptions (`formatKeyHint`) render with the theme's glyphs: use the default unicode preset and
// Linux key captions, as an interactive omp shows them, not the ascii fallback of a theme-less process.
const symbols = await import(`${PKG}/tui/src/theme/symbols.ts`);
const active = await import(`${PKG}/tui/src/theme/active-symbols.ts`);
const hints = await import(`${PKG}/tui/src/key-hint-format.ts`);
active.setActiveSymbolTheme({ symbol: (k: string) => symbols.SYMBOL_PRESETS.unicode[k] });
hints.setKeyHintPlatform("linux");
await import(`${PKG}/coding-agent/src/config/all-settings.ts`);
const reg = await import(`${PKG}/coding-agent/src/config/registry.ts`);
const out = [];
for (const s of reg.all()) {
	const e: Record<string, unknown> = { id: s.id, type: s.type };
	const d = s.definition.default;
	if (d !== undefined) e.default = d;
	if (s.enumValues) e.values = s.enumValues;
	if (s.ui?.description) e.description = s.ui.description;
	if (s.isCredential) e.credential = true;
	if (s.envName) e.env = s.envName;
	if (s.envFallback) e.envFallback = s.envFallback;
	out.push(e);
}
console.log(JSON.stringify(out));
