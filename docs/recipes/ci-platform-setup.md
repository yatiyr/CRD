# Setting up the Cerid CI platform

<!-- doc-role: reference -->
> Operator recipe. Live owner: [CI-1](../ROADMAP.md#slice-ci-1) to [CI-4](../ROADMAP.md#slice-ci-4); decision:
> [ADR-0135](../decisions/0135-cerid-ci-platform.md); contract: [CI platform](../design/ci-platform.md).

Every step here is a **user action**: it creates an account, turns on billing, issues a token or stores a credential.
No agent performs these steps. Do them in order; after each, `python ci/ops.py accounts` shows the input as configured
(by name only; it never prints a value). Steps 1 to 6 make the cloud lanes run; 7 to 10 add device classes.

## Parameters

| Name | Meaning | Default | Where it lives |
|---|---|---|---|
| `CERID_BUILDKITE_ORG` | Buildkite organization slug | (yours) | launcher environment |
| `buildkite-agent-token` | the cluster's agent token (`bkct_...`), used only by the launcher | — | Secret Manager in the CI project |
| `CERID_BUILDKITE_AGENT_TOKEN_FILE` | file the launcher reads that token from | `/run/cerid-ci/agent-token` | launcher |
| `buildkite-api-token` | optional REST token (read builds and agents) for `ops.py status` | — | Secret Manager |
| `CERID_CI_GCP_PROJECT` | the CI project | `cerid-ci` | launcher and runner environment |
| `CERID_CI_GCP_ZONE` | zone for machines | `us-central1-a` | launcher |
| `CERID_CI_RUNNER_SERVICE_ACCOUNT` | runner identity attached to every job VM | `cerid-ci-runner@cerid-ci.iam.gserviceaccount.com` | launcher |
| `CERID_CI_STORE` | result store | `gs://cerid-ci-results` | launcher and runners |
| `CERID_CI_SCCACHE_BUCKET` | compiler cache bucket | `cerid-ci-sccache` | runners |
| `CERID_CI_BUDGET_SUBSCRIPTION` | Pub/Sub pull subscription of the budget | `projects/cerid-ci/subscriptions/cerid-ci-budget-launcher` | launcher |
| monthly cap | the hard limit | 50 USD | `ci/devices.json` `budget.monthly_cap_usd` and the GCP budget |
| `CERID_FTL_PROJECT` | Firebase project for Test Lab (the `FTL_GCP_PROJECT` repository variable's value) | — | launcher |
| `cloudflare-pages-token`, `CERID_CLOUDFLARE_TOKEN_FILE`, `CERID_CLOUDFLARE_ACCOUNT_ID`, `CERID_CI_PAGES_PROJECT` | dashboard deployment | project `cerid-qual` | Secret Manager and launcher |
| `CERID_SSH_<HOST>_HOST`, `_KEY_FILE`, `_KNOWN_HOSTS` | an SSH host (AMD_DEVCLOUD, MAC, WINDOWS_ARM64, INTEL_TIBER) | — | launcher |

<a id="buildkite-organization-and-agent-token"></a>
## 1. The Buildkite organization and the agent token

1. Open <https://buildkite.com/signup>, sign up (the Free plan allows 5 users, 10 concurrent self-hosted jobs, 30 days
   of retention; checked 2026-10-30) and create an organization, for example `cerid`.
2. **Agents → Clusters → Default cluster** (or **Create a cluster** named `cerid`). In the cluster, **Queues → New
   queue** three times, self-hosted: `cerid-launcher`, `cerid-cloud`, `cerid-owned`.
3. In the cluster, **Agent tokens → New token**, description `cerid-launcher`. Copy the token once; do not paste it
   anywhere but step 3.6 below. (It starts with `bkct_`.)
4. Optional: **Personal settings → API access tokens → New API access token**, scopes `read_builds`, `read_agents`,
   organization `cerid`. Keep it for step 3.6 as `buildkite-api-token`.
5. **Pipelines → New pipeline**: name `cerid`, repository your Git remote (GitHub stays only a remote), cluster
   `cerid`. Replace the default steps with exactly:

   ```yaml
   steps:
     - label: ":pipeline: plan"
       command: python3 ci/pipeline.py upload
       agents: {queue: cerid-launcher}
   ```

6. **Pipeline settings → Schedules → New schedule**: `0 3 * * *` (UTC), branch `main`, label `nightly complete`. A
   second weekly schedule with environment `CERID_CI_FRESH=1` reruns everything regardless of the cache.
7. **Pipeline settings → GitHub** (or your Git host): build pushes to `main` and pull requests; turn off "skip
   builds for existing commits" so the planner, not the host, decides what is cached.
8. Verify the Stacks API is available to the organization: after step 5 of section 3, `python3 ci/pipeline.py dispatch
   --once` on the launcher prints no error from `/v3/stacks/register`. If Buildkite refuses it on your plan, stop and
   record it in CI-2: the dispatcher depends on one-job acquisition.

<a id="the-cerid-ci-project-and-billing"></a>
## 2. The `cerid-ci` Google Cloud project and its billing

1. <https://console.cloud.google.com/projectcreate>: project name `cerid-ci`, project id `cerid-ci` (or the nearest
   free id; use it everywhere below). Do **not** reuse the Firebase project.
2. **Billing → Link a billing account** to `cerid-ci` (create one if needed; a card is required). This is the only
   project that can spend.
3. **APIs & Services → Enable APIs**: Compute Engine, Cloud Storage, Secret Manager, Pub/Sub, Cloud Billing Budget,
   IAM Service Account Credentials.
4. **Cloud Storage → Buckets → Create**: `cerid-ci-results` (region `us-central1`, Standard, uniform access, public
   access prevention on) and `cerid-ci-sccache` (same; add a lifecycle rule "delete objects older than 30 days").
5. **IAM & Admin → Quotas**, region `us-central1`: set CPUs to 48, `NVIDIA_L4_GPUS` and `NVIDIA_T4_GPUS` to 1 each,
   preemptible CPUs to 48. The quotas bound concurrency, and with it the worst hour of spend.

<a id="the-50-usd-budget-and-its-automated-stop"></a>
## 3. The 50 USD budget and its automated stop

1. **Pub/Sub → Topics → Create topic** `cerid-ci-budget`; **Create subscription** `cerid-ci-budget-launcher`, pull,
   retention 7 days.
2. **Billing → Budgets & alerts → Create budget**: name `cerid-ci monthly`, scope project `cerid-ci`, all services,
   time range monthly, amount **50 USD**, thresholds 50 %, 90 % and 100 % of actual spend.
3. In the same budget, **Manage notifications → Connect a Pub/Sub topic to this budget** → `projects/cerid-ci/topics/
   cerid-ci-budget`. Cloud Billing then publishes the month's cost several times a day.
4. The launcher (section 5) sets `CERID_CI_BUDGET_SUBSCRIPTION=projects/cerid-ci/subscriptions/cerid-ci-budget-launcher`;
   every dispatcher tick pulls the notifications and, once the reported cost reaches the budget, starts no paid
   machine. `python3 ci/ops.py budget --pull` shows the state. The pipeline's own ledger caps the month independently
   and earlier, because the billing notification lags by hours.
5. Optional backstop: in **Billing → Budgets**, email alerts to yourself at 90 %.

<a id="service-accounts-and-workload-identity"></a>
## 4. Service accounts and Workload Identity

1. **IAM & Admin → Service accounts → Create**: `cerid-ci-launcher` and `cerid-ci-runner`.
2. Grant `cerid-ci-launcher`, on project `cerid-ci`: Compute Instance Admin (v1), Service Account User on
   `cerid-ci-runner` only, Storage Object Admin on both buckets, Secret Manager Secret Accessor, Pub/Sub Subscriber on
   `cerid-ci-budget-launcher`, Compute Image User.
3. Grant `cerid-ci-runner`: Storage Object Admin on both buckets, and Compute Instance Admin (v1) with the IAM
   condition `resource.name.extract("/instances/{name}").startsWith("cerid-ci-")` so a job machine can delete itself
   and nothing else.
4. Keyless identity: the launcher is a Compute Engine VM, so it uses its attached service account through the metadata
   server (no key file is ever created). If you run the launcher on an owned machine instead: **IAM → Workload Identity
   Federation → Create pool** `cerid-ci`, provider type OIDC, issuer `https://agent.buildkite.com`, audience
   `//iam.googleapis.com/projects/<number>/locations/global/workloadIdentityPools/cerid-ci/providers/buildkite`,
   attribute mapping `google.subject=assertion.sub`, condition
   `assertion.organization_slug == "cerid"`; grant the pool's principal Workload Identity User on `cerid-ci-launcher`;
   then `gcloud auth login --cred-file=<the generated credential configuration>` on that machine, whose executable
   source runs `buildkite-agent oidc request-token --audience <audience>`. Never download a service-account key.
5. **Secret Manager → Create secret**: `buildkite-agent-token` (paste the token of 1.3) and, if made,
   `buildkite-api-token` (1.4). Nobody else needs to read them.

## 5. The launcher

1. **Compute Engine → Create instance** `cerid-ci-launcher`: `e2-micro` in `us-central1-a` (free tier), Ubuntu 24.04
   LTS, 30 GB standard disk, service account `cerid-ci-launcher`, access scope "Allow full access to all Cloud APIs"
   (IAM roles above are the real limit).
2. SSH into it (console button) and install the pinned agent and Python:
   `sudo apt-get install -y git python3 curl`; clone the repository to `/opt/cerid`; then
   `python3 /opt/cerid/scripts/pins.py fetch tools buildkite-agent-linux-amd64 --destination /opt/cerid-agent` and
   extract `buildkite-agent` from the verified archive into `/usr/local/bin`.
3. Create `/etc/cerid-ci/launcher.env` (mode 0600), values only you know:

   ```
   CERID_BUILDKITE_ORG=cerid
   CERID_CI_GCP_PROJECT=cerid-ci
   CERID_CI_GCP_ZONE=us-central1-a
   CERID_CI_RUNNER_SERVICE_ACCOUNT=cerid-ci-runner@cerid-ci.iam.gserviceaccount.com
   CERID_CI_STORE=gs://cerid-ci-results
   CERID_CI_SCCACHE_BUCKET=cerid-ci-sccache
   CERID_CI_BUDGET_SUBSCRIPTION=projects/cerid-ci/subscriptions/cerid-ci-budget-launcher
   CERID_BUILDKITE_AGENT_TOKEN_FILE=/run/cerid-ci/agent-token
   ```

4. Two systemd units. `cerid-ci-dispatch.service`: `EnvironmentFile=/etc/cerid-ci/launcher.env`,
   `ExecStartPre=/bin/sh -c 'install -d -m 0700 /run/cerid-ci && gcloud secrets versions access latest
   --secret=buildkite-agent-token > /run/cerid-ci/agent-token'`, `ExecStart=/usr/bin/python3 /opt/cerid/ci/pipeline.py
   dispatch`, `Restart=always`. `cerid-ci-agent.service`: the same environment file, `ExecStart=/usr/local/bin/
   buildkite-agent start --token file:///run/cerid-ci/agent-token --tags queue=cerid-launcher --spawn 2`.
   `sudo systemctl enable --now cerid-ci-agent cerid-ci-dispatch`.
5. Bake the images once, from the launcher: `python3 /opt/cerid/ci/ops.py bake gce-linux-x64-intel`, then
   `gce-linux-arm64-ampere`, `gce-windows-x64`, `gce-linux-x64-nvidia-l4` and `gce-windows-x64-nvidia-t4` (one per
   image family; `ops.py status` lists any family still NOT BAKED). Rebake monthly or after a toolchain change; a
   rebake changes every input key on that family, so its cells rerun.
6. `python3 /opt/cerid/ci/ops.py accounts` and `status`: every cloud cell reads `runnable`.

<a id="firebase-test-lab"></a>
## 6. Firebase Test Lab (Tier B phones and tablets)

1. In the **Firebase project** (the one `FTL_GCP_PROJECT` names), **IAM → Grant access**: principal
   `cerid-ci-launcher@cerid-ci.iam.gserviceaccount.com`, roles Firebase Test Lab Admin and Firebase Analytics Viewer.
   The Firebase project stays on Spark; nothing is billed there.
2. Add `CERID_FTL_PROJECT=<that project id>` to `/etc/cerid-ci/launcher.env`. The adapter keeps within the Spark
   plan's daily quota (5 physical and 10 virtual device tests) and marks the rest `budget`.

<a id="cloudflare-pages-and-access-for-the-dashboard"></a>
## 7. Cloudflare Pages and Access for the dashboard

1. <https://dash.cloudflare.com/sign-up> (free). Note the **Account ID** (right column of the account home).
2. **Workers & Pages → Create → Pages → Direct Upload**, project name `cerid-qual`; upload any placeholder folder once.
3. **Zero Trust → Access → Applications → Add an application → Self-hosted**: application domain
   `cerid-qual.pages.dev` (and `*.cerid-qual.pages.dev` for previews), policy "Allow", include "Emails" with your
   address (and your team's). The dashboard is then private: Access asks for a one-time code by email.
4. **My Profile → API Tokens → Create token → Custom**: permission Account / Cloudflare Pages / Edit, this account
   only. Store it as the Secret Manager secret `cloudflare-pages-token`.
5. On the launcher add `CERID_CLOUDFLARE_ACCOUNT_ID=<id>`, `CERID_CI_PAGES_PROJECT=cerid-qual`,
   `CERID_CLOUDFLARE_TOKEN_FILE=/run/cerid-ci/cloudflare-token`, and to the agent unit an `ExecStartPre` that writes
   that file from the secret the same way as the agent token. The dashboard step then deploys with the locked
   wrangler on the pinned Node.js.

<a id="optional-ssh-hosts-amd-mac-windows-arm64-intel"></a>
## 8. Optional SSH hosts: AMD Developer Cloud, a rented Mac, Windows ARM64, Intel

Each is used only when all three of its names exist. For a host `X` (`AMD_DEVCLOUD`, `MAC`, `WINDOWS_ARM64`,
`INTEL_TIBER`):

1. Get the host from its provider (AMD Developer Cloud credit; an hourly Mac; an Azure Cobalt 100 Windows 11 ARM64
   spot VM or an owned Snapdragon laptop; Intel Tiber AI Cloud). Create an unprivileged user `cerid`.
2. On the launcher: `ssh-keygen -t ed25519 -f /etc/cerid-ci/ssh-X -N ''`; put the public key in the host's
   `~cerid/.ssh/authorized_keys`; store the private key as the Secret Manager secret named in `ci/devices.json`
   (`amd-devcloud-ssh-key`, `mac-ssh-key`, `windows-arm64-ssh-key`, `intel-tiber-ssh-key`).
3. Record the host key: `ssh-keyscan <host> > /etc/cerid-ci/known-X`, and compare its fingerprint with the provider's
   console before trusting it.
4. Add `CERID_SSH_X_HOST=cerid@<host>`, `CERID_SSH_X_KEY_FILE=/etc/cerid-ci/ssh-X` and
   `CERID_SSH_X_KNOWN_HOSTS=/etc/cerid-ci/known-X` to the launcher environment. An hourly Mac may name a power command
   (`power_on_command_env` in its cell) that the launcher runs before a Mac job.

<a id="owned-machines"></a>
## 9. Owned machines

1. `python ci/ops.py add-device --id owned-<name> --executor owned_agent ...` (or flip a planned owned cell's status
   to `active`). It prints the agent configuration and install steps.
2. On the machine: install the pinned agent (the digest is in the printed steps), write the agent token (a separate
   cluster token named after the machine, never the launcher's) to the printed token file, readable only by the agent
   user, and start the service. Set `CERID_CI_OWNED_AGENTS=1` on the launcher so the planner schedules owned cells.

<a id="console-platform-packs"></a>
## 10. Console platform packs

When a platform program accepts Cerid (ID@Xbox / Partner Center, PlayStation Partners, the Nintendo Developer
Portal), its SDK, dev kit and agreement are the user's. The pack is a separate **private** repository with its own
access control, cloned on the dev-kit host to `external/platform-packs/<name>` (an ignored directory) or named by
`CERID_PLATFORM_PACKS`; it is never committed here. It carries a `devices.json` with this registry's schema (the
console cells move from "waiting on platform program" to active there) and its own lane scripts, and the dev-kit host
runs an owned agent ([ADR-0135 §8](../decisions/0135-cerid-ci-platform.md#8-console-platform-packs-the-plug-in-contract)).

## What it costs

`python ci/ops.py budget` prints the price of one complete run from the registry's prices and the recorded durations.
At the registry's default durations (90 minutes a lane) a complete run costs about 16 USD before the cache (on
2026-10-30; the Windows licence dominates), so three uncached complete runs fit a month; with the cache a nightly run
reruns only what changed, and the recorded durations replace the defaults after the first runs. The weekly fresh run and the tier C GPU cells are the first to wait
when the month's ledger nears the cap.

## Traps

- A token pasted into a pipeline step, an environment block or a log is a leaked token: the agent's secret redaction
  is a backstop, not a channel. Tokens travel only as files named by `*_FILE` variables or as Secret Manager secrets.
- The billing notification lags; the cap that actually stops spend first is the pipeline's ledger.
- A cell whose image family is not baked fails at launch; `ops.py status` names it.
- Acquired jobs ignore agent tags: a job's `device=` rule, not a tag, decides where it runs.
