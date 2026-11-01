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

Recover any single live `(offset, size, name)` triple — from a runtime probe of the loaded
record array (the 0x70-stride slots at `ctx+0x68d8`/`0x6948`/`0x69b8`, or the 0x58-byte
records they point at). One triple validates the cipher, and after that the pack becomes
readable, the title's tag and payload become visible, and a replacement can be built as a
same-tag payload served under the same name through the existing `_Fiopen` redirect.
