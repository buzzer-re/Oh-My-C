TypeScript identifiers:

```ts
obj.method(a, b)
obj.prop
this.x = y
function foo(a, b = 1) { return a }
class A extends B { m(x) { this.y = x } }
const o = { a: 1, b: c, d() {} }
arr[i] = x[0]
x => x + 1
await fetch(url)
import { a, b } from 'm'
export default foo
type T = { a: string }
interface I { a: number; f(x: string): void }
let s: string[] = []
if (a === b && !c) {}
throw new Error('x')
foo.bar().baz
const [p, q] = r
let t = cond ? a : b
let n = new Foo()
for (const k of ks) {}
const f = (a) => a
let k = j => j
x.y = 2
let m = a.b
```

```js
const x = 1
let y = x
while (x) { y() }
obj.prop
foo.bar().baz
x.y.z = 1
arr[i] = x[0]
const o = { a: 1, b: c, d() {} }
class A extends B { m(x) {} }
throw new Error(1)
let n = new Foo()
```
