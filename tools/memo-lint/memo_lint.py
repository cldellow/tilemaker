#!/usr/bin/env python3
"""memo-lint: check and infer tilemaker's way_memo / node_memo declarations.

tilemaker can replay way_function/node_function from a memo (systemed/tilemaker#993) if the
profile promises things tilemaker can't check at run time:

  way_memo = true          way_function is a function of what it reads (no state across objects)
  way_memo_values          tags whose value is only tested for "" and copied into attributes
  way_memo_area            MercatorArea() is only compared with these numbers
  way_memo_relations       NextRelation()'s id is only tested for nil

A wrong promise silently writes wrong tiles. This tool proves (or refutes) them from the
profile's source: it loads the profile in a real Lua (to see its configuration: globals, tables,
upvalues), then abstractly interprets way_function/node_function from the AST, following values
through locals, helper functions, tables and branches.

Usage: memo_lint.py profile.lua [--json]
"""
import argparse
import json
import sys
from collections import defaultdict

from luaparser import ast as luaast
from luaparser import astnodes as N

try:
    import lupa.lua54 as lupa_mod
except ImportError:  # pragma: no cover
    import lupa as lupa_mod

# ---------------------------------------------------------------- the tilemaker API

API_REPLAYED = {  # recorded as calls and replayed
    'Layer', 'LayerAsCentroid', 'Attribute', 'AttributeNumeric', 'AttributeBoolean',
    'AttributeInteger', 'MinZoom', 'ZOrder', 'Score', 'ModifyId', 'MinZoomByArea'}
API_READS = {'Find', 'Holds', 'IsClosed', 'MercatorArea', 'NextRelation', 'FindInRelation'}
API_LUA_ONLY = {  # tilemaker sends objects that reach these through Lua: safe, but not replayed
    'Id', 'OsmType', 'AllKeys', 'AllTags', 'HasTags', 'SetTag', 'FindIntersecting', 'Intersects',
    'FindCovering', 'CoveredBy', 'IsMultiPolygon', 'Area', 'Length', 'Centroid',
    'RestartRelations', 'Accept', 'AreaIntersecting', 'SetData', 'GetData'}
API = API_REPLAYED | API_READS | API_LUA_ONLY

# Entry points run once per object. The memo replays the first two; the others matter for state.
MEMO_ENTRIES = {'way_function': 'way', 'node_function': 'node'}
OTHER_ENTRIES = ['relation_scan_function', 'relation_function', 'relation_postscan_function']

PURE_BUILTIN_PREFIXES = ('string.', 'math.', 'utf8.')
PURE_BUILTINS = {'tonumber', 'tostring', 'type', 'select', 'ipairs', 'pairs', 'next',
                 'rawequal', 'rawlen', 'rawget', 'unpack', 'table.unpack', 'table.concat',
                 'assert', 'error', 'getmetatable'}
IMPURE_BUILTINS = {'math.random', 'math.randomseed', 'print', 'require', 'dofile', 'loadfile',
                   'load', 'loadstring', 'collectgarbage', 'setmetatable', 'rawset'}
TABLE_MUTATORS = {'table.insert', 'table.remove', 'table.sort', 'rawset', 'setmetatable'}

MAX_UNROLL = 256
MAX_CONSTS = 64
MAX_DEPTH = 40

# ---------------------------------------------------------------- abstract values

NIL = ('nil',)


def const_of(x):
    """A Python value from Lua -> a hashable tagged constant."""
    if x is None:
        return NIL
    if isinstance(x, bool):
        return ('b', x)
    if isinstance(x, (int, float)):
        return ('n', float(x))
    if isinstance(x, bytes):
        x = x.decode('utf-8', 'replace')
    if isinstance(x, str):
        return ('s', x)
    return None


def const_py(c):
    if c == NIL:
        return None
    v = c[1]
    if c[0] == 'n' and v == int(v):
        return int(v)
    return v


def truthy_const(c):
    return not (c == NIL or c == ('b', False))


def as_pair(r):
    r = r if isinstance(r, tuple) else (r,)
    return r[0], (r[1] if len(r) > 1 else None)


class LTab:
    """A table made during this object's run: mutating it is harmless."""
    __slots__ = ('taints',)

    def __init__(self):
        self.taints = set()


class V:
    """Abstract Lua value.

    consts: frozenset of tagged constants it may be, or None (unknown)
    nonnil: known not to be nil/false (only meaningful when consts is None)
    taints: frozenset of labels: ('val', key) a tag value from Find, ('len', key) its length,
            ('area',) MercatorArea(), ('rel',) a relation id from NextRelation()
    table:  a persistent (module-level) Lua table, with its id
    ltab:   an LTab
    func:   ('api', name) | ('builtin', name) | ('lua', line, luafunc) | ('other',)
    """
    __slots__ = ('consts', 'nonnil', 'taints', 'table', 'tid', 'ltab', 'func')

    def __init__(self, consts=None, nonnil=False, taints=frozenset(), table=None, tid=None,
                 ltab=None, func=None):
        self.consts = consts
        self.nonnil = nonnil
        self.taints = frozenset(taints)
        self.table = table
        self.tid = tid
        self.ltab = ltab
        self.func = func

    @staticmethod
    def const(x):
        return V(consts=frozenset([const_of(x)]))

    @staticmethod
    def top(taints=frozenset(), nonnil=False):
        return V(taints=taints, nonnil=nonnil)

    def is_known(self):
        return self.consts is not None and self.table is None and self.ltab is None and self.func is None

    def single(self):
        if self.is_known() and len(self.consts) == 1:
            return next(iter(self.consts))
        return None

    def truth(self):
        """True / False if known, else None."""
        if self.table is not None or self.ltab is not None or self.func is not None:
            return True
        if self.consts is None:
            return True if self.nonnil else None
        ts = {truthy_const(c) for c in self.consts}
        return ts.pop() if len(ts) == 1 else None

    def strings(self):
        """The strings it may be (None: unknown, or not only strings)."""
        if not self.is_known():
            return None
        out = []
        for c in self.consts:
            if c[0] != 's':
                return None
            out.append(c[1])
        return out

    def key(self):
        return (self.consts, self.nonnil, self.taints, self.tid, id(self.ltab) if self.ltab else None,
                self.func[:2] if self.func else None)

    def __repr__(self):
        return f'V({self.consts},{set(self.taints)},{self.tid},{self.func and self.func[:2]})'


def join(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a.key() == b.key():
        return a
    taints = a.taints | b.taints
    if a.is_known() and b.is_known():
        cs = a.consts | b.consts
        if len(cs) <= MAX_CONSTS:
            return V(consts=cs, taints=taints)
        return V(taints=taints, nonnil=all(truthy_const(c) for c in cs))
    same_tab = a.tid is not None and a.tid == b.tid
    same_func = a.func is not None and b.func is not None and a.func[:2] == b.func[:2]
    if same_tab:
        return V(table=a.table, tid=a.tid, taints=taints)
    if same_func:
        return V(func=a.func, taints=taints)
    if a.ltab is not None and a.ltab is b.ltab:
        return V(ltab=a.ltab, taints=taints)
    return V(taints=taints, nonnil=(a.truth() is True and b.truth() is True))


def join_all(vs):
    out = None
    for v in vs:
        out = join(out, v)
    return out if out is not None else V.const(None)


# ---------------------------------------------------------------- interpreter state

class Ctx:
    """Collects what leaves a function (returns) or a loop (breaks)."""

    def __init__(self):
        self.returns = []  # (state, [V])
        self.breaks = []   # state


class Frame:
    __slots__ = ('scopes', 'upvals', 'ctx', 'loops', 'fname')

    def __init__(self, upvals, ctx, fname):
        self.scopes = [{}]
        self.upvals = upvals  # name -> V (this closure's upvalues, from the loaded profile)
        self.ctx = ctx
        self.loops = []
        self.fname = fname

    def copy(self):
        f = Frame(self.upvals, self.ctx, self.fname)
        f.scopes = [dict(s) for s in self.scopes]
        f.loops = list(self.loops)
        return f


class State:
    """One path through the object's run."""
    __slots__ = ('frame', 'gw', 'defw', 'alive', 'luaonly')

    def __init__(self, frame):
        self.frame = frame
        self.gw = {}          # persistent name ('G'|'U', name) -> V, written during this run
        self.defw = set()     # persistent names definitely written during this run (before here)
        self.alive = True
        self.luaonly = False  # definitely called a Lua-only function (the memo won't replay this path)

    def copy(self):
        s = State(self.frame.copy())
        s.gw = dict(self.gw)
        s.defw = set(self.defw)
        s.alive = self.alive
        s.luaonly = self.luaonly
        return s


# ---------------------------------------------------------------- the analyzer

class Analyzer:
    def __init__(self, path):
        self.path = path
        with open(path, encoding='utf-8') as f:
            self.src = f.read()
        self.tree = luaast.parse(self.src)
        self.funcs_by_line = {}
        for node in luaast.walk(self.tree):
            if isinstance(node, (N.Function, N.LocalFunction, N.AnonymousFunction, N.Method)):
                self.funcs_by_line.setdefault(node.line, node)
        self._load()
        self.reset_events()
        # persistent state, across all entries
        self.writes = defaultdict(list)      # pname -> [(entry, line, protected)]
        self.unsafe_reads = defaultdict(list)  # pname -> [(entry, line, protected)] read before a write in the same run
        self.mutations = defaultdict(list)   # ('T', tid) -> [(entry, line, protected)]
        self.table_reads = defaultdict(list)  # ('T', tid) -> [(entry, line, protected)]
        self.tables = {}                     # tid -> description

    # ---- loading the profile in Lua, to know its configuration
    def _load(self):
        L = lupa_mod.LuaRuntime(unpack_returned_tuples=True)
        self.L = L
        L.execute(r'''
            ml_builtin = {}
            for _, lib in ipairs({"string","math","table","os","io","utf8","coroutine","debug"}) do
              if _G[lib] then for k, f in pairs(_G[lib]) do
                if type(f) == "function" then ml_builtin[f] = lib .. "." .. k end end end
            end
            for k, f in pairs(_G) do if type(f) == "function" and not ml_builtin[f] then ml_builtin[f] = k end end
            ml_api = {}
            function ml_stub(name) local f = function(...) return nil end; _G[name] = f; ml_api[f] = name end
            function ml_classify(f)
              if ml_api[f] then return "api", ml_api[f] end
              if ml_builtin[f] then return "builtin", ml_builtin[f] end
              local i = debug.getinfo(f, "S")
              if i.what == "Lua" then return "lua", i.linedefined end
              return "other", ""
            end
            function ml_upvalues(f)
              local t, i = {}, 1
              while true do
                local n, v = debug.getupvalue(f, i)
                if not n then break end
                if n ~= "_ENV" then t[n] = {v} end
                i = i + 1
              end
              return t
            end
            local ids, n = setmetatable({}, {__mode = "k"}), 0
            function ml_tid(t) if not ids[t] then n = n + 1; ids[t] = n end; return ids[t] end
            function ml_len(t) return #t end
            function ml_call(f, ...) return pcall(f, ...) end
        ''')
        for name in sorted(API):
            L.globals().ml_stub(name)
        chunk = L.execute  # noqa
        L.globals()['ml_src'] = self.src
        ok, err = as_pair(L.eval('pcall(function() local f = assert(load(ml_src, "=profile")); f() end)'))
        if not ok:
            raise SystemExit(f'memo-lint: the profile failed to load: {err}')
        G = L.globals()
        if G.init_function is not None:
            ok, err = as_pair(L.globals().ml_call(G.init_function, 'memo-lint', True))
            if not ok:
                print(f'memo-lint: note: init_function failed under the stubs ({err}); continuing',
                      file=sys.stderr)
        self.G = G

    def from_lua(self, x):
        c = const_of(x)
        if c is not None:
            return V(consts=frozenset([c]))
        t = lupa_mod.lua_type(x)
        if t == 'table':
            tid = self.G.ml_tid(x)
            return V(table=x, tid=tid)
        if t == 'function':
            kind, info = self.G.ml_classify(x)
            if kind == 'lua':
                return V(func=('lua', info, x))
            return V(func=(kind, info))
        return V.top(nonnil=True)

    def upvals_of(self, luaf):
        out = {}
        for name, box in self.G.ml_upvalues(luaf).items():
            out[name] = box[1]
        return out

    # ---- events
    def reset_events(self):
        self.ev = defaultdict(set)  # label -> {(kind, detail, line)}
        self.notes = defaultdict(set)  # kind -> {(detail, line)}
        self.relreads = set()
        self.attr_sources = defaultdict(set)  # attribute -> {('val', k) | ('len', k) | ('other', line)}

    def use(self, v, kind, detail, line, allow=()):
        """v is used in a way that isn't one of `allow`: record it for each of its taints."""
        for t in v.taints:
            if t[0] in allow:
                continue
            self.ev[t].add(('other', detail, line))

    # ---- names
    def lookup_local(self, st, name):
        for scope in reversed(st.frame.scopes):
            if name in scope:
                return scope
        return None

    def pname(self, st, name):
        return ('U', name) if name in st.frame.upvals else ('G', name)

    def read_name(self, st, name, line):
        scope = self.lookup_local(st, name)
        if scope is not None:
            return scope[name]
        pn = self.pname(st, name)
        if pn not in st.defw:
            self.unsafe_reads[pn].append((self.entry, line, st.luaonly))
        snap = st.frame.upvals[name] if pn[0] == 'U' else self.G[name]
        snapv = self.from_lua(snap)
        if pn in st.gw:
            return st.gw[pn] if pn in st.defw else join(st.gw[pn], snapv)
        return snapv

    def write_name(self, st, name, v, line, local=False):
        if local:
            st.frame.scopes[-1][name] = v
            return
        scope = self.lookup_local(st, name)
        if scope is not None:
            scope[name] = v
            return
        pn = self.pname(st, name)
        self.writes[pn].append((self.entry, line, st.luaonly))
        st.gw[pn] = v
        st.defw.add(pn)

    # ---- joins
    def join_states(self, states):
        live = [s for s in states if s.alive]
        if not live:
            d = states[0].copy() if states else None
            if d is not None:
                d.alive = False
            return d
        if len(live) == 1:
            return live[0]
        out = live[0].copy()
        for s in live[1:]:
            for i, scope in enumerate(out.frame.scopes):
                if i >= len(s.frame.scopes):
                    break
                other = s.frame.scopes[i]
                for k in list(scope):
                    if k in other:
                        scope[k] = join(scope[k], other[k])
            for k, v in s.gw.items():
                out.gw[k] = join(out.gw.get(k), v)
            out.defw &= s.defw
            out.luaonly = out.luaonly and s.luaonly
        return out

    # ---- running an entry point
    def run_entry(self, name):
        luaf = self.G[name]
        if luaf is None or lupa_mod.lua_type(luaf) != 'function':
            return False
        self.entry = name
        fv = self.from_lua(luaf)
        if fv.func[0] != 'lua':
            return False
        ctx = Ctx()
        st = State(Frame({}, ctx, name))
        self.call_lua(fv, [], st, line=0, depth=0)
        return True

    def call_lua(self, fv, args, st, line, depth):
        """Inline a profile function. Returns (state after, [V results])."""
        fnode = self.funcs_by_line.get(fv.func[1])
        if fnode is None or depth > MAX_DEPTH:
            self.notes['unanalysed-call'].add((f'function at line {fv.func[1]}', line))
            for a in args:
                self.use(a, 'other', 'passed to a function memo-lint could not follow', line)
            return st, [V.top()]
        ctx = Ctx()
        cst = State(Frame(self.upvals_of(fv.func[2]), ctx, getattr(fnode, 'name', None)))
        cst.gw, cst.defw, cst.luaonly = dict(st.gw), set(st.defw), st.luaonly
        params = list(fnode.args)
        if isinstance(fnode, N.Method):
            params = [N.Name('self')] + params
        for i, p in enumerate(params):
            if isinstance(p, N.Varargs):
                cst.frame.scopes[0]['...'] = join_all(args[i:]) if args[i:] else V.const(None)
                break
            cst.frame.scopes[0][p.id] = args[i] if i < len(args) else V.const(None)
        end = self.exec_block(fnode.body, cst, depth)
        outs = [end] + [s for s, _ in ctx.returns]
        res_lists = ([[]] if end.alive else []) + [r for _, r in ctx.returns]
        st2 = self.join_states(outs)
        if st2 is not None and any(o.alive for o in outs):
            st.gw, st.defw, st.luaonly = st2.gw, st2.defw, st2.luaonly
        n = max((len(r) for r in res_lists), default=0)
        results = []
        for i in range(max(n, 1)):
            results.append(join_all([r[i] if i < len(r) else V.const(None) for r in res_lists]))
        return st, results

    # ---- statements
    def exec_block(self, block, st, depth):
        body = block.body if isinstance(block, N.Block) else block
        st.frame.scopes.append({})
        for stmt in body:
            if not st.alive:
                break
            st = self.exec_stmt(stmt, st, depth)
        st.frame.scopes.pop()
        return st

    def exec_stmt(self, s, st, depth):
        line = getattr(s, 'line', None)
        if isinstance(s, N.LocalAssign):
            vals = self.eval_list(s.values, st, depth, len(s.targets))
            for i, t in enumerate(s.targets):
                self.write_name(st, t.id, vals[i] if i < len(vals) else V.const(None), line, local=True)
            return st
        if isinstance(s, N.Assign):
            vals = self.eval_list(s.values, st, depth, len(s.targets))
            for i, t in enumerate(s.targets):
                v = vals[i] if i < len(vals) else V.const(None)
                if isinstance(t, N.Name):
                    self.write_name(st, t.id, v, line)
                elif isinstance(t, N.Index):
                    self.assign_index(t, v, st, depth, line)
                else:
                    self.notes['unsupported'].add((type(t).__name__ + ' assignment', line))
            return st
        if isinstance(s, (N.Call, N.Invoke)):
            self.eval_multi(s, st, depth)
            return st
        if isinstance(s, N.If):
            return self.exec_if(s, st, depth)
        if isinstance(s, N.Do):
            return self.exec_block(s.body, st, depth)
        if isinstance(s, N.Return):
            vals = self.eval_list(s.values, st, depth, None) if s.values else []
            st.frame.ctx.returns.append((st.copy(), vals))
            st.alive = False
            return st
        if isinstance(s, N.Break):
            if st.frame.loops:
                st.frame.loops[-1].append(st.copy())
            st.alive = False
            return st
        if isinstance(s, N.While):
            return self.exec_loop(st, depth, test=s.test, body=s.body)
        if isinstance(s, N.Repeat):
            return self.exec_loop(st, depth, test=s.test, body=s.body, repeat=True)
        if isinstance(s, N.Fornum):
            return self.exec_fornum(s, st, depth)
        if isinstance(s, N.Forin):
            return self.exec_forin(s, st, depth)
        if isinstance(s, (N.LocalFunction, N.Function, N.Method)):
            self.notes['unsupported'].add(('function defined while processing an object', line))
            if isinstance(s, N.LocalFunction):
                self.write_name(st, s.name.id, V.top(nonnil=True), line, local=True)
            return st
        if isinstance(s, (N.SemiColon, N.Comment)):
            return st
        if isinstance(s, (N.Goto, N.Label)):
            self.notes['unsupported'].add(('goto/label', line))
            return st
        self.notes['unsupported'].add((type(s).__name__, line))
        return st

    def exec_if(self, s, st, depth):
        cond = self.eval(s.test, st, depth)
        t = cond.truth()
        outs = []
        if t is not False:
            a = st.copy()
            self.refine(s.test, a, True, depth)
            outs.append(self.exec_block(s.body, a, depth))
        if t is not True:
            b = st.copy()
            self.refine(s.test, b, False, depth)
            orelse = s.orelse
            if orelse is None:
                outs.append(b)
            elif isinstance(orelse, N.ElseIf):
                outs.append(self.exec_if(orelse, b, depth))
            else:
                outs.append(self.exec_block(orelse, b, depth))
        return self.join_states(outs)

    def var_slot(self, st, name):
        """(get, set) for a local, or a persistent name definitely written in this run."""
        scope = self.lookup_local(st, name)
        if scope is not None:
            return (lambda: scope[name]), (lambda v: scope.__setitem__(name, v))
        pn = self.pname(st, name)
        if pn in st.defw:
            return (lambda: st.gw[pn]), (lambda v: st.gw.__setitem__(pn, v))
        return None

    def refine(self, test, st, truth, depth):
        """What a branch learns from its test: x == "c" makes x "c"; x makes x non-nil."""
        if isinstance(test, N.ULNotOp):
            return self.refine(test.operand, st, not truth, depth)
        if isinstance(test, N.AndLoOp) and truth:
            self.refine(test.left, st, True, depth)
            self.refine(test.right, st, True, depth)
            return
        if isinstance(test, N.OrLoOp) and not truth:
            self.refine(test.left, st, False, depth)
            self.refine(test.right, st, False, depth)
            return
        if isinstance(test, (N.EqToOp, N.NotEqToOp)):
            eq = truth if isinstance(test, N.EqToOp) else not truth
            for var, other in ((test.left, test.right), (test.right, test.left)):
                if isinstance(var, N.Name) and isinstance(other, (N.String, N.Number, N.Nil, N.TrueExpr, N.FalseExpr)):
                    slot = self.var_slot(st, var.id)
                    if slot is None:
                        return
                    get, put = slot
                    cur = get()
                    c = const_of(self.literal(other))
                    if eq:
                        put(V(consts=frozenset([c]), taints=cur.taints))
                    elif cur.is_known():
                        rest = cur.consts - {c}
                        if rest:
                            put(V(consts=rest, taints=cur.taints))
                    return
            return
        if isinstance(test, N.Name):
            slot = self.var_slot(st, test.id)
            if slot is None:
                return
            get, put = slot
            cur = get()
            if cur.is_known():
                keep = frozenset(c for c in cur.consts if truthy_const(c) == truth)
                if keep:
                    put(V(consts=keep, taints=cur.taints))
            elif truth:
                put(V(consts=cur.consts, nonnil=True, taints=cur.taints, table=cur.table,
                                   tid=cur.tid, ltab=cur.ltab, func=cur.func))

    def exec_loop(self, st, depth, test, body, repeat=False):
        breaks = []
        st.frame.loops.append(breaks)
        exits = []
        entry = st
        for _ in range(3):
            if not repeat:
                c = self.eval(test, entry, depth).truth()
                if c is not True:
                    e = entry.copy()
                    exits.append(e)
                if c is False:
                    break
                b = entry.copy()
                self.refine(test, b, True, depth)
            else:
                b = entry.copy()
            out = self.exec_block(body, b, depth)
            if repeat and out.alive:
                c = self.eval(test, out, depth).truth()
                if c is not False:
                    exits.append(out.copy())
                if c is True:
                    break
            entry = self.join_states([entry, out]) if out.alive else entry
            if not out.alive and not repeat:
                break
        st.frame.loops.pop()
        res = self.join_states(exits + breaks) if (exits or breaks) else None
        if res is None:
            res = st.copy()
            res.alive = False
        return res

    def exec_fornum(self, s, st, depth):
        a = self.eval(s.start, st, depth)
        b = self.eval(s.stop, st, depth)
        c = self.eval(s.step, st, depth) if s.step is not None else V.const(1)
        for v in (a, b, c):
            self.use(v, 'other', 'numeric for bound', s.line)
        vals = None
        if all(x.single() and x.single()[0] == 'n' for x in (a, b, c)):
            lo, hi, step = (x.single()[1] for x in (a, b, c))
            if step != 0 and abs((hi - lo) / step) < MAX_UNROLL:
                vals = []
                i = lo
                while (step > 0 and i <= hi) or (step < 0 and i >= hi):
                    vals.append(V.const(i))
                    i += step
        return self.run_iterations(s, st, depth, [[v] for v in vals] if vals is not None else None,
                                   [V.top(nonnil=True)])

    def exec_forin(self, s, st, depth):
        iters = None
        top = [V.top(nonnil=True), V.top()]
        it = s.iter[0] if len(s.iter) == 1 else None
        if isinstance(it, N.Call) and len(it.args) == 1:
            f = self.eval(it.func, st, depth)
            tv = self.eval(it.args[0], st, depth)
            fname = f.func[1] if f.func and f.func[0] == 'builtin' else None
            if fname in ('ipairs', 'pairs') and tv.table is not None:
                self.table_reads[('T', tv.tid)].append((self.entry, s.line, st.luaonly))
                items = []
                if fname == 'ipairs':
                    i = 1
                    while tv.table[i] is not None and len(items) < MAX_UNROLL:
                        items.append([V.const(i), self.from_lua(tv.table[i])])
                        i += 1
                else:
                    for k, v in tv.table.items():
                        items.append([self.from_lua(k), self.from_lua(v)])
                        if len(items) >= MAX_UNROLL:
                            break
                    # pairs' order is unspecified: memo-safe only if the body doesn't depend on it;
                    # we analyse every element, which covers any order
                if len(items) < MAX_UNROLL:
                    iters = items
            elif fname in ('ipairs', 'pairs') and tv.ltab is not None:
                top = [V.top(nonnil=True), V.top(taints=tv.ltab.taints)]
            else:
                vals = self.eval_multi(it, st, depth)
                top = [V.top(nonnil=True, taints=frozenset().union(*[v.taints for v in vals]))] * 2
        else:
            for e in s.iter:
                self.eval(e, st, depth)
        return self.run_iterations(s, st, depth, iters, top)

    def run_iterations(self, s, st, depth, iters, top):
        targets = s.targets if isinstance(s, N.Forin) else [s.target]
        breaks = []
        st.frame.loops.append(breaks)
        cur = st
        if iters is not None:
            for vals in iters:
                b = cur.copy()
                b.frame.scopes.append({})
                for i, t in enumerate(targets):
                    b.frame.scopes[-1][t.id] = vals[i] if i < len(vals) else V.const(None)
                out = self.exec_block(s.body, b, depth)
                if out.alive:
                    out.frame.scopes.pop()
                    cur = out
                else:
                    cur = cur  # the iteration ended (break/return): later iterations start from before it
                    # a break ends the loop: stop unrolling only if this path definitely broke
                    if breaks and not out.alive:
                        pass
            exits = [cur] + breaks
        else:
            entry = cur
            exits = [cur.copy()]
            for _ in range(3):
                b = entry.copy()
                b.frame.scopes.append({})
                for i, t in enumerate(targets):
                    b.frame.scopes[-1][t.id] = top[i] if i < len(top) else V.top()
                out = self.exec_block(s.body, b, depth)
                if not out.alive:
                    break
                out.frame.scopes.pop()
                entry = self.join_states([entry, out])
                exits.append(entry.copy())
            exits += breaks
        st.frame.loops.pop()
        for e in exits:
            while len(e.frame.scopes) > len(st.frame.scopes):
                e.frame.scopes.pop()
        return self.join_states(exits)

    def assign_index(self, t, v, st, depth, line):
        tv = self.eval(t.value, st, depth)
        kv = self.eval_key(t, st, depth)
        self.use(kv, 'other', 'used as a table key', line)
        if tv.ltab is not None:
            tv.ltab.taints |= set(v.taints)
            return
        if tv.table is not None:
            self.mutations[('T', tv.tid)].append((self.entry, line, st.luaonly))
            self.use(v, 'other', 'stored in a table that outlives the object', line)
            return
        self.use(v, 'other', 'stored in a table memo-lint could not follow', line)
        self.notes['unanalysed-table-write'].add(('', line))

    # ---- expressions
    def literal(self, e):
        if isinstance(e, N.String):
            s = e.s
            return s.decode('utf-8', 'replace') if isinstance(s, bytes) else s
        if isinstance(e, N.Number):
            return e.n
        if isinstance(e, N.TrueExpr):
            return True
        if isinstance(e, N.FalseExpr):
            return False
        return None

    def eval_list(self, exprs, st, depth, want):
        out = []
        for i, e in enumerate(exprs):
            last = i == len(exprs) - 1
            if want is not None and i >= want and isinstance(e, N.Name):
                continue  # an extra value that is discarded: reading a name has no effect
            if last and isinstance(e, (N.Call, N.Invoke, N.Varargs)):
                out.extend(self.eval_multi(e, st, depth))
            else:
                out.append(self.eval(e, st, depth))
        return out

    def eval(self, e, st, depth):
        if isinstance(e, (N.Call, N.Invoke)):
            r = self.eval_multi(e, st, depth)
            return r[0] if r else V.const(None)
        return self.eval1(e, st, depth)

    def eval_key(self, idx, st, depth):
        if idx.notation == N.IndexNotation.DOT and isinstance(idx.idx, N.Name):
            return V.const(idx.idx.id)
        return self.eval(idx.idx, st, depth)

    def index(self, tv, kv, st, line):
        self.use(kv, 'other', 'used as a table key', line)
        if tv.table is not None:
            self.table_reads[('T', tv.tid)].append((self.entry, line, st.luaonly))
            k = kv.single()
            if k is not None and k != NIL and not kv.taints:
                return self.from_lua(tv.table[const_py(k)])
            ks = kv.consts if kv.is_known() and len(kv.consts) <= MAX_CONSTS else None
            if ks is not None:
                return join_all([self.from_lua(tv.table[const_py(k)]) if k != NIL else V.const(None) for k in ks])
            # unknown key: any value of the table, or nil
            vals = [V.const(None)]
            for _, v in tv.table.items():
                vals.append(self.from_lua(v))
                if len(vals) > MAX_CONSTS * 4:
                    return V.top()
            return join_all(vals)
        if tv.ltab is not None:
            return V.top(taints=tv.ltab.taints)
        strs = tv.strings()
        if tv.taints or strs is not None:
            # indexing a string: its methods (s.len etc.)
            self.use(tv, 'other', 'indexed', line)
        return V.top()

    def eval1(self, e, st, depth):
        line = getattr(e, 'line', None)
        lit_types = (N.String, N.Number, N.TrueExpr, N.FalseExpr)
        if isinstance(e, lit_types):
            return V.const(self.literal(e))
        if isinstance(e, N.Nil):
            return V.const(None)
        if isinstance(e, N.Name):
            return self.read_name(st, e.id, line)
        if isinstance(e, N.Varargs):
            return st.frame.scopes[0].get('...', V.top())
        if isinstance(e, N.Index):
            tv = self.eval(e.value, st, depth)
            kv = self.eval_key(e, st, depth)
            return self.index(tv, kv, st, line)
        if isinstance(e, N.Table):
            lt = LTab()
            for f in e.fields:
                if not isinstance(f.key, N.Name) and f.key is not None:
                    k = self.eval(f.key, st, depth)
                    self.use(k, 'other', 'used as a table key', line)
                v = self.eval(f.value, st, depth)
                lt.taints |= set(v.taints)
            return V(ltab=lt)
        if isinstance(e, (N.AnonymousFunction,)):
            self.notes['unsupported'].add(('function made while processing an object', line))
            return V.top(nonnil=True)
        if isinstance(e, N.AndLoOp):
            a = self.eval(e.left, st, depth)
            ta = a.truth()
            if ta is False:
                return a
            b2 = st.copy()
            self.refine(e.left, b2, True, depth)
            b = self.eval(e.right, b2, depth)
            if ta is True:
                return b
            falsy = (V(consts=frozenset(c for c in a.consts if not truthy_const(c)), taints=a.taints)
                     if a.is_known() else V(consts=frozenset([NIL, ('b', False)]), taints=a.taints))
            return join(falsy, b) if b.truth() is False else join(a if not a.is_known() else falsy, b)
        if isinstance(e, N.OrLoOp):
            a = self.eval(e.left, st, depth)
            ta = a.truth()
            if ta is True:
                return a
            b2 = st.copy()
            self.refine(e.left, b2, False, depth)
            b = self.eval(e.right, b2, depth)
            if ta is False:
                return b
            if a.is_known():
                a = V(consts=frozenset(c for c in a.consts if truthy_const(c)) or frozenset([NIL]), taints=a.taints)
            return join(a, b)
        if isinstance(e, N.ULNotOp):
            a = self.eval(e.operand, st, depth)
            self.use(a, 'other', 'not', line, allow=('val', 'rel', 'area', 'len'))  # truthiness: no use
            t = a.truth()
            return V.const(not t) if t is not None else V.top()
        if isinstance(e, N.ULengthOP):
            a = self.eval(e.operand, st, depth)
            return self.length_of(a, line)
        if isinstance(e, (N.EqToOp, N.NotEqToOp)):
            return self.compare_eq(e, st, depth, line)
        if isinstance(e, (N.LessThanOp, N.LessOrEqThanOp, N.GreaterThanOp, N.GreaterOrEqThanOp)):
            return self.compare_order(e, st, depth, line)
        if isinstance(e, (N.BinaryOp, N.UnaryOp)):
            operands = [e.left, e.right] if isinstance(e, N.BinaryOp) else [e.operand]
            vals = [self.eval(o, st, depth) for o in operands]
            what = {'Concat': 'concatenated'}.get(type(e).__name__, 'used in arithmetic')
            for v in vals:
                self.use(v, 'other', what, line)
            return self.fold(e, vals)
        self.notes['unsupported'].add((type(e).__name__, line))
        return V.top()

    def fold(self, e, vals):
        """Constant folding, by asking Lua."""
        cs = [v.single() for v in vals]
        if any(c is None for c in cs):
            return V.top(nonnil=True)
        op = {'AddOp': '+', 'SubOp': '-', 'MultOp': '*', 'FloatDivOp': '/', 'FloorDivOp': '//',
              'ModOp': '%', 'ExpoOp': '^', 'Concat': '..', 'BAndOp': '&', 'BOrOp': '|',
              'BXorOp': '~', 'BShiftLOp': '<<', 'BShiftROp': '>>', 'UMinusOp': '-', 'UBNotOp': '~'
              }.get(type(e).__name__)
        if op is None:
            return V.top(nonnil=True)
        args = [const_py(c) for c in cs]
        try:
            if len(args) == 2:
                f = self.L.eval(f'function(a, b) return a {op} b end')
            else:
                f = self.L.eval(f'function(a) return {op} a end')
            return V.const(f(*args))
        except Exception:
            return V.top(nonnil=True)

    def length_of(self, a, line):
        taints = set()
        for t in a.taints:
            if t[0] == 'val':
                taints.add(('len', t[1]))
            else:
                self.ev[t].add(('other', 'length taken', line))
        if a.table is not None:
            return V(consts=frozenset([('n', float(self.G.ml_len(a.table)))]), taints=frozenset(taints))
        s = a.strings()
        if s is not None:
            return V(consts=frozenset(('n', float(len(x.encode()))) for x in s), taints=frozenset(taints))
        return V.top(nonnil=True, taints=frozenset(taints))

    def compare_eq(self, e, st, depth, line):
        a = self.eval(e.left, st, depth)
        b = self.eval(e.right, st, depth)
        for x, y in ((a, b), (b, a)):
            ys = y.consts if y.is_known() and not y.taints else None
            for t in x.taints:
                if t[0] == 'val':
                    if ys is not None and ys <= {('s', ''), NIL}:
                        self.ev[t].add(('empty', '', line))
                    else:
                        what = 'compared with ' + (', '.join(repr(const_py(c)) for c in sorted(ys, key=str)) if ys
                                                   else 'a value that is not a constant')
                        self.ev[t].add(('other', what, line))
                elif t[0] == 'rel':
                    if ys is not None and ys <= {NIL, ('b', False)}:
                        self.ev[t].add(('test', '', line))
                    else:
                        self.ev[t].add(('other', 'compared with something other than nil', line))
                elif t[0] == 'area':
                    self.area_cmp(t, ys, line)
                else:
                    self.ev[t].add(('other', 'compared', line))
        if a.is_known() and b.is_known() and not a.taints and not b.taints:
            res = {(x == y) for x in a.consts for y in b.consts}
            if isinstance(e, N.NotEqToOp):
                res = {not r for r in res}
            if len(res) == 1:
                return V.const(res.pop())
        return V.top()

    def area_cmp(self, t, ys, line):
        if ys is not None and all(c[0] == 'n' for c in ys):
            for c in ys:
                self.ev[t].add(('cmp', c[1], line))
        else:
            self.ev[t].add(('other', 'compared with a value that is not a constant number', line))

    def compare_order(self, e, st, depth, line):
        a = self.eval(e.left, st, depth)
        b = self.eval(e.right, st, depth)
        for x, y in ((a, b), (b, a)):
            ys = y.consts if y.is_known() and not y.taints else None
            for t in x.taints:
                if t[0] == 'area':
                    self.area_cmp(t, ys, line)
                else:
                    self.ev[t].add(('other', 'compared with <, >', line))
        return self.fold_cmp(e, a, b)

    def fold_cmp(self, e, a, b):
        ca, cb = a.single(), b.single()
        if ca and cb and ca[0] == cb[0] and ca[0] in 'ns' and not a.taints and not b.taints:
            x, y = ca[1], cb[1]
            r = {'LessThanOp': x < y, 'LessOrEqThanOp': x <= y, 'GreaterThanOp': x > y,
                 'GreaterOrEqThanOp': x >= y}[type(e).__name__]
            return V.const(r)
        return V.top()

    # ---- calls
    def eval_multi(self, e, st, depth):
        line = getattr(e, 'line', None)
        if isinstance(e, N.Varargs):
            return [st.frame.scopes[0].get('...', V.top())]
        if isinstance(e, N.Invoke):
            obj = self.eval(e.source, st, depth)
            meth = e.func.id
            args = [self.eval(a, st, depth) for a in e.args]
            if obj.table is not None or obj.ltab is not None:
                f = self.index(obj, V.const(meth), st, line)
                return self.call(f, [obj] + args, st, depth, line)
            # a string method
            return self.call_builtin('string.' + meth, [obj] + args, st, line)
        f = self.eval(e.func, st, depth)
        args = self.eval_list(e.args, st, depth, None)
        return self.call(f, args, st, depth, line)

    def call(self, f, args, st, depth, line):
        if f.func is None:
            for a in args:
                self.use(a, 'other', 'passed to a value memo-lint could not resolve as a function', line)
            self.notes['unanalysed-call'].add(('call of an unknown value', line))
            return [V.top()]
        kind = f.func[0]
        if kind == 'api':
            return self.call_api(f.func[1], args, st, line)
        if kind == 'builtin':
            return self.call_builtin(f.func[1], args, st, line)
        if kind == 'lua':
            _, res = self.call_lua(f, args, st, line, depth + 1)
            return res
        for a in args:
            self.use(a, 'other', 'passed to a C function', line)
        self.notes['unanalysed-call'].add(('C function', line))
        return [V.top()]

    def call_api(self, name, args, st, line):
        a = lambda i: args[i] if i < len(args) else V.const(None)  # noqa: E731
        if name in API_LUA_ONLY:
            self.notes['lua-only'].add((name, line))
            for x in args:
                self.use(x, 'other', f'passed to {name}()', line)
            st.luaonly = True
            if name == 'NextRelation':
                return [V.top(), V.top()]
            return [V.top()]
        if name == 'Find':
            k = a(0)
            self.use(k, 'other', 'used as a tag key in Find()', line)
            keys = k.strings()
            if keys is None:
                self.notes['dynamic-key'].add(('Find', line))
                return [V.top(nonnil=True, taints=frozenset([('val', '*')]))]
            return [V.top(nonnil=True, taints=frozenset(('val', key) for key in keys))]
        if name == 'Holds':
            self.use(a(0), 'other', 'used as a tag key in Holds()', line)
            return [V.top()]  # a boolean: may be false
        if name == 'IsClosed':
            return [V.top()]
        if name == 'MercatorArea':
            return [V.top(nonnil=True, taints=frozenset([('area',)]))]
        if name == 'NextRelation':
            self.notes['next-relation'].add(('', line))
            return [V.top(taints=frozenset([('rel',)])), V.top()]
        if name == 'FindInRelation':
            self.use(a(0), 'other', 'used as a tag key in FindInRelation()', line)
            ks = a(0).strings()
            self.relreads.update(ks or ['*'])
            self.notes['find-in-relation'].add((','.join(ks or ['*']), line))
            return [V.top(nonnil=True)]
        if name in ('Attribute', 'AttributeNumeric', 'AttributeInteger', 'AttributeBoolean'):
            key, val = a(0), a(1)
            self.use(key, 'other', f'used as an attribute name in {name}()', line)
            for x in args[2:]:
                self.use(x, 'other', f'passed as minzoom to {name}()', line)
            attrs = key.strings()
            for at in attrs or ['*']:
                srcs = {t for t in val.taints if (t[0] == 'val' and name == 'Attribute')
                        or (t[0] == 'len' and name in ('AttributeNumeric', 'AttributeInteger'))}
                if len(srcs) == 1 and not val.is_known():
                    self.attr_sources[at].add(next(iter(srcs)))
                else:
                    self.attr_sources[at].add(('other', line))
            for t in val.taints:
                if t[0] == 'val' and name == 'Attribute' and attrs is not None:
                    for at in attrs:
                        self.ev[t].add(('copy', at, line))
                elif t[0] == 'len' and name in ('AttributeNumeric', 'AttributeInteger') and attrs is not None:
                    for at in attrs:
                        self.ev[t].add(('copylen', at, line))
                else:
                    self.ev[t].add(('other', f'passed to {name}()' + ('' if attrs is not None else
                                    ' with an attribute name that is not a constant'), line))
            return []
        if name in API_REPLAYED:
            for x in args:
                self.use(x, 'other', f'passed to {name}()', line)
            return []
        return [V.top()]

    def call_builtin(self, name, args, st, line):
        if name in ('string.len',) and args:
            return [self.length_of(args[0], line)]
        if name in TABLE_MUTATORS and args:
            t = args[0]
            if t.table is not None:
                self.mutations[('T', t.tid)].append((self.entry, line, st.luaonly))
            elif t.ltab is not None:
                for x in args[1:]:
                    t.ltab.taints |= set(x.taints)
                return [V.top()]
        for x in args:
            if x.table is not None and name not in TABLE_MUTATORS:
                self.table_reads[('T', x.tid)].append((self.entry, line, st.luaonly))
        if name in IMPURE_BUILTINS or name.startswith(('os.', 'io.', 'debug.', 'coroutine.')):
            self.notes['side-effect'].add((name, line))
        for x in args:
            self.use(x, 'other', f'passed to {name}()', line)
        pure = name in PURE_BUILTINS or (name.startswith(PURE_BUILTIN_PREFIXES) and name not in IMPURE_BUILTINS)
        if pure and name not in ('ipairs', 'pairs', 'next') and all(x.single() for x in args) \
                and not any(x.taints for x in args):
            try:
                f = self.L.eval(name)
                ok_and = self.G.ml_call(f, *[const_py(x.single()) for x in args])
                res = ok_and if isinstance(ok_and, tuple) else (ok_and,)
                if res and res[0] is True:
                    out = [self.from_lua(x) for x in res[1:]]
                    return out or [V.const(None)]
            except Exception:
                pass
        if name == 'tonumber' or name.startswith('math.'):
            return [V.top()]
        return [V.top(nonnil=name in ('tostring', 'type', 'string.format', 'string.lower', 'string.upper',
                                      'string.sub', 'string.rep', 'string.gsub'))]

    # ---- the whole analysis
    def analyse(self):
        per_kind = {}
        for entry, kind in MEMO_ENTRIES.items():
            self.reset_events()
            if not self.run_entry(entry):
                continue
            per_kind[kind] = {'events': self.ev, 'notes': self.notes, 'relreads': self.relreads,
                              'attr_sources': self.attr_sources}
        for entry in OTHER_ENTRIES:
            self.reset_events()
            self.run_entry(entry)
        return per_kind


# ---------------------------------------------------------------- conclusions

def conclude(an, per_kind):
    G = an.G
    report = {'profile': an.path, 'state': [], 'kinds': {}}

    # State carried from one object to the next
    for pn, reads in an.unsafe_reads.items():
        writes = an.writes.get(pn)
        if not writes:
            continue
        rd = [r for r in reads if r[0] in MEMO_ENTRIES and not r[2]]
        wr = [w for w in writes if w[0] in MEMO_ENTRIES and not w[2]]
        if rd or wr:
            report['state'].append({
                'name': pn[1], 'what': 'module local' if pn[0] == 'U' else 'global',
                'read_before_written': sorted({(r[0], r[1]) for r in reads}, key=str)[:5],
                'written': sorted({(w[0], w[1]) for w in writes}, key=str)[:5]})
    for tk, muts in an.mutations.items():
        reads = an.table_reads.get(tk, [])
        rd = [r for r in reads if r[0] in MEMO_ENTRIES and not r[2]]
        wr = [m for m in muts if m[0] in MEMO_ENTRIES and not m[2]]
        if rd or wr:
            report['state'].append({
                'name': f'table #{tk[1]}', 'what': 'table',
                'read_before_written': sorted({(r[0], r[1]) for r in reads}, key=str)[:5],
                'written': sorted({(m[0], m[1]) for m in muts}, key=str)[:5]})

    for kind, data in per_kind.items():
        ev, notes = data['events'], data['notes']
        prefix = f'{kind}_memo'

        def declared(name):
            v = G[name]
            return v

        # values
        keys = sorted({t[1] for t in ev if t[0] == 'val'})
        dynamic_other = [d for d in ev.get(('val', '*'), ()) if d[0] == 'other']
        values = {}
        rejected = {}
        unhelpful = {}
        for k in keys:
            if k == '*':
                continue
            uses = ev[('val', k)]
            lens = ev.get(('len', k), set())
            other = sorted([u for u in uses | lens if u[0] == 'other'], key=lambda u: (u[2] or 0))
            copies = sorted({u[1] for u in uses if u[0] == 'copy'})
            lcopies = sorted({u[1] for u in lens if u[0] == 'copylen'})
            if other:
                if copies:
                    rejected[k] = [{'line': u[2], 'why': u[1]} for u in other[:3]]
                continue
            if dynamic_other:
                rejected[k] = [{'line': u[2], 'why': 'Find() with a key memo-lint could not work out, '
                                f'whose value is {u[1]}'} for u in dynamic_other[:3]]
                continue
            if copies or lcopies:
                shared = [(at, src) for at in copies for src in data['attr_sources'].get(at, ()) if src != ('val', k)] \
                    + [(at, src) for at in lcopies for src in data['attr_sources'].get(at, ()) if src != ('len', k)]
                if shared:
                    at, src = shared[0]
                    why = (f'"{at}" is also set from "{src[1]}"' if src[0] in ('val', 'len')
                           else f'"{at}" is also set to something else (line {src[1]})')
                    unhelpful[k] = why + ': tilemaker would send those objects through Lua'
                    continue
                values[k] = {'copies': copies, 'lengths': lcopies}

        # area
        area_ev = ev.get(('area',), set())
        area_other = sorted([u for u in area_ev if u[0] == 'other'], key=lambda u: (u[2] or 0))
        thresholds = sorted({u[1] for u in area_ev if u[0] == 'cmp'})
        # relations
        rel_ev = ev.get(('rel',), set())
        rel_other = sorted([u for u in rel_ev if u[0] == 'other'], key=lambda u: (u[2] or 0))
        uses_relations = bool(notes.get('next-relation')) or bool(data['relreads'])

        # what the profile declares
        decl_on = bool(declared(prefix))
        decl_values = {}
        dv = declared(prefix + '_values')
        if dv is not None and lupa_mod.lua_type(dv) == 'table':
            for k, lst in dv.items():
                attrs, lens = [], []
                for kk, vv in lst.items():
                    if isinstance(kk, (int, float)):
                        attrs.append(vv)
                    elif vv == 'length':
                        lens.append(kk)
                decl_values[k] = {'copies': sorted(attrs), 'lengths': sorted(lens)}
        da = declared(prefix + '_area')
        decl_area = sorted(float(x) for x in da.values()) if da is not None and lupa_mod.lua_type(da) == 'table' else []
        decl_rel = bool(declared(prefix + '_relations'))

        findings = []
        if decl_on and report['state']:
            findings.append(('error', f'{prefix} = true, but state is carried from one object to the next (see above)'))
        for k, d in decl_values.items():
            if k in rejected:
                r = rejected[k][0]
                findings.append(('error', f'{prefix}_values declares "{k}", but line {r["line"]}: its value is {r["why"]}'))
            elif k in unhelpful:
                findings.append(('warning', f'{prefix}_values declares "{k}", which is safe but slows it down: {unhelpful[k]}'))
            elif k not in values:
                if ('val', k) in ev:
                    others = sorted([u for u in ev[('val', k)] if u[0] == 'other'], key=lambda u: (u[2] or 0))
                    if others:
                        findings.append(('error', f'{prefix}_values declares "{k}", but line {others[0][2]}: its value is {others[0][1]}'))
                    else:
                        findings.append(('warning', f'{prefix}_values declares "{k}", which is only tested, never copied'))
                else:
                    findings.append(('warning', f'{prefix}_values declares "{k}", which is never read with Find()'))
            else:
                missing = sorted(set(values[k]['copies']) - set(d['copies']))
                missing_l = sorted(set(values[k]['lengths']) - set(d['lengths']))
                if missing or missing_l:
                    findings.append(('warning', f'{prefix}_values["{k}"] is copied into attributes it does not list: '
                                     f'{", ".join(missing + [m + " (length)" for m in missing_l])} '
                                     '(objects that do this are not replayed)'))
        for k in values:
            if k not in decl_values:
                findings.append(('suggestion', f'"{k}" could be declared in {prefix}_values'))
        if decl_area:
            if area_other:
                u = area_other[0]
                findings.append(('error', f'{prefix}_area is declared, but line {u[2]}: MercatorArea() is {u[1]}'))
            missing = [t for t in thresholds if t not in decl_area]
            if missing:
                findings.append(('error', f'{prefix}_area misses thresholds MercatorArea() is compared with: '
                                 f'{", ".join(f"{t:g}" for t in missing)}'))
        elif thresholds and not area_other:
            findings.append(('suggestion', f'{prefix}_area could be declared (MercatorArea() is only compared with constants)'))
        if decl_rel and rel_other:
            u = rel_other[0]
            findings.append(('error', f'{prefix}_relations = true, but line {u[2]}: the relation from NextRelation() is {u[1]}'))
        if not decl_rel and uses_relations and not rel_other:
            findings.append(('suggestion', f'{prefix}_relations = true could be declared'))
        side = sorted(notes.get('side-effect', ()), key=str)
        if side:
            where = ', '.join(f'{n} (line {ln})' for n, ln in side[:4])
            findings.append(('error' if decl_on else 'warning',
                             f'{kind}_function has effects or reads that a replay skips or can\'t repeat: {where}'))
        unproven = sorted(notes.get('unsupported', ()), key=str) + sorted(notes.get('unanalysed-call', ()), key=str) \
            + sorted(notes.get('unanalysed-table-write', ()), key=str)
        if unproven:
            where = ', '.join(f'{n or "table write"} (line {ln})' for n, ln in unproven[:4])
            findings.append(('warning', f'memo-lint could not follow all of {kind}_function, so it proves nothing '
                             f'about these parts: {where}'))
        if not decl_on and not report['state'] and not side and not unproven:
            findings.append(('suggestion', f'{prefix} = true could be declared'))

        rec = []
        if not report['state'] and not side and not unproven:
            rec.append(f'{prefix} = true')
        if values:
            parts = []
            for k, d in values.items():
                items = [json.dumps(c) for c in d['copies']] + [f'{lattr} = "length"' if lattr.isidentifier()
                                                                 else f'[{json.dumps(lattr)}] = "length"'
                                                                 for lattr in d['lengths']]
                kk = k if k.isidentifier() else f'[{json.dumps(k)}]'
                parts.append(f'{kk} = {{ {", ".join(items)} }}')
            rec.append(f'{prefix}_values = {{ {", ".join(parts)} }}')
        if thresholds and not area_other:
            rec.append(f'{prefix}_area = {{ {", ".join(f"{t!r}" for t in thresholds)} }}')
        if uses_relations and not rel_other:
            rec.append(f'{prefix}_relations = true')

        report['kinds'][kind] = {
            'values': values, 'rejected_values': rejected, 'unhelpful_values': unhelpful,
            'area_thresholds': thresholds, 'area_rejected': [{'line': u[2], 'why': u[1]} for u in area_other[:3]],
            'relations_ok': uses_relations and not rel_other,
            'relations_rejected': [{'line': u[2], 'why': u[1]} for u in rel_other[:3]],
            'lua_only': sorted({n[0] for n in notes.get('lua-only', ())}),
            'side_effects': sorted(notes.get('side-effect', ()), key=str),
            'dynamic_keys': sorted({n[1] for n in notes.get('dynamic-key', ())}, key=lambda x: x or 0),
            'unsupported': sorted(notes.get('unsupported', ()), key=str)
                           + sorted(notes.get('unanalysed-call', ()), key=str),
            'declared': {'on': decl_on, 'values': decl_values, 'area': decl_area, 'relations': decl_rel},
            'findings': findings, 'recommended': rec}
    return report


def print_report(rep, out=sys.stdout):
    p = lambda *a: print(*a, file=out)  # noqa: E731
    p(f'memo-lint: {rep["profile"]}')
    if rep['state']:
        p('\nState carried from one object to the next (the memo would skip it):')
        for s in rep['state']:
            rb = ', '.join(f'{e} line {ln}' for e, ln in s['read_before_written'])
            wr = ', '.join(f'{e} line {ln}' for e, ln in s['written'])
            p(f'  {s["what"]} {s["name"]}: written at {wr}; read before written at {rb}')
    for kind, k in rep['kinds'].items():
        p(f'\n== {kind}_function ==')
        for sev, msg in k['findings']:
            p(f'  {sev.upper():10} {msg}')
        if k['rejected_values']:
            p('  Tags copied into attributes that can\'t be memo values:')
            for tag, rs in k['rejected_values'].items():
                p(f'    {tag:20} line {rs[0]["line"]}: {rs[0]["why"]}')
        if k['lua_only']:
            p(f'  Paths that call {", ".join(k["lua_only"])} always run through Lua.')
        if k['side_effects']:
            p('  Side effects the memo would skip: ' + ', '.join(f'{n} (line {ln})' for n, ln in k['side_effects']))
        if k['dynamic_keys']:
            p('  Find() with a key memo-lint could not work out at lines ' + ', '.join(map(str, k['dynamic_keys'])))
        if k['unsupported']:
            p('  Not analysed: ' + ', '.join(f'{n} (line {ln})' for n, ln in k['unsupported'][:8]))
        p('  Recommended declarations:')
        for r in k['recommended']:
            p(f'    {r}')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('profile')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args(argv)
    an = Analyzer(args.profile)
    per_kind = an.analyse()
    rep = conclude(an, per_kind)
    if args.json:
        json.dump(rep, sys.stdout, indent=2, default=str)
        print()
    else:
        print_report(rep)
    errors = sum(1 for k in rep['kinds'].values() for f in k['findings'] if f[0] == 'error')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
