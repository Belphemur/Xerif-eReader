#pragma once

// Batch page-turn policy for a queued-press jump (queued-input aggregation,
// docs/design/2026-10-27-queued-input-aggregation.md §3.2): |steps| single-step
// turns applied back to back with ONE render at the end of the dispatch. The
// pure loop decides only control flow — each step is the activity's existing
// pageTurn() logic (chapter crossing, EOB, prefetch promotion), unchanged.
namespace pageturn {

// One single-step turn's outcome, reported by the caller's step callback:
enum class Step : uint8_t {
  // The step moved the position cleanly; batching may continue.
  Turned,
  // The step moved but reached a boundary that needs a render/load before the
  // next step can be applied state-safely (chapter crossing, end of book) —
  // the jump clamps here.
  Boundary,
  // The step did not move (start of book going backward, failed step).
  Blocked,
};

// Runs up to |steps| single steps. Returns true when any step moved the
// position. steps == 0 (netted out) is a no-op with no step calls.
template <typename StepFn>
bool turnBy(int steps, StepFn&& step) {
  if (steps == 0) return false;
  const int magnitude = steps > 0 ? steps : -steps;
  const bool forward = steps > 0;
  bool turned = false;
  for (int i = 0; i < magnitude; ++i) {
    const Step outcome = step(forward);
    if (outcome == Step::Blocked) break;
    turned = true;
    if (outcome == Step::Boundary) break;
  }
  return turned;
}

}  // namespace pageturn
