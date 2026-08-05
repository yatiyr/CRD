// RAF-12.4: `IRasterContext::create_command_encoder()` is now PURE VIRTUAL — each backend returns its own
// `detail::CommandEncoder<Concrete>` (see detail/command_lowering.hpp and each backend's `create_command_encoder`
// override). No base definition remains here, so the base can no longer instantiate `CommandEncoder<IRasterContext>`
// — which is exactly what lets Phase B de-virtualize the verbs OFF IRasterContext. This TU is intentionally empty and
// is removed from the gpu-context target at the RAF-12.4 Phase C cleanup.
