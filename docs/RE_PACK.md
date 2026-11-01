# The `asset/asset` pack: format, cipher, and what is still unknown

Status: the **read path is reversed, implemented and validated against real plaintext**
(`tools/re/pack_crypt.py`). The index is read live from the running process, names are recovered
from a parallel key array, and a slice decrypts to a verifiable TrueType font.

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

## Confirmed: the cipher is validated against real plaintext

The pack is decoded. Two things were missing and both are now answered: the live
`(offset, size)` records (from the probe's memory scan) and the fact that a slice's plaintext
can be recognised objectively, so the CRC seed never has to be guessed.

**Method.** The resolver binary-searches `std::string` keys, but the records it returns carry
empty names, so the keys live in a parallel array. The probe now also finds runs of *strictly
ascending* `0x20`-byte `std::string` elements (a `std::vector<std::string>`), and pairs them with
a record array by count. One such pair is a **24-entry key array**
`[6eye, bronze, gold, hand1bones, …, hyper, hyper2, silver, soratheye, sorathmask,
sorathtexture, sorathvector, unranked, unranked2]` matched to a 24-entry record array whose
offsets chain exactly, giving index-aligned name → `(offset, size, tag)`.

**The validation is objective, not a score.** For the eleven records of a newly seen
`tag 4096` array, scanning all 1024 keystreams for an sfnt magic in the first 32 bytes gives
**exactly one seed each**, and at that seed the magic sits at **`+0x18`** and a full 22-table
TrueType directory follows with every table inside the slice:

```
off=303451631 size=219736 tag=4096 seed=0x3f2  sfnt at +0x18, 22 tables
  DSIG GDEF GPOS GSUB LTSH OS/2 VDMX cmap cvt  fpgm gasp glyf hdmx head hhea
  hmtx loca maxp meta name post prep
```

One seed out of 1024 producing a self-consistent 22-entry table directory is not chance, so:
the cipher, the table-derived tables, the state recurrence and the 0x18-byte payload header are
all confirmed against shipped data, and `tag 4096` is **TrueType font**.

**Open question (not explained).** With the correct seed the first `0x18` bytes decrypt to the
same short string across unrelated records (`…x3_as`, then `02 00`), while everything after
`+0x18` is valid. Either the header is encoded differently from the payload or it is written
after decryption; the bytes after it are what matter and they are correct.

## Confirmed: the seed is the bare entry name, not a path

`zlib.crc32(name) & 0x3ff` reproduces the key class of real slices exactly. The eleven
`tag 4096` records decrypt to `Georgia` (Regular/Bold/Italic/Bold Italic), `Goethe Bold`,
`Mongolian Baiti`, `Tinos` (Regular/Bold), `Consolas`, `Font Awesome 5 Free Solid` and `Hack`,
and their key classes are `crc32` of `georgia`, `georgiab`, `georgiai`, `georgiaz`,
`goethebold`, `monbaiti`, `tinos`, `tinosb`, … — i.e. the record's local name, unprefixed and
without an extension. Materials behave the same way (`skinning`, `warp`, `portal`, `squid`,
`fract3`, `vs.ricochet_charge2` all decrypt with `crc32(name)`).

Consequence: a slice is addressable by *name alone* once its record's `(offset, size)` is known
from the live table, and no guessing is needed anywhere.

**Correction to the earlier pairing.** Index-aligning a name array with a record array is not
reliable: the 24-name skin array pairs with a 24-record run, but the members are off by a shift
(`hyper`'s own key class does not decrypt that array's `hyper`-position record). Names must be
assigned **by recovered key class**, which is what `tools/re/name_records.py` does. That is why
earlier conclusions drawn from positional pairing are superseded, not merely refined.

## Confirmed: slices round-trip byte-for-byte, and a patcher exists

For every named slice tested, `encrypt(decrypt(slice))` reproduces the shipped ciphertext
**exactly** (8/8, sizes 1,443 B – 9.96 MB, tags 1/2/8/4096). So `pack_crypt.encrypt` is the true
inverse of the game's reader, not merely the inverse of its own decoder — which is what makes a
replacement payload possible at all.

`tools/re/pack_patch.py` exploits it: it copies the pack while substituting one re-encrypted
slice, leaving every other byte alone. The check that matters is that a *no-change* patch
reproduces the file: patching `fract3` (offset 306,572,658, 7,039 B, seed `0x2b0`) into a copy of
the installed 306,794,369-byte pack yields a file with the **same sha256**
(`2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7`). That equality is only
possible if the substitution is byte-exact, so the delivery path is proven end-to-end offline.

| Constraint | Why |
|---|---|
| replacement must be **exactly** the slice's byte length | every other record's offset is fixed by the table, and `seekg` uses it |
| the seed comes from the **name**, so the name cannot change | the loader re-derives the seed from the name it looked up |
| patches go to a **new file**, never the installation | the original pack stays pristine and verifiable |

## Next concrete step

Name the remaining slices (`tools/re/name_records.py` currently recovers 10 of 863 because the
probe only harvests part of the key arrays), then identify the record the title screen draws
its lettering from and serve a replacement through `pack_patch.py` + the `_Fiopen` redirect.
