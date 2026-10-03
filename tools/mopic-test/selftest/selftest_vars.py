"""Self-test of gamepilot's --var substitution (no game): unit tests, the CLI (--dry-run, unknown / missing vars,
--steps, a run whose window never appears) and every existing recipe unchanged without vars."""
import copy
import glob
import json
import locale
import os
import subprocess
import sys

TOOLS = r"C:\Users\zzong\source\repos\UEVR-jh\tools\mopic-test"
sys.path.insert(0, TOOLS)
import gamepilot  # noqa: E402

work = sys.argv[1]
os.makedirs(work, exist_ok=True)
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("ok    " if ok else "FAIL  ") + name + ("" if ok else f"  {detail}"))
    fails += 0 if ok else 1


# --- unit: resolve_vars (declared {name: default}, null = required)
declared = {"play_s": 60, "cp": "hud", "pos": None, "keys": "w a s d", "flag": True, "k": "1", "name": None,
            "txt": None, "cps": None}
given = {"play_s": "180", "pos": "[110, 305]", "name": "Guangzhi", "txt": "w a s d", "cps": '["hud", "loading"]'}
resolved, problems = gamepilot.resolve_vars({"vars": declared}, given)
check("resolve: no problems", problems == [], problems)
check("resolve: --var read as JSON", resolved["play_s"] == ("180", 180) and resolved["pos"] == ("[110, 305]", [110, 305])
      and resolved["cps"][1] == ["hud", "loading"], resolved)
check("resolve: --var text that isn't JSON stays text", resolved["name"] == ("Guangzhi", "Guangzhi")
      and resolved["txt"] == ("w a s d", "w a s d"), resolved)
check("resolve: defaults taken as written", resolved["cp"] == ("hud", "hud") and resolved["flag"] == ("true", True)
      and resolved["k"] == ("1", "1"), resolved)
check("resolve: a required var without --var is not resolved", "nope" not in resolved)
r2, p2 = gamepilot.resolve_vars({"vars": {"a": 1}}, {"a": "2", "b": "3", "c": "4"})
check("resolve: undeclared --var is a problem", len(p2) == 1 and "b, c" in p2[0] and "declares no such var" in p2[0], p2)
r3, p3 = gamepilot.resolve_vars({"vars": ["a"]}, {})
check("resolve: \"vars\" that is not an object is a problem", r3 == {} and p3 and "must be an object" in p3[0], p3)
r5, p5 = gamepilot.resolve_vars({"vars": None}, {})
check("resolve: \"vars\": null is no vars (as the harness reads it)", r5 == {} and p5 == [], p5)
r4, p4 = gamepilot.resolve_vars({}, {"a": "1"})
check("resolve: a recipe without vars takes no --var", r4 == {} and p4 and "declares no such var" in p4[0], p4)

# --- unit: substitute_vars
steps = [
    {"play": "${play_s}", "keys": "${txt}", "every": 0.8},
    {"wait_for": "${cp}", "timeout": "${play_s}"},
    {"wait_for": ["${cp}", "loading"]},
    {"click": "${pos}"},
    {"click": ["${play_s}", 305]},
    {"key": "${k}"},
    {"note": "boss ${name} at ${play_s} s"},
    {"shot": "${play_s}"},
    {"seek": "slot_${k}", "press": "${k}"},
    {"wait": 2},
    {"if": "${flag}", "key": "esc"},
    {"wait_for": "${cps}", "timeout": 5},
    {"note": "literal ${HOME} and $${x} and ${ not a var } and ${undeclared_thing}"},
    {"note": "required but unset: ${unset_req}"},
    {"phase": "${play_s}"},
]
decl2 = dict(declared, unset_req=None)
unfilled = set()
before = copy.deepcopy(steps)
out = gamepilot.substitute_vars(steps, resolved, decl2, unfilled)
check("play number", out[0]["play"] == 180 and isinstance(out[0]["play"], int), out[0])
check("keys stay text", out[0]["keys"] == "w a s d", out[0])
check("checkpoint name, typed timeout", out[1]["wait_for"] == "hud" and out[1]["timeout"] == 180, out[1])
check("list element", out[2]["wait_for"] == ["hud", "loading"], out[2])
check("whole list value", out[3]["click"] == [110, 305], out[3])
check("number inside a list", out[4]["click"] == [180, 305], out[4])
check("key stays a string", out[5]["key"] == "1", out[5])
check("text in a sentence (the --var text)", out[6]["note"] == "boss Guangzhi at 180 s", out[6])
check("shot stays a string", out[7]["shot"] == "180", out[7])
check("partial in a checkpoint name, press is text", out[8]["seek"] == "slot_1" and out[8]["press"] == "1", out[8])
check("non-strings untouched", out[9] == {"wait": 2}, out[9])
check("json bool from a default", out[10]["if"] is True, out[10])
check("list var for wait_for", out[11]["wait_for"] == ["hud", "loading"], out[11])
check("undeclared ${...} stays as written", out[12]["note"] == steps[12]["note"], out[12])
check("declared var without a value: left as is, reported", out[13]["note"] == steps[13]["note"] and unfilled == {"unset_req"}, (out[13], unfilled))
check("phase is a text field", out[14]["phase"] == "180", out[14])
check("input steps not modified", steps == before)

# --- unit: recipe_steps_with_vars
lit = {"steps": [{"note": "cost ${HOME}"}, {"keys": "${x}"}, {"expect_exit": 5}]}
s, txt, prob, unf = gamepilot.recipe_steps_with_vars(lit, {})
check("no \"vars\": the recipe's own steps, untouched", s is lit["steps"] and s == [{"note": "cost ${HOME}"}, {"keys": "${x}"}, {"expect_exit": 5}]
      and txt == {} and prob == [] and all(not u for u in unf), (s, prob, unf))
s, txt, prob, unf = gamepilot.recipe_steps_with_vars(dict(lit, vars={}), {})
check("empty \"vars\": steps untouched", s == lit["steps"] and prob == [], (s, prob))
rec = {"vars": {"a": None, "b": 2}, "steps": [{"wait": "${b}"}, {"note": "${a}"}, {"keys": "${a} ${b}"}]}
s, txt, prob, unf = gamepilot.recipe_steps_with_vars(rec, {})
check("unfilled per step", [sorted(u) for u in unf] == [[], ["a"], ["a"]] and s[0] == {"wait": 2} and txt == {"b": "2"}, (unf, s, txt))
s, txt, prob, unf = gamepilot.recipe_steps_with_vars(rec, {"a": "q"})
check("filled: per-step sets empty, --var text in text fields", all(not u for u in unf) and s[2] == {"keys": "q 2"}, (unf, s))

# --- parse_vars
check("parse_vars", gamepilot.parse_vars(["a=1", "b=x=y", "c=", 'd=["h", "l"]']) == {"a": "1", "b": "x=y", "c": "", "d": '["h", "l"]'})
for bad in ["noequals", "1a=2", "=v", "a-b=1"]:
    try:
        gamepilot.parse_vars([bad])
        check(f"parse_vars rejects {bad!r}", False, "accepted")
    except SystemExit:
        check(f"parse_vars rejects {bad!r}", True)

# --- mouse tokens: malformed look: / wheel: are refused before any input is sent
for bad in ("look:1", "look:a,b", "look:1,2,3", "wheel:", "wheel:x"):
    try:
        gamepilot.press(bad, 10)
        check(f"press refuses {bad!r}", False, "accepted")
    except SystemExit:
        check(f"press refuses {bad!r}", True)

# --- every existing recipe: no vars, steps identical (also with a --var: refused, steps still untouched)
for path in sorted(glob.glob(os.path.join(TOOLS, "recipes", "*.json"))):
    recipe = gamepilot.load_recipe(path)
    orig = copy.deepcopy(recipe["steps"])
    if recipe.get("vars"):
        # a recipe that declares vars (Wukong-save): its defaults, plus a value for each var without one (null: the
        # ladder rung must give it, e.g. Stray-chapter's chapter_index), fill every ${name}; an undeclared --var is
        # refused
        required = {n: "1" for n, v in recipe["vars"].items() if v is None}
        s, txt, prob, unf = gamepilot.recipe_steps_with_vars(recipe, required)
        left = [n for n in recipe["vars"] if "${" + n + "}" in json.dumps(s)]
        check(f"recipe {os.path.basename(path)} (vars): defaults fill every var", prob == [] and not any(unf) and not left
              and len(s) == len(orig), (prob, left))
        s, txt, prob, unf = gamepilot.recipe_steps_with_vars(recipe, {**required, "mopicselftest_typo": "1"})
        check(f"recipe {os.path.basename(path)} (vars): an undeclared --var is refused", len(prob) == 1, prob)
        continue
    s, txt, prob, unf = gamepilot.recipe_steps_with_vars(recipe, {})
    check(f"recipe {os.path.basename(path)} unchanged", s == orig and prob == [] and not any(unf) and "vars" not in recipe, prob)
    s, txt, prob, unf = gamepilot.recipe_steps_with_vars(recipe, {"play_s": "1"})
    check(f"recipe {os.path.basename(path)}: an unrelated --var is refused, steps unchanged", s == orig and len(prob) == 1, prob)

# --- CLI on a tiny recipe
tiny = os.path.join(work, "tiny.json")
with open(tiny, "w", encoding="utf-8") as f:
    json.dump({"game": "tiny", "process": "mopicselftest_noproc", "source": "window", "window_timeout": 1,
               "vars": {"play_s": 60, "keys": None, "pos": None, "rung": "default rung"},
               "checkpoints": {}, "steps": [{"phase": "play"}, {"play": "${play_s}", "keys": "${keys}"},
                                            {"click": "${pos}"}, {"note": "rung ${rung} ${HOME}"}, {"expect_exit": 5}]}, f)
py = sys.executable
gp = os.path.join(TOOLS, "gamepilot.py")


# the child writes in the ANSI code page (cp949 here, as under the harness), not an inherited PYTHONIOENCODING
child_env = {k: v for k, v in os.environ.items() if k.upper() not in ("PYTHONIOENCODING", "PYTHONUTF8")}
child_enc = locale.getpreferredencoding(False)


def run(*argv, timeout=60):
    r = subprocess.run([py, gp, *argv], capture_output=True, timeout=timeout, env=child_env)
    r.stdout = r.stdout.decode(child_enc, errors="replace").replace("\r\n", "\n")
    r.stderr = r.stderr.decode(child_enc, errors="replace").replace("\r\n", "\n")
    return r


r = run("-", "run", tiny, "--dry-run", "--var", "play_s=180", "--var", "keys=w a s d", "--var", "pos=[110,305]",
        "--var", "rung=ch1 Guangzhi")
print("  dry-run output:\n    " + r.stdout.strip().replace("\n", "\n    "))
check("dry-run exit 0", r.returncode == 0, r.stderr)
check("dry-run substituted", '{"play": 180, "keys": "w a s d"}' in r.stdout and '{"click": [110, 305]}' in r.stdout
      and '{"note": "rung ch1 Guangzhi ${HOME}"}' in r.stdout, r.stdout)
r = run("-", "run", tiny, "--dry-run", "--var", "keys=w", "--var", "pos=[1,2]")
check("dry-run: defaults fill the rest", r.returncode == 0 and '{"play": 60, "keys": "w"}' in r.stdout
      and '{"note": "rung default rung ${HOME}"}' in r.stdout, r.stdout + r.stderr)
r = run("-", "run", tiny, "--dry-run", "--var", "play_s=180")
check("dry-run missing vars exit 1", r.returncode == 1 and '"missing": ["keys", "pos"]' in r.stdout, r.stdout + r.stderr)
r = run("-", "run", tiny, "--dry-run", "--var", "keys=w", "--var", "pos=1", "--var", "typo=1")
check("dry-run undeclared --var exit 1", r.returncode == 1 and "typo: the recipe declares no such var" in r.stdout, r.stdout + r.stderr)
r = run("-", "run", tiny, "--dry-run", "--var", "bad")
check("bad --var rejected", r.returncode != 0 and "--var expects name=value" in (r.stdout + r.stderr), r.stdout + r.stderr)
r = run("-", "run", tiny, "--dry-run", "--var", "keys=w", "--var", "pos=1", "--var", "rung=\u9ed1\u98ce\u5c71 \"q\"")
# U+98CE (simplified) is not in cp949: printed as \u98ce instead of failing the run
check("dry-run with Chinese / quotes in a var: no encoding error", r.returncode == 0
      and ("\u9ed1\u98ce\u5c71 \\\"q\\\"" in r.stdout or "\\u98ce" in r.stdout) and "Traceback" not in r.stderr, r.stdout + r.stderr)

status = os.path.join(work, "status-missing.json")
r = run("mopicselftest_noproc", "run", tiny, "--out", os.path.join(work, "pilot-missing"), "--status", status, "--var", "play_s=1")
st = json.load(open(status, encoding="utf-8"))
check("missing var fails the run before the window wait", r.returncode == 1 and st["state"] == "failed"
      and st["error_kind"] == "recipe" and "${keys}" in st["error"] and "${pos}" in st["error"], st)

status = os.path.join(work, "status-steps.json")
r = run("mopicselftest_noproc", "run", tiny, "--out", os.path.join(work, "pilot-steps"), "--status", status, "--steps", "4-5")
st = json.load(open(status, encoding="utf-8"))
check("--steps: vars of steps that don't run aren't needed", st["error_kind"] == "no_window", st)

status = os.path.join(work, "status-unknown.json")
r = run("mopicselftest_noproc", "run", tiny, "--out", os.path.join(work, "pilot-unknown"), "--status", status,
        "--var", "keys=w", "--var", "pos=1", "--var", "typo=1")
st = json.load(open(status, encoding="utf-8"))
check("undeclared --var fails the run before the window wait", st["error_kind"] == "recipe" and "typo" in st["error"], st)

status = os.path.join(work, "status-vars.json")
r = run("mopicselftest_noproc", "run", tiny, "--out", os.path.join(work, "pilot-vars"), "--status", status,
        "--var", "play_s=1", "--var", "keys=w", "--var", "pos=[1,2]", "--var", "rung=x")
st = json.load(open(status, encoding="utf-8"))
log = open(os.path.join(work, "pilot-vars", "pilot.log"), encoding="utf-8").read()
check("with all vars it gets to the window wait", st["error_kind"] == "no_window", st)
check("status records the vars", st.get("vars") == {"play_s": "1", "keys": "w", "pos": "[1,2]", "rung": "x"}, st.get("vars"))
check("pilot.log records the vars", 'vars: {"play_s": "1"' in log, log)

# a recipe without "vars" keeps a literal ${...} in a real run (no recipe error)
plain = os.path.join(work, "plain.json")
with open(plain, "w", encoding="utf-8") as f:
    json.dump({"game": "plain", "window_timeout": 1, "checkpoints": {}, "steps": [{"note": "${HOME}"}, {"expect_exit": 5}]}, f)
status = os.path.join(work, "status-plain.json")
r = run("mopicselftest_noproc", "run", plain, "--out", os.path.join(work, "pilot-plain"), "--status", status)
st = json.load(open(status, encoding="utf-8"))
check("recipe without vars and a literal ${HOME}: runs (no_window), vars {}", st["error_kind"] == "no_window" and st.get("vars") == {}, st)
r = run("-", "run", plain, "--dry-run")
check("recipe without vars: dry run prints the step as written", r.returncode == 0 and '{"note": "${HOME}"}' in r.stdout, r.stdout + r.stderr)

print(f"python: {fails} failure(s)")
sys.exit(1 if fails else 0)
