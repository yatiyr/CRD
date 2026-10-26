# tracy-syspower-checked-scan — applied by crd_patched_copy() to tracy/public/client/TracySysPower.cpp (0.14.1).
#
# Reading a RAPL domain's max_energy_range_uj cast fscanf's result to void, which GCC does not take as using a
# warn_unused_result value (linux-gcc-shipping's one warning in this file): a file that held no number left the range
# at whatever the scan stored. The scan's count is now checked and a failed scan leaves the range 0, the value a domain
# without the file has. An anchor that stops matching fails the configure (see docs/design/pinned-inputs.md).
crd_patch_replace(
    [[                    (void)fscanf( f, "%" PRIu64, &maxRange );
]]
    [[                    if( fscanf( f, "%" PRIu64, &maxRange ) != 1 ) // crd-patch: a scan that read no number
                    {
                        maxRange = 0;
                    }
]])
