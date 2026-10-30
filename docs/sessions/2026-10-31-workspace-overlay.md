# Session — 2026-10-31 — workspace overlay and NAS sync

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md#slice-workspace); current rules: [AGENTS](../../AGENTS.md).

Row: [WORKSPACE](../ROADMAP.md#slice-workspace). Decision: [ADR-0136](../decisions/0136-workspace-overlay.md).
Operator steps: [workspace sync recipe](../recipes/workspace-sync.md). Branch `workspace-overlay`, worked in a
separate worktree beside the serial SIMD-DISPATCH workflow (Python and documents only; no C++ build, CTest or WSL).

## Goal

Make the whole working environment (code, documents and every AI-agent file) portable through the operator's
Synology NAS, while the code hosts (GitLab, mirrored to a private GitHub) never receive AI-agent files.

## User decisions

2026-10-31, verbatim:

- "I want code repo and agent repo merged for nas! ... whenever we just take the only file in my nas and I distribute
  to my friends for example or try on another machine agents can start working perfectly so everything every claude
  stuff and the ones in .claude stuff as well everything about this project must exist portable on crd but agent
  related ai related stuff must not be pushed to repositories in github and gitlab! we will only have them on nas!"
- "sessions stay on gitlab, exclude transcripts, synology nas is here Z:\Arsivlik\Assets\Repos and I want you to add
  crd here" (the operator created `Z:\Arsivlik\Assets\Repos\crd`; nothing else under `Repos` is touched).
- "we can count all the docs as part of the code so we don't count them as ai stuff" (all documents, AGENTS.md,
  MEMORY.md, context.md, START_HERE.md and docs/ included, stay in the code repository).
- History cleanup: forward-only (past commits keep their files; no history rewrite).
- "do not forget to update all the agent related docs and the whole docs. Everything needs to be fully healthy and
  not drifted from now on! ... Everything needs to be fully serious"
- NAS safety: "You are not going to write many small files to nas right? you will archive the structure and copy to
  nas and extract the archive or what is the most secure way to do it without damaging my nas I want you to be very
  careful". Made binding: only single large files on the NAS (Git bundles, pinned archives, exports, one manifest);
  every write to `.partial`, read back and compared by SHA-256 (bundles also `git bundle verify`) before a rename;
  nothing ever overwritten; only recorded generations pruned (default 5); nothing outside the folder touched (links
  and junctions resolved); free space at least twice the write; one writer through a lock; restore reads only; the
  recipe tells the operator to enable Synology snapshots and the Recycle Bin; a dry run before the first real sync.

## What we built / changed

- `scripts/workspace.py` (stdlib only): `sync`, `status`, `export`, `restore`, `verify`, `overlay`. The agent overlay
  is a second Git repository (`.agent-overlay/`, work tree = the checkout) tracking CLAUDE.md, CLAUDE.local.md,
  opencode.json, .mcp.json, `.claude/`, `.codex/`, `.agents/` (machine-local state excluded) and `.agent/`
  (`workspace.json`, `claude-memory/`, `workflows/<session>/`).
- Code repository: CLAUDE.md and opencode.json untracked (`git rm --cached`, forward-only); `.gitignore` ignores
  every agent path; `check-repository.py` treats them as ignored local state and fails if one is tracked again;
  `.github/ci-tiers.json` no longer lists opencode.json; `cerid_dev/selection.py` no longer classifies CLAUDE.md;
  `project_sync` protects `.agent/` and `.agent-overlay/` like `.claude/`.
- `scripts/test-workspace.py` (28 tests) and one added assertion in `scripts/test-project-sync.py`.
- Documents: ADR-0136, the workspace sync recipe, the WORKSPACE row, and routes in AGENTS, START_HERE, MEMORY,
  README, docs/README, BUILDING, CONTRIBUTING, the repository layout, the scripts index and the ADR/recipe indexes.

## Plain-English explanation

The Cerid folder now holds two Git histories. The usual one has the code and every document and goes to GitLab and
GitHub. The second one, the agent overlay, has only the agent files and a copy of the Claude Code memory and workflow
scripts, and goes only to the NAS. `workspace.py sync` keeps both up to date: it writes the overlay and the code as
one bundle file each, the pinned tool archives and a manifest to the NAS, checking every file after writing it, then
pushes the code to the hosts after making sure no agent file, transcript or secret goes with it. `workspace.py export`
makes one zip that restores itself on another machine; `workspace.py restore` rebuilds the folder anywhere and puts
the memory where Claude Code looks for it there.

## Decisions made

- Overlay Git directory `.agent-overlay/`, copied state under `.agent/`; explicit staging with `git add --force`
  (the code's `.gitignore` applies to the overlay work tree) and `info/attributes` turning off all conversions (the
  code's `.gitattributes` would otherwise change line endings).
- Memory and workflow copies store the project root as `{{CERID_ROOT}}` / `{{CERID_ROOT_NATIVE}}`, so two machines
  merge without path conflicts; a conflicting line leaves the merge open until resolved, and the next sync finishes it.
- NAS: full bundles (no incremental chain), generations, lock, confinement, verified writes (ADR-0136 §6-§8). The
  Synology Git Server package (SSH) was not used: it keeps bare repositories with many small files on the NAS.
- Export: zip as a Python zip application rather than `.tar.zst` (zstd needs Python 3.14 or a tool on the receiving
  machine; the payload is already compressed).
- Claude Code memory key: every character outside `[A-Za-z0-9]` becomes `-` (observed, not documented by the tool).

## Findings fixed on the way (failing test first)

- The code repository's `.gitattributes` applied to the overlay and normalized CRLF agent files (the first real sync
  stored six such files converted). The synthetic test repository now carries the real `.gitattributes` with CRLF
  memory and workflow files; the overlay writes `info/attributes`, and an older overlay is renormalized on its next
  sync (second real sync: "agent files are now stored byte-exact").
- The SHA-256 read-back could be served from the Windows SMB client cache; it now reads unbuffered on Windows
  (cached pages dropped on POSIX), tested for every boundary size. The files of the first sync were re-hashed
  unbuffered from the NAS and all matched.
- A manual overlay merge would have been overwritten by the next sync's copy of the live memory; sync now leaves a
  conflicting merge open, refuses while conflict markers remain, and finishes it on the next run.
- Report labels used `→`, which a cp1254 console cannot print; labels are ASCII and output never stops on encoding.
- The code-host check counted a deletion of an agent path as "touching" it, so the commit that untracks CLAUDE.md
  and opencode.json (this branch) would have been refused by any host lacking it; deletions now pass, additions and
  modifications are still refused.
- A checkout that syncs against a populated NAS without a restore (the main checkout after this merge) adopted the
  NAS history but then dropped its files (`.agent/workspace.json`, NAS-only notes) from the next commit. Adoption
  now writes the files missing here, gives Claude Code the NAS-only notes without overwriting live ones, and lists
  the files again before committing.

## Files touched

- `scripts/workspace.py`, `scripts/test-workspace.py` — new.
- `.gitignore`, `scripts/check-repository.py`, `.github/ci-tiers.json`, `scripts/cerid_dev/selection.py`,
  `scripts/project_sync/storage.py`, `scripts/test-project-sync.py`; `CLAUDE.md` and `opencode.json` untracked.
- `docs/decisions/0136-workspace-overlay.md`, `docs/recipes/workspace-sync.md`, this session — new;
  `docs/ROADMAP.md` (row 1090), `docs/decisions/README.md`, `docs/recipes/README.md`, `AGENTS.md`, `START_HERE.md`,
  `MEMORY.md`, `README.md`, `docs/README.md`, `docs/BUILDING.md`, `docs/CONTRIBUTING.md`,
  `docs/design/repository-layout.md`, `scripts/README.md`.

## Tests / verification

- `python scripts/test-workspace.py`: 28 of 28 pass (Windows 11, Python 3.14.4, Git 2.52.0). `test-project-sync.py`
  60 of 60; `test-dev-workflow.py` 59 of 59; `check-repository.py`, `check-ci-tiers.py`, `check-master-plan.py` PASS.
  `test-repository-tools.py`: 160 of 161, the failure being `test_generated_target_folders`, which needs Ninja on PATH
  and a CMake configure (not available in this shell and outside this session's no-configure limit; unrelated to the
  change). `test-module-selection.py` needs the same and was not run.
- Dry run before the first real sync (the NAS folder was empty): ten files, 754.2 MiB, 10.7 TiB free.

```
would write Z:/Arsivlik/Assets/Repos/crd/overlay/overlay-20261030T232437Z.bundle  1.4 MiB (1,433,536 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/code/code-20261030T232437Z.bundle  494.8 MiB (518,806,235 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/tools/OpenBLAS-ce3f668c.tar.gz  23.4 MiB (24,532,468 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/tools/boost_1_92_0.tar.bz2  189.8 MiB (199,030,664 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/tools/eigen-3.4.0.tar.gz  2.6 MiB (2,705,005 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/tools/highway-2607d3b5.tar.gz  3.5 MiB (3,692,133 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/tools/hptt-94253864.tar.gz  635,801 bytes
would write Z:/Arsivlik/Assets/Repos/crd/tools/node-v24.22.0-linux-x64.tar.xz  30.5 MiB (32,015,600 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/tools/sccache-v0.17.0-x86_64-pc-windows-msvc.zip  7.6 MiB (7,936,022 bytes)
would write Z:/Arsivlik/Assets/Repos/crd/MANIFEST.json  (index, replaced atomically)
```

- First real sync (from this worktree, `--no-code-remotes`; the main checkout was only read through the shared object
  store): overlay 545 files; code bundle with 11 branches and 1 tag; 7 pinned archives (8 unpinned downloads stayed
  local); every file verified. Second sync: the byte-exact renormalization and two moved branches, one new overlay and
  one new code bundle. NAS afterwards, 1.3 GiB in 12 files: `MANIFEST.json` (6,828 bytes), two code bundles
  (518,813,102 and 518,829,743 bytes), two overlay bundles (1,488,506 and 1,490,905 bytes), seven tool archives
  (270,547,693 bytes).
- Restore from the NAS into a scratch folder, with a scratch Claude Code folder and download folder: 11.8 s; all 12
  refs equal the newest bundle; `main` checked out; `gitlab` and `origin` remotes; CLAUDE.md, opencode.json,
  `.claude/settings.local.json` and `.agent/workspace.json` byte-identical; 531 memory files placed under the derived
  key with `MEMORY.md` pointing at the new root and a lesson byte-identical; 10 workflow scripts; 7 downloads at their
  original relative paths; `status` of the restored checkout: in sync; the documentation validator passes there; the
  NAS file list, sizes and modification times unchanged by the restore and the status. The restored `main` shows
  `.agent/` and `.agent-overlay/` as untracked because `main` does not yet have this branch's `.gitignore`.
- Real export to a local scratch folder (not the NAS): 754.3 MiB, 14 members, the branch-tip secret scan and the
  confidential check clean; `python crd-workspace-20261031.zip verify` passes.

## Limitations and open gates

- CTest registration belongs to the tests/ owner: `add_test(NAME crd-workspace COMMAND Python3::Interpreter
  "${CMAKE_SOURCE_DIR}/scripts/test-workspace.py")`. Until then no hosted lane runs it.
- After the merge, the operator's first `workspace.py sync` in `D:\Dev\cerid` creates `.agent-overlay/` there and
  adopts the NAS overlay history. The NAS bundles predate the last two fixes and this record; that sync refreshes
  them. The push to the code hosts was not exercised against GitLab or GitHub.
- Real-NAS evidence is Windows only; Linux and macOS paths are designed and covered by portable tests, not run here.
- Synology snapshots and the Recycle Bin are user actions (recipe); they were not yet enabled when the first sync ran.

## Next session starts with

The WORKSPACE row in ROADMAP owns the remaining gates.
