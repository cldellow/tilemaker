"""Tests for memo-lint: each profile breaks (or keeps) one promise of way_memo.

Run: python3 -m pytest tools/memo-lint/tests -q
"""
import os
import sys
import tempfile
import textwrap

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import memo_lint  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', '..')


def lint(src):
    with tempfile.NamedTemporaryFile('w', suffix='.lua', delete=False) as f:
        f.write(textwrap.dedent(src))
        path = f.name
    try:
        an = memo_lint.Analyzer(path)
        return memo_lint.conclude(an, an.analyse())
    finally:
        os.unlink(path)


def findings(rep, kind='way', sev=None):
    return [m for s, m in rep['kinds'][kind]['findings'] if sev is None or s == sev]


def errors(rep, kind='way'):
    return findings(rep, kind, 'error')


# ---------------------------------------------------------------- way_memo: state

def test_pure_profile_is_clean():
    rep = lint('''
        way_memo = true
        way_memo_values = { name = { "name" } }
        function way_function()
          local hw = Find("highway")
          if hw == "" then return end
          Layer("roads", false)
          Attribute("class", hw)
          local name = Find("name")
          if name ~= "" then Attribute("name", name) end
        end
    ''')
    assert rep['state'] == []
    assert errors(rep) == []
    # highway, too, is only tested for "" and copied: it could be a memo value
    assert rep['kinds']['way']['values'] == {'highway': {'copies': ['class'], 'lengths': []},
                                             'name': {'copies': ['name'], 'lengths': []}}


def test_global_counter_is_state():
    rep = lint('''
        way_memo = true
        count = 0
        function way_function()
          count = count + 1
          if count % 100 == 0 then Layer("sample", false) end
        end
    ''')
    assert [s['name'] for s in rep['state']] == ['count']
    assert any('state is carried' in e for e in errors(rep))


def test_module_local_counter_is_state():
    rep = lint('''
        way_memo = true
        local seen = 0
        function way_function()
          seen = seen + 1
          if seen > 10 then return end
          Layer("first", false)
        end
    ''')
    assert [s['name'] for s in rep['state']] == ['seen']


def test_scratch_global_written_before_read_is_fine():
    # like split() in the OpenMapTiles profile: i = 1 before every use
    rep = lint('''
        way_memo = true
        function count_parts(s)
          local t = {} ; i = 1
          for str in string.gmatch(s, "[^;]+") do t[i] = str; i = i + 1 end
          return t
        end
        function way_function()
          local parts = count_parts(Find("surface"))
          Layer("x", false)
        end
    ''')
    assert rep['state'] == []
    assert errors(rep) == []


def test_global_written_on_one_path_only_is_state():
    rep = lint('''
        way_memo = true
        function way_function()
          if Find("highway") == "motorway" then last_class = "motorway" end
          if last_class == "motorway" then Layer("roads", false) end
        end
    ''')
    assert [s['name'] for s in rep['state']] == ['last_class']


def test_cache_table_is_state():
    rep = lint('''
        way_memo = true
        cache = {}
        function way_function()
          local hw = Find("highway")
          if cache[hw] then return end
          cache[hw] = true
          Layer("first_of_each", false)
        end
    ''')
    assert any(s['what'] == 'table' for s in rep['state'])


def test_relation_scan_table_read_through_id_is_fine():
    rep = lint('''
        way_memo = true
        routes = {}
        function relation_scan_function()
          routes[Id()] = Find("network")
          Accept()
        end
        function way_function()
          local hw = Find("highway")
          if hw == "" then return end
          local r = routes[Id()]
          Layer("roads", false)
        end
    ''')
    assert rep['state'] == []


def test_relation_scan_table_read_without_id_is_state():
    rep = lint('''
        way_memo = true
        routes = {}
        function relation_scan_function()
          routes[Id()] = Find("network")
          Accept()
        end
        function way_function()
          if next(routes) ~= nil then Layer("roads", false) end
        end
    ''')
    assert any(s['what'] == 'table' for s in rep['state'])


def test_side_effects_are_reported():
    rep = lint('''
        way_memo = true
        function way_function()
          if math.random() < 0.5 then Layer("sample", false) end
          print("way")
        end
    ''')
    names = {n for n, _ in rep['kinds']['way']['side_effects']}
    assert names == {'math.random', 'print'}


def test_lua_only_functions_are_safe():
    rep = lint('''
        way_memo = true
        function way_function()
          if Find("building") ~= "" and Area() > 100 then Layer("building", true) end
        end
    ''')
    assert errors(rep) == []
    assert rep['kinds']['way']['lua_only'] == ['Area']


# ---------------------------------------------------------------- way_memo_values

def name_profile(body, decl='way_memo_values = { name = { "name" } }'):
    return f'''
        way_memo = true
        {decl}
        function way_function()
          Layer("roads", false)
          local name = Find("name")
{textwrap.indent(textwrap.dedent(body), "          ")}
        end
    '''


def test_value_compared_with_constant():
    rep = lint(name_profile('if name == "Main Street" then MinZoom(5) end\nAttribute("name", name)'))
    assert any('"name"' in e and "compared with 'Main Street'" in e for e in errors(rep))


def test_value_transformed():
    rep = lint(name_profile('Attribute("name", name:upper())'))
    assert any('passed to string.upper()' in e for e in errors(rep))


def test_value_concatenated():
    rep = lint(name_profile('Attribute("name", name .. " (" .. Find("ref") .. ")")'))
    assert any('concatenated' in e for e in errors(rep))


def test_value_as_table_key():
    rep = lint('''
        way_memo = true
        way_memo_values = { name = { "name" } }
        famous = { ["Broadway"] = true }
        function way_function()
          local name = Find("name")
          if famous[name] then MinZoom(5) end
          Layer("roads", false)
          Attribute("name", name)
        end
    ''')
    assert any('used as a table key' in e for e in errors(rep))


def test_value_through_helper_and_table():
    rep = lint('''
        way_memo = true
        way_memo_values = { name = { "name" } }
        function short(n) return #n < 10 end
        function way_function()
          local t = { n = Find("name") }
          Layer("roads", false)
          if short(Find("name")) then Attribute("name", Find("name")) end
        end
    ''')
    assert any('compared with <, >' in e for e in errors(rep))


def test_value_length_declared():
    rep = lint(name_profile('if name ~= "" then Attribute("name", name); AttributeInteger("name_len", #name) end',
                            decl='way_memo_values = { name = { "name", name_len = "length" } }'))
    assert errors(rep) == []
    assert rep['kinds']['way']['values']['name'] == {'copies': ['name'], 'lengths': ['name_len']}


def test_value_length_used_in_arithmetic():
    rep = lint(name_profile('Attribute("name", name); AttributeInteger("w", #name * 2)'))
    assert any('used in arithmetic' in e for e in errors(rep))


def test_value_copied_into_undeclared_attribute():
    rep = lint(name_profile('Attribute("name", name); Attribute("label", name)'))
    assert any('label' in w for w in findings(rep, sev='warning'))


def test_dynamic_key_resolved_through_config_table():
    rep = lint('''
        way_memo = true
        way_memo_values = { name = { "name" } }
        langs = { "name", "name:en" }
        function way_function()
          Layer("roads", false)
          for _, k in ipairs(langs) do
            local v = Find(k)
            if v == "x" then MinZoom(3) end
          end
        end
    ''')
    assert any("compared with 'x'" in e for e in errors(rep))


def test_dead_code_under_configuration_is_ignored():
    rep = lint('''
        way_memo = true
        way_memo_values = { name = { "name" } }
        compare_names = false
        function way_function()
          Layer("roads", false)
          local name = Find("name")
          if compare_names and name == Find("name:en") then return end
          Attribute("name", name)
        end
    ''')
    assert errors(rep) == []


# ---------------------------------------------------------------- way_memo_area

def test_area_threshold_inferred_and_checked():
    src = '''
        way_memo = true
        {decl}
        SMALL = 1000
        function way_function()
          Layer("landuse", true)
          if MercatorArea() < SMALL then MinZoom(14) elseif MercatorArea() >= SMALL * 5 then MinZoom(8) end
        end
    '''
    ok = lint(src.format(decl='way_memo_area = { 1000, 5000 }'))
    assert errors(ok) == []
    assert ok['kinds']['way']['area_thresholds'] == [1000.0, 5000.0]
    missing = lint(src.format(decl='way_memo_area = { 1000 }'))
    assert any('misses thresholds' in e and '5000' in e for e in errors(missing))


def test_area_used_as_number():
    rep = lint('''
        way_memo = true
        way_memo_area = { 1000 }
        function way_function()
          Layer("landuse", true)
          MinZoom(math.floor(14 - math.log(MercatorArea(), 4)))
        end
    ''')
    assert any('MercatorArea()' in e for e in errors(rep))


# ---------------------------------------------------------------- way_memo_relations

def test_relation_id_tested_only():
    rep = lint('''
        way_memo = true
        way_memo_relations = true
        function way_function()
          while true do
            local rel, role = NextRelation()
            if not rel then break end
            Layer("routes", false)
            Attribute("network", FindInRelation("network"))
          end
        end
    ''')
    assert errors(rep) == []
    assert rep['kinds']['way']['relations_ok']


def test_relation_id_used():
    rep = lint('''
        way_memo = true
        way_memo_relations = true
        function way_function()
          while true do
            local rel = NextRelation()
            if not rel then break end
            Layer("routes", false)
            Attribute("rel_id", rel)
          end
        end
    ''')
    assert any('way_memo_relations' in e for e in errors(rep))


# ---------------------------------------------------------------- the bundled profile

def test_openmaptiles_profile():
    rep = memo_lint.conclude(*(lambda an: (an, an.analyse()))(
        memo_lint.Analyzer(os.path.join(ROOT, 'resources', 'process-openmaptiles.lua'))))
    assert rep['state'] == []
    way = rep['kinds']['way']
    assert errors(rep) == [] and errors(rep, 'node') == []
    # what the profile used to declare by hand (it now leaves it to tilemaker)
    assert way['values']['name'] == {'copies': ['name:latin'], 'lengths': []}
    assert way['values']['ref'] == {'copies': ['ref'], 'lengths': ['ref_length']}
    assert way['area_thresholds'] == [611.5 ** 2]
    assert way['relations_ok']
