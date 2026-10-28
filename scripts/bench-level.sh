# The CPU level arm of a board that links its program by hand (ADR-0134 §8), sourced inside
# `wsl-lane.sh run <preset> -- bash -c` (S the source mirror, B the lane tree, which built cpu_level_record):
#   at_level <level|detected> <taskset CPU list> <program> [arguments]
# records the level line of $B/runtime/cpu_level_record under the program's CRD_CPU_LEVEL (unset for detected) and
# CPU list, stops the board when the level did not apply, then runs the program the same way.
at_level() {
    local level=$1 cpus=$2
    shift 2
    local select=()
    if [[ $level != detected ]]; then
        select=("CRD_CPU_LEVEL=$level")
    fi
    env "${select[@]}" taskset -c "$cpus" "$B/runtime/cpu_level_record" --expect "$level"
    env "${select[@]}" taskset -c "$cpus" "$@"
}
