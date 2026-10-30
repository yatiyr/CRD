# Workspace sync: one portable Cerid workspace on the NAS

<!-- doc-role: reference -->
> Operator recipe. Live owner: [WORKSPACE](../ROADMAP.md#slice-workspace); decision:
> [ADR-0136](../decisions/0136-workspace-overlay.md); tool: [scripts/workspace.py](../../scripts/workspace.py).

Your Cerid folder holds two things: the **code** (with every document) and the **agent files** (CLAUDE.md,
opencode.json, `.claude/`, `.codex/`, the Claude Code memory of this project and your workflow scripts). The code goes
to GitLab and GitHub. The agent files go only to your NAS. One command keeps both up to date; one file on the NAS
carries the whole workspace to another machine or a friend. The NAS only ever receives a few large files, each
checked after it is written.

## Parameters

| Name | Meaning | Default | Where it lives |
|---|---|---|---|
| NAS folder | the only folder the tool writes on the NAS (it must already exist) | `Z:/Arsivlik/Assets/Repos/crd` | `"nas"` in `.agent/workspace.json`; `--nas` or `CERID_WORKSPACE_NAS` override it |
| `generations` | how many code bundles, overlay bundles and exports are kept | `5` | `.agent/workspace.json` |
| `code_remotes` | the code hosts a sync pushes to, by Git remote name | every remote (this workspace: `gitlab`, `origin`) | `.agent/workspace.json` |
| `code_remote_exclude` | branch name patterns never pushed to a code host | `worktree-*` | `.agent/workspace.json` |
| `default_branch` | branch a restore checks out | `main` | `.agent/workspace.json` |
| Claude Code folder | where Claude Code keeps its configuration | `CLAUDE_CONFIG_DIR`, else `~/.claude` | `--claude-config-dir` |
| Memory folder | this project's Claude Code memory | `<Claude Code folder>/projects/<key>/memory` | `--memory-dir` |
| Downloads folder | pinned tool downloads (`cmake/pins.json`) | `~/.cerid/downloads` | `--downloads-dir` |
| Confidential marker | a file that keeps its branch out of every export | `.cerid-confidential` | any folder of a branch |

`<key>` is the absolute folder path with every character other than a letter or digit turned into `-`:
`D:\Dev\cerid` → `D--Dev-cerid`, `/home/ada/cerid` → `-home-ada-cerid`.

## What is where

```
D:\Dev\cerid\                 one folder, two Git histories
  .git\                         the code (pushed to GitLab and GitHub; bundled to the NAS)
  .agent-overlay\               the agent overlay's history (bundled to the NAS only)
  CLAUDE.md, opencode.json,     agent files: ignored by the code, tracked by the overlay
  .claude\, .codex\, .agents\
  .agent\workspace.json         this table's settings
  .agent\claude-memory\         copy of the Claude Code memory folder
  .agent\workflows\<session>\   copies of the workflow scripts

Z:\Arsivlik\Assets\Repos\crd\   only large single files, every one listed in MANIFEST.json
  MANIFEST.json                 each file's kind, size, SHA-256, time, machine (and a bundle's branches)
  code\code-<UTC time>.bundle   the whole code history, one file (newest 5 kept)
  overlay\overlay-<UTC>.bundle  the whole agent overlay history, one file (newest 5 kept)
  tools\<archive>               pinned tool downloads, one archive each
  exports\crd-workspace-YYYYMMDD.zip   archives for a friend or another machine (newest 5 kept)
  crd.lock                      present only while a sync or export runs
```

## How the NAS is protected

- Every file is written as `<name>.partial`, flushed, **read back from the NAS (past this computer's cache) and
  compared by SHA-256** with the local file (a bundle is also checked with `git bundle verify`), and only then
  renamed. A mismatch deletes only that `.partial` file and stops with an error.
- An existing file is never overwritten. A sync that changed something adds new dated files; the oldest of each kind
  beyond `generations` are deleted only if this tool created them and recorded them in `MANIFEST.json`.
- The tool resolves every path (shortcuts, links and junctions included) and refuses anything outside the NAS folder.
  It never creates the NAS folder and never touches its neighbours.
- It refuses to start unless the share has **twice** the planned amount free.
- `crd.lock` lets one machine write at a time. A lock left by a crashed run on the same machine is cleared
  automatically; another machine's lock stops the run (use `--break-lock` only if that machine is not syncing).
- `restore` and `status` only read the NAS; a restore refuses while another machine holds the lock.

## Protect the NAS folder

Do this once, before the first sync (DSM 7.2; wording can differ slightly in other versions). `Z:` is
`\\Arsiv-Dere\home`, your user home, which lives in the **homes** shared folder.

1. **Recycle Bin:** DSM → **Control Panel** → **Shared Folder** → select **homes** → **Edit** → **General** → tick
   **Enable Recycle Bin** (and **Restrict access to administrators only**) → **Save**. Deleted files then go to
   `#recycle` instead of disappearing.
2. **Snapshots** (needs a Btrfs volume; **Storage Manager** shows the file system): **Package Center** → search
   **Snapshot Replication** → **Install**. Open **Snapshot Replication** → **Snapshots** → select **homes** →
   **Settings** → **Schedule** tab → tick **Enable snapshot schedule**, choose daily → **Retention** tab → choose a
   retention policy (for example keep the latest 30 snapshots) → **OK**.
3. Take one now: **Snapshot Replication** → **Snapshots** → select **homes** → **Snapshot** → **Take a Snapshot**.

To bring back an older state: **Snapshot Replication** → **Snapshots** → **homes** → **Snapshot** → **Browse**, then
copy the file you need, or restore the whole folder to that snapshot.

## Daily sync

```powershell
python scripts/workspace.py sync --dry-run   # lists every file it would write, with sizes; writes nothing
python scripts/workspace.py sync
```

A sync copies the Claude Code memory and workflow scripts into `.agent/`, commits the overlay, merges what another
machine wrote to the NAS (and writes those memory changes back into Claude Code's folder), writes a new overlay bundle
and a new code bundle when they changed, adds new pinned downloads, updates `MANIFEST.json`, prunes old generations,
and finally pushes branches and tags to the code hosts. Running it twice writes nothing the second time. Add
`--no-code-remotes` to leave the code hosts alone. `python scripts/workspace.py status` lists what differs, reading
only (`--offline` skips the hosts).

## Export for a friend or a backup

```powershell
python scripts/workspace.py export                         # to the NAS: exports\crd-workspace-YYYYMMDD.zip
python scripts/workspace.py export --output-dir D:\Handover   # to a local folder or USB disk instead
```

The archive is built on your disk, checked, and copied to the NAS as one verified file. It holds both histories in
full, the pinned downloads and the restore program. Sync first: an export refuses agent files that changed since the
last sync. Anything marked confidential stops the export.

## Restore on a new machine

You need Git and Python 3.12 or newer. Pick a new or empty folder; any path works. Nothing is written to the NAS.

```powershell
python crd-workspace-20261031.zip verify
python crd-workspace-20261031.zip restore D:\Work\cerid                # from an export
python scripts/workspace.py restore --from Z:\Arsivlik\Assets\Repos\crd D:\Work\cerid   # from the NAS
```

(The second form needs `scripts/workspace.py` from any Cerid checkout or an unzipped export.) A restore checks the
files against `MANIFEST.json`, checks out the code with every branch and tag and the code-host remotes, restores the
overlay, places the memory and workflow scripts where Claude Code looks for that folder on that machine (with paths
adjusted), puts the pinned downloads back and prints the next steps: the documentation validator, `status`,
configuring a preset and `git fetch --all`. If memory already exists there it stops; `--replace` moves the old
memory aside first.

## Several machines

Each machine syncs to the same NAS, one at a time (the lock). Memory notes and agent files from both are merged. If
both machines changed the same lines, the sync stops before writing to the NAS and names the files: open each one
under `.agent/` (or the agent file itself), keep the lines you want, delete the `<<<<<<<`, `=======` and `>>>>>>>`
markers, and run sync again; it finishes the merge and writes the result back to Claude Code's memory.
`python scripts/workspace.py overlay <git arguments>` runs any Git command on the overlay (`log`, `show`, `diff`).
If another machine wrote code commits you do not have yet, the sync keeps the code bundle as it is and tells you
which branch to fetch first; nothing on the NAS is lost.

## What a sync refuses, and why

- **Transcripts** (`*.jsonl`): Claude Code's session logs never leave the machine.
- **Credential files** (`.env`, `*.pem`, `*.key`, `.credentials.json`, `auth.json`, `.netrc` and similar).
- **Secret shapes** in any agent file: private keys and GitHub, GitLab, AWS, Slack, Anthropic, OpenAI, Google,
  Hugging Face or npm tokens, or a password inside a URL. The report names the file and line, never the value.
- **Agent files on a code host**: a branch whose unpublished commits touch CLAUDE.md, opencode.json, `.claude/`,
  `.codex/`, `.agents/`, `.agent/` or `.agent-overlay/` is not pushed to GitLab or GitHub (the NAS bundle still has it).

## Traps

- *"the NAS holds commits this checkout has never seen"*: another machine synced that branch. Fetch it from the
  newest code bundle (`git fetch Z:/Arsivlik/Assets/Repos/crd/code/<newest>.bundle <branch>`), merge or rebase, then
  sync.
- *"locked by <machine>"*: that machine is syncing. Wait; use `--break-lock` only when you know it is not.
- A plain `git status` in the code shows no agent files: they are ignored there by design. Use `workspace.py status`.
- Old commits still contain the CLAUDE.md and opencode.json they had: history is not rewritten.
- Settings tracked in place (such as `.claude/settings.local.json`) keep this machine's absolute paths after a restore
  elsewhere; Claude Code simply asks again for any permission whose rule no longer matches.

## Where the code lives

[scripts/workspace.py](../../scripts/workspace.py) (stdlib only; also the archive's restore program),
[scripts/test-workspace.py](../../scripts/test-workspace.py), the ignore rules in [.gitignore](../../.gitignore) and the
guard in [scripts/check-repository.py](../../scripts/check-repository.py).
