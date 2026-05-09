#pragma once

// crd-profile umbrella header (Phase 3.0 v1n5; ADR-0060).
//
// v1n5 ships the substrate: ProfileContext + closed predicate schema +
// Profile runtime struct + ProfileResource + ProfileLoader +
// ProfileArtifactBuilder. v1n6 adds the resolver (additive composition,
// runtime context detection, hot-reload).

#include <crd/profile/profile.hpp>
#include <crd/profile/profile_artifact_builder.hpp>
#include <crd/profile/profile_context.hpp>
#include <crd/profile/profile_loader.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/profile/profile_resolver.hpp>
#include <crd/profile/profile_resource.hpp>
