"""Sharing files end to end through vyrn-cli: pack, inspect, import, trust and export-brain (docs/dev/package.md).

A small catch-numbers brain is trained once for the module:
    vyrn-cli train <temp copy of templates/catch-numbers> --ticks 3000 --seed 1 --json
then shared as a .vrlp and a .vrl, inspected, imported into a temporary projects folder and trusted. Hostile
archives (built with Python's zipfile and raw bytes) must be refused with exit status 5 and leave nothing behind.

Environment: VYRN_CLI = the vyrn-cli program (skips unless it has the sharing commands), VYRN_DATA = the repo
folder (python/ and templates/). VYRN_TRAIN_CLI (optional) = a vyrn-cli that can train and watch, when VYRN_CLI
is a build that only has the sharing commands. Nothing is written outside pytest's temporary folders.
"""

import json
import os
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
CLI = os.environ.get("VYRN_CLI", "")
DATA = Path(os.environ.get("VYRN_DATA", str(REPO)))
TRAIN_CLI = os.environ.get("VYRN_TRAIN_CLI", CLI)


def _has_package_commands():
    if not CLI or not Path(CLI).is_file():
        return False
    try:
        r = subprocess.run([CLI, "inspect", "--help"], capture_output=True, text=True, timeout=60)
    except OSError:
        return False
    return r.returncode == 0 and "inspect" in r.stdout and "vrlp" in r.stdout


if not _has_package_commands():
    pytest.skip("VYRN_CLI doesn't have the sharing commands (pack, inspect, import, trust) yet",
                allow_module_level=True)


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    e = dict(os.environ)
    e["VYRN_DATA"] = str(DATA)
    e["VYRN_PROJECTS"] = str(tmp_path_factory.mktemp("default-projects"))   # never ~/Vyrn
    return e


def run(env, *args, cli=None, timeout=300):
    return subprocess.run([cli or CLI, *map(str, args)], capture_output=True, text=True, env=env, timeout=timeout)


def run_json(env, *args, cli=None):
    r = run(env, *args, "--json", cli=cli)
    assert r.returncode == 0, f"{args}: exit {r.returncode}\n{r.stdout}\n{r.stderr}"
    return json.loads(r.stdout)


def template_copy(dest, name="catch-numbers"):
    shutil.copytree(DATA / "templates" / name, dest, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__"))
    return dest


@pytest.fixture(scope="module")
def trained(env, tmp_path_factory):
    project = template_copy(tmp_path_factory.mktemp("trained") / "catch")
    r = run(env, "train", project, "--ticks", 3000, "--seed", 1, "--json", cli=TRAIN_CLI, timeout=1200)
    assert r.returncode == 0, f"training failed: exit {r.returncode}\n{r.stderr[-2000:]}"
    assert json.loads((project / "vyrn.json").read_text())["brain"], "training saved no brain"
    return project


@pytest.fixture(scope="module")
def packed(env, trained, tmp_path_factory):
    out = tmp_path_factory.mktemp("packed") / "Catch.vrlp"
    info = run_json(env, "pack", trained, "--out", out, "--author", "Ada", "--description", "Catch it!")
    assert out.is_file() and info["kind"] == "project"
    return out, info


def tree(folder):
    folder = Path(folder)
    return sorted(str(p.relative_to(folder)) for p in folder.rglob("*")) if folder.exists() else []


# ---- the happy path ------------------------------------------------------------------------------------------


def test_pack_inspect_import_trust(env, packed, tmp_path):
    out, info = packed
    paths = [f["path"] for f in info["files"]]
    assert {"inputs.py", "points.py", "responses.py", "world.py", "vyrn.json"} <= set(paths)
    assert not any(p.startswith(("brains/", "runs/")) for p in paths)
    assert len(info["brains"]) == 1 and info["brains"][0]["net"] == "mlp"
    assert info["author"] == "Ada" and info["mode"] == "sim"

    # Any zip tool can read it; the first entry is the stored mimetype (the desktop's magic rule).
    with zipfile.ZipFile(out) as z:
        assert z.testzip() is None
        assert z.namelist()[0] == "mimetype" and z.getinfo("mimetype").compress_type == zipfile.ZIP_STORED
        assert z.namelist()[1] == "vyrn-project.json"
        settings = json.loads(z.read("project/vyrn.json"))
        assert "python" not in settings and settings["brain"] == info["brains"][0]["id"]
    assert out.read_bytes()[30:30 + 34] == b"mimetypeapplication/x-vyrn-project"

    inspected = run_json(env, "inspect", out)
    assert inspected["name"] == info["name"] and inspected["files"] == info["files"]
    assert inspected["brains"][0]["id"] == info["brains"][0]["id"]
    text = run(env, "inspect", out)
    assert text.returncode == 0 and "a Vyrn project" in text.stdout

    projects = tmp_path / "projects"
    result = run_json(env, "import", out, "--projects-dir", projects)
    project = Path(result["project"])
    assert project.parent == projects and project.is_dir()
    assert result["brain_id"] == info["brains"][0]["id"]
    assert (project / "brains" / result["brain_id"] / "best" / "current").is_file()
    marker = json.loads((project / ".vyrn-imported").read_text())
    assert marker["trusted"] is False and marker["file"] == "Catch.vrlp" and marker["author"] == "Ada"
    assert not list(projects.glob(".import-*"))

    again = run_json(env, "import", out, "--projects-dir", projects)
    assert Path(again["project"]).name == project.name + "-2"

    r = run(env, "trust", project)
    assert r.returncode == 0, r.stderr
    assert json.loads((project / ".vyrn-imported").read_text())["trusted"] is True
    assert "already" in run(env, "trust", project).stdout


def test_watch_waits_for_trust(env, packed, tmp_path):
    if not TRAIN_CLI:
        pytest.skip("no vyrn-cli that can watch")
    result = run_json(env, "import", packed[0], "--projects-dir", tmp_path / "projects")
    r = run(env, "watch", result["project"], "--ticks", 20, "--speed", 0, "--json", cli=TRAIN_CLI)
    assert r.returncode == 2, r.stdout[-2000:] + r.stderr[-2000:]
    assert "not_trusted" in r.stdout + r.stderr
    error = next(json.loads(line) for line in r.stdout.splitlines() if '"error"' in line)
    assert error["kind"] == "not_trusted" and "came from Catch.vrlp" in error["message"]
    assert "vyrn-cli trust" in error["message"]
    plain = run(env, "check", result["project"], cli=TRAIN_CLI)
    assert plain.returncode == 2 and plain.stdout.count("This project came from Catch.vrlp.") == 1, plain.stdout
    assert not (Path(result["project"]) / "runs").exists() and not (Path(result["project"]) / "__pycache__").exists()
    assert run(env, "trust", result["project"]).returncode == 0
    r = run(env, "watch", result["project"], "--ticks", 20, "--speed", 0, "--json", cli=TRAIN_CLI)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]


def test_export_brain_then_import_into_a_fresh_copy(env, trained, tmp_path):
    out = tmp_path / "catch.vrl"
    info = run_json(env, "export-brain", trained, "--out", out)
    assert info["kind"] == "brain" and len(info["brains"]) == 1
    brain = info["brains"][0]
    assert brain["input"] == "numbers" and brain["choices"]

    before = out.read_bytes()
    r = run(env, "export-brain", trained, "--out", out)
    assert r.returncode == 5 and "already a file" in r.stderr
    assert out.read_bytes() == before and not Path(str(out) + ".part").exists()
    assert run(env, "export-brain", trained, "--out", out, "--force").returncode == 0

    # A trusted project: the import runs a Check to compare the brain with what the scripts produce.
    copy = template_copy(tmp_path / "fresh")
    result = run_json(env, "import", out, "--into", copy)
    assert result["brain_id"]
    assert json.loads((copy / "vyrn.json").read_text())["brain"] == result["brain_id"]
    assert not any("press Check" in n for n in result["notes"]), result["notes"]
    if TRAIN_CLI:
        r = run(env, "watch", copy, "--ticks", 50, "--speed", 0, "--json", cli=TRAIN_CLI)
        assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]

    # A project whose scripts see something else refuses it, with the reason.
    pictures = template_copy(tmp_path / "pictures", "catch-pictures")
    r = run(env, "import", out, "--into", pictures)
    assert r.returncode == 5 and "doesn't fit this project" in r.stderr, r.stderr
    assert not (pictures / "brains").exists()

    no_activate = run_json(env, "import", out, "--into", copy, "--no-activate")
    assert json.loads((copy / "vyrn.json").read_text())["brain"] == result["brain_id"]
    assert no_activate["brain_id"] != result["brain_id"]


def test_import_into_an_untrusted_project_compares_with_its_brain(env, packed, trained, tmp_path):
    out = tmp_path / "catch.vrl"
    run_json(env, "export-brain", trained, "--out", out)
    project = run_json(env, "import", packed[0], "--projects-dir", tmp_path / "projects")["project"]
    result = run_json(env, "import", out, "--into", project)
    assert any("isn't trusted yet" in n for n in result["notes"]), result["notes"]


def test_usage_errors(env, tmp_path):
    assert run(env, "inspect").returncode == 2
    assert run(env, "pack", tmp_path, "--sideways").returncode == 2
    assert run(env, "pack", tmp_path, "--brain", "worst").returncode == 2
    assert run(env, "import", tmp_path / "x.vrl").returncode == 2           # a brain needs --into
    assert run(env, "import", tmp_path / "x.vrlp", "--into", tmp_path).returncode == 2
    assert run(env, "trust", tmp_path, "--json").returncode == 2
    r = run(env, "pack", "--help")
    assert r.returncode == 0 and "export-brain" in r.stdout
    assert run(env, "inspect", tmp_path / "missing.vrlp").returncode == 5
    assert run(env, "pack", tmp_path / "not-a-project").returncode == 5


# ---- hostile archives ----------------------------------------------------------------------------------------


def rewrite(src, dest, extra=(), drop=(), change=None):
    """Copies a package entry by entry (mimetype first, stored), adding/dropping/changing entries."""
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dest, "w") as zout:
        for item in zin.infolist():
            if item.filename in drop:
                continue
            data = zin.read(item.filename)
            if change and item.filename in change:
                data = change[item.filename](data)
            zout.writestr(item, data)
        for name, data in extra:
            zout.writestr(zipfile.ZipInfo(name), data)
    return dest


def patch(data, offset, fmt, value):
    data = bytearray(data)
    struct.pack_into(fmt, data, offset, value)
    return bytes(data)


def central_records(data):
    pos = data.find(b"PK\x01\x02")
    while pos != -1:
        yield pos
        pos = data.find(b"PK\x01\x02", pos + 4)


def hostile_files(good, folder):
    files = {}
    raw = good.read_bytes()

    files["zip slip"] = rewrite(good, folder / "slip.vrlp", extra=[("project/../../evil.py", b"import os\n")])
    files["absolute name"] = rewrite(good, folder / "abs.vrlp", extra=[("/tmp/evil.py", b"import os\n")])
    files["backslash"] = rewrite(good, folder / "back.vrlp", extra=[("project\\..\\evil.py", b"x")])
    files["hidden file"] = rewrite(good, folder / "hidden.vrlp", extra=[("project/.bashrc", b"x")])
    files["case twins"] = rewrite(good, folder / "case.vrlp", extra=[("project/INPUTS.py", b"x")])
    files["unlisted file"] = rewrite(good, folder / "unlisted.vrlp", extra=[("project/extra.py", b"x")])
    files["changed script"] = rewrite(good, folder / "changed.vrlp",
                                      change={"project/inputs.py": lambda d: d + b"\nimport os\n"})
    files["no manifest"] = rewrite(good, folder / "nomanifest.vrlp", drop=("vyrn-project.json",))
    files["pickle brain"] = rewrite(good, folder / "pickle.vrlp",
                                    extra=[("brains/evil.vrl", b"\x80\x04\x95 not a zip, a pickle")])

    with zipfile.ZipFile(good) as zin, zipfile.ZipFile(folder / "late-mime.vrlp", "w") as zout:
        items = zin.infolist()
        for item in items[1:] + items[:1]:
            zout.writestr(item, zin.read(item.filename))
    files["mimetype not first"] = folder / "late-mime.vrlp"

    with zipfile.ZipFile(good) as zin, zipfile.ZipFile(folder / "brainmime.vrlp", "w") as zout:
        for item in zin.infolist():
            data = b"application/x-vyrn-brain" if item.filename == "mimetype" else zin.read(item.filename)
            zout.writestr(item, data)
    files["wrong mimetype"] = folder / "brainmime.vrlp"

    with zipfile.ZipFile(good) as zin, zipfile.ZipFile(folder / "zip64.vrlp", "w") as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            with zout.open(zipfile.ZipInfo(item.filename), "w", force_zip64=item.filename == "project/points.py") as f:
                f.write(data)
    files["zip64"] = folder / "zip64.vrlp"

    encrypted = raw
    for pos in central_records(raw):   # set the encryption flag in every central directory record
        flags = struct.unpack_from("<H", raw, pos + 8)[0]
        encrypted = patch(encrypted, pos + 8, "<H", flags | 1)
    files["encryption flag"] = folder / "encrypted.vrlp"
    files["encryption flag"].write_bytes(encrypted)

    last = list(central_records(raw))[-1]
    crc = struct.unpack_from("<I", raw, last + 16)[0]
    local = struct.unpack_from("<I", raw, last + 42)[0]
    bad_crc = patch(patch(raw, last + 16, "<I", crc ^ 0xFFFF), local + 14, "<I", crc ^ 0xFFFF)
    files["bad CRC"] = folder / "crc.vrlp"
    files["bad CRC"].write_bytes(bad_crc)

    usize = struct.unpack_from("<I", raw, last + 24)[0]
    lying = patch(patch(raw, last + 24, "<I", usize + 1000), local + 22, "<I", usize + 1000)
    files["lying size"] = folder / "lying.vrlp"
    files["lying size"].write_bytes(lying)

    files["cut off"] = folder / "cut.vrlp"
    files["cut off"].write_bytes(raw[: len(raw) // 2])
    files["not a zip"] = folder / "text.vrlp"
    files["not a zip"].write_bytes(b"just some text, not a package\n")

    # One entry listed twice in the central directory: both records point at the same bytes (a zip bomb trick).
    with zipfile.ZipFile(folder / "base.zip", "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), b"application/x-vyrn-project")
        z.writestr(zipfile.ZipInfo("a.txt"), b"hello")
    base = (folder / "base.zip").read_bytes()
    records = list(central_records(base))
    cd_start, end = records[0], base.find(b"PK\x05\x06")
    overlap_cd = base[cd_start:end] + base[records[1]:end]
    eocd = bytearray(base[end:end + 22])
    struct.pack_into("<HHII", eocd, 8, 3, 3, len(overlap_cd), cd_start)
    files["overlapping entries"] = folder / "overlap.vrlp"
    files["overlapping entries"].write_bytes(base[:cd_start] + overlap_cd + bytes(eocd))
    return files


def test_hostile_archives_are_refused_and_write_nothing(env, packed, tmp_path):
    folder = tmp_path / "hostile"
    folder.mkdir()
    projects = tmp_path / "projects"
    projects.mkdir()
    for what, path in hostile_files(packed[0], folder).items():
        r = run(env, "import", path, "--projects-dir", projects)
        assert r.returncode == 5, f"{what}: exit {r.returncode}\n{r.stdout}\n{r.stderr}"
        assert r.stderr.strip(), what
        assert tree(projects) == [], f"{what} left {tree(projects)}"
        assert run(env, "inspect", path).returncode == 5, what
    assert not (tmp_path / "evil.py").exists()


def test_hostile_brain_files_are_refused(env, trained, tmp_path):
    good = tmp_path / "good.vrl"
    run_json(env, "export-brain", trained, "--out", good)
    copy = template_copy(tmp_path / "copy")
    cases = {
        "pickle weights": rewrite(good, tmp_path / "pickle.vrl", drop=("model.safetensors",),
                                  extra=[("model.pt", b"\x80\x04\x95 pickle")]),
        "extra entry": rewrite(good, tmp_path / "extra.vrl", extra=[("run_me.py", b"import os\n")]),
        "damaged weights": rewrite(good, tmp_path / "weights.vrl",
                                   change={"model.safetensors": lambda d: d[:-4] + b"\x00\x00\xc0\x7f"}),
        "project inside": tmp_path / "project.vrl",
    }
    with zipfile.ZipFile(good) as zin, zipfile.ZipFile(cases["project inside"], "w") as zout:
        for item in zin.infolist():
            data = b"application/x-vyrn-project" if item.filename == "mimetype" else zin.read(item.filename)
            zout.writestr(item, data)
    for what, path in cases.items():
        r = run(env, "import", path, "--into", copy)
        assert r.returncode == 5, f"{what}: exit {r.returncode}\n{r.stdout}\n{r.stderr}"
        assert not (copy / "brains").exists(), what
    r = run(env, "inspect", cases["damaged weights"])
    assert "weights are damaged" in r.stderr


# ---- the rest of vyrn-cli that sharing relies on -----------------------------------------------------------------


def test_help_lists_every_command(env):
    r = run(env, "--help")
    assert r.returncode == 0
    for word in ("check", "train", "watch", "bench", "mcp", "pack", "export-brain", "inspect", "import", "trust"):
        assert f"vyrn-cli {word}" in r.stdout, word
    assert r.stdout.count("Exit status") == 1


def test_train_and_watch_wait_for_another_trainer(env, tmp_path):
    project = template_copy(tmp_path / "catch")
    lock = project / ".vyrn-lock"
    lock.write_text(json.dumps({"pid": os.getpid(), "by": "app", "since": "2026-11-13T17:05:00"}))
    for command in ("train", "watch"):
        r = run(env, command, project, "--ticks", 10)
        assert r.returncode == 2, (command, r.stdout, r.stderr)
        assert "The Vyrn app is already training this project (since 2026-11-13 17:05)" in r.stderr, r.stderr
    assert json.loads(lock.read_text())["by"] == "app"   # never taken over from a live holder
    assert not (project / "runs").exists()


# ---- security review regressions ----------------------------------------------------------------------------


def hand_made_vrlp(dest, files, name="Hand made"):
    """A .vrlp written from scratch (no brain): files maps project paths to bytes."""
    import hashlib
    listed = [{"path": p, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()} for p, d in sorted(files.items())]
    manifest = {"format": "vyrn-project", "format_version": 1, "vyrn": "0.1.0", "name": name,
                "template": "catch-numbers", "mode": "sim", "files": listed, "brains": []}
    with zipfile.ZipFile(dest, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), b"application/x-vyrn-project")
        z.writestr("vyrn-project.json", json.dumps(manifest), compress_type=zipfile.ZIP_DEFLATED)
        for path, data in sorted(files.items()):
            z.writestr("project/" + path, data, compress_type=zipfile.ZIP_DEFLATED)
    return dest


def catch_files(proof=None):
    files = {p.name: p.read_bytes() for p in (DATA / "templates" / "catch-numbers").iterdir()
             if p.is_file() and p.name not in ("AGENTS.md", "CLAUDE.md")}
    if proof:
        files["inputs.py"] = f"open({str(proof)!r}, 'w').write('ran')\n".encode() + files["inputs.py"]
    return files


def test_a_folder_inside_an_imported_project_waits_for_trust_too(env, tmp_path):
    proof = tmp_path / "nested-code-ran.txt"
    files = catch_files()
    for path, data in catch_files(proof).items():
        files["levels/sub/" + path] = data
    files["levels/sub/vyrn.json"] = json.dumps({"name": "Sub", "agents": {"screen": True}, "round_ticks": 50}).encode()
    package = hand_made_vrlp(tmp_path / "nested.vrlp", files, name="Nested")
    project = Path(run_json(env, "import", package, "--projects-dir", tmp_path / "projects")["project"])
    sub = project / "levels" / "sub"
    assert (sub / "inputs.py").is_file()

    if TRAIN_CLI:
        r = run(env, "check", sub, cli=TRAIN_CLI)
        assert r.returncode == 2, r.stdout[-2000:] + r.stderr[-2000:]
        assert "This project came from nested.vrlp." in r.stdout and f"vyrn-cli trust {project})" in r.stdout
        assert not proof.exists() and not (sub / "__pycache__").exists() and not (sub / ".vyrn-lock").exists()
    r = run(env, "trust", sub)
    assert r.returncode == 5 and f"part of the imported project {project}" in r.stderr, r.stderr
    assert not (sub / ".vyrn-imported").exists()

    assert run(env, "trust", project).returncode == 0
    if TRAIN_CLI:
        r = run(env, "check", sub, cli=TRAIN_CLI)
        assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
        assert proof.read_text() == "ran"


def test_shared_settings_are_bounded_both_ways(env, tmp_path):
    # Import: a hand-made package can't ask Train for a huge replay buffer, nor carry odd link settings.
    files = catch_files()
    files["vyrn.json"] = json.dumps({"name": "Big", "advanced": {"buffer": 10 ** 8, "batch": 65536, "gamma": 0.9},
                                     "link": {"port": 7457, "host": "0.0.0.0"}}).encode()
    result = run_json(env, "import", hand_made_vrlp(tmp_path / "big.vrlp", files), "--projects-dir", tmp_path / "p")
    settings = json.loads((Path(result["project"]) / "vyrn.json").read_text())
    assert settings["advanced"] == {"gamma": 0.9} and settings["link"] == {"port": 7457}
    assert any("advanced.buffer, advanced.batch, link.host" in n for n in result["notes"]), result["notes"]

    # Export: the author's home folder and absolute paths stay at home.
    home = tmp_path / "home" / "ada"
    project = template_copy(tmp_path / "mine")
    (project / "vyrn.json").write_text(json.dumps({
        "name": "Mine", "template": "catch-numbers", "python": f"{home}/venv/bin/python",
        "start_program": f"{home}/Games/supertux2 --userdir {home}/.st chrome://dino",
        "game_window": f"SuperTux {home}/Games", "advanced": {"note": f"{home}/notes.txt", "frames": 2},
        "link": {"socket": f"{home}/link.sock"}}))
    out = tmp_path / "Mine.vrlp"
    info = run_json(dict(env, HOME=str(home)), "pack", project, "--no-brain", "--out", out)
    assert info["start_program"] == "supertux2 --userdir .st chrome://dino"
    with zipfile.ZipFile(out) as z:
        for name in z.namelist():
            assert str(tmp_path).encode() not in z.read(name), name
        packaged = json.loads(z.read("project/vyrn.json"))
    assert "game_window" not in packaged and packaged["advanced"] == {"frames": 2}
    assert "python" not in packaged and packaged["link"] == {}
