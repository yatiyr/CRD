# Setting up the Cerid CI platform

<!-- doc-role: reference -->
> Operator recipe. Live owner: [CI-1](../ROADMAP.md#slice-ci-1) to [CI-4](../ROADMAP.md#slice-ci-4); decision:
> [ADR-0135](../decisions/0135-cerid-ci-platform.md) (the orchestrator: §12 to §16); contract:
> [CI platform](../design/ci-platform.md); day-to-day operations: [runbooks](ci-platform-runbooks.md).

The default route has no public endpoint at all: **polling** finds new commits (no webhook has to reach the
coordinator) and the web UI opens through a **Google IAP TCP tunnel** from your own computer, under your own Google
login. Tailscale (for a phone) and webhooks are optional add-ons; Cloudflare is the documented alternative.

There are two parts. **Part A is yours**: four steps that involve money and your Google account. **Part B is the
operator's runbook**: exact commands, each safe to run twice and each followed by a check, which the operator runs
with the `gcloud` you already installed and signed in to.

## Part A — your four steps

<a id="the-cerid-ci-project-and-billing"></a>
### A1. Create the `cerid-ci` project

1. Open <https://console.cloud.google.com/projectcreate>.
2. **Project name:** `cerid-ci`. Under the name, Google shows the **Project ID**; if `cerid-ci` is taken it proposes
   another (for example `cerid-ci-123456`). Write that ID down: the operator needs it.
3. **Location:** leave "No organization". Click **Create**.

*Check:* the project picker at the top of the console shows `cerid-ci`.

### A2. Link billing

1. Open <https://console.cloud.google.com/billing/linkedaccount?project=cerid-ci> (replace `cerid-ci` with your
   project ID if it differs).
2. Click **Link a billing account** and choose your billing account (or **Create billing account**, which asks for
   a card). Do not reuse the Firebase project; this is the only project that can spend.

*Check:* the page shows "This project is linked to billing account …".

<a id="the-50-usd-budget-and-its-automated-stop"></a>
### A3. Set the 50 USD budget

1. Open <https://console.cloud.google.com/billing/budgets> and click **Create budget**.
2. **Name:** exactly `cerid-ci monthly` (the operator finds it by this name). **Projects:** only `cerid-ci`.
   **Services:** all. **Time range:** monthly. Click **Next**.
3. **Budget type:** specified amount, **50** USD. Click **Next**.
4. **Thresholds:** 50 %, 90 % and 100 % of actual spend. Keep "Email alerts to billing admins". Click **Finish**.

*Check:* the budget list shows `cerid-ci monthly`, 50 USD. (The operator connects it to the automatic stop in B2.)

### A4. Say go

Tell the operator: "go, project ID `<your project ID>`". Saying go authorizes the operator to run Part B with your
signed-in `gcloud` in that project (create the buckets, the service accounts, the launcher VM, the firewall rules,
the secrets, bake the images) and, if the repository is private, to add one **read-only deploy key** for the launcher
to the repository with your signed-in GitHub CLI (`gh`). Nothing else on your accounts changes.

When Part B is done, the operator tells you to run `python ci/ops.py ui` (see [B9](#opening-the-web-ui)).

## Part B — the operator's runbook

Run from a checkout of this repository on the user's computer, in Git Bash (or any POSIX shell) with `gcloud`
signed in as the user. Every block is idempotent: running it again changes nothing that is already right. Every
block ends with a check whose expected output is stated.

```bash
export PROJECT=<project ID from A1>   # for example cerid-ci
export REGION=us-central1 ZONE=us-central1-a
export LAUNCHER_SA=cerid-ci-launcher@$PROJECT.iam.gserviceaccount.com
export RUNNER_SA=cerid-ci-runner@$PROJECT.iam.gserviceaccount.com
export ACCOUNT=$(gcloud config get-value account)
gcloud config set project $PROJECT
```

*Check:* `gcloud config get-value project` prints the project ID; `gcloud billing projects describe $PROJECT
--format='value(billingEnabled)'` prints `True`.

### B1. APIs, buckets and quotas

```bash
gcloud services enable compute.googleapis.com storage.googleapis.com secretmanager.googleapis.com \
  pubsub.googleapis.com billingbudgets.googleapis.com iap.googleapis.com iamcredentials.googleapis.com
for bucket in cerid-ci-results-$PROJECT cerid-ci-sccache-$PROJECT; do
  gcloud storage buckets describe gs://$bucket >/dev/null 2>&1 || gcloud storage buckets create gs://$bucket \
    --location=$REGION --uniform-bucket-level-access --public-access-prevention
done
printf '{"rule":[{"action":{"type":"Delete"},"condition":{"age":30}}]}' > /tmp/cerid-sccache-lifecycle.json
gcloud storage buckets update gs://cerid-ci-sccache-$PROJECT --lifecycle-file=/tmp/cerid-sccache-lifecycle.json
gcloud storage buckets update gs://cerid-ci-results-$PROJECT --no-versioning
```

The bucket names carry the project ID because bucket names are global. Object versioning stays off on the results
bucket: the orchestrator's compare-and-swap uses live object generations.

*Check:* `gcloud storage buckets list --format='value(name)'` lists both buckets;
`gcloud compute regions describe $REGION --format="table(quotas.metric,quotas.limit)" | grep -E "^CPUS |PREEMPTIBLE_CPUS|NVIDIA_(L4|T4)_GPUS"`
shows the regional limits. The planner runs at most six 8-vCPU cloud machines at once (48 vCPUs). If `CPUS` or
`PREEMPTIBLE_CPUS` is below 48 or a GPU limit is 0, request the increase in the console
(<https://console.cloud.google.com/iam-admin/quotas>, filter by region `us-central1`, select the quota, **Edit
quotas**); until Google grants it, launches past the quota fail and are retried, then reported.

### B2. The budget's automatic stop

```bash
gcloud pubsub topics describe cerid-ci-budget >/dev/null 2>&1 || gcloud pubsub topics create cerid-ci-budget
gcloud pubsub subscriptions describe cerid-ci-budget-launcher >/dev/null 2>&1 || gcloud pubsub subscriptions create \
  cerid-ci-budget-launcher --topic=cerid-ci-budget --message-retention-duration=7d
gcloud pubsub topics add-iam-policy-binding cerid-ci-budget \
  --member=serviceAccount:billing-budget-alert@system.gserviceaccount.com --role=roles/pubsub.publisher
export BILLING=$(gcloud billing projects describe $PROJECT --format='value(billingAccountName)' | sed 's#.*/##')
export BUDGET=$(gcloud billing budgets list --billing-account=$BILLING --filter="displayName='cerid-ci monthly'" \
  --format='value(name)')
gcloud billing budgets update $BUDGET --billing-account=$BILLING \
  --notifications-rule-pubsub-topic=projects/$PROJECT/topics/cerid-ci-budget
```

*Check:* `gcloud billing budgets describe $BUDGET --billing-account=$BILLING --format='value(notificationsRule.pubsubTopic)'`
prints `projects/<project>/topics/cerid-ci-budget`. If `BUDGET` is empty, the budget of A3 is missing or named
differently: ask the user, never create a budget yourself.

<a id="service-accounts-and-workload-identity"></a>
### B3. Service accounts and their roles

```bash
for sa in cerid-ci-launcher cerid-ci-runner; do
  gcloud iam service-accounts describe $sa@$PROJECT.iam.gserviceaccount.com >/dev/null 2>&1 || \
    gcloud iam service-accounts create $sa --display-name="$sa"
done
for role in roles/compute.instanceAdmin.v1 roles/compute.imageUser roles/secretmanager.secretAccessor; do
  gcloud projects add-iam-policy-binding $PROJECT --member=serviceAccount:$LAUNCHER_SA --role=$role --condition=None
done
gcloud iam service-accounts add-iam-policy-binding $RUNNER_SA --member=serviceAccount:$LAUNCHER_SA \
  --role=roles/iam.serviceAccountUser
gcloud pubsub subscriptions add-iam-policy-binding cerid-ci-budget-launcher --member=serviceAccount:$LAUNCHER_SA \
  --role=roles/pubsub.subscriber
for bucket in cerid-ci-results-$PROJECT cerid-ci-sccache-$PROJECT; do
  for sa in $LAUNCHER_SA $RUNNER_SA; do
    gcloud storage buckets add-iam-policy-binding gs://$bucket --member=serviceAccount:$sa --role=roles/storage.objectAdmin
  done
done
gcloud projects add-iam-policy-binding $PROJECT --member=serviceAccount:$RUNNER_SA \
  --role=roles/compute.instanceAdmin.v1 \
  --condition='expression=resource.name.extract("/instances/{name}").startsWith("cerid-ci-"),title=own-job-machines'
gcloud projects add-iam-policy-binding $PROJECT --member=user:$ACCOUNT --role=roles/iap.tunnelResourceAccessor \
  --condition=None
```

The runner account gets **no** Secret Manager role: a job receives only the secrets its step declares, from the
coordinator, and only in a trusted build. No service-account key is ever created: the launcher uses its attached
identity, every job VM proves its own.

*Check:* `gcloud projects get-iam-policy $PROJECT --flatten=bindings --filter="bindings.members:$RUNNER_SA"
--format='value(bindings.role)'` prints only `roles/compute.instanceAdmin.v1` (with the condition), and the same for
`user:$ACCOUNT` includes `roles/iap.tunnelResourceAccessor`.

### B4. Network: no public door

```bash
gcloud compute firewall-rules describe cerid-ci-iap >/dev/null 2>&1 || gcloud compute firewall-rules create \
  cerid-ci-iap --network=default --direction=INGRESS --action=allow --rules=tcp:8780,tcp:22 \
  --source-ranges=35.235.240.0/20 --target-tags=cerid-ci-launcher
gcloud compute firewall-rules describe cerid-ci-agents >/dev/null 2>&1 || gcloud compute firewall-rules create \
  cerid-ci-agents --network=default --direction=INGRESS --action=allow --rules=tcp:8780 \
  --source-ranges=10.128.0.0/9 --target-tags=cerid-ci-launcher
for rule in default-allow-ssh default-allow-rdp; do
  gcloud compute firewall-rules describe $rule >/dev/null 2>&1 && gcloud compute firewall-rules delete $rule --quiet
done
```

`35.235.240.0/20` is Google's IAP TCP-forwarding range: only principals holding the IAP-secured Tunnel User role
(B3) reach port 8780 (the UI) and 22 (SSH) through it. The job VMs reach the coordinator inside the network. The two
default rules that open SSH and RDP to the whole Internet are removed; nothing in this project needs them.

*Check:* `gcloud compute firewall-rules list --format="table(name,sourceRanges.list(),allowed[].map().firewall_rule().list())"`
shows `cerid-ci-iap` and `cerid-ci-agents` and no rule with source `0.0.0.0/0` on 22 or 3389.

### B5. Secrets

```bash
gcloud secrets describe cerid-repository-deploy-key >/dev/null 2>&1 || \
  gcloud secrets create cerid-repository-deploy-key --replication-policy=automatic
```

The key itself is generated on the launcher in B7 and added as a version there; no secret is ever typed or printed.
(The webhook secret of the add-on and the commit-status token are created only when those options are chosen.)

*Check:* `gcloud secrets list --format='value(name)'` lists `cerid-repository-deploy-key`.

<a id="the-coordinator-on-the-launcher"></a>
### B6. The launcher VM

```bash
gcloud compute instances describe cerid-ci-launcher --zone=$ZONE >/dev/null 2>&1 || gcloud compute instances create \
  cerid-ci-launcher --zone=$ZONE --machine-type=e2-micro --image-family=ubuntu-2404-lts-amd64 \
  --image-project=ubuntu-os-cloud --boot-disk-size=30GB --boot-disk-type=pd-standard \
  --service-account=$LAUNCHER_SA --scopes=cloud-platform --tags=cerid-ci-launcher --shielded-secure-boot
export INTERNAL=$(gcloud compute instances describe cerid-ci-launcher --zone=$ZONE \
  --format='value(networkInterfaces[0].networkIP)')
```

The e2-micro in `us-central1` is in Google's free tier. It keeps an ephemeral external address for outbound traffic
(git, packages); the console shows that address's hourly price when it applies. Nothing listens on it: the firewall
admits only B4's two sources.

*Check:* `gcloud compute ssh cerid-ci-launcher --zone=$ZONE --tunnel-through-iap --command='uname -a'` prints an
Ubuntu kernel line (the first SSH creates your key; accept the prompt).

### B7. Install the coordinator

```bash
ssh_l() { gcloud compute ssh cerid-ci-launcher --zone=$ZONE --tunnel-through-iap --command="$1"; }
ssh_l 'sudo apt-get update -q && sudo apt-get install -y -q git python3 python-is-python3 && \
  (id cerid-ci >/dev/null 2>&1 || sudo useradd --system --create-home --home-dir /var/lib/cerid-ci cerid-ci) && \
  sudo install -d -o cerid-ci -g cerid-ci -m 0700 /etc/cerid-ci /var/lib/cerid-ci/mirror'
ssh_l 'sudo test -f /etc/cerid-ci/deploy-key || sudo -u cerid-ci ssh-keygen -q -t ed25519 -N "" \
  -C cerid-ci-launcher -f /etc/cerid-ci/deploy-key'
ssh_l 'sudo cat /etc/cerid-ci/deploy-key | gcloud secrets versions add cerid-repository-deploy-key --data-file=- \
  >/dev/null && sudo cat /etc/cerid-ci/deploy-key.pub' > /tmp/cerid-ci-deploy-key.pub
```

If the repository is private, add the public key as a **read-only** deploy key (the user's go in A4 covers this one
GitHub change): `gh repo deploy-key add /tmp/cerid-ci-deploy-key.pub --repo <owner>/<repository> --title
cerid-ci-launcher` (no `--allow-write`). Then clone and configure:

```bash
export REPO=git@github.com:<owner>/<repository>.git REVISION=<the reviewed tag or commit>
ssh_l "sudo -u cerid-ci sh -c 'test -d /var/lib/cerid-ci/cerid || GIT_SSH_COMMAND=\"ssh -i /etc/cerid-ci/deploy-key \
  -o StrictHostKeyChecking=accept-new\" git clone -q $REPO /var/lib/cerid-ci/cerid; cd /var/lib/cerid-ci/cerid && \
  GIT_SSH_COMMAND=\"ssh -i /etc/cerid-ci/deploy-key\" git fetch -q origin && git checkout -q $REVISION'"
ssh_l "sudo tee /etc/cerid-ci/launcher.env >/dev/null <<EOF
CERID_CI_URL=http://localhost:8780
CERID_CI_INTERNAL_URL=http://$INTERNAL:8780
CERID_CI_LISTEN=127.0.0.1:8780,$INTERNAL:8780
CERID_CI_REPOSITORY_URL=$REPO
CERID_CI_GCP_PROJECT=$PROJECT
CERID_CI_GCP_ZONE=$ZONE
CERID_CI_RUNNER_SERVICE_ACCOUNT=$RUNNER_SA
CERID_CI_STORE=gs://cerid-ci-results-$PROJECT
CERID_CI_SCCACHE_BUCKET=cerid-ci-sccache-$PROJECT
CERID_CI_BUDGET_SUBSCRIPTION=projects/$PROJECT/subscriptions/cerid-ci-budget-launcher
CERID_CI_MIRROR_DIR=/var/lib/cerid-ci/mirror
GIT_SSH_COMMAND=ssh -i /etc/cerid-ci/deploy-key -o IdentitiesOnly=yes
EOF
sudo chmod 0600 /etc/cerid-ci/launcher.env && sudo chown cerid-ci /etc/cerid-ci/launcher.env"
ssh_l 'cd /var/lib/cerid-ci/cerid && sudo -u cerid-ci env $(sudo cat /etc/cerid-ci/launcher.env | xargs -d "\n") \
  python ci/coordinator/main.py check'
```

*Check:* the last command prints `configuration complete: 1 repositories, UI iap-tunnel`.

Two systemd units, both `User=cerid-ci`, `EnvironmentFile=/etc/cerid-ci/launcher.env`, `Restart=always`,
`WorkingDirectory=/var/lib/cerid-ci/cerid`:

```bash
ssh_l "sudo tee /etc/systemd/system/cerid-ci-coordinator.service >/dev/null <<'EOF'
[Unit]
Description=Cerid CI coordinator
After=network-online.target
[Service]
User=cerid-ci
EnvironmentFile=/etc/cerid-ci/launcher.env
WorkingDirectory=/var/lib/cerid-ci/cerid
ExecStart=/usr/bin/python3 ci/coordinator/main.py serve
Restart=always
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/var/lib/cerid-ci
[Install]
WantedBy=multi-user.target
EOF
sudo tee /etc/systemd/system/cerid-ci-launcher-agent.service >/dev/null <<'EOF'
[Unit]
Description=Cerid CI launcher agent
After=cerid-ci-coordinator.service
[Service]
User=cerid-ci
EnvironmentFile=/etc/cerid-ci/launcher.env
WorkingDirectory=/var/lib/cerid-ci/cerid
ExecStart=/usr/bin/python3 ci/agent/agent.py start --coordinator http://127.0.0.1:8780 --mode long-running --registration-token-file /etc/cerid-ci/launcher-token --name launcher --build-path /var/lib/cerid-ci/launcher-builds
Restart=always
[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload && sudo systemctl enable --now cerid-ci-coordinator"
ssh_l 'cd /var/lib/cerid-ci/cerid && (sudo test -f /etc/cerid-ci/launcher-token || sudo -u cerid-ci env \
  $(sudo cat /etc/cerid-ci/launcher.env | xargs -d "\n") python ci/ops.py token create --name launcher \
  --queue cerid-launcher --out /etc/cerid-ci/launcher-token) && sudo systemctl enable --now cerid-ci-launcher-agent'
```

*Check:* `ssh_l 'cd /var/lib/cerid-ci/cerid && sudo -u cerid-ci env $(sudo cat /etc/cerid-ci/launcher.env | xargs -d
"\n") python ci/ops.py health'` prints `coordinator: ticking` and exits 0; `ssh_l 'journalctl -u cerid-ci-coordinator
-n 5'` shows JSON lines, the first `coordinator_started`.

### B8. Bake the images

```bash
for device in gce-linux-x64-intel gce-linux-arm64-ampere gce-windows-x64 gce-linux-x64-nvidia-l4 gce-windows-x64-nvidia-t4; do
  ssh_l "cd /var/lib/cerid-ci/cerid && sudo -u cerid-ci env \$(sudo cat /etc/cerid-ci/launcher.env | xargs -d '\n') \
    python ci/ops.py device bake $device"
done
```

One bake per image family, up to an hour each (a bake is a paid machine; the Windows ones are the dearest). Rebake
monthly or after a toolchain change; a rebake changes every input key of that family, so its cells run again.

*Check:* `python ci/ops.py status` on the launcher shows no `NOT BAKED`.

<a id="opening-the-web-ui"></a>
### B9. Open the web UI (the user's daily command)

On the user's computer, in the repository: `python ci/ops.py ui`. It starts `gcloud compute start-iap-tunnel
cerid-ci-launcher 8780` to `localhost:8780` (another free port if that one is taken), waits until the coordinator
answers and opens the browser. Ctrl+C closes the tunnel. Set `CERID_CI_GCP_PROJECT` (or pass `--project`) when the
project ID is not `cerid-ci`.

*Check:* the browser shows the Builds page; the header names you as "operator (IAP tunnel)". The first build
appears within a minute of the next push to `main` (polling every 60 s), or start one with **New build**.

## Optional pieces

<a id="tailscale-add-on"></a>
### Tailscale (to open the UI from a phone)

1. On the launcher: install Tailscale (`curl -fsSL https://tailscale.com/install.sh | sh`, its official script),
   `sudo tailscale up --hostname cerid-ci` (the user approves the device in the Tailscale admin console: a user
   action) and note its address (`tailscale ip -4`).
2. Add a UI listener with Tailscale identity: append `,<tailscale ip>:8781=ui@tailscale` to `CERID_CI_LISTEN` and set
   `CERID_CI_UI_LOGINS=<the user's tailnet login>`; restart the coordinator. Identity comes from the local
   tailscaled (`whois` of the connecting peer), never from a header.
3. The tailnet policy (ACL) allows only that user to `tag:cerid-ci:8781`; install Tailscale on the phone and the
   Windows PC, sign in, open `http://cerid-ci:8781/` (MagicDNS).

*Check:* from a device signed in as another tailnet user the page answers 401.

<a id="webhooks-and-commit-status"></a>
### Webhooks and commit status

Polling (every 60 s, one `git ls-remote` per watched branch: about 1,440 small requests a day, far inside every
host's limits, and no inbound door) is the default because it needs no public endpoint. A webhook makes builds
start within seconds but needs the coordinator reachable from the git host: add it together with a public route
(Cloudflare below). Then create the secret `cerid-webhook-secret` (`openssl rand -hex 32 | gcloud secrets create
cerid-webhook-secret --data-file=-`), add `"webhook_secret": "cerid-webhook-secret"` to the repository in
`ci/coordinator.json`, add a `=webhook` listener, and in the repository's **Settings → Webhooks** (a user action)
point `https://<public host>/webhooks/cerid` at it with that secret, events "Pushes" and "Pull requests". Pull
requests from forks never build unless the repository sets `"build_forks": true`.

Commit status (optional, user action for the token): a fine-grained personal access token for this repository only,
"Commit statuses: Read and write", stored as `cerid-status-token`; set `CERID_CI_STATUS_REPOSITORY=<owner>/<name>`.

<a id="cloudflare-tunnel-and-access-for-the-web-ui"></a>
### The alternative: Cloudflare Tunnel and Access

For a public HTTPS name (needed by webhooks): a domain on Cloudflare, a Cloudflare Tunnel from the launcher to
`localhost:8780`, an Access application on that host (Allow: the user's e-mail; Bypass: `/webhooks/*`, `/healthz`),
`CERID_CI_UI_MODE=cloudflare-access`, `CERID_CI_ACCESS_TEAM=<team>.cloudflareaccess.com`,
`CERID_CI_ACCESS_AUD=<AUD tag>`. The coordinator verifies Access's signed assertion itself. Google IAP behind an HTTPS
load balancer (`iap` mode) also works but costs about 18 USD a month, a third of the cap.

### Notifications

Add channels to `notifications.channels` in `ci/coordinator.json` (failures, recoveries, budget thresholds, flaky
quarantines and first-bad-commit results; branch `main` by default):
`{"kind": "email", "host": "smtp.<provider>", "port": 587, "from": "...", "to": ["..."], "username": "...",
"password_secret": "cerid-smtp-password"}` (the user creates an app password at their provider: a user action) or
`{"kind": "ntfy", "url": "http://<ntfy host on the tailnet>/cerid-ci", "token_secret": "cerid-ntfy-token"}`.
`python ci/ops.py notify test` sends one test message.

<a id="firebase-test-lab"></a>
### Firebase Test Lab (Tier B phones and tablets)

In the Firebase project (the one `FTL_GCP_PROJECT` names): `gcloud projects add-iam-policy-binding <firebase
project> --member=serviceAccount:$LAUNCHER_SA --role=roles/cloudtestservice.testAdmin` and
`--role=roles/firebase.analyticsViewer`; add `CERID_FTL_PROJECT=<firebase project>` to `launcher.env`. The project
stays on Spark; the adapter keeps within 5 physical and 10 virtual tests a day and marks the rest budget.

<a id="optional-ssh-hosts-amd-mac-windows-arm64-intel"></a>
### SSH hosts: AMD Developer Cloud, a rented Mac, Windows ARM64, Intel

Each is used only when all three of its names exist. For a host `X` (`AMD_DEVCLOUD`, `MAC`, `WINDOWS_ARM64`,
`INTEL_TIBER`): get the host (a user action with the provider), create an unprivileged user `cerid` with Python 3.12
and git; on the launcher `ssh-keygen -t ed25519 -f /etc/cerid-ci/ssh-X -N ''`, put the public key in the host's
`authorized_keys`, record and compare the host key (`ssh-keyscan <host> > /etc/cerid-ci/known-X`), and add
`CERID_SSH_X_HOST=cerid@<host>`, `CERID_SSH_X_KEY_FILE` and `CERID_SSH_X_KNOWN_HOSTS`. An SSH host is outside the
project's network, so it needs a route to the coordinator: the Tailscale add-on (its agent API on the tailnet) or the
Cloudflare alternative.

<a id="owned-machines"></a>
### Owned machines

`python ci/ops.py device add --id owned-<name> --executor owned_agent ...` (or flip a planned owned cell to
`active`) prints the agent's command line. On the launcher `python ci/ops.py token create --name owned-<name>
--queue cerid-owned --tags device=owned-<name> --out /tmp/owned-<name>-token`; move the file to the machine, readable
only by the agent's user, and delete the copy. The machine reaches the coordinator through the Tailscale add-on (add
`=agent` to the tailnet listener) or a persistent `gcloud compute start-iap-tunnel` under an account with the tunnel
role. Set `CERID_CI_OWNED_AGENTS=1` on the launcher so the planner schedules owned cells.

<a id="console-platform-packs"></a>
### Console platform packs

When a platform program accepts Cerid, its SDK, dev kit and agreement are the user's. The pack is a separate private
repository, cloned on the dev-kit host to `external/platform-packs/<name>` or named by `CERID_PLATFORM_PACKS`, never
committed here; the dev-kit host runs an owned agent
([ADR-0135 §8](../decisions/0135-cerid-ci-platform.md#8-console-platform-packs-the-plug-in-contract)).

<a id="optional-static-copy-of-the-qual-page"></a>
### A static copy of the QUAL page

The coordinator serves the QUAL matrix live. A static copy on Cloudflare Pages is optional: a Pages project
`cerid-qual` behind its own Access application, an API token "Cloudflare Pages: Edit" stored as
`cloudflare-pages-token`, and `python ci/dashboard/generate.py publish` with `CERID_CLOUDFLARE_TOKEN_FILE` and
`CERID_CLOUDFLARE_ACCOUNT_ID`.

## What it costs

`python ci/ops.py cost` prints the month's spend, the forecast, the breakdown by lane, device or build, and the
price of one complete run before the cache (about 16 USD at the registry's default durations on 2026-10-30; the
Windows licence dominates). The launcher is free tier; the bucket holds kilobytes of state plus the logs of the
retention window. The other recurring charge is bucket operations: every idle agent poll (one every 15 s per
polling agent), every open live page (one read every 2 s per build or log on screen) and every heartbeat reads or
writes small objects; at Cloud Storage's per-operation prices this is a few USD a month at most with one or two
agents (check the current price list; `ops.py cost` shows only machine time). The weekly fresh run and the tier C GPU
cells are the first to wait when the month nears the cap.
