const handler = {
	get(t, k) {
		if (k === "__esModule") return true;
		if (k === Symbol.toPrimitive) return () => "";
		if (k === Symbol.iterator) return function* () {};
		if (k === "then") return undefined;
		return p;
	},
	apply() { return p; },
	construct() { return p; },
};
const p = new Proxy(function () {}, handler);
export default p;
