# memo-lint (prototype)

Checks and infers the `way_memo` / `node_memo` declarations of a tilemaker profile
(the replay feature of systemed/tilemaker#993).

Those declarations are promises tilemaker can't check while it runs. If one is wrong,
tilemaker writes wrong tiles and gives no error. memo-lint proves the promises (or shows
where they fail) from the profile's source code:

```bash
pip install luaparser lupa
python3 tools/memo-lint/memo_lint.py resources/process-openmaptiles.lua
```

## How it works

1. It loads the profile in a real Lua (lupa), with stub versions of tilemaker's functions,
   and runs `init_function`. That gives it the profile's configuration as tilemaker sees
   it: globals, constant tables, and module `local`s (read as upvalues).
2. It runs `way_function` and `node_function` abstractly on the AST (luaparser):
   - Helper functions are inlined at each call site.
   - Branches whose test is a configuration constant are pruned
     (e.g. `if preferred_language and ...` when `preferred_language = nil`).
   - Loops over constant tables are unrolled, so `Find(k)` for each `k` in a list
     resolves to the actual tag keys.
   - `x == "c"` narrows `x` to `"c"` inside the branch.
3. It tracks these values as taints:
   - `Find(k)` results: may only be tested against `""` and copied into an attribute
     (`Attribute(attr, v)`), or have their length copied (`#v`, `v:len()` into
     `AttributeInteger`/`AttributeNumeric`).
   - `MercatorArea()`: may only be compared with constants. Those constants are the
     `way_memo_area` thresholds.
   - The relation from `NextRelation()`: may only be tested for nil.
4. It tracks state carried from one object to the next: globals, module locals and
   module-level tables that are read before being written in the same run, and are
   written by any per-object function. A global that is always written before it is read
   (scratch, like `i` in the OpenMapTiles `split()`) is fine. A read that comes after a
   Lua-only call such as `Id()` is also fine, because tilemaker always sends that path
   through Lua.
5. It reports side effects that a replay would skip (`print`, `math.random`, `io`, `os`).

It fails closed: if it can't follow something (closures, `goto`, calls it can't resolve),
it says so and doesn't recommend `way_memo = true`.

## Output

- `ERROR`: a declared promise doesn't hold, with the line and the reason.
- `WARNING`: a value is copied into attributes the declaration doesn't list (those objects
  are never replayed), or parts of the code could not be analysed.
- `SUGGESTION`: a declaration that would be safe but is missing.
- `Recommended declarations`: the largest set of declarations it can prove.

## Tests

```bash
python3 -m pytest tools/memo-lint/tests -q
```
