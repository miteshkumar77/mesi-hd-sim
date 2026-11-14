# MESI cache coherency sim
Implements MESI cache coherency simulation with the following assumptions:
1. Home/Directory based approach, cores can only program their private cache through exchanging messages with the Home/Directory node.
2. Point-to-point messaging between cores and Home Nodes can be re-ordered.
3. Little Endian integer memory layout
4. cores can invoke load <byte_addr>, store <byte_addr> <uint64_t>, evict <line_addr>
5. A core can only have one active command at a time and they are executed FIFO for the core . I.e. `std::memory_order_seq_cst`.
    - consider the case where core 1 writes Y=0->1, reads X; core 2 writes X=0->1, reads Y. In our simulation, its impossible for both X and Y to read as 0. If core 1 reads X = 0, its write Y=0->1 must have happened before core 2 reads Y. And vice versa. 
6. Load/Store on unaligned addresses is natively supported. Load/Store on a word that spans two cache lines is non-atomic (like x86)
7. Cache is unbounded, so we do not simulate an eviction policy, though we've made it trivial to add by implementing evict.
8. L1/L2 are treated as one unified cache, no promotion/demotion.

## Optimizations
1. A core that is the sole sharer is promoted to exclusive always. This avoids an extra cache write miss when the core likely eventually invokes store.
2. A S-state core that requests E/M state directly is promoted once all other S-state cores are demoted to I, instead of also first being demoted to I.

## TODOs
1. Implement F state (MESIF) to allow core-core data forwarding instead of reading contents from shared cache or DRAM on every cache miss.
2. Add read/write-miss and DRAM vs F-state read counters on every operation completion.

## Interactive traces

Replay files for `mesi_hd_interactive_sim_exe`. Each was recorded by driving the TUI in
interactive mode, choosing the order in which bus messages are delivered, and saving the session from
the Save panel. Each file starts with a comment describing the scenario.

All files assume 4 cores, the default single home and the default interactive strategy, because
`D TOKEN` lines refer to the oracle tokens that configuration hands out:

```
bazel run //sim:mesi_hd_interactive_sim_exe -- \
    -o traffic.txt -c 4 -r $PWD/interactive_traces/07_invalidation_overtakes_grant.txt
```

Replay stops after the last action fires. Step a few more ticks to let the last command complete.

The cases are standard MESI transitions and races, as described in the
[MESI protocol article](https://en.wikipedia.org/wiki/MESI_protocol) and in Nagarajan, Sorin, Hill & Wood,
*A Primer on Memory Consistency and Cache Coherence* (2nd ed.), chapters 7–8.

| File | Case |
|------|------|
| `01_exclusive_then_silent_upgrade` | Sole reader is granted E; a store upgrades E→M with no bus traffic |
| `02_exclusive_downgraded_by_remote_read` | Clean E copy downgraded to S by a remote read (ack carries no data) |
| `03_dirty_downgrade_writeback` | M copy downgraded to S; dirty data written back and forwarded to the reader |
| `04_dirty_ownership_migration` | Dirty line migrates M→M between cores without losing either core's write |
| `05_shared_upgrade_invalidates_sharers` | S→M upgrade invalidates every other sharer before the write grant |
| `06_concurrent_upgrade_race` | Two sharers upgrade at once; the loser is invalidated while its own upgrade is outstanding |
| `07_invalidation_overtakes_grant` | ProbeI reaches a cache before the Grant it targets; the probe is deferred |
| `08_downgrade_overtakes_grant` | ProbeS reaches a cache before its write Grant; the probe is deferred, then answered with a write-back |
| `09_dirty_eviction_writeback` | Evicting an M line writes it back; a later read is served from home memory |
| `10_eviction_races_remote_write_getm_first` | Dirty eviction vs. remote GetM, GetM reaches the home first |
| `11_eviction_races_remote_write_evict_first` | Dirty eviction vs. remote GetM, EvictReq reaches the home first |
| `12_shared_eviction_races_invalidation` | Clean S eviction overtaken by an invalidation; the stale EvictReq is just acknowledged |
| `13_false_sharing_ping_pong` | Two cores alternately write different words of one line |
| `14_split_word_across_lines` | Unaligned 8-byte store/load at `0x7f` needs both lines `0x40` and `0x80` |
