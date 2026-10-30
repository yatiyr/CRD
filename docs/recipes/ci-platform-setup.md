# Setting up the Cerid CI platform

<!-- doc-role: reference -->
> Operator recipe. Live owner: [CI-1](../ROADMAP.md#slice-ci-1) to [CI-4](../ROADMAP.md#slice-ci-4); decision:
> [ADR-0135](../decisions/0135-cerid-ci-platform.md) (the orchestrator: §12 to §15); contract:
> [CI platform](../design/ci-platform.md).

Every step here is a **user action**: it creates an account, turns on billing, issues a token or stores a credential.
No agent performs these steps. Do them in order; after each, `python ci/ops.py accounts` shows the input as configured
(by name only; it never prints a value). Steps 1 to 6 make the cloud lanes run behind a private web UI; 7 to 11 add
device classes and options. There is no third-party CI account: the coordinator in `ci/coordinator/` is Cerid's own.

## Parameters

| Name | Meaning | Default | Where it lives |
|---|---|---|---|
| `CERID_CI_URL` | the coordinator's public HTTPS URL (behind Cloudflare Access) | `https://ci.<your domain>` | launcher environment |
| `CERID_CI_INTERNAL_URL` | the coordinator as the job VMs reach it inside the project's network | `http://<launcher internal IP>:8780` | launcher environment |
| `CERID_CI_LISTEN` | the coordinator's listener | `0.0.0.0:8780` on the launcher (the firewall admits only the project's network) | launcher environment |
| `CERID_CI_REPOSITORY_URL` | the repository the coordinator mirrors and builds | (your remote, SSH form) | launcher environment |
| `CERID_CI_GIT_HOST` | its webhook kind: `github`, `gitea`, `gitlab` or `generic` | `github` | launcher environment |
| `CERID_CI_ACCESS_TEAM`, `CERID_CI_ACCESS_AUD`, `CERID_CI_UI_EMAILS` | Cloudflare Access team domain, application AUD tag, optional allow-list | — | launcher environment |
| `CERID_CI_STATUS_REPOSITORY` | `owner/name` for commit statuses (unset: no statuses) | — | launcher environment |
| `cerid-webhook-secret`, `cerid-status-token`, `cerid-repository-deploy-key` | webhook HMAC secret, commit-status token, read-only deploy key | — | Secret Manager in the CI project |
| `CERID_CI_GCP_PROJECT` | the CI project | `cerid-ci` | launcher environment |
| `CERID_CI_GCP_ZONE` | zone for machines | `us-central1-a` | launcher |
| `CERID_CI_RUNNER_SERVICE_ACCOUNT` | runner identity attached to every job VM (and checked in its identity token) | `cerid-ci-runner@cerid-ci.iam.gserviceaccount.com` | launcher |
| `CERID_CI_STORE` | result store and orchestrator state | `gs://cerid-ci-results` (an owned coordinator: a local directory) | launcher and runners |
| `CERID_CI_SCCACHE_BUCKET` | compiler cache bucket | `cerid-ci-sccache` | runners |
| `CERID_CI_BUDGET_SUBSCRIPTION` | Pub/Sub pull subscription of the budget | `projects/cerid-ci/subscriptions/cerid-ci-budget-launcher` | launcher |
| monthly cap | the hard limit | 50 USD | `ci/devices.json` `budget.monthly_cap_usd` and the GCP budget |
| `CERID_FTL_PROJECT` | Firebase project for Test Lab (the `FTL_GCP_PROJECT` repository variable's value) | — | launcher |
| `CERID_SSH_<HOST>_HOST`, `_KEY_FILE`, `_KNOWN_HOSTS` | an SSH host (AMD_DEVCLOUD, MAC, WINDOWS_ARM64, INTEL_TIBER) | — | launcher |
| `CERID_CI_OWNED_AGENTS` | set to `1` once an owned agent is registered: the planner then schedules owned cells | — | launcher |

<a id="the-cerid-ci-project-and-billing"></a>
## 1. The `cerid-ci` Google Cloud project and its billing

1. <https://console.cloud.google.com/projectcreate>: project name `cerid-ci`, project id `cerid-ci` (or the nearest
   free id; use it everywhere below). Do **not** reuse the Firebase project.
2. **Billing → Link a billing account** to `cerid-ci` (create one if needed; a card is required). This is the only
   project that can spend.
3. **APIs & Services → Enable APIs**: Compute Engine, Cloud Storage, Secret Manager, Pub/Sub, Cloud Billing Budget,
   IAM Service Account Credentials.
4. **Cloud Storage → Buckets → Create**: `cerid-ci-results` (region `us-central1`, Standard, uniform access, public
   access prevention on, **object versioning off**; the orchestrator's compare-and-swap uses object generations) and
   `cerid-ci-sccache` (same; add a lifecycle rule "delete objects older than 30 days").
5. **IAM & Admin → Quotas**, region `us-central1`: set CPUs to 48, `NVIDIA_L4_GPUS` and `NVIDIA_T4_GPUS` to 1 each,
   preemptible CPUs to 48. The quotas bound concurrency (the planner's cloud concurrency group is six 8-vCPU
   machines), and with it the worst hour of spend.

<a id="the-50-usd-budget-and-its-automated-stop"></a>
## 2. The 50 USD budget and its automated stop

1. **Pub/Sub → Topics → Create topic** `cerid-ci-budget`; **Create subscription** `cerid-ci-budget-launcher`, pull,
   retention 7 days.
2. **Billing → Budgets & alerts → Create budget**: name `cerid-ci monthly`, scope project `cerid-ci`, all services,
   time range monthly, amount **50 USD**, thresholds 50 %, 90 % and 100 % of actual spend.
3. In the same budget, **Manage notifications → Connect a Pub/Sub topic to this budget** → `projects/cerid-ci/topics/
   cerid-ci-budget`. Cloud Billing then publishes the month's cost several times a day.
4. The launcher (section 4) sets `CERID_CI_BUDGET_SUBSCRIPTION=projects/cerid-ci/subscriptions/cerid-ci-budget-launcher`;
   every dispatcher tick pulls the notifications and, once the reported cost reaches the budget, starts no paid
   machine. `python3 ci/ops.py budget --pull` shows the state; the web UI's Spend page shows the month. The
   pipeline's own ledger caps the month independently and earlier, because the billing notification lags by hours.
5. Optional backstop: in **Billing → Budgets**, email alerts to yourself at 90 %.

<a id="service-accounts-and-workload-identity"></a>
## 3. Service accounts and secrets

1. **IAM & Admin → Service accounts → Create**: `cerid-ci-launcher` and `cerid-ci-runner`.
2. Grant `cerid-ci-launcher`, on project `cerid-ci`: Compute Instance Admin (v1), Service Account User on
   `cerid-ci-runner` only, Storage Object Admin on both buckets, Secret Manager Secret Accessor, Pub/Sub Subscriber on
   `cerid-ci-budget-launcher`, Compute Image User.
3. Grant `cerid-ci-runner`: Storage Object Admin on both buckets, and Compute Instance Admin (v1) with the IAM
   condition `resource.name.extract("/instances/{name}").startsWith("cerid-ci-")` so a job machine can delete itself
   and nothing else. **No** Secret Manager role: a runner receives only the secrets its step declares, from the
   coordinator, and only for a trusted build.
4. Keyless identity everywhere: the launcher is a Compute Engine VM and uses its attached service account through the
   metadata server; every job VM proves its identity to the coordinator with a Google-signed identity token. No
   service-account key is ever created or downloaded. (A coordinator on an owned machine uses the filesystem backend,
   `CERID_CI_STORE=/var/lib/cerid-ci/store`, and needs no Google credential at all unless it also launches cloud
   machines; then run it on the launcher VM instead.)
5. **Secret Manager → Create secret**, each value generated on your own machine and pasted only here:
   `cerid-webhook-secret` (for example the output of `python -c "import secrets; print(secrets.token_hex(32))"`),
   `cerid-status-token` (section 6) and `cerid-repository-deploy-key` (section 4.3). Nobody else needs to read them.

<a id="the-coordinator-on-the-launcher"></a>
## 4. The coordinator on the launcher

1. **Compute Engine → Create instance** `cerid-ci-launcher`: `e2-micro` in `us-central1-a` (free tier), Ubuntu 24.04
   LTS, 30 GB standard disk, service account `cerid-ci-launcher`, access scope "Allow full access to all Cloud APIs"
   (IAM roles above are the real limit), network tag `cerid-ci-launcher`. Note its internal IP.
2. **VPC network → Firewall → Create rule** `cerid-ci-agents`: ingress, targets the tag `cerid-ci-launcher`, source
   range the default network's subnet (`10.128.0.0/9`), TCP 8780. No external port is opened; the public route is the
   Cloudflare Tunnel of section 5. (Google encrypts VM-to-VM traffic in its network; the agent refuses plain HTTP to
   anything but a loopback or private address.)
3. SSH into it (console button): `sudo apt-get install -y git python3 python-is-python3`; create the user `cerid-ci`;
   `ssh-keygen -t ed25519 -f /etc/cerid-ci/deploy-key -N ''` (owned by `cerid-ci`, mode 0600), add the public key to
   the repository as a **read-only deploy key** and store the private key as `cerid-repository-deploy-key`; then, as
   `cerid-ci`, clone the repository to `/opt/cerid` with that key and check out the revision you reviewed
   (`git checkout <tag or commit>`; upgrades in "Upgrades and backups" below).
4. Create `/etc/cerid-ci/launcher.env` (mode 0600, owner `cerid-ci`), values only you know:

   ```
   CERID_CI_URL=https://ci.<your domain>
   CERID_CI_INTERNAL_URL=http://<launcher internal IP>:8780
   CERID_CI_LISTEN=0.0.0.0:8780
   CERID_CI_REPOSITORY_URL=git@github.com:<owner>/<repository>.git
   CERID_CI_GIT_HOST=github
   CERID_CI_ACCESS_TEAM=<team>.cloudflareaccess.com
   CERID_CI_ACCESS_AUD=<application AUD tag>
   CERID_CI_UI_EMAILS=<you@example.com>
   CERID_CI_STATUS_REPOSITORY=<owner>/<repository>
   CERID_CI_GCP_PROJECT=cerid-ci
   CERID_CI_GCP_ZONE=us-central1-a
   CERID_CI_RUNNER_SERVICE_ACCOUNT=cerid-ci-runner@cerid-ci.iam.gserviceaccount.com
   CERID_CI_STORE=gs://cerid-ci-results
   CERID_CI_SCCACHE_BUCKET=cerid-ci-sccache
   CERID_CI_BUDGET_SUBSCRIPTION=projects/cerid-ci/subscriptions/cerid-ci-budget-launcher
   CERID_CI_MIRROR_DIR=/var/lib/cerid-ci/mirror
   GIT_SSH_COMMAND=ssh -i /etc/cerid-ci/deploy-key -o IdentitiesOnly=yes
   ```

5. `python3 /opt/cerid/ci/coordinator/main.py check` prints any missing value by name.
6. Two systemd units, both `User=cerid-ci`, `EnvironmentFile=/etc/cerid-ci/launcher.env`, `Restart=always`:
   `cerid-ci-coordinator.service` with `ExecStart=/usr/bin/python3 /opt/cerid/ci/coordinator/main.py serve`, and
   `cerid-ci-launcher-agent.service` with `ExecStart=/usr/bin/python3 /opt/cerid/ci/agent/agent.py start
   --coordinator http://127.0.0.1:8780 --mode long-running --registration-token-file /etc/cerid-ci/launcher-token
   --name launcher --build-path /var/lib/cerid-ci/launcher-builds`. Before starting the agent, create its token once:
   `python3 /opt/cerid/ci/ops.py agent-token create --name launcher --queue cerid-launcher --out
   /etc/cerid-ci/launcher-token` (run as `cerid-ci` with the environment file loaded; the token is written to the
   file, never printed). `sudo systemctl enable --now cerid-ci-coordinator cerid-ci-launcher-agent`. The
   coordinator's own log is JSON lines in `journalctl -u cerid-ci-coordinator`.
7. Bake the images once, from the launcher: `python3 /opt/cerid/ci/ops.py bake gce-linux-x64-intel`, then
   `gce-linux-arm64-ampere`, `gce-windows-x64`, `gce-linux-x64-nvidia-l4` and `gce-windows-x64-nvidia-t4` (one per
   image family; `ops.py status` lists any family still NOT BAKED). Rebake monthly or after a toolchain change; a
   rebake changes every input key on that family, so its cells rerun.
8. `python3 /opt/cerid/ci/ops.py health`, `accounts` and `status`: the coordinator ticks and every cloud cell reads
   `runnable`. The poller already builds new heads of `main` every two minutes; section 6 adds instant webhooks.

<a id="cloudflare-tunnel-and-access-for-the-web-ui"></a>
## 5. Cloudflare Tunnel and Access for the web UI

The web UI has no anonymous mode: the coordinator verifies Cloudflare Access's signed assertion on every UI request.

1. <https://dash.cloudflare.com/sign-up> (free) and add a domain you own to Cloudflare (any registrar; a domain is the
   only cost here, outside the CI budget).
2. **Zero Trust → Networks → Tunnels → Create a tunnel** (Cloudflared), name `cerid-ci`; follow its install command
   on the launcher (it installs `cloudflared` as a service with the tunnel's own token, which stays on the launcher).
   **Public hostname**: `ci.<your domain>` → service `http://localhost:8780`.
3. **Zero Trust → Access → Applications → Add an application → Self-hosted**: domain `ci.<your domain>`, policy
   "Allow", include "Emails" with your address (and your team's). Copy the **Application Audience (AUD) tag** into
   `CERID_CI_ACCESS_AUD` and the team domain (`<team>.cloudflareaccess.com`) into `CERID_CI_ACCESS_TEAM`.
4. In the same application, add a second policy **Bypass / Everyone** limited to the paths `/webhooks/*`,
   `/api/agent/*`, `/agent/*`, `/git/*` and `/healthz` (Access application paths): those authenticate themselves
   (HMAC signatures, job and agent tokens) and are called by git hosts and machines, not people. Every other path
   stays behind the Allow policy and is verified again by the coordinator.
5. Restart `cerid-ci-coordinator`; open `https://ci.<your domain>`: Access asks for a one-time code by email, then the
   builds page shows. Google IAP is the supported alternative (`ui.mode: iap`), but it needs an HTTPS load balancer,
   about 18 USD a month: a third of the cap, so Cloudflare Access is the recommendation.

<a id="webhooks-and-commit-status"></a>
## 6. Webhooks and commit status

1. **Webhook** (GitHub: repository **Settings → Webhooks → Add webhook**): payload URL
   `https://ci.<your domain>/webhooks/cerid`, content type `application/json`, secret: the value of
   `cerid-webhook-secret` (copy it from Secret Manager into this form only), events "Pushes" and "Pull requests".
   Gitea, Forgejo and Codeberg take the same form (`CERID_CI_GIT_HOST=gitea`); GitLab takes the value as its "Secret
   token" (`gitlab`); any other host can relay through the `generic` signature (design doc, surfaces). Pull requests
   from forks do not build (their own `ci/pipeline.py` would run on the launcher) unless you set `"build_forks": true`
   on the repository in `ci/coordinator.json`; pull requests from the repository itself build untrusted, and a step
   that declares secrets is refused for them.
2. **Commit status** (optional): a fine-grained personal access token limited to this repository with "Commit
   statuses: Read and write" only; store it as `cerid-status-token` and set `CERID_CI_STATUS_REPOSITORY`. Each build
   then reports `cerid-ci` pending, success or failure with a link to its page.
3. Without a webhook the poller still builds every new head of the watched branches within two minutes.

<a id="firebase-test-lab"></a>
## 7. Firebase Test Lab (Tier B phones and tablets)

1. In the **Firebase project** (the one `FTL_GCP_PROJECT` names), **IAM → Grant access**: principal
   `cerid-ci-launcher@cerid-ci.iam.gserviceaccount.com`, roles Firebase Test Lab Admin and Firebase Analytics Viewer.
   The Firebase project stays on Spark; nothing is billed there.
2. Add `CERID_FTL_PROJECT=<that project id>` to `/etc/cerid-ci/launcher.env`. The adapter keeps within the Spark
   plan's daily quota (5 physical and 10 virtual device tests) and marks the rest `budget`.

<a id="optional-ssh-hosts-amd-mac-windows-arm64-intel"></a>
## 8. Optional SSH hosts: AMD Developer Cloud, a rented Mac, Windows ARM64, Intel

Each is used only when all three of its names exist. For a host `X` (`AMD_DEVCLOUD`, `MAC`, `WINDOWS_ARM64`,
`INTEL_TIBER`):

1. Get the host from its provider (AMD Developer Cloud credit; an hourly Mac; an Azure Cobalt 100 Windows 11 ARM64
   spot VM or an owned Snapdragon laptop; Intel Tiber AI Cloud). Create an unprivileged user `cerid` with Python 3.12
   and git.
2. On the launcher: `ssh-keygen -t ed25519 -f /etc/cerid-ci/ssh-X -N ''`; put the public key in the host's
   `~cerid/.ssh/authorized_keys`; store the private key as the Secret Manager secret named in `ci/devices.json`
   (`amd-devcloud-ssh-key`, `mac-ssh-key`, `windows-arm64-ssh-key`, `intel-tiber-ssh-key`).
3. Record the host key: `ssh-keyscan <host> > /etc/cerid-ci/known-X`, and compare its fingerprint with the provider's
   console before trusting it.
4. Add `CERID_SSH_X_HOST=cerid@<host>`, `CERID_SSH_X_KEY_FILE=/etc/cerid-ci/ssh-X` and
   `CERID_SSH_X_KNOWN_HOSTS=/etc/cerid-ci/known-X` to the launcher environment. The host downloads the agent from
   `CERID_CI_URL` (through the tunnel) and checks its digest; its one job token arrives on standard input. An hourly
   Mac may name a power command (`power_on_command_env` in its cell) that the launcher runs before a Mac job.

<a id="owned-machines"></a>
## 9. Owned machines

1. `python ci/ops.py add-device --id owned-<name> --executor owned_agent ...` (or flip a planned owned cell's status
   to `active`). It prints the agent's command line and install steps.
2. On the launcher: `python3 /opt/cerid/ci/ops.py agent-token create --name owned-<name> --queue cerid-owned --tags
   device=owned-<name> --out /tmp/owned-<name>-token`, then move that file to the machine (`scp`), readable only by
   the agent's user, and delete the copy. The token's `device` tag pins the machine to its cell; the agent cannot
   claim another. Revoke it any time with `ops.py agent-token revoke --name owned-<name>`.
3. On the machine: a checkout of the repository at the revision the coordinator runs, then the printed command as a
   service (systemd, launchd, or a Windows service or logon task for a workstation that only runs between quiet
   sessions). It reaches the coordinator at `CERID_CI_URL` through the tunnel. Set `CERID_CI_OWNED_AGENTS=1` on the
   launcher so the planner schedules owned cells; the agent shows on the Agents page.
4. The result store: the step document already carries `CERID_CI_STORE`; the machine needs credentials for that
   bucket without a key file (`gcloud auth login` as the agent user, with your own account limited to the bucket).
   Without them a lane still passes or fails on its own evidence, but its result, cache entry and duration are not
   recorded (the lane logs "result store unavailable") and its dashboard cell keeps its previous state.

<a id="console-platform-packs"></a>
## 10. Console platform packs

When a platform program accepts Cerid (ID@Xbox / Partner Center, PlayStation Partners, the Nintendo Developer
Portal), its SDK, dev kit and agreement are the user's. The pack is a separate **private** repository with its own
access control, cloned on the dev-kit host to `external/platform-packs/<name>` (an ignored directory) or named by
`CERID_PLATFORM_PACKS`; it is never committed here. It carries a `devices.json` with this registry's schema (the
console cells move from "waiting on platform program" to active there) and its own lane scripts, and the dev-kit host
runs an owned agent ([ADR-0135 §8](../decisions/0135-cerid-ci-platform.md#8-console-platform-packs-the-plug-in-contract)).

<a id="optional-static-copy-of-the-qual-page"></a>
## 11. Optional: a static copy of the QUAL page

The coordinator serves the live QUAL page at `/qual` (the web UI's QUAL tab), behind Access. A static copy on
Cloudflare Pages is optional: create a Pages project `cerid-qual` (Direct Upload), protect it with its own Access
application, store an API token limited to Cloudflare Pages: Edit as `cloudflare-pages-token`, and run
`python ci/dashboard/generate.py publish` with `CERID_CLOUDFLARE_TOKEN_FILE` and `CERID_CLOUDFLARE_ACCOUNT_ID` set (the
locked wrangler on the pinned Node.js).

## Upgrades and backups

- **Backup:** `python3 /opt/cerid/ci/ops.py backup --out /var/backups/cerid-ci-$(date +%F).tar.gz` (the state and the
  result store with a SHA-256 manifest; `--all` adds caches, logs and artifacts). `restore <file>` verifies every
  object and keeps existing ones unless `--overwrite`. A weekly cron job on the launcher plus the bucket itself is
  enough.
- **Upgrade:** back up; `git fetch` and `git checkout <new reviewed revision>` in `/opt/cerid` as `cerid-ci`;
  `sudo systemctl restart cerid-ci-coordinator cerid-ci-launcher-agent`; `ops.py health`. Cloud machines fetch the new
  agent from the coordinator automatically; update each owned machine's checkout the same way. A coordinator refuses a
  store written by a newer schema, and an agent of another protocol is refused with the coordinator's version, so a
  half-done upgrade fails loudly. Roll back by checking out the previous revision.

## What it costs

`python ci/ops.py budget` prints the price of one complete run from the registry's prices and the recorded durations.
At the registry's default durations (90 minutes a lane) a complete run costs about 16 USD before the cache (on
2026-10-30; the Windows licence dominates), so three uncached complete runs fit a month; with the cache a nightly run
reruns only what changed, and the recorded durations replace the defaults after the first runs. The weekly fresh run
and the tier C GPU cells are the first to wait when the month's ledger nears the cap. The coordinator itself runs on
the free e2-micro; the bucket holds kilobytes of state and the logs of the retention window.

## Traps

- A token or secret pasted into a pipeline step, an environment block or a log is a leaked secret: the agent's
  redaction is a backstop, not a channel. Secrets travel only from Secret Manager to the step that declares them, and
  tokens only as files named by the recipe.
- The billing notification lags; the cap that actually stops spend first is the pipeline's ledger.
- A cell whose image family is not baked fails at launch (and is retried, then failed); `ops.py status` names it.
- Cloud machines are routed by the step's `device` tag; an owned agent can take only jobs whose tags its registration
  token carries.
- Object versioning on the results bucket must stay off: compare-and-swap relies on live generations, and versions
  would only accumulate cost.
