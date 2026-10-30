# ADR-0136 — The workspace overlay: agent files on the NAS only, code and documents on the code hosts

<!-- doc-role: decision -->
> Decision contract. Live work: [ROADMAP / WORKSPACE](../ROADMAP.md#slice-workspace). Operator steps:
> [workspace sync recipe](../recipes/workspace-sync.md).

Date: 2026-10-31. Status: accepted (user decisions of 2026-10-31, quoted verbatim in the
[session](../sessions/2026-10-31-workspace-overlay.md#user-decisions)).

## Context

The whole working environment (code, documents and every AI-agent file) must be portable: one file on the
operator's Synology NAS, handed to a friend or opened on another machine, must let agents start working at once.
AI-agent files must never reach the code hosts (GitLab, mirrored to a private GitHub). Every document counts as code:
AGENTS.md, MEMORY.md, context.md, START_HERE.md and docs/ stay in the code repository. Past commits keep the agent
files they already hold; history is not rewritten. The NAS must be treated with great care: no working trees or
many small files, every write verified, nothing outside the workspace folder ever touched.

## Decision

1. **Two Git histories in one folder.** The code repository (`.git`) is unchanged and is the only history pushed to
   the code hosts. The *agent overlay* is a second repository whose Git directory is `.agent-overlay/` and whose work
   tree is the checkout root. The name mirrors `.git` beside it; the overlay never nests a Git directory inside a
   tracked folder, and `.agents/` (a tool convention) stays a tracked overlay path rather than a name to collide with.
2. **What the overlay tracks**, exactly: `CLAUDE.md`, `CLAUDE.local.md`, `opencode.json` and `.mcp.json` at the root;
   `.claude/`, `.codex/` and `.agents/` except machine-local state (directories named worktrees, cache(s), log(s),
   tmp/temp, shell-snapshots, file-history, paste-cache, todos, statsig, sessions, projects, backups, `__pycache__`;
   files `*.lock`, `*.log`, `*.tmp`, `*.pid`, `*.swp`); and `.agent/`: `workspace.json` (the NAS folder, code
   remotes and generations), `claude-memory/` (a copy of the Claude Code memory folder of this project) and
   `workflows/<session>/` (each session's `workflows/scripts/`). The overlay stages this explicit list with
   `git add --force`, because the code repository's root `.gitignore`, which ignores all of these paths, also applies
   to the overlay's work tree. For the same reason the code's `.gitattributes` (`* text=auto`, `*.json eol=lf`)
   would convert line endings; the overlay's `info/attributes` (`* -text !eol -filter -ident
   !working-tree-encoding`) outranks it, so agent files are stored and restored byte-exact.
3. **Refusals.** Session transcripts (`*.jsonl`), credential files (`.env*`, `*.pem`, `*.key`, `.credentials.json`,
   `auth.json`, `.netrc`, ...) and high-confidence secret shapes (private keys; AWS, GitHub, GitLab, Slack, Anthropic,
   OpenAI, Google, Hugging Face and npm tokens; credentials inside a URL) stop a sync or export before anything is
   written. Keyword matches such as "password" are not refusals: they occur harmlessly in permission rules. A file
   named `.cerid-confidential` (a future console-NDA marker) anywhere in a branch or its history refuses an export.
4. **Machine-neutral memory.** Copies under `.agent/` store this machine's project root as `{{CERID_ROOT}}`
   (forward slashes) and `{{CERID_ROOT_NATIVE}}` (Windows separators); writing them back expands the placeholders
   to the local root, byte-exact on the same machine. Two machines can therefore merge their memory without
   path conflicts; a line both changed stops the sync with the merge left open until the file is resolved.
   Files tracked in place (for example `.claude/settings.local.json`) stay verbatim.
5. **Claude Code's memory location** is `<config>/projects/<key>/memory`, where `<config>` is `CLAUDE_CONFIG_DIR` or
   `~/.claude` and `<key>` is the project folder's absolute path with every character outside `[A-Za-z0-9]` replaced
   by `-` (`D:\Dev\cerid` → `D--Dev-cerid`, `/home/u/cerid` → `-home-u-cerid`). This is observed behaviour, consistent
   with every project folder on the operator's machine (five pinned in the tests), not a published contract; a key
   over 200 characters (shortened by Claude Code with a hash) resolves only to an existing folder, and `--memory-dir`
   overrides the derivation. A linked worktree uses its main checkout's key.
6. **The NAS holds only a few large single files** (user safety requirements of 2026-10-31, verbatim in the
   session): `MANIFEST.json` (every file with kind, size, SHA-256, creation time, host and, for bundles, their refs);
   `code/code-<UTC>.bundle` and `overlay/overlay-<UTC>.bundle` (full Git bundles of every branch and tag; a new one
   only when refs changed); `tools/<archive>` (pinned downloads matched by `cmake/pins.json` SHA-256, already single
   compressed files); `exports/crd-workspace-YYYYMMDD.zip`; and `crd.lock` while a sync or export runs. No bare
   repository, working tree or loose object ever lands there. Full rather than incremental bundles: each generation
   restores on its own, so losing or damaging one file never breaks a chain; the cost is one code bundle (about
   500 MiB) per sync that changed refs, bounded by the generation count.
7. **Every NAS write is atomic and verified.** A file goes to `<name>.partial`, is flushed and synced, read back from
   the NAS past this machine's file cache (unbuffered on Windows, cached pages dropped on POSIX), compared by
   SHA-256 with the local source (bundles also pass `git bundle verify`), and only then renamed; on a mismatch only
   that `.partial` is deleted and the run fails loudly. An existing file is never overwritten. `MANIFEST.json`, the index, is the one file replaced, atomically, after the same read-back check.
   The newest `generations` (default 5) of code bundles, overlay bundles and exports are kept; pruning removes the
   manifest entries first, then exactly those recorded files, never anything the tool did not create.
8. **Confinement, space and one writer.** Every path is resolved (links and junctions included) and refused unless
   it lies inside the NAS folder; the tool never creates or touches anything outside it, and the folder itself must
   already exist. A run refuses unless the share has twice the planned write free. `crd.lock` is created exclusively;
   a lock of this host whose process is gone is stale and replaced; another host's lock refuses the run (`--break-lock`
   overrides it deliberately). Restore and status only read the NAS, and restore refuses while a lock is held.
9. **Ref discipline.** Only `refs/heads/*` and `refs/tags/*` travel: tool namespaces such as `refs/original/` or
   checkpoint refs stay local. A new code bundle is written only if every ref of the newest NAS bundle is known here
   (a rebase made here is fine); a commit from another machine blocks it until fetched, so no generation drops work.
   Code hosts receive branches (except `worktree-*`) and tags without force; a branch whose unpublished commits touch
   an agent path, add a transcript or add a secret shape is not pushed and the sync reports it.
10. **Transport and archive.** The share is reached as a mounted path (`Z:` → `\\Arsiv-Dere\home`); the Synology Git
    Server package (Git over SSH) was not chosen: it would keep bare repositories with many small files on the NAS
    and needs an account and key. An export is `crd-workspace-YYYYMMDD.zip`, built on local disk and copied as one
    verified file: both bundles, the pinned downloads, `manifest.json`, `SHA256SUMS`, `RESTORE.txt` and the restore
    program, laid out as a Python zip application so `python crd-workspace-YYYYMMDD.zip restore <folder>` works with
    only Git and Python 3.12+. Zip rather than `.tar.zst`: zstd needs Python 3.14 or an external tool on the receiving
    machine, while the payload (Git packs and compressed archives) gains under a few percent from it; zip opens
    everywhere and can run itself.

## Consequences

The code hosts stop receiving agent files from the first commit of this change; earlier commits keep theirs.
Validators no longer depend on `CLAUDE.md` or `opencode.json`; `check-repository.py` treats the agent paths as
ignored local state and fails if one is tracked again. A machine without the NAS restores from an export; a new
machine with the NAS restores from it directly. Synology snapshots and the Recycle Bin on the shared folder are the
operator's backstop ([recipe](../recipes/workspace-sync.md#protect-the-nas-folder)). The tool is plain repository
tooling with no AI content: [scripts/workspace.py](../../scripts/workspace.py), tested by
[scripts/test-workspace.py](../../scripts/test-workspace.py).
