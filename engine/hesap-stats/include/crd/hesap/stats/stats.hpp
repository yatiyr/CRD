#pragma once

// crd-hesap-stats umbrella — Phase 3.1.6. STATISTICS substrate (the v12 cluster's module, created EARLY as a
// v12-pull: the Philox counter-based RNG was needed by v7-i for reproducible minibatch sampling with the full
// determinism moat, and pulling the tiny frozen-interface primitive forward beat shipping v7-i with an
// asterisk). The rest of the v12 stats cluster (distributions, estimators, hypothesis tests, ...) grows here.

#include <crd/hesap/stats/philox.hpp>
