# MD380 Vocoder Cache – Design Notes & Ideas

## Current Approach: memcpy-based State Swapping

The MD380 firmware is linked to fixed memory addresses:

```
.firmware = 0x0800C000   (code, read-only)
.sram     = 0x20000000   (RAM, 128 KB)
```

The ARM code references absolute addresses for global variables, buffers, and
codec state. This means only **one firmware instance** can exist within a single
process address space. Running multiple instances (via fork or loading the
library twice) is not possible because both would map to the same virtual
address.

Since the AMBE2+ codec is stateful (pitch tracking, filter coefficients, and
synthesis state persist across frames), each active DMR stream needs its own
encoder state. The solution is a per-stream cache that saves and restores the
entire 128 KB SRAM via `memcpy` on stream switches.

### Why this works well enough

- With 2x AMBE3003 transcoders, the practical limit is ~3 concurrent streams
- 3 streams = 384 KB cache memory, negligible
- A 128 KB memcpy takes ~50-100 us on a Raspberry Pi
- DMR frames arrive every 60 ms, so even 2 swaps per frame use < 0.5% of the budget
- The mutex serialization is not a bottleneck at this stream count

### When it would become a problem

- 10-15+ simultaneous active streams (swap overhead and mutex contention)
- Aggressive cache eviction (30s timeout) causing state loss under heavy load
- Scenarios where every frame triggers a stream switch across many streams

Given the hardware constraints (2x AMBE3003), none of these are realistic.

---

## Alternative Approaches Considered

### 1. mmap Page Swapping

Instead of copying 128 KB, use `mmap` with `MAP_FIXED` to swap page table
entries at `0x20000000` between per-stream memory backings.

- **Pro:** O(1) kernel page table update instead of memcpy
- **Con:** Still need backing storage per stream; complexity of managing
  fd-backed or anonymous mappings; marginal gain at 128 KB
- **Verdict:** Not worth the added complexity for 3 streams

### 2. Multi-Process Worker Pool

Spawn N worker processes, each with its own virtual address space. Each process
owns one stream and runs its own firmware instance. Communication via Unix
sockets or shared memory.

- **Pro:** True parallelism, no swapping needed, clean isolation
- **Con:** IPC overhead, architectural complexity, process management
- **Verdict:** Good option if stream count ever grows significantly

### 3. Multiple Dynarmic Instances (x86_64 / aarch64 only)

On non-ARM platforms, the MD380 firmware runs inside dynarmic (ARM Cortex-M JIT
emulator). Dynarmic virtualizes memory internally, so multiple emulator
instances could each have their own independent SRAM at the emulated address
`0x20000000` without conflict.

- **Pro:** Cleanest solution, true parallelism, no page tricks needed
- **Con:** Only works with dynarmic, not on native ARM; requires changes to
  the md380_vocoder_dynarmic library
- **Verdict:** Most elegant approach for x86/aarch64 if needed in the future

---

## Conclusion

The current memcpy-based cache is simple, correct, and performant within the
hardware constraints. No changes needed unless the number of concurrent streams
grows well beyond what 2x AMBE3003 can handle.
