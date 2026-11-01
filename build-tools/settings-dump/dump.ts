// Prints omp's settings-panel definitions (overlays/settings-defs.ts over config/settings-ui.ts host
// entries) as JSON. Run by build-tools/extract_settings.sh with omp's own bun; dev-time only.
const PKG = process.env.UPSTREAM_PACKAGES!;
const ui = await import(`${PKG}/coding-agent/src/config/settings-ui.ts`);
const defs = await import(`${PKG}/tui/src/overlays/settings-defs.ts`);
const host = ui.createSettingsHost();
const settings = [];
for (const e of host.entries) {
	const d = defs.getSettingDef(host.entries, e.path);
	if (!d) continue;
	const s: Record<string, unknown> = {
		path: e.path,
		schemaType: d.schemaType,
		type: d.type,
		default: e.defaultValue,
		tab: d.tab,
		label: d.label,
		description: d.description,
	};
	if (d.group) s.group = d.group;
	if (d.warning) s.warning = d.warning;
	if (e.ui?.condition) s.condition = e.ui.condition;
	if (d.values) s.values = d.values;
	if (d.options) s.options = d.options;
	if (d.secret) s.secret = true;
	if (d.ordered) s.ordered = true;
	settings.push(s);
}
const tabs = defs.SETTING_TABS.map((id: string) => ({ id, ...defs.TAB_METADATA[id], groups: defs.TAB_GROUPS[id] }));
console.log(JSON.stringify({ tabs, settings }));
