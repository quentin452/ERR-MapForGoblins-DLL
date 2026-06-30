# Item-stack toggle — concurrent bucket-rebuild race (resolved)

Status: resolved 2026-06-30 (`fix/rebuild-markers-race`, commit on master line).

## Symptom

Access violation crash (`crash_320`, `MapForGoblins.dll +0x6B265`) when toggling the
`stack_identical_items` ("Stack identical items / ~5 m") checkbox in the F1 menu — especially toggling
it rapidly, or together with the require-fragment button. Faulting instruction was `mov (%rax),%rax`
inside an `std::unordered_map` **rehash** walk (FNV-hashed string key) — a corrupted node `next`
pointer ⇒ the map was mutated by two threads at once.

The other crash dumps from the same machine (`+0x1EB9999` in `eldenring.exe`) are an unrelated
recurring game/Proton crash — not us.

## Root cause

`rebuild_markers()` was added so the stack toggle rebuilds the marker buckets live. Its first version,
on the **UI/render thread**, did: `g_disk_built=false; g_disk_kicked=false; for(b) b.clear();
ensure_buckets()`. Resetting `g_disk_kicked` and clearing `g_buckets` did NOT wait for any in-flight
build. A second toggle (or a kick racing a rebuild) spawned a **second worker thread**, so two workers
ran `build_buckets_impl()` concurrently — both appending to `g_buckets` and rehashing the same
build-time `unordered_map` — and the render-thread `clear()` raced a worker's `push_back`. Result:
corrupted container internals → AV in the next rehash.

## Fix

Serialize all bucket builds through one worker:
- `g_disk_running` (atomic, CAS) guarantees only ONE build worker exists at a time.
- A rebuild requested mid-build sets `g_rebuild_pending`; the running worker loops once more (latest
  config wins), with a re-check after releasing `g_disk_running` to avoid a lost wakeup.
- The worker is the ONLY mutator of `g_buckets`: it clears+refills with `g_disk_built=false` held for
  the whole mutation, so `markers()` readers get the empty set, never a half-written one. The
  render/UI thread never touches `g_buckets`.

## Takeaway

Any "rebuild live on toggle" path that re-runs an async builder MUST serialize against the in-flight
build (single-worker + pending flag), and must keep the ready-latch false for the entire mutation.
Never clear/append a shared container from the UI thread while a worker may touch it. See
[[cluster-runtime-queue]] (related disable→enable→reopen stale-icon race).
