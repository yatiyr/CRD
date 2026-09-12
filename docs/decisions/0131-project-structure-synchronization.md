# ADR-0131 — Durable project structure edits from Visual Studio and agents

<!-- doc-role: decision -->
> Configuration amendment: [ADR-0132](0132-visual-studio-configuration-matrix.md) defines the full preset/native matrix.
> Decision contract. Only live work: [ROADMAP / REPO.SYNC](../ROADMAP.md#slice-repo.sync).

Date: 2026-09-12. Status: implementation decision within the user's explicit synchronization mandate.

The user requires generated Visual Studio project edits to become real source/build changes. Both removal modes
must be selectable: exclude while preserving the file, or delete the file. Moving a module between solution folders
must move its physical source directory and update references. Existing CMake, source families and cross-platform
builds remain supported; generated MSBuild files cannot become the Windows-only build authority.

Use a portable transaction engine shared by CLI/agent operations and a saved-project Visual Studio adapter.
CMake owns target semantics; a versioned, tracked structure manifest records deliberate source membership and
navigation overrides which CMake consumes on every platform. Generated projects are projections and edit inputs,
not independent build definitions. Never attempt to evaluate arbitrary MSBuild or rewrite arbitrary CMake logic.

Changes are compared against a recorded generation, validated as a batch, journaled with original bytes, and applied
under a repository lock with file preconditions. Conflicts stop before overwriting a different edit. Recovery checks
current bytes before restoring; neither rollback nor regeneration may silently discard work. Build output, vendor
projects, path escapes, symlinks/reparse points and compiler-generated files are outside the mutation surface.

Physical path moves preserve public target identity unless explicitly renamed. Source memberships, build references
and include/document links migrate with paths. New target definitions use explicit Cerid CMake semantics; unsupported
foreign project features produce an actionable conflict rather than partial import. No dependency is inferred from
a solution folder or ProjectReference without an explicit supported mapping.

[Implementation contract](../design/project-structure-sync.md) owns operation semantics, commands, recovery and
qualification. This does not change renderer, geometry, physics or public numerical algorithms.

Primary references: [CMake regeneration](https://cmake.org/cmake/help/latest/variable/CMAKE_SUPPRESS_REGENERATION.html),
[CMake file API](https://cmake.org/cmake/help/latest/manual/cmake-file-api.7.html),
[in-generation queries](https://cmake.org/cmake/help/latest/command/cmake_file_api.html),
[Visual Studio project filters](https://learn.microsoft.com/en-us/cpp/build/reference/vcxproj-filters-files?view=msvc-170),
[native CMake manipulation](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-170#cmake-project-manipulation).
