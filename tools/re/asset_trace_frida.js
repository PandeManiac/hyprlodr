// Temporary, read-only trace for the verified HYPER DEMON executable build.
// Usage/controller: tools/re/asset_trace.py capture ...
// No target memory is written; all hooks observe function arguments/returns.

'use strict';

const TARGET = 'hand2';
const EXPECTED_IMAGE_BASE = ptr('0x140000000');
const HOOKS = [
  { name: 'asset_lookup', rva: 0x211e70, bytes: '488bc44489482089' },
  { name: 'record_loader', rva: 0x212920, bytes: '4056574155415641' },
  { name: 'decrypt_read', rva: 0x213370, bytes: '48895c2408488974' },
  { name: 'tag1_parser', rva: 0x205ec0, bytes: '44894c2420448944' },
  { name: 'mesh_parser', rva: 0x21a710, bytes: '488bc45541564157' },
];
const MAX_NAME_BYTES = 512;
const MAX_CAPTURE_BYTES = 256 * 1024 * 1024;
const startedAt = Date.now();
let nextRequestId = 1;
const resolverStacks = Object.create(null);
const loaderStacks = Object.create(null);

function emit(event, context, fields) {
  const row = Object.assign({
    event: event,
    request_id: context ? context.request_id : null,
    name: context ? context.name : null,
    elapsed_ms: Date.now() - startedAt,
    thread_id: Process.getCurrentThreadId(),
  }, fields || {});
  send(row);
}

function stackFor(table, tid) {
  const key = String(tid);
  if (!table[key]) table[key] = [];
  return table[key];
}

function topContext(table, tid) {
  const stack = table[String(tid)];
  // A nested non-target lookup deliberately pushes null. Returning the nearest
  // older non-null context would misattribute its read/decode to an outer hand2.
  return stack && stack.length ? stack[stack.length - 1] : null;
}

function readU32(address) {
  return address.readU32();
}

// Microsoft x64 std::string: 16-byte local buffer, then size and capacity.
function readMsvcString(object) {
  if (object.isNull()) return null;
  try {
    const size = object.add(0x10).readU64().toNumber();
    const capacity = object.add(0x18).readU64().toNumber();
    if (size > MAX_NAME_BYTES) return null;
    const data = capacity > 15 ? object.readPointer() : object;
    return data.readUtf8String(size);
  } catch (_) {
    return null;
  }
}

function sha256(address, length) {
  if (address.isNull() || length < 0 || length > MAX_CAPTURE_BYTES) return null;
  try {
    const source = new Uint8Array(address.readByteArray(length));
    const paddedLength = Math.ceil((length + 9) / 64) * 64;
    const bytes = new Uint8Array(paddedLength);
    bytes.set(source);
    bytes[length] = 0x80;
    const bitLength = length * 8;
    const view = new DataView(bytes.buffer);
    view.setUint32(paddedLength - 8, Math.floor(bitLength / 0x100000000), false);
    view.setUint32(paddedLength - 4, bitLength >>> 0, false);
    const constants = [
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    ];
    const state = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
    const rotr = (value, bits) => (value >>> bits) | (value << (32 - bits));
    const words = new Uint32Array(64);
    for (let block = 0; block < paddedLength; block += 64) {
      for (let i = 0; i < 16; ++i) words[i] = view.getUint32(block + i * 4, false);
      for (let i = 16; i < 64; ++i) {
        const x = words[i - 15], y = words[i - 2];
        const s0 = rotr(x, 7) ^ rotr(x, 18) ^ (x >>> 3);
        const s1 = rotr(y, 17) ^ rotr(y, 19) ^ (y >>> 10);
        words[i] = (words[i - 16] + s0 + words[i - 7] + s1) >>> 0;
      }
      let a=state[0], b=state[1], c=state[2], d=state[3];
      let e=state[4], f=state[5], g=state[6], h=state[7];
      for (let i = 0; i < 64; ++i) {
        const s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const choose = (e & f) ^ (~e & g);
        const t1 = (h + s1 + choose + constants[i] + words[i]) >>> 0;
        const s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const majority = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (s0 + majority) >>> 0;
        h=g; g=f; f=e; e=(d + t1) >>> 0;
        d=c; c=b; b=a; a=(t1 + t2) >>> 0;
      }
      state[0]=(state[0]+a)>>>0; state[1]=(state[1]+b)>>>0;
      state[2]=(state[2]+c)>>>0; state[3]=(state[3]+d)>>>0;
      state[4]=(state[4]+e)>>>0; state[5]=(state[5]+f)>>>0;
      state[6]=(state[6]+g)>>>0; state[7]=(state[7]+h)>>>0;
    }
    return state.map(value => value.toString(16).padStart(8, '0')).join('');
  } catch (_) {
    return null;
  }
}

const hashFixture = Memory.allocUtf8String('abc');
const hashFixtureDigest = sha256(hashFixture, 3);
if (hashFixtureDigest !== 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') {
  throw new Error('internal SHA-256 implementation failed its fixture');
}
send({
  event: 'tracer_preflight',
  sha256_self_test: 'passed',
  direct_target_memory_writes: 0,
});

function matchingLookupContext(tid, name) {
  const fromLoader = topContext(loaderStacks, tid);
  if (fromLoader && fromLoader.name === name) return fromLoader;
  const fromResolver = topContext(resolverStacks, tid);
  return fromResolver && fromResolver.name === name ? fromResolver : null;
}

function checkedFunction(module, hook) {
  const address = module.base.add(hook.rva);
  const observed = Array.from(new Uint8Array(address.readByteArray(hook.bytes.length / 2)))
    .map(value => value.toString(16).padStart(2, '0')).join('');
  if (observed !== hook.bytes) {
    throw new Error(hook.name + ' signature mismatch at RVA 0x' + hook.rva.toString(16) +
      ': expected ' + hook.bytes + ', observed ' + observed);
  }
  return address;
}

const module = Process.enumerateModules().find(m => m.name.toLowerCase() === 'hyperdemon.exe');
if (!module) throw new Error('hyperdemon.exe module was not found in the selected process');
if (!module.base.equals(EXPECTED_IMAGE_BASE)) {
  throw new Error('unexpected image base ' + module.base + ', expected ' + EXPECTED_IMAGE_BASE);
}
const addresses = Object.create(null);
HOOKS.forEach(hook => { addresses[hook.name] = checkedFunction(module, hook); });

Interceptor.attach(addresses.asset_lookup, {
  onEnter(args) {
    const tid = Process.getCurrentThreadId();
    const name = readMsvcString(args[2]);
    this.context = name === TARGET ? {
      request_id: nextRequestId++, name,
    } : null;
    stackFor(resolverStacks, tid).push(this.context);
    if (this.context) {
      emit('asset_lookup', this.context, {
        kind: args[1].toUInt32(),
        flags: args[3].toUInt32(),
      });
    }
  },
  onLeave(retval) {
    if (this.context) {
      emit('asset_lookup_result', this.context, {
        found: !retval.isNull(),
        result_pointer: retval.toString(),
      });
    }
    const stack = stackFor(resolverStacks, Process.getCurrentThreadId());
    stack.pop();
  },
});

Interceptor.attach(addresses.record_loader, {
  onEnter(args) {
    const tid = Process.getCurrentThreadId();
    const name = readMsvcString(args[2]);
    this.context = name === TARGET ? matchingLookupContext(tid, name) : null;
    if (name === TARGET && !this.context) this.context = { request_id: nextRequestId++, name };
    stackFor(loaderStacks, tid).push(this.context);
    if (this.context) {
      const record = args[1];
      try {
        emit('record_resolved', this.context, {
          archive_offset: readU32(record),
          encrypted_size: readU32(record.add(4)),
          type_tag: readU32(record.add(0x40)),
          record_pointer: record.toString(),
          flags: args[3].toUInt32(),
        });
      } catch (error) {
        emit('trace_error', this.context, { stage: 'record_resolved', message: String(error) });
      }
    }
  },
  onLeave() {
    stackFor(loaderStacks, Process.getCurrentThreadId()).pop();
  },
});

Interceptor.attach(addresses.decrypt_read, {
  onEnter(args) {
    const tid = Process.getCurrentThreadId();
    this.context = topContext(loaderStacks, tid);
    if (!this.context) return;
    this.buffer = args[1];
    this.length = args[2].toInt32();
    this.seed = args[3].toUInt32();
  },
  onLeave() {
    if (!this.context) return;
    emit('payload_decrypted', this.context, {
      size: this.length,
      seed: this.seed,
      key_class: this.seed & 0x3ff,
      payload_sha256: sha256(this.buffer, this.length),
      read_result_checked: false,
    });
  },
});

Interceptor.attach(addresses.tag1_parser, {
  onEnter(args) {
    const tid = Process.getCurrentThreadId();
    this.context = topContext(loaderStacks, tid);
    if (!this.context) return;
    this.data = args[1];
    this.length = args[2].toUInt32();
      // FUN_140212920 passes pvVar3 + 0x18 while retaining the full allocation
      // size as its length argument. Only size - 0x18 bytes are readable here.
      this.readableLength = this.length >= 0x18 ? this.length - 0x18 : 0;
    try {
      this.version = readU32(this.data);
      this.sectionSize = readU32(this.data.add(4));
    } catch (_) {
      this.version = null;
      this.sectionSize = null;
    }
    emit('tag1_parse_begin', this.context, {
      parser_input_size_argument: this.length,
      parser_input_readable_size: this.readableLength,
      graph_version: this.version,
      section_size: this.sectionSize,
      parser_input_sha256: sha256(this.data, this.readableLength),
    });
  },
  onLeave(retval) {
    if (!this.context) return;
    const result = retval.toInt32();
    emit('tag1_parse_result', this.context, {
      graph_version: this.version,
      parser_return: result,
      success: result === 1 && this.version === 3 && this.sectionSize !== null &&
        this.sectionSize + 8 <= this.readableLength,
    });
  },
});

Interceptor.attach(addresses.mesh_parser, {
  onEnter(args) {
    this.context = topContext(loaderStacks, Process.getCurrentThreadId());
    if (!this.context) return;
    this.object = args[0];
  },
  onLeave() {
    if (!this.context) return;
    try {
      const mesh = this.object;
      const indexCount = readU32(mesh.add(0x0c));
      const vertexCount = readU32(mesh.add(0x10));
      const stride = readU32(mesh.add(8));
      const complete = readU32(mesh.add(0x4c)) === 3;
      const vertexPointer = mesh.add(0x20).readPointer();
      const indexPointer = mesh.add(0x28).readPointer();
      const vertexBytes = vertexCount * stride;
      const indexBytes = indexCount * 4;
      const bounded = vertexCount > 0 && indexCount > 0 && stride > 0 &&
        vertexBytes <= MAX_CAPTURE_BYTES && indexBytes <= MAX_CAPTURE_BYTES;
      emit('mesh_decode', this.context, {
        completion_marker: readU32(mesh.add(0x4c)),
        complete,
        vertex_count: vertexCount,
        index_count: indexCount,
        vertex_stride: stride,
        vertex_array_sha256: bounded ? sha256(vertexPointer, vertexBytes) : null,
        index_array_sha256: bounded ? sha256(indexPointer, indexBytes) : null,
        arrays_bounded: bounded,
      });
    } catch (error) {
      emit('trace_error', this.context, { stage: 'mesh_decode', message: String(error) });
    }
  },
});

// This event is deliberately last: readiness means every interceptor is live.
emit('trace_ready', null, {
  module: module.name,
  module_base: module.base.toString(),
  module_size: module.size,
  hook_rvas: HOOKS.map(hook => '0x' + hook.rva.toString(16)),
  hook_installation: 'complete',
  signature_check: 'passed',
  sha256_self_test: 'passed',
  direct_target_memory_writes: 0,
});
