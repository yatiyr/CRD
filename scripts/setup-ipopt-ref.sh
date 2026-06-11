#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup-ipopt-ref.sh -- install the IPOPT reference (+ cyipopt) for the v7-z
# gold-standard NLP scoreboard (vs crd-hesap-opt's minimize_interior_point /
# minimize_sqp / minimize_auglag).
#
# PROBED 2026-06-10 (v7-n-2): WSL Ubuntu has python3.12 + pip 24; the apt
# package `coinor-libipopt-dev` (Ipopt 3.11.9) is AVAILABLE but needs sudo.
# Two paths:
#   FAST (correctness/iteration-count peer): the apt Ipopt 3.11 + cyipopt.
#       sudo apt-get install -y coinor-libipopt-dev pkg-config   # <- USER (sudo)
#       bash scripts/setup-ipopt-ref.sh
#   HONEST WALL-CLOCK peer (v7-z speed rows): Ipopt 3.11 is ~2014-era; build
#       a modern 3.14 via coinbrew with MUMPS (we already build MUMPS for the
#       v5 benches -- IPOPT can reuse it) and point cyipopt at it:
#       https://coin-or.github.io/coinbrew/  (document the exact recipe at v7-z).
#
# LOCAL-ONLY oracle (PRINCIPLES.md tak-cikar): Cerid SHIPS its own NLP stack;
# IPOPT is a scoreboard peer, never a dependency.
#
# Run (WSL):  bash scripts/setup-ipopt-ref.sh
# ---------------------------------------------------------------------------
set -euo pipefail

if ! dpkg -s coinor-libipopt-dev >/dev/null 2>&1; then
    echo "[setup-ipopt-ref] coinor-libipopt-dev is NOT installed."
    echo "    Run first (needs sudo):  sudo apt-get install -y coinor-libipopt-dev pkg-config"
    exit 1
fi

echo "[setup-ipopt-ref] Ipopt dev package present; installing cyipopt into the user site..."
pip3 install --user --break-system-packages cyipopt

python3 - <<'EOF'
import cyipopt
print(f"[setup-ipopt-ref] cyipopt {cyipopt.__version__} importable -- OK")
EOF

echo "[setup-ipopt-ref] DONE. The v7-z scoreboard scripts can now import cyipopt."
