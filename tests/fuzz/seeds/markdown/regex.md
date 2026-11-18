PRegex:

```re
^foo\.bar$
\bword\b|\d+\s*\w{2,5}
[a-z0-9_-]+@[^\s@]+\.(com|org)
(?:abc)(?=x)(?!y)(?<=z)(?<!w)
(?<name>\d+)\k<name>\1(?P<n>x)
a*?b+?c??d{3}e{2,}f{1,4}?g{,3}
\x41é\t\n\\\/\.\-
.*[^\]a-]
[[:alpha:]]+ \p{L} \Q*\E
(?i)case(?#comment)x
TODO|FIXME
fn\s+\w+\(
{3}a{b
```

More:

```re
\B\A\z\Z\G\h\H\v\V\R\X\N\K
\12\0\012\cA\e\f\r\a\u{1F600}\x{41}\o{12}
a++b*+c?+(?>atomic)(?|x|y)
[\d\w.$^*+?(){}|][a^b][\x00-\x1f][a\-z][-a][a-]
(?i:x)(?-i)(?im-sx:y)(?^:z)
\k'n'(?'n'x)(?P=n)\g<1>\g{-1}\g1(?1)(?R)(?&n)
x{2, 5}x{a}x{1,2,3}x{}
(*FAIL)(*SKIP)(?(1)a|b)(?(<n>)a)
^$|^\s*$
(a(b(c)))[[:^digit:][:foo:]]
\p{Lu}\P{Greek}\pL\p{^L}
```

Partial:

```re
*a+b?|x||y
```

```re
(?
```

```re
(?<
```

```re
[a-
```

```re
x{2
```

```re
(?#open
```

```re
\Q abc
```

```re
[]a][^]a][a-\d][\d-z]
```

```re
x(?=
```

```re
\x4\x\u12\cX
```

```re
a{1,}b{0}(?i-)(?-)c
```

```re
foo\
```
