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
