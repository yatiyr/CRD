#pragma once

// Umbrella header for crd-resources.
//
// v1a ships:
//   - ResourceId (128-bit UUID; mint_random, from_content, parse, to_string)
//   - CRDR chunked container reader + writer (no compression yet — v1b)
//   - ManifestEntry + manifest_write / manifest_read_entries
//   - ILoader interface (type-erased loader)
//   - ResourceManager shell (register_loader, mount_manifest, unmount; no loading yet)

#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/log_channel.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
