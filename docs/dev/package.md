# Sharing: `.vrl` and `.vrlp` files (binding)

People share what they trained:

| File | Name | Holds | Opening it |
|---|---|---|---|
| `.vrl` | Vyrn brain | One trained brain: its weights, what it sees, its choices in order, training stats, the points chart | Adds the brain to a project whose scripts fit it. **Never runs code** |
| `.vrlp` | Vyrn project | A whole project: scripts, pictures, settings, plus its trained brain (as an embedded `.vrl`) | Creates a new project in `~/Vyrn`, ready to Watch. It is **untrusted** until the person trusts it (its Python is someone else's code) |

Public API: `core/include/vyrn/package.h` (std-only). Implementation: `core/src/package/` → static library
`vyrn_package` (C++17; links `vyrn_project`, `vyrn_core`, torch, zlib, nlohmann_json). No pickle, no Python and no
worker process is ever involved in reading a package.

## 1. User flows

- **Share a project:** Project window → **Share ▸ Share project (.vrlp)…** (also in the launcher card menu). The dialog
  has Author (optional, remembered in the app settings, **never filled in from the system**), Description (optional),
  Include the trained AI (Best / Latest / Don't include), and shows the file list and size. It saves
  `<suggested name>.vrlp`, then shows "Saved Catch.vrlp (412 KB). Anyone with Vyrn can double-click it to open your
  project and watch your AI." with **Show in folder**.
- **Share a brain:** **Brain ▾ → Export this brain (.vrl)…**: same fields minus the file list.
- **Open a project file:** double-click a `.vrlp` (MIME registration, section 9), drag it onto any Vyrn window, run
  `vyrn file.vrlp`, or use **Open shared file…** in the launcher. The import dialog shows name, author, description,
  preview, mode, brain stats with a mini points chart, and the warnings of section 7; **Import** creates the project
  and opens it with the trust banner.
- **Open a brain file:** in a project, **Brain ▾ → Import a brain (.vrl)…** (or drop it on the project window). From
  the launcher (double-click/`vyrn file.vrl`): Vyrn lists "New <template> project with this brain" first when the
  brain names a template Vyrn ships, then all your projects, those made from the same template first (marked
  "(same template)"). It doesn't pre-filter by shape: the fit is checked when you pick a project
  (`brain::compare`), and a brain that doesn't fit is refused with a plain reason.

## 2. Container: a strict ZIP subset

Both files are ZIP archives, so `unzip -l file.vrlp` and Python's `zipfile` can read them. Vyrn **writes** exactly:

- The first entry is `mimetype`: stored (method 0), no extra field, no data descriptor, content exactly
  `application/x-vyrn-brain` or `application/x-vyrn-project` (no newline). So bytes 30.. of the file are
  `mimetype` + that string, which section 9's magic rule matches. This is the EPUB/ODF convention.
- The second entry is the manifest (`vyrn-brain.json` / `vyrn-project.json`), deflated, UTF-8 JSON.
- The other entries in byte order of their names. JSON, `.py` and text are deflated; PNG, `.safetensors` and nested
  `.vrl` files are stored.
- Every entry: general-purpose flag bit 11 (UTF-8 names) set; version needed 20; DOS time = the export time (UTC);
  external attributes = regular file 0644 (`0100644 << 16`), "made by" Unix (3); no data descriptors, no ZIP64, no
  encryption, no archive comment. Folders have no entries of their own.
- Writing goes to `<out>.part` in the same folder, then fsync, then rename (fsync the folder). On any error the
  `.part` file is removed. `overwrite=false` refuses an existing out file before writing anything.

Vyrn **reads** only that subset, plus what common zip tools produce from it, so a person can unzip, edit and re-zip:

- The End Of Central Directory must be found within the last 65,557 bytes; one disk only; the entry count and
  central directory offset/size must agree and lie inside the file, and the central directory must end exactly
  where the end record starts (nothing hidden between them). An archive comment is allowed and ignored, but it
  must end exactly at the end of the file.
- Each central directory entry must match its local header (name, method, CRC, sizes; sizes may be 0 in the local
  header only when flag bit 3 is set). Methods 0 and 8 only. Refused: encryption (bit 0, or method 99), ZIP64
  (0xFFFFFFFF sizes or offsets, or a ZIP64 extra field), version needed > 45, patched data (bit 5), and
  strong encryption (bit 6). Extra fields are skipped, but must be well-formed.
- Entry data ranges must not overlap each other or the central directory (overlap is a zip-bomb technique).
- Inflate is bounded: output stops at the declared size; more or less data, or a CRC mismatch → damaged.
- The `mimetype` entry must be first, stored, and hold the right type for the file kind (a `.vrl` must say
  brain, a `.vrlp` project; the extension decides what the person expects, the content decides what it is).
- **Names**, checked for every entry: UTF-8, valid; 1–1024 bytes; '/'-separated components of 1–255 bytes; no
  empty, `.` or `..` component; no leading `/`; no `\`, `:`, NUL or control characters; no component starting with
  `.` (hidden files are never shared). Names ending in `/` (folder entries made by zip tools) are ignored if empty.
  Two names equal ignoring case → refused (portability, and a classic parser-confusion trick).
- **Limits:** the file ≤ 1 GiB; ≤ 4096 entries (`.vrl`: ≤ 16); each entry ≤ 64 MiB uncompressed; all entries
  ≤ 512 MiB together; manifests ≤ 4 MiB.
- A nested `.vrl` inside a `.vrlp` is read from memory with the same reader and limits, and only at
  `brains/<name>.vrl` (no deeper folders, no other place).
- **Manifests** are parsed strictly: duplicate keys in any object are refused (two parsers could disagree on which
  one wins), and nesting is at most 32 levels deep. Messages never quote the file's text raw: a key is shown as
  printable ASCII ('?' for anything else, terminal escapes included), cut to 40 characters.
- `format_version` 1 (or 1.x; unknown manifest keys are ignored, and an import says "Some details in this file come
  from a newer Vyrn and were left out.") is read; 2 or more is refused with "<file> was made by a newer version of
  Vyrn. Please update Vyrn to open it."
- Endianness: the code works on little-endian hosts only (Pi 5); a `static_assert` says so.

Everything is checked before anything is written (section 6).

## 3. Weights: `model.safetensors`

The [safetensors](https://github.com/huggingface/safetensors) layout: `u64 header_len` (little-endian) | JSON header
| raw data. It's chosen because it can't carry code (unlike torch's pickle-based `.pt` files) and is well known.

- Header: `{"__metadata__": {"format": "vyrn-qnet", "net": "<mlp|small_cnn|der>"}, "<name>": {"dtype": "F32",
  "shape": [..], "data_offsets": [begin, end]}, ...}`, keys sorted, padded with spaces to a multiple of 8 bytes.
- Tensors: exactly the `named_parameters()` and `named_buffers()` of the **online** network
  (`make_qnet(net_config_for(shape))`), e.g. `fc1.weight`, `head.bias`. float32, little-endian, C order, laid out in
  key order, contiguous, covering the data section exactly.
- Reading checks: header_len ≤ 1 MiB and inside the file; valid JSON object; only `__metadata__` plus tensor entries;
  dtype `F32` only; rank 1–4, each dim 1–16,777,216; byte size = 4 × product; offsets in order, contiguous, no gaps or
  overlap, ending at the data end; names `[A-Za-z0-9_.]{1,128}`; ≤ 64 tensors. Then against the network built from
  the manifest's shape: the same set of names, the same shapes, every value finite. Anything else → "The brain's
  weights are damaged or don't match its description."

## 4. `.vrl` (brain)

Entries: `mimetype`, `vyrn-brain.json`, `model.safetensors`, optional `preview.png` (≤ 4 MiB, a PNG signature +
a sane IHDR of at most 4096 × 4096 pixels; it's only displayed).

`vyrn-brain.json`:

```json
{
  "format": "vyrn-brain", "format_version": 1, "vyrn": "0.1.0",
  "name": "Catch (pictures)", "description": "", "author": "", "created": "2026-11-13T19:20:00Z",
  "project": "Catch (pictures)", "template": "catch-pictures", "mode": "sim",
  "brain": {"id": "20261113-181009", "which": "best"},
  "shape": {"kind": "picture", "in_c": 12, "in_h": 10, "in_w": 10, "d": 0, "small_picture": true, "frames": 4,
            "choices": ["left", "right", "stay"], "inputs": []},
  "net": "small_cnn", "net_description": "Small CNN, 12x10x10 -> 3 choices",
  "obs": {"kind": "picture", "shape": [10, 10, 3], "names": []},
  "choices": ["left", "right", "stay"], "labels": ["left", "right", "stay"],
  "effects": ["call left(world)", "call right(world)", "do nothing"],
  "normalizer": null,
  "model_state": {"updates": 1000, "max_abs_reward": 1.0, "reward_frozen": true},
  "counters": {"ticks": 6000, "rounds": 666, "updates": 1000, "seconds": 36.9},
  "best_mean20": 0.98,
  "history": [[20, -0.1], [40, 0.2]],
  "weights": {"file": "model.safetensors", "sha256": "<hex>", "tensors": 10, "parameters": 123456}
}
```

- `shape`, `obs`, `choices`, `labels`, `effects`, `normalizer`, `counters`, `best_mean20` come from the saved
  `brain.json` (core-internals; see `BrainHost::meta`). `model_state` is the `ModelState` stored with the weights.
- `history`: `[round, mean20]` pairs, ≤ 1000, rounds strictly increasing, taken from the project's
  `runs/*/rounds.csv` files that belong to this brain (`runs/<ts>/run.json` → `{"brain": "<id>"}`; runs without
  `run.json` are skipped), evenly thinned to ≤ 1000 points, always keeping the last one. When none of those runs
  has points (an imported brain that hasn't trained here yet), the `history` kept in the brain's `brain.json` is
  used: `import_brain` stores the package's history there (section 6). Empty when unknown; a malformed kept
  history is left out, never a reason to refuse the export.
- Reading validates every field with explicit bounds, and **only these fields** are ever written into a new
  `brain.json` (everything else in the manifest is display-only):
  - `shape` via `from_json(BrainShape)` **plus** bounds `from_json` doesn't check: pictures: `small_picture` →
    `1 ≤ in_h, in_w ≤ 32`, else `in_h = in_w = 84`; `frames` 1–8; `in_c = 3 × frames`; numbers: `1 ≤ d ≤ 4096`,
    `inputs` empty or `d` names; choices 2–256 unique names of 1–200 characters; input names ≤ 200 characters.
  - `choices` must equal `shape.choices`; `labels`/`effects` the same length (else replaced by the choices / "").
  - `normalizer`: null for pictures; for numbers `{"count" ≥ 0 finite, "mean": [d finite], "var": [d finite ≥ 0]}`,
    or null (a numbers brain saved before it gathered any statistics).
  - `model_state`: `updates` 0–10^12, `max_abs_reward` finite ≥ 0, `reward_frozen` bool.
  - `counters`: ticks/rounds/updates 0–10^15, seconds finite 0–10^9; `best_mean20` finite or null.
  - `history`: ≤ 1000 `[round, mean20]` pairs, rounds 1–10^15 and strictly increasing, means finite.
  - Text (`name`, `author`, `description`, ...): valid UTF-8 (invalid → replaced), control characters (C0, DEL and
    C1) removed except `\n` in the description, the invisible characters that change the direction of text removed
    (U+202A–202E, U+2066–2069, U+200E, U+200F, U+061C, U+FEFF: they can disguise a name), cut to 200/100/2000
    characters.
- `weights.sha256` must match `model.safetensors`.

## 5. `.vrlp` (project)

Entries: `mimetype`, `vyrn-project.json`, `project/<path>` for each project file, `brains/<id>.vrl` for each
included brain (a complete `.vrl`, stored), optional `preview.png`.

`vyrn-project.json`:

```json
{
  "format": "vyrn-project", "format_version": 1, "vyrn": "0.1.0",
  "name": "Catch (pictures)", "description": "", "author": "", "created": "2026-11-13T19:20:00Z",
  "template": "catch-pictures", "mode": "sim", "uses_link": false, "start_program": "",
  "files": [{"path": "inputs.py", "size": 123, "sha256": "<hex>"}],
  "brains": [{"file": "brains/20261113-181009.vrl", "id": "20261113-181009", "active": true}]
}
```

- `files` lists exactly the `project/` entries (same set, sizes and SHA-256); `brains` exactly the `brains/` entries;
  at most one is `active`, at most 8 brains. `mode` is recomputed on import (`world.py` present → sim).
- **What goes in** (export walks the project folder, never following symlinks):
  - regular files whose extension (lower-cased) is one of `.py .json .txt .md .csv .tsv .png .jpg .jpeg .gif .bmp
    .webp .wav .ogg`, or named `LICENSE`, `README` or `COPYING` (any case);
  - in subfolders up to 8 levels deep;
  - each ≤ 32 MiB, ≤ 2000 files and ≤ 256 MiB together (more → PackageError naming the biggest files).
- **What stays out**, each reported as a note by the exporter where it's not obvious:
  - `brains/` (the chosen brain goes in as a `.vrl` instead), `runs/`, `__pycache__/`, `*.pyc`;
  - hidden files and folders (`.vyrn-lock`, `.vyrn-imported`, `.git`, ...), symlinks, sockets, devices;
  - other file types ("Skipped game.exe: Vyrn shares only scripts, pictures, sounds and text files.");
  - agent instruction files at any depth: `AGENTS.md`, `CLAUDE.md`, `CLAUDE.local.md`, `GEMINI.md` (any case). The
    importer writes Vyrn's own copies from the data dir's template instead (section 6);
  - names other systems can't use (the name rules of section 2: `\`, `:`, control characters, ...), and every name
    that differs only in case from one already kept (possible on Linux; the walk's first one stays), including a
    file whose name matches a folder's.
- **`vyrn.json` is rewritten**, keeping only these keys (allow-list): `name`, `template`, `description`,
  `decisions_per_second`, `round_seconds`, `round_ticks`, `link`, `advanced`, `brain`. Dropped:
  `python` (a path on the author's computer, and on import it would choose the program that runs),
  `start_program` (moved to the manifest as a hint), `agents` (permissions are the receiver's decision),
  `game_window` (a window title of the author's desktop, which can hold their user name, host name and folders;
  Vyrn doesn't use it: the person clicks their own game window), anything else unknown. `brain` is set to the included active brain's id, or removed when no brain is included.
  - `advanced` keeps only the brain settings Vyrn knows (core/src/brain/dqn.cpp `visit_fields`), as numbers in their
    ranges, and the ones that size memory or work per step only up to what a shared project may ask for: `buffer`
    ≤ 100,000, `batch` ≤ 256, `frames` ≤ 8, `updates_per_tick` ≤ 4 (the core itself accepts 10^8, 65,536, 16 and
    64, which would make Train allocate gigabytes). Anything else is dropped and named in the import's note
    (`advanced.buffer`, ...).
  - `link` keeps `true`/`false`, or an object with only `port` (1024–65535) and `lockstep` (bool); other keys are
    dropped (`link.host`, ...).
  - No home folder and no absolute paths leave the author's computer: in the manifest's `start_program` hint, the
    home folder (`$HOME` and the account's home) becomes `~`, and every absolute path (a word
    starting with `/` or `~/`, or one after `=`, a quote or `file://`) is cut to its last part:
    `/home/ada/Games/supertux2 --userdir /home/ada/.st chrome://dino` → `supertux2 --userdir .st chrome://dino`.

## 6. Importing

Common rules:

- The whole package is read and validated first (container, manifests, every checksum, every nested `.vrl`, every
  weight), then written. Nothing from the package decides a path outside the target: entry names go through the
  checks of section 2 and `safe_join`-style joining, and the final path must pass `is_inside()`.
- Files are created with mode 0644 (folders 0755), never as symlinks, with `O_EXCL`; mtimes are the import time.

`import_project(file, root)`:

1. Folder name: `folder_name_for(name)`; when it exists, `"<name> 2"`, `"<name> 3"`, ... (folder_name_for of that).
2. Build in `root/.import-<random>/`; on any error remove it. First the trust marker (section 7), so the folder is
   never a project Vyrn would run, not even while it is built. Then the project files; a sanitized `vyrn.json` (the
   allow-list again: the package may be hand-made), with `name` set to the final display name; each brain from its
   nested `.vrl` exactly as `import_brain` would (same id when it's a valid id `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`, else
   a new one); then Vyrn's own `AGENTS.md` and `CLAUDE.md` copied from `<data>/templates/<template>/` (or
   `my-game` when the template is unknown). Rename into place.
3. Notes: settings that were dropped, the Start program hint ("The author started `chromium chrome://dino` with this
   project. To do the same, set it in Settings → Start program."), unknown template, newer `format_version` minor
   fields ignored.

`import_brain(file, project, spec_json, make_active)`:

1. Read and validate the `.vrl`.
2. Fit check: with `spec_json`, `parse_spec` it and use `brain::compare(shape, spec)`: `Same` → ok (keep the note),
   `AppendedChoices` → ok with the note, `Incompatible` → PackageError "This brain doesn't fit this project: <why>".
   Without it, compare with the active brain's saved shape (exact equality of kind, sizes, choices and input
   names); different → PackageError explaining the difference. Neither available → import with the note "Vyrn
   couldn't check that this brain fits your scripts yet: press Check."
3. New id: the package's id if valid and free, else the local time `YYYYMMDD-HHMMSS` (then `-2`, `-3`, ...).
4. Write `brains/<id>/best` and `brains/<id>/latest` with `save_checkpoint(dir, which, Policy, meta)` (weights only;
   target = online; a fresh optimizer when training resumes). `meta` = the validated fields of section 4 plus
   `"name"`, `"imported": {"file", "author", "created"}`, `"brain": <id>`, `"saved"`, and `"history"` when the
   package has one (so sharing the brain again keeps its points chart). Training it further keeps `name`,
   `imported` and `history`: `BrainHost` copies them from the brain.json it loaded into every new save.
5. `make_active` → `merge_settings(project, {"brain": id})`.

## 7. Trust (imported projects)

- `import_project` writes `<project>/.vyrn-imported`: `{"file": "Catch.vrlp", "name", "author", "created",
  "imported": "YYYY-MM-DD HH:MM", "start_program": "...", "trusted": false}`.
- `is_trusted`: no marker → true; `"trusted": true` → true; anything else (including a damaged marker) → false.
  `trust_project` rewrites it with `"trusted": true` (atomic replace).
- **Folders inside an imported project are part of it.** A package may hold a sub-folder that looks like a project
  of its own (`levels/x/vyrn.json`, `inputs.py`, ...: only the top `vyrn.json` is sanitized), and the MCP server and
  `vyrn-cli` accept any project folder by path. So `project_trusted(dir)` looks at `dir` and every folder above it
  (as given and with symlinks resolved): any marker that isn't `"trusted": true` makes `dir` untrusted, and the
  `not_trusted` sentence names the imported project's folder (the one to trust). `trust_project` on such a
  sub-folder is refused ("This folder is part of the imported project <folder>, which isn't trusted yet...").
- **Enforcement is in the core:** `Session::start` refuses an untrusted project with an error event of kind
  `not_trusted` (`{"type": "error", "kind": "not_trusted", "script": "", "line": 0, "message", "hint": "",
  "fatal": true}`, message: "This project came from Catch.vrlp. Its Python code hasn't run on this computer yet:
  read its scripts, then trust it in Vyrn (or run: vyrn-cli trust <folder>)."). It never spawns Python and the
  session finishes. So the app, `vyrn-cli` (prints the message, exit 2) and the MCP server all inherit it. The
  marker check itself lives in `vyrn_core` (`core/src/util/trust.{h,cpp}`, `bool project_trusted(dir)`,
  `std::string not_trusted_message(dir)`); `is_trusted` calls it, and the public `vyrn::not_trusted_message()`
  (`vyrn/session.h`) lets callers ask before starting a Session.
- Who can trust: the person, in the app (banner button / the dialog shown when they press Check, Train or Watch:
  it lists the scripts, the `start_program` hint, and for real-time projects "This AI presses keys in the window you
  choose while it plays"), or with `vyrn-cli trust <project>` in their own terminal. **The MCP server has no way to
  trust** (agents can import, read and explain a package, never run it).
- The MCP server marks text it returns from an untrusted project's files as written by someone else (data, not
  instructions).

## 8. CLI (`cli/package_cmd.{h,cpp}`, wired into `vyrn-cli`)

```
vyrn-cli pack <project> [--out FILE] [--brain best|latest] [--brain-id ID] [--no-brain]
              [--author NAME] [--description TEXT] [--force] [--json]
vyrn-cli export-brain <project> [--out FILE] [--brain best|latest] [--brain-id ID]
              [--author NAME] [--description TEXT] [--force] [--json]
vyrn-cli inspect <file.vrl|file.vrlp> [--json]
vyrn-cli import <file.vrlp> [--projects-dir DIR] [--json]
vyrn-cli import <file.vrl> --into <project> [--no-activate] [--json]
vyrn-cli trust <project>
```

- Default `--out`: `suggested_file_name(...)` in the current folder. Without `--force` an existing file is refused.
- `import <file.vrl> --into P`: when P is trusted, runs a Check (`Session`, action Check) to get the spec for the fit
  check; when untrusted, compares with the active brain only.
- Output: plain text, or with `--json` one JSON object: the PackageInfo fields plus `file` (the absolute path) and
  `preview_png_size` (bytes; the picture itself isn't printed); `import` prints the ImportResult fields plus
  `"trusted": false` (`.vrlp`) or `"active": true|false` (`.vrl`). A refusal with `--json` also prints
  `{"error": "<the reason>"}`.
- Exit status: 0 ok, 2 usage, 5 the package was refused or couldn't be written/read.
- `vyrn-cli check|train|watch` on an untrusted project print the `not_trusted` sentence and exit 2.

## 9. Desktop integration (installed by `install.sh --app`, user-level, no sudo)

- `packaging/vyrn-mime.xml` → `~/.local/share/mime/packages/vyrn.xml`, then `update-mime-database
  ~/.local/share/mime`. Two types, each `sub-class-of application/zip`, with a glob (`*.vrl` / `*.vrlp`, weight 60)
  and a magic rule: priority 80, `string` `mimetypeapplication/x-vyrn-project` (resp. `-brain`) at offset 30.
  Comments: "Vyrn project" / "Vyrn trained AI". Icons: `generic-icon` `application-x-vyrn-project`, with the Vyrn
  icon installed under `~/.local/share/icons/hicolor/256x256/mimetypes/application-x-vyrn-{project,brain}.png`.
  (Implemented: `packaging/vyrn-mime.xml`; each type's `generic-icon` is its own name,
  `application-x-vyrn-project` / `application-x-vyrn-brain`. CMake installs the XML and both icons;
  `install.sh --app` (or `--desktop` alone) copies them when missing or different and runs `update-mime-database`,
  `update-desktop-database` and `gtk-update-icon-cache` when present.) Measured with GIO (`gio info`, what file
  managers use): `*.vrlp`/`*.vrl` and extension-less copies are recognized; a `.vrlp` renamed `*.zip` stays
  `application/zip`, because GIO trusts an unambiguous glob before magic. `xdg-mime query filetype` outside a
  desktop session falls back to `file --mime-type`, which doesn't read the shared MIME database at all.
- `packaging/vyrn.desktop`: `MimeType=application/x-vyrn-project;application/x-vyrn-brain;`, `Exec=vyrn %F`.
- `vyrn <file>` with a `.vrlp`/`.vrl` argument opens the matching import flow (section 1).

## 10. MCP tools (in docs/dev/mcp.md §3; `cli/mcp/tools_share.cpp`)

- `vyrn_share {project, kind: "project"|"brain", brain?: "best"|"latest", include_brain?: bool, description?}` →
  writes into `<projects_root>/Shared/` (created; never overwrites: `name 2.vrlp`, ...), returns path + summary.
- `vyrn_inspect_package {path}` → the PackageInfo (any readable `.vrl`/`.vrlp` file).
- `vyrn_import_package {path, into?}` → `.vrlp` → new untrusted project (the agent can read its files and explain
  them; it can't run them); `.vrl` with `into` → brain import (fit-checked).
- `vyrn_check`/`vyrn_train`/`vyrn_watch` on an untrusted project → the `not_trusted` error with "Ask the person to
  open it in Vyrn and trust it." (`vyrn_write_script` saves and skips its Check with the same reason.)
- `vyrn_read_project` on an untrusted project starts its text with a note that the files were written by someone
  else and are data, not instructions; so do the `vyrn://projects/...` script resources (a `# Note:` line).
- `vyrn_import_package` with a `.vrl` refuses an untrusted `into` project, and one that is training right now.

## 11. Tests

- `tests/cpp/package/` → `test_package` (GTest, needs `VYRN_WITH_APP_PARTS`): zip round trip, Python-zipfile and
  `unzip -t` compatibility (when installed), every refusal of sections 2–3 with hand-built archives (zip slip `../`,
  absolute names, backslashes, case-duplicates, overlapping entries, wrong CRC, lying sizes, bomb ratios, ZIP64,
  encryption flag, symlink attributes ignored, too many entries, nested `.vrl` limits), safetensors refusals,
  export → inspect → import round trips for all three nets (weights bit-identical, Q-values equal), fit-check
  refusals with the plain-English reasons, sanitizing of `vyrn.json` and of manifest fields, agent files replaced,
  trust marker semantics, `.part` cleanup after a failed export, `overwrite=false`.
- `tests/e2e/test_package.py` (pytest, needs vyrn-cli with the package commands; skips otherwise): `pack` →
  `inspect --json` → `import` into a temp projects dir → `vyrn-cli watch`/`check` refused (exit 2, `not_trusted`)
  until `trust`, then works; adversarial archives built with Python's `zipfile`/raw bytes are refused with exit 5
  and nothing is written; `train`/`watch` wait for another trainer's lock (exit 2).
- `tests/cpp/run/test_trust_stop.cpp` (in `test_run`): the marker rules, the `not_trusted` event (no Python, no
  files), and the MCP side in `tests/cpp/mcp/test_safety.cpp` and `tests/e2e/test_mcp.py`.
