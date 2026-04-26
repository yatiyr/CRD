# Cerid — How I Work With This Repo

> Personal cheat sheet. This file is for **you** (the human), not the agents.
> Agents read AGENTS.md. You read this.

---

## Setup checklist (one-time)

Status as of last update: ✅ all done. Re-verify if you change machines.

- [x] **SearXNG running** on `http://localhost:8080`
  - Verified: `curl "http://localhost:8080/search?q=test&format=json" -UseBasicParsing` returns JSON
- [x] **SearXNG `settings.yml`** has JSON format enabled
- [x] **Ollama models created**: `cerid-coder` (14B, 64k) and `cerid-deep` (30B, 48k)
- [x] **`mcp-searxng` installed globally** (`npm install -g mcp-searxng`)
- [x] **`SEARXNG_URL` env var** set system-wide to `http://localhost:8080`
  - This is also set in `opencode.json` per-MCP, so either alone works. Belt-and-suspenders.
- [ ] **(Optional) Anthropic / OpenAI auth** in OpenCode for `@heavy`
  - Set up via OpenCode GUI: top menu → Auth or Provider settings → sign in to Anthropic / OpenAI

---

## Launching OpenCode (GUI app)

You use the **OpenCode GUI application**, not the `opencode` CLI command. Workflow:

1. Open the OpenCode app.
2. Open the Cerid repo: `File → Open Folder → D:\Dev\cerid` (or use the recent-projects list).
3. OpenCode reads `opencode.json` from the repo root automatically.
4. The bottom bar should show:
   - **Active agent:** `planner` (the default we set)
   - **Active model:** `ollama/cerid-coder`
   - **MCP servers:** `searxng` (green = connected, red/yellow = check Ollama + SearXNG)
5. If the MCP server didn't connect, restart OpenCode after confirming `mcp-searxng` works:
   ```powershell
   $env:SEARXNG_URL = "http://localhost:8080"
   mcp-searxng
   # Should print MCP startup messages and stay alive. Ctrl+C to stop.
   ```

---

## TL;DR — every session

```
/session-start             → @planner re-orients you, proposes today's plan
                             you say "go" (or refine)
(/research <topic>)        → only if approach unclear
(@architect)               → only for new systems / big changes
@coder                     → implement
/verify                    → @tester iterates build+tests until green
/review                    → @reviewer checks Definition of Done
/session-end               → @docs-keeper writes session doc + updates ROADMAP
git add . && git commit -m "feat(...)"   ← YOU run this in your terminal
```

---

## Detailed walkthrough

### Step 1 — Start the session (`/session-start`)

In the OpenCode chat box, type `/session-start` and press Enter.

OpenCode routes the command to `@planner`. The planner reads:
- `context.md`
- Two slices of `docs/ROADMAP.md` (the step list + "Where I left off")
- The most recent session doc under `docs/sessions/`
- `git status`, `git log -10`, `git branch`

Then it produces a brief like:

```
# Session brief — 2026-04-27

## Where we left off
Math formatting polish landed. v1d (primitive geometry) is next.
141/141 tests pass.

## What's next on the roadmap
- 7d  crd-math v1d: Ray, Plane, AABB, Sphere, Triangle, Frustum
- 8a  crd-platform: Window (GLFW), Timer, basic input
- 9   Phase 1 closeout

## Open questions
none

## Git state
- Branch: main
- Uncommitted: none
- Last commit: feat(math): natvis + std::format support

## Proposed plan for today
- Slice 7d step 1: Ray<T> + Plane<T> with f32/f64 aliases
- Pattern follows existing math types — no @architect needed
- @coder, /verify, /review, /session-end

## Confirm
Reply "go" to start, or tell me what to change.
```

Three responses:
- **"go"** — accept
- **"instead, let's do AABB first"** — refine
- **"actually, today let's start crd-platform"** — change direction entirely

The planner has `tools.task: false` — it CANNOT spawn other agents. After "go" it stays quiet. You drive every next step manually.

### Step 2 — Research (skip for routine work)

**Skip when:**
- The pattern follows existing modules (e.g. another `Vec` operation)
- The decision is already in `docs/ROADMAP.md`'s decision log
- You already know what to do

**Run `/research <topic>` when:**
- New module with unfamiliar territory (Vulkan, GLFW, etc.)
- Modern C++ pattern you're unsure about (PMR allocators, coroutines, modules)
- Library evaluation ("std::format vs libfmt for our use")

What happens:
- `@researcher` calls `searxng_web_search` to find sources
- Calls `web_url_read` on 3-5 of them to get full content
- Synthesizes findings against Cerid's constraints (C++20, MSVC, etc.)
- Writes `docs/research/YYYY-MM-DD-<slug>.md` using `RESEARCH_TEMPLATE.md`
- Updates `docs/research/README.md` index table
- Returns: file path + 3-bullet TL;DR + recommendation

Read the file. If satisfied, move on. If thin, ask follow-up or `/escalate <reason>`.

### Step 3 — Architecture (skip for additions to existing systems)

**Skip when:**
- Adding a function/class to an existing module
- Bug fix
- Cosmetic change

**Use `@architect` when:**
- New module
- Cross-cutting refactor (touches 3+ modules)
- Plugin / ABI / file-format design

Type `@architect` followed by what you want designed:

```
@architect design the crd-platform Window abstraction. We'll use GLFW
underneath but keep the public API API-agnostic so we can swap to
SDL or native Win32 later if needed.
```

The architect reads ROADMAP decision log first to avoid contradicting prior decisions, then produces a full ADR in chat (Context, Constraints, Options, Decision, Consequences, Affected files, Interface sketch, Test strategy, Open questions).

If you accept: hand to coder ("@coder implement the ADR above"). If you don't: refine, or `/escalate`.

### Step 4 — Implement (`@coder`)

Be explicit about scope. Vague prompts produce vague code:

```
@coder implement Ray<T> and Plane<T> for crd-math, following the pattern
in engine/math/include/crd/math/vec3.hpp. Same template/alias structure
(Rayf, Rayd). Place in engine/math/include/crd/math/ray.hpp and plane.hpp.
Update math.hpp umbrella header. No tests yet — @tester will do those.
```

The coder:
- Reads AGENTS.md (auto-loaded), reads affected files in full
- Edits or creates files following Cerid conventions
- Runs ONE build to catch obvious errors (`cmake --build build --config Debug`)
- Stops there. Does NOT iterate on test failures — that's tester's job.

Coder reports: files changed, build result, suggested commit message.

If the coder says "I need a design decision," answer it inline (small) or call `@architect` (big).

### Step 5 — Verify (`/verify`)

Type `/verify`.

`@tester`:
1. Writes any missing tests for new code
2. Builds: `cmake --build build --config Debug`
3. Runs tests: `ctest --test-dir build --output-on-failure`
4. If failures: fixes obvious issues OR delegates to `@debugger` via subagent call
5. Loops up to 5 iterations
6. Tripwire: if passing test count DECREASES, stops immediately

Final report: `141/141 passed, 1 iteration` (or a clear failure summary if it stopped).

If tester gives up:
- Read its report — what was the failure mode?
- If clear, fix yourself or invoke `@coder` / `@debugger` directly
- If unclear, `/escalate <reason>`

### Step 6 — Review (`/review`)

Type `/review`.

`@reviewer` (cerid-deep, 30B):
- Runs `git diff` and reads full changed files
- Checks Definition of Done (warnings, tests, naming, RAII, noexcept, etc.)
- Categorizes findings: BLOCKER / MAJOR / MINOR / NIT
- Verdict: APPROVED / APPROVED_WITH_NITS / CHANGES_REQUESTED

If `CHANGES_REQUESTED`:
1. Hand findings back to `@coder` ("@coder fix the BLOCKER and MAJOR findings from the review above")
2. Re-run `/verify`
3. Re-run `/review`

### Step 7 — Close the session (`/session-end`)

Type `/session-end`.

`@docs-keeper` (cerid-deep) does the full ritual:
1. Writes `docs/sessions/YYYY-MM-DD-<slug>.md` using `SESSION_TEMPLATE.md`
2. Updates `docs/ROADMAP.md`: status table emoji, decision log append, "Where I left off" replace
3. Creates/updates `docs/systems/<module>.md` if a system shipped or changed
4. Updates `context.md` (status table, dependencies, "Where to look")
5. Updates `docs/research/README.md` "Used by" column if applicable
6. Reports paths touched + suggests a Conventional Commits message

YOU then commit yourself in your terminal (PowerShell):

```powershell
git status                                    # confirm what changed
git diff --stat
git add .
git commit -m "feat(math): v1d Ray + Plane primitives"
```

Agents NEVER commit. Permission system blocks `git commit` and `git push` at three levels. This is your safety net.

---

## Cost-conscious rules

You said: use local models as much as possible. Here's how the system enforces that:

1. **Default agent is `@planner`** — local, no cost. Sessions start cheap.
2. **Skip `/research` for routine work** — local models can write Vec3 from training data.
3. **Skip `@architect` for additions to existing modules** — pattern is already there.
4. **`@heavy` is opt-in only.** No command auto-invokes it. You type `/escalate <reason>` deliberately.
5. **`@heavy` refuses routine work** — its prompt redirects you back to local agents if you misuse it.
6. **`small_model` is `cerid-coder`** in `opencode.json` — title generation, summaries, etc. all stay local.

### When to spend Opus / GPT tokens (`/escalate`)

- Two contradictory ADRs from local `@architect` on the same problem
- `@debugger` looped 3 times without finding root cause
- Final review of a Phase milestone (e.g. before merging Phase 1 close)
- Modern C++ pattern where local models gave incorrect/unsafe code
- Long-term direction decisions (graphics API, scripting model)

**Estimated heavy invocations per Phase: 3-5**, not per session. At ~$0.30 per call, that's $1-2 per Phase. Hobby budget.

---

## SearXNG details (already set up — reference only)

**MCP server:** `mcp-searxng` v1.0.x by ihor-sokoliuk (719 stars on GitHub, MIT, current).

**Tools exposed to `@researcher`:**
- `searxng_web_search(query, pageno?, time_range?, language?, safesearch?)`
- `web_url_read(url, startChar?, maxLength?, section?, paragraphRange?, readHeadings?)`

**SearXNG `settings.yml` requirements** (already done — keep this if you ever rebuild):
```yaml
search:
  formats:
    - html
    - json
```

**If `@researcher` reports it can't reach SearXNG:**
1. `curl "http://localhost:8080/search?q=test&format=json" -UseBasicParsing` — should return JSON
2. If 403: re-check `settings.yml` JSON format
3. If connection refused: SearXNG container/service is down — restart it
4. If SearXNG works but MCP doesn't: in PowerShell run `mcp-searxng` standalone — should start without error
5. Restart OpenCode app after fixing — MCP servers connect at startup

---

## Modelfiles (already created — reference only)

```powershell
# cerid-coder: Qwen 2.5 Coder 14B at 64k
@'
FROM qwen2.5-coder:14b
PARAMETER num_ctx 65536
PARAMETER temperature 0.2
PARAMETER top_p 0.8
'@ | Set-Content -Encoding ascii Modelfile.cerid-coder
ollama create cerid-coder -f Modelfile.cerid-coder

# cerid-deep: Qwen3 Coder 30B at 48k
@'
FROM qwen3-coder:30b
PARAMETER num_ctx 49152
PARAMETER temperature 0.3
'@ | Set-Content -Encoding ascii Modelfile.cerid-deep
ollama create cerid-deep -f Modelfile.cerid-deep
```

### Why these context sizes (and why NOT 256k)

Larger context ≠ higher quality. Two costs:
1. **Effective attention drops past ~32-48k.** "Lost in the middle" — info in the middle of long prompts gets ignored.
2. **Prompt processing is O(n²).** Doubling context quadruples first-token latency.

VRAM/context cost (your hardware: 4070 Ti Super 16GB + 64GB RAM, Q4_K_M):

| Config | Total | Speed | Verdict |
|---|---|---|---|
| cerid-coder (14B) at 32k | ~12 GB | ~60 tok/s | conservative |
| **cerid-coder (14B) at 64k** ← current | **~15 GB** | **~50 tok/s** | **sweet spot** |
| cerid-coder (14B) at 128k | ~21 GB | ~15 tok/s (RAM spill) | not worth it |
| cerid-deep (30B) at 32k | ~25 GB | ~15 tok/s | starting point |
| **cerid-deep (30B) at 48k** ← current | **~28 GB** | **~10 tok/s** | **good balance** |
| cerid-deep (30B) at 64k | ~31 GB | ~7 tok/s | experiment-only |
| cerid-deep (30B) at 256k | ~67 GB | <2 tok/s | DON'T |

**Native trained context:**
- Qwen 2.5 Coder 14B: 32k native (128k via YaRN — quality degrades beyond ~64k)
- Qwen3 Coder 30B: 256k native — but YOUR hardware caps you ~48-64k

### Modelfile experimentation guide

Safe upgrade ladder for `cerid-deep`:
1. Run sessions at 48k. Note speed and quality (current state).
2. If 48k feels good, edit Modelfile to `num_ctx 65536`, re-run `ollama create cerid-deep -f Modelfile.cerid-deep`.
3. Above 64k → linear speed decay, no quality gain. Don't.

For `cerid-coder`, **don't increase past 64k**. The 14B's effective attention degrades past 64k more sharply than the 30B does.

---

## Common pitfalls

| Symptom | Cause | Fix |
|---|---|---|
| Researcher returns no sources | SearXNG offline, JSON disabled, or MCP not connected | `curl` test → check `settings.yml` → restart OpenCode |
| Researcher uses `webfetch` only, no SearXNG tools | MCP server failed to start | Check OpenCode bottom bar; restart MCP |
| Local model very slow first response | Cold-load into VRAM | Normal — first call after Ollama restart takes ~10s |
| Local model very slow ongoing | Context overflow / spillover to RAM | Reduce `num_ctx` in Modelfile |
| `/verify` loops on same error | Tester can't fix it | Read its report; fix yourself or invoke `@debugger` directly |
| ROADMAP got reformatted weirdly | Docs-keeper rewrote a section | `git restore docs/ROADMAP.md`; tighten next time |
| Agent suggested `git commit` | It missed the no-commit rule | The permission system blocks it anyway |
| `@coder` doesn't follow conventions | Didn't read AGENTS.md or ignored it | AGENTS.md is auto-loaded; if model ignored, model quality is the issue |
| `@heavy` responded to a simple question | You invoked it for routine work | Don't. Heavy will refuse next time. |
| `/session-end` produces empty session doc | No commits this session | Make at least one commit OR tell docs-keeper "uncommitted session" so it documents based on `git status` |
| OpenCode bottom bar shows MCP red/yellow | `mcp-searxng` failed to start | Run `mcp-searxng` from terminal manually to see error |

---

## Workflow variants

### "I just want to fix one bug"

```
/bugfix <description>     → debugger reproduces, fixes, regression test, reviewer checks
/session-end              → log it
git commit                ← in your terminal
```

### "I just want to research something, no code"

```
/research <topic>         → @researcher writes log to docs/research/
                          → no /session-end needed unless you want to log it
git add docs/research/<file>.md && git commit -m "docs: research <topic>"
```

### "I'm exploring, not implementing"

Just chat with `@planner`. It's read-only — can answer "what's the state of X?" or "what does the decision log say about Y?" without spawning anything.

### "I want a quick state check, not a full plan"

```
/status                   → git + module status + active focus, no proposal
```

### "I want to recover from a bad session"

OpenCode has snapshots — use the GUI's undo (check the OpenCode shortcuts panel for the keybind). Or in your terminal:
```powershell
git restore .             # discard ALL uncommitted changes
git restore docs/ROADMAP.md  # discard just one file
```

Since agents never commit, your committed history is always safe.

---

## Quick reference card

```
COMMANDS
/session-start          start every session — @planner proposes plan
/session-end            end every session — @docs-keeper writes docs
/status                 quick state glance, no planning
/research <topic>       @researcher writes docs/research/<file>.md
/feature <desc>         print the standard pipeline as a checklist
/bugfix <desc>          full bug pipeline (debugger + reviewer)
/verify                 @tester iterates build+tests until green (max 5)
/build [preset]         one-shot build+test (no iteration)
/review                 @reviewer DoD check on current diff
/escalate <reason>      hand off to @heavy (Opus/GPT — costs $$)

AGENTS (invoke with @name)
@planner                primary, default — read-only, no subagent calls
@researcher             web research → file
@architect              ADR (subagent)
@coder                  implementation (subagent)
@tester                 tests + verify (subagent)
@debugger               bugs (subagent)
@reviewer               DoD review (subagent)
@docs-keeper            docs (subagent)
@heavy                  primary, expensive — escalation only
```

---

## When to update this doc

When the workflow changes — new command, retired agent, model swap. This doc reflects how YOU work. Agents read AGENTS.md; you read this.
