# HYPRLODR project plan

## Goal
Build the first verifiable native plugin loading vertical slice in this empty checkout, while keeping game integration claims evidence-based.

## Phases
- [x] Inspect workspace, Git state, and local analysis/build tools.
- [ ] Milestone 1 foundation: define a versioned C ABI, synthetic host, and independently compiled diagnostic plugin.
- [ ] Expand native host into deterministic dependency manager and five-mod suite.
- [ ] Add cross-platform local game diagnostics and accurate project documentation.
- [ ] Evaluate loader integration candidates from actual local game evidence; prepare a safe local test package.
- [ ] Establish build/test workflow and record actual results and limitations.

## Current design decision
Start with a small C ABI and a synthetic host that loads a separately built shared library. This directly tests the top priority without assuming the proprietary executable is available. Keep the first host portable for local Linux verification; add Windows x64 behavior behind the same API after the vertical slice. Do not patch the game or add an audio proxy until local diagnostics and binary evidence support that choice.

## Errors encountered
| Error | Attempt | Resolution |
|---|---|---|
| Workspace has no Git repository or source files | Initial inspection | Confirmed empty except `.freebuff`; initialized a new `main` branch repository after requesting write access. |

## Next Step
Implement the C ABI and synthetic host vertical slice.
