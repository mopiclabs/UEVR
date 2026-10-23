"""Recipe lint (no game): every recipe's checkpoint images exist (a "draft": true recipe may still miss some) and are
the size of their region, as gamepilot's checkpoint_score needs them; every recipe's steps pass gamepilot's
step_problems. Unit tests of step_problems, and a wait_for with two ways to poke the game refused before a run."""
import glob
import json
import os
import subprocess
import sys

from PIL import Image

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


def lint(path):
    """-> (checkpoints whose image is another size than its region, checkpoints without an image, draft, folder)"""
    recipe = gamepilot.load_recipe(path)
    folder = gamepilot.checkpoint_dir(path, recipe)
    missing, wrong = [], []
    for cp_name, cp in (recipe.get("checkpoints") or {}).items():
        image = os.path.join(folder, cp["image"])
        if not os.path.isfile(image):
            missing.append(cp_name)
            continue
        with Image.open(image) as im:
            size = list(im.size)
        if size != list(cp["region"][2:]):
            wrong.append(f"{cp_name} image {size[0]}x{size[1]}, region {cp['region'][2]}x{cp['region'][3]}")
    return wrong, missing, recipe.get("draft") is True, folder


# --- the lint itself, on a recipe made for it: one image of the wrong size, one missing
os.makedirs(os.path.join(work, "lintme"), exist_ok=True)
Image.new("RGB", (220, 60)).save(os.path.join(work, "lintme", "a.png"))
Image.new("RGB", (10, 10)).save(os.path.join(work, "lintme", "b.png"))
for draft in (False, True):
    with open(os.path.join(work, "lintme.json"), "w", encoding="utf-8") as f:
        json.dump({"game": "lintme", "draft": draft, "steps": [], "checkpoints": {
            "a": {"image": "a.png", "region": [0, 0, 198, 54]}, "b": {"image": "b.png", "region": [5, 5, 10, 10]},
            "c": {"image": "c.png", "region": [0, 0, 4, 4]}}}, f)
    wrong, missing, is_draft, _ = lint(os.path.join(work, "lintme.json"))
    check(f"lint finds the wrong size and the missing image ({'draft' if draft else 'not a draft'})",
          wrong == ["a image 220x60, region 198x54"] and missing == ["c"] and is_draft == draft, (wrong, missing))

# --- every recipe: images where checkpoint_score looks for them, at the size of their region
for path in sorted(glob.glob(os.path.join(TOOLS, "recipes", "*.json"))):
    name = os.path.basename(path)
    wrong, missing, draft, folder = lint(path)
    check(f"{name}: checkpoint images sized as their regions", not wrong, "; ".join(wrong))
    check(f"{name}: every checkpoint image there" + (" (draft: missing ones allowed)" if draft and missing else ""),
          draft or not missing, f"missing in {folder}: {', '.join(missing)}")
    problems = gamepilot.step_problems(gamepilot.load_recipe(path).get("steps") or [])
    check(f"{name}: steps the pilot reads as written", not problems, "; ".join(problems))

# --- unit: step_problems
steps = [{"wait_for": "a", "press": "esc", "every": 2}, {"wait_for": "a", "move": [1, 2], "every": 2},
         {"wait_for": "a", "click": [1, 2], "press": "enter"}, {"wait_for": "a", "move": [1, 2], "click": [3, 4], "press": "x"},
         {"move": [1, 2]}, {"press": "esc"}, {"wait_for": "a"}]
p = gamepilot.step_problems(steps)
check("step_problems: wait_for with press + click and with all three refused, one alone fine",
      len(p) == 2 and p[0].startswith("step 3: ") and "press and click" in p[0] and p[1].startswith("step 4: ")
      and "press and click and move" in p[1], p)
check("step_problems: only the steps that run (first / last)", gamepilot.step_problems(steps, 4) == p[1:]
      and gamepilot.step_problems(steps, 1, 2) == [], gamepilot.step_problems(steps, 4))

# --- CLI: refused by --dry-run, and by a run before it waits for the window
both = os.path.join(work, "both.json")
with open(both, "w", encoding="utf-8") as f:
    json.dump({"game": "both", "process": "mopicselftest_noproc", "source": "window", "window_timeout": 1,
               "checkpoints": {"x": {"image": "x.png", "region": [0, 0, 1, 1]}},
               "steps": [{"wait_for": "x", "move": [1, 2], "press": "esc", "every": 2}, {"expect_exit": 5}]}, f)
gp = os.path.join(TOOLS, "gamepilot.py")
r = subprocess.run([sys.executable, gp, "-", "run", both, "--dry-run"], capture_output=True, text=True, errors="replace")
check("dry-run of a wait_for with move and press: exit 1, the problem named", r.returncode == 1
      and "step 1: wait_for takes one of press / click / move, not press and move" in r.stdout, r.stdout + r.stderr)
status = os.path.join(work, "status-both.json")
subprocess.run([sys.executable, gp, "mopicselftest_noproc", "run", both, "--out", os.path.join(work, "pilot-both"),
                "--status", status], capture_output=True, timeout=60)
with open(status, encoding="utf-8") as f:
    st = json.load(f)
check("a run with that step fails as a recipe error before the window wait", st["state"] == "failed"
      and st["error_kind"] == "recipe" and "not press and move" in st["error"], st)

print(f"python: {fails} failure(s)")
sys.exit(1 if fails else 0)
