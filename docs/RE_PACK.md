# The `asset/asset` pack: format, cipher, and what is still unknown

Status: the **read path is reversed and implemented** (`tools/re/pack_crypt.py`); the
name→offset **index is not yet located**, so no real slice has been decrypted and the
implementation is unverified against game plaintext.

## Confirmed: the pack is encrypted, not compressed

- Size 306,794,369 bytes, single file, nothing else under `asset/`.
- Entropy over the first MiB is **8.000 bits/byte** with all 256 values present — the
  signature of encryption, not of a compressor (no literals, no repeated blocks).
- No plaintext names anywhere in the file: `entity_lobby_dagger_hyper`, `lobby`,
  `skull_giga` all return zero hits, although those strings exist in the executable.

## Confirmed: records are typed slices

From `FUN_140212920` (see `RE_TITLE.md`): a record is

| Field | Meaning |
|---|---|
| `+0x00` u32 | byte offset into the pack (`seekg`) |
| `+0x04` u32 | payload length (`malloc`) |
| `+0x40` u32 | type tag selecting the parser |

The loader seeks, reads `size` bytes, un-XORs them, and dispatches `buffer + 0x18` — so every
payload carries a **0x18-byte header**. Tags observed: `1` named sub-object graph, `2` compact
struct reader, `4` length-prefixed string + ints, `0x20` text-format parser, `0x100` audio
(`alGenBuffers`/`alBufferData`).

## Confirmed: the seed is a CRC-32 of the entry name

`FUN_140215280(0, name, len)` is a table-driven reflected CRC-32: it starts from `~param_1`,
mixes four bytes per iteration through a 256-entry table at RVA `0x36f700`, and returns
`~state`. The table in the shipped executable **matches the standard reflected polynomial
(`0xEDB88320`) byte for byte**, so the seed is exactly `zlib.crc32(name)`.

## Confirmed: the byte cipher

`FUN_140213370(stream, buffer, size, seed)` reads the slice and un-XORs it in place with a
rolling 16-bit state:

```
plain[i] = T0[ ((T1[(s >> 2) & 0xff] >> 1) + s) & 0xff ] ^ T1[s & 0xff] ^ cipher[i]
s        = (s + 1 + sbyte(cipher[i])) & 0xffff
```

with `T0` at RVA `0x361710` and `T1` at RVA `0x361810` (256 bytes each, both with ~160
distinct entries, i.e. real substitution tables). The state advances on the **ciphertext**
byte, which is why the stream is self-referencing: encryption is still trivial (produce the
byte, then advance), but a random access into a slice is not possible without replaying the
state from the slice start.

`tools/re/pack_crypt.py` implements both directions and reads the tables from the local
executable at run time, so **no game data is stored in this repository**.

```bash
python3 tools/re/pack_crypt.py --exe "$GAME/hyperdemon.exe" --pack "$GAME/asset/asset" \
    --name entity_lobby_dagger_hyper --offset 0 --size 128 --hex
```

## Confirmed: the keystream is only 10 bits wide, and it is validated against the pack

The state update and the key lookup both ignore the high six bits of the 16-bit state:
`T1[(s >> 2) & 0xff]` reads bits 2..9, `T1[s & 0xff]` reads bits 0..7, and the `+s` is masked to
8 bits. Therefore

* `key(s)` depends only on `s & 0x3ff` — checked exhaustively, `key(s) == key(s + 0x400)` for
  every state — so there are **1024 distinct keystream sequences** (they draw on only 253
  distinct byte values) and the CRC-32 seed only matters modulo `0x400`;
* two states that differ by a multiple of `0x400` stay that way forever.

That is why a brute force over the seed for a known plaintext region returns 64 seeds
(`s, s+0x400, s+0x800, ...`), not one.

**Validation against real ciphertext.** A run of encrypted zeros produces a fixed cycle of the
state walk, which is a 13-byte repetition — measured, the phase at offset 201,588,736 is
`934d63c7d23f8e23368f0dec69`, and other offsets show rotations of the same cycle
(`ec69934d...`, `4d63c7d2...`, `368f0dec...`). Of the 4,681 64 KiB blocks in the pack, 12 have
entropy below 7.0 and several sit at exactly **3.700 bits/byte with 13 distinct byte values**.
Decrypting those blocks yields all zeros for exactly the seeds congruent mod `0x400` and for no
others, which is a 1-in-1024 hit rate rather than chance: these are genuine plaintext zeros, so
the recurrence (state advancing on the *ciphertext* byte) and the table use are confirmed
against the shipped data.

**Backwards decryption.** `state_i = state_{i+1} - 1 - sbyte(cipher_i)`, so a slice can be
replayed in either direction from a known state. Walking back from 201,588,736 puts the start of
that zero run at 201,583,721; the bytes immediately before it decrypt to
`ff 00 00 ff ff 00 00 ff ff ...` (a coverage-mask pattern) and then to 7.5–7.9 bits/byte
data for at least a megabyte. Because every candidate seed agrees there — the keystream is
identical within a `0x400` class — that pattern is real plaintext, not a lucky guess.

**Hypothesis (not confirmed):** the pack allocates generous all-zero tails on payloads (the run
above is ~70 KiB), so a slice's plaintext end is easy to recognise, but recognising an end does
not reveal the slice's *start* offset, which is what the missing index would supply.

## Consequence: a known slice can be decrypted without its name

`tools/re/pack_crypt.py --crack` tries all 1024 keystreams for a given `(offset, size)` and ranks
the results by plausibility (entropy, share of `0x00`/`0xff`/`0x80`, printable ratio). So the
name→CRC seed never has to be recovered: one `(offset, size)` pair is enough, and the name can be
inferred afterwards from `zlib.crc32(name) & 0x3ff` matching the winning class.

## Confirmed in-game: the pack is opened through `_Fiopen`, which the loader already intercepts

From the instrumented run on the virtual display (`hyprlodr-files.jsonl`, `t_ms` 178, thread
328): `event: file_open`, `via: _Fiopen`, `requested: asset/asset`, `ok: true`. The container
is therefore opened by the C-runtime path, **not** `CreateFileA`, and `_Fiopen` is one of the
slots the loader already patches (`file_hook_detail`: `CreateFileA=patched(7) fopen=patched(1)
fopen_s=patched(1) _wfopen=patched(1) _Fiopen=patched(1)`). So serving a re-encrypted
`asset/asset` needs **no new hook**: a redirect registration for that name is enough.

The same run also shows the game opening
`C:/users/steamuser/AppData/Roaming/HyperDemon/{training,config,cache/scores}`, so those are the
per-user files a mod must not disturb.

### What the GL instrument can and cannot see (measured, not assumed)

`hyprlodr-gl.jsonl` over a full start-up contains only `delay_imports` 1, `capture_ready` 1,
`lookup` 4, `frame` 2, `repatch` 80, `frame_dump` 1 — and **zero** `shader`/`program`/`draw`
events. The frame records carry `program: -1`, so the engine's draw path still does not resolve
through the slots we own, even with 42 slots repatched across 72 modules. Frames and state can
be read at `wglSwapBuffers`; per-draw tracing needs a different instrument (the engine's own
dispatch, not the import table).

## Confirmed in live memory: the record table exists, and it stores no names

`native/win32/asset_probe.c` (new) scans the running process for the measured 0x58-byte
record layout and writes `hyprlodr-assets.jsonl`. Requirements per slot: `offset >= 0x18`,
`size >= 0x18`, `offset + size <= pack size`, a tag from the loader's dispatch set, and **three
consecutive slots** parsing (two produced hundreds of coincidences in unrelated memory).

What one instrumented run produced (400-record cap, hit immediately):

- **2 contiguous runs, longest 266 records**, and the members are *perfectly* contiguous:
  `232015452 + 3839395 = 235854847`, `235854847 + 349559 = 236204406`,
  `236204406 + 87415 = 236291821`, then `+87415 = 236379236` … That arithmetic cannot be
  coincidence, so these are the real per-kind slice tables the resolver walks.
- **Tag histogram: 266 × `0x100` (audio), 134 × `1`** over the 400 captured; other tags did
  not appear in the arrays the scan reached first.
- A raw dump of one record (offset `0dd4465c`, size `003a95a3`, tag at `+0x40` = `2`) confirms
  the field positions from the decompilation, and the `std::string` at `+0x20` reads
  **empty** (`size` 0 at `+0x30`, capacity 15 at `+0x38`, i.e. the small-string default).

**Names are not stored in these records.** The probe was then changed to report only slots whose
`std::string` looks like an asset name; over a full run it scanned **722 MB across 282 regions
and found zero**. Two consequences:

1. The name → record association must live elsewhere (the caller passes the name and the table is
   searched by something derived from it, e.g. a precomputed key), or in a parallel array that
   the scan did not reach.
2. Slices can still be identified **by content**, and that path does not need names at all.

### Content-based identification, in progress

Decrypting the first 64 bytes of each slice with all 1024 keys and ranking by the share of
`0x00`/`0xff` (a coverage-mask signature) gives a clear leader among the 267 slices ≥ 64 KiB:
`share 0.66, seed 0x287, offset 297650309, size 258588, tag 1`, then several at 0.52–0.56.
That leader is the strongest candidate yet for a 1-bit/mask payload, but it has **not** been
rendered or confirmed to contain glyphs.

**Correction to an earlier reading:** the `ff 00 00 ff` pattern found last session at offset
201,583,721 falls inside record `offset 200701587, size 971233, tag 0x100` — an *audio* slice.
So the "coverage mask" interpretation of that window is not supported; the bytes are decrypted
correctly (the key family is fixed by the zero run), but what they belong to is unexplained.
Treat "the title is a stencil mask" as a hypothesis from the screenshots, not a confirmed fact.

## Unverified

- **Correctness against real plaintext.** The cipher cannot be validated without a known
  `(offset, size)` pair: a brute-force over 35,319 candidate names from the executable at
  offset 0 produced no plausible plaintext, which is expected — offset 0 is not a named
  entry and the record's own offset is still unknown. Round-trip (`encrypt` → `decrypt`)
  is self-consistent but only proves the implementation is the inverse of itself.
- **Whether the per-slice seed is the bare name or a path-like key.** The CRC is taken over
  whatever string the record's name field holds; the exact spelling (for example
  `entity_lobby_dagger_hyper` versus a prefixed path) matters and is not yet known.

## Next concrete step

Recover any single live `(offset, size)` pair — from a runtime probe of the loaded
record array (the 0x70-stride slots at `ctx+0x68d8`/`0x6948`/`0x69b8`, or the 0x58-byte
records they point at). One triple validates the cipher, and after that the pack becomes
readable, the title's tag and payload become visible, and a replacement can be built as a
same-tag payload served under the same name through the existing `_Fiopen` redirect.
