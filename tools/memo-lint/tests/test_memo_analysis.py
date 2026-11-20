"""Tests for tilemaker's built-in memo analysis (src/memo_analysis, run with run_analysis.lua).

Each profile breaks (or keeps) one promise of the memo; the analysis must never enable what isn't safe.
Run: python3 -m pytest tools/memo-lint/tests/test_memo_analysis.py -q   (LUA=lua5.1|luajit to switch)
"""
import json
import os
import subprocess
import tempfile
import textwrap

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
RUNNER = os.path.join(HERE, '..', 'run_analysis.lua')
SRC = os.path.join(ROOT, 'src', 'memo_analysis')
LUAS = [os.environ['LUA']] if os.environ.get('LUA') else ['lua5.4']


def analyse_file(path, lua=None):
    out = subprocess.run([lua or LUAS[0], RUNNER, SRC, path], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def analyse(src, lua=None):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'profile.lua')
        with open(path, 'w') as f:
            f.write(textwrap.dedent(src))
        return analyse_file(path, lua)


def way(rep):
    return rep['kinds']['way']


def values(rep, kind='way'):
    return rep['kinds'][kind]['values']


# ---------------------------------------------------------------- state

def test_pure_profile():
    rep = analyse('''
        function way_function()
          local hw = Find("highway")
          if hw == "" then return end
          Layer("roads", false)
          Attribute("class", hw)
          local name = Find("name")
          if name ~= "" then Attribute("name", name) end
        end
    ''')
    assert way(rep)['enabled']
    assert values(rep) == {'highway': ['class'], 'name': ['name']}


@pytest.mark.parametrize('src', [
    # a global counter
    'count = 0\nfunction way_function() count = count + 1; if count % 100 == 0 then Layer("s", false) end end',
    # a module local counter
    'local seen = 0\nfunction way_function() seen = seen + 1; if seen > 10 then return end; Layer("f", false) end',
    # a global written on one path only
    'function way_function() if Find("highway") == "motorway" then last = 1 end; if last == 1 then Layer("r", false) end end',
    # a cache table
    'cache = {}\nfunction way_function() local h = Find("highway"); if cache[h] then return end; cache[h] = true; Layer("x", false) end',
    # a table filled by relation_scan_function, read without Id()
    'routes = {}\nfunction relation_scan_function() routes[Id()] = Find("network"); Accept() end\n'
    'function way_function() if next(routes) ~= nil then Layer("r", false) end end',
    # table.insert into a module table
    'log = {}\nfunction way_function() table.insert(log, Find("highway")); if #log > 3 then Layer("r", false) end end',
    # Find wrapped in init_function, with a counter
    'n = 0\nfunction init_function() local f = Find; Find = function(k) n = n + 1; return f(k) end end\n'
    'function way_function() if Find("highway") ~= "" and n > 5 then Layer("r", false) end end',
    # a standard function replaced by one with state
    'local calls = 0\nlocal lower = string.lower\nstring.lower = function(s) calls = calls + 1; return lower(s) end\n'
    'function way_function() local h = string.lower(Find("highway")); if calls > 2 then Layer("r", false) end end',
])
def test_state_disables_the_memo(src):
    rep = analyse(src)
    assert not way(rep)['enabled'], way(rep)


def test_scratch_global_written_before_read():
    rep = analyse('''
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
    assert way(rep)['enabled'], way(rep)['reasons']


def test_relation_scan_table_read_through_id():
    rep = analyse('''
        routes = {}
        function relation_scan_function() routes[Id()] = Find("network"); Accept() end
        function way_function()
          local hw = Find("highway")
          if hw == "" then return end
          local r = routes[Id()]
          Layer("roads", false)
        end
    ''')
    assert way(rep)['enabled'], way(rep)['reasons']


def test_two_module_locals_with_one_name():
    # x in the first block is scratch; the x way_function reads after it is another local, with state
    rep = analyse('''
        local helper
        do
          local x
          helper = function() x = Find("highway"); return x end
        end
        local x = 0
        function way_function()
          helper()
          x = x + 1
          if x > 3 then Layer("r", false) end
        end
    ''')
    assert not way(rep)['enabled']


@pytest.mark.parametrize('src', [
    'function way_function() if math.random() < 0.5 then Layer("s", false) end end',
    'function way_function() print("way"); Layer("s", false) end',
    'function way_function() local co = coroutine.create(function() end); Layer("s", false) end',
    'function way_function() local f = function(x) return x end; Layer("r", false) end',
    'function way_function() local k = "high" .. "way"; if _G[Find("x")] then Layer("r", false) end end',
    'cfg = setmetatable({}, { __index = function(t, k) return k end })\n'
    'function way_function() if cfg[Find("highway")] == "x" then Layer("r", false) end end',
    'function way_function() goto skip; Layer("r", false); ::skip:: end',
])
def test_effects_and_unfollowable_code_disable_the_memo(src):
    if 'goto' in src and LUAS[0] == 'lua5.1':
        pytest.skip('Lua 5.1 has no goto')
    rep = analyse(src)
    assert not way(rep)['enabled'], way(rep)


def test_lua_only_functions_are_fine():
    rep = analyse('function way_function() if Find("building") ~= "" and Area() > 100 then Layer("b", true) end end')
    assert way(rep)['enabled'] and way(rep)['lua_only'] == ['Area']


# ---------------------------------------------------------------- value tags

def name_profile(body):
    return 'function way_function()\n  Layer("roads", false)\n  local name = Find("name")\n' + body + '\nend\n'


@pytest.mark.parametrize('body', [
    'if name == "Main Street" then MinZoom(5) end\nAttribute("name", name)',
    'Attribute("name", name:upper())',
    'Attribute("name", ("%s!"):format(name))',
    'Attribute("name", name .. " (" .. Find("ref") .. ")")',
    'Attribute("name", name); AttributeInteger("w", #name * 2)',
    'Attribute("name", name); if name == Find("name:en") then MinZoom(3) end',
    'Attribute("name", name); if tonumber(name) then MinZoom(3) end',
])
def test_value_used_beyond_copy_is_not_a_value_tag(body):
    rep = analyse(name_profile(body))
    assert 'name' not in values(rep), values(rep)


def test_value_as_table_key():
    rep = analyse('''
        famous = { ["Broadway"] = true }
        function way_function()
          local name = Find("name")
          if famous[name] then MinZoom(5) end
          Layer("roads", false)
          Attribute("name", name)
        end
    ''')
    assert 'name' not in values(rep)


def test_value_through_helper():
    rep = analyse('''
        function short(n) return #n < 10 end
        function way_function()
          Layer("roads", false)
          if short(Find("name")) then Attribute("name", Find("name")) end
        end
    ''')
    assert 'name' not in values(rep)


def test_value_rotated_through_a_loop():
    # the tag value reaches `a` only after three passes: the loop must be run to a fixpoint
    rep = analyse('''
        function way_function()
          Layer("roads", false)
          Attribute("name", Find("name"))
          local a, b, c = "", "", Find("name")
          for i = 1, tonumber(Find("n")) or 0 do a, b, c = b, c, a end
          if a == "x" then MinZoom(3) end
        end
    ''')
    assert 'name' not in values(rep)


def test_value_length():
    rep = analyse(name_profile('if name ~= "" then Attribute("name", name); AttributeInteger("name_len", #name) end'))
    assert values(rep) == {'name': ['name']} and way(rep)['lengths'] == {'name_len': 'name'}


def test_value_copied_into_two_attributes():
    rep = analyse(name_profile('Attribute("name", name); Attribute("label", name)'))
    assert values(rep) == {'name': ['label', 'name']}


def test_dynamic_key_through_config_table():
    rep = analyse('''
        langs = { "name", "name:en" }
        function way_function()
          Layer("roads", false)
          Attribute("name", Find("name"))
          for _, k in ipairs(langs) do
            if Find(k) == "x" then MinZoom(3) end
          end
        end
    ''')
    assert 'name' not in values(rep)


def test_dead_code_under_configuration():
    rep = analyse('''
        compare_names = false
        function way_function()
          Layer("roads", false)
          local name = Find("name")
          if compare_names and name == Find("name:en") then return end
          Attribute("name", name)
        end
    ''')
    assert values(rep) == {'name': ['name']}


def test_shared_attribute_is_not_a_value_tag():
    rep = analyse('''
        function way_function()
          local l = Find("natural")
          if l == "wetland" then Layer("landcover", true); Attribute("subclass", Find("wetland"))
          elseif l == "wood" then Layer("landcover", true); Attribute("subclass", "forest") end
        end
    ''')
    assert 'wetland' not in values(rep) and way(rep)['unhelpful'] == {'wetland': 'subclass'}


# ---------------------------------------------------------------- area, relations

def test_area_thresholds():
    rep = analyse('''
        SMALL = 1000
        function way_function()
          Layer("landuse", true)
          if MercatorArea() < SMALL then MinZoom(14) elseif MercatorArea() >= SMALL * 5 then MinZoom(8) end
        end
    ''')
    assert way(rep)['area'] == [1000, 5000]


def test_area_used_as_number():
    rep = analyse('function way_function() Layer("l", true); MinZoom(math.floor(14 - math.log(MercatorArea(), 4))) end')
    assert way(rep)['area'] == [] and way(rep)['area_rejected']


def test_relation_tested_only():
    rep = analyse('''
        function way_function()
          while true do
            local rel, role = NextRelation()
            if not rel then break end
            Layer("routes", false)
            Attribute("network", FindInRelation("network"))
          end
        end
    ''')
    assert way(rep)['enabled'] and way(rep)['relations']


def test_relation_id_used():
    rep = analyse('''
        function way_function()
          while true do
            local rel = NextRelation()
            if not rel then break end
            Layer("routes", false)
            Attribute("rel_id", rel)
          end
        end
    ''')
    assert not way(rep)['relations']


# ---------------------------------------------------------------- real profiles

def test_openmaptiles():
    rep = analyse_file(os.path.join(ROOT, 'resources', 'process-openmaptiles.lua'))
    w, n = rep['kinds']['way'], rep['kinds']['node']
    assert rep['state'] == [] and w['enabled'] and n['enabled']
    # what the profile declares by hand is derived
    assert w['values']['name'] == ['name:latin'] and w['values']['addr:housenumber'] == ['housenumber']
    assert w['values']['ref'] == ['ref'] and w['lengths'] == {'ref_length': 'ref'}
    assert w['area'] == [611.5 ** 2] and w['relations']
    assert n['values']['name'] == ['name:latin'] and n['values']['addr:housenumber'] == ['housenumber']
    # and nothing that sends objects through Lua
    assert not {'information', 'religion', 'wetland'} & set(w['values'])


def test_openmaptiles_with_additional_languages():
    with open(os.path.join(ROOT, 'resources', 'process-openmaptiles.lua')) as f:
        src = f.read().replace('additional_languages = { }', 'additional_languages = { "de", "fr" }')
    rep = analyse(src)
    assert 'name' not in values(rep) and 'name' not in values(rep, 'node')


@pytest.mark.parametrize('lua', ['lua5.1', 'luajit'])
def test_same_result_on_other_luas(lua):
    path = os.path.join(ROOT, 'resources', 'process-openmaptiles.lua')
    a, b = analyse_file(path), analyse_file(path, lua)
    a.pop('seconds'); b.pop('seconds')
    assert a == b


def test_embedded_header_is_up_to_date(tmp_path):
    out = tmp_path / 'memo_analysis_lua.h'
    subprocess.run([LUAS[0], os.path.join(SRC, 'embed.lua'), SRC, str(out)], check=True, capture_output=True)
    with open(os.path.join(ROOT, 'include', 'memo_analysis_lua.h'), 'rb') as f:
        assert f.read() == out.read_bytes(), 'run: lua src/memo_analysis/embed.lua src/memo_analysis include/memo_analysis_lua.h'
