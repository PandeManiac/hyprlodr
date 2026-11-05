#define HYPRLODR_MINIMAL_WIN32 1
#include "route_manifest.h"
#include "crt/stdio.h"
#include "crt/stdlib.h"
#include "crt/string.h"
#include "hypr_win32.h"

/* Deliberately small strict JSON reader: strings in this schema are ASCII and
 * no escaped strings are emitted by pack_append.py. Rejecting escapes also
 * avoids alternate spellings of paths and identity strings. */
#define JSON_LIMIT 16384u
#define TOKEN_LIMIT 512u
typedef enum {
  J_OBJECT,
  J_ARRAY,
  J_STRING,
  J_NUMBER,
  J_TRUE,
  J_FALSE,
  J_NULL
} jtype;
typedef struct {
  jtype type;
  unsigned start, end, next, child;
} jtoken;
typedef struct {
  const char *s;
  unsigned length, used, depth;
  jtoken t[TOKEN_LIMIT];
} jdoc;
static int bytes_equal(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return 0;
  return 1;
}

static void ws(jdoc *d, unsigned *p) {
  while (*p < d->length && (d->s[*p] == ' ' || d->s[*p] == '\t' ||
                            d->s[*p] == '\r' || d->s[*p] == '\n'))
    ++*p;
}
static int parse_value(jdoc *d, unsigned *p, unsigned *out);
static int parse_string(jdoc *d, unsigned *p, unsigned *out) {
  if (*p >= d->length || d->s[*p] != '"' || d->used >= TOKEN_LIMIT)
    return 0;
  unsigned id = d->used++, begin = ++*p;
  while (*p < d->length && d->s[*p] != '"') {
    unsigned char c = (unsigned char)d->s[*p];
    if (c < 0x20 || c == '\\')
      return 0;
    ++*p;
  }
  if (*p >= d->length)
    return 0;
  d->t[id] = (jtoken){J_STRING, begin, *p, 0, 0};
  ++*p;
  *out = id;
  return 1;
}
static int parse_value(jdoc *d, unsigned *p, unsigned *out) {
  ws(d, p);
  if (*p >= d->length || d->used >= TOKEN_LIMIT)
    return 0;
  unsigned id = d->used++, start = *p;
  char c = d->s[*p];
  if (c == '"') {
    --d->used;
    return parse_string(d, p, out);
  }
  if (c == '{' || c == '[') {
    int object = c == '{';
    if (++d->depth > 32)
      return 0;
    ++*p;
    d->t[id] = (jtoken){object ? J_OBJECT : J_ARRAY, start, 0, 0, 0};
    ws(d, p);
    unsigned previous = 0;
    int have = 0;
    while (*p < d->length && d->s[*p] != (object ? '}' : ']')) {
      unsigned key = 0, val = 0;
      if (object) {
        if (!parse_string(d, p, &key))
          return 0;
        ws(d, p);
        if (*p >= d->length || d->s[(*p)++] != ':')
          return 0;
        /* Duplicate keys are rejected at every object depth. */
        for (unsigned q = d->t[id].child; q;) {
          unsigned v = d->t[q].next;
          if (d->t[q].end - d->t[q].start == d->t[key].end - d->t[key].start &&
              bytes_equal(d->s + d->t[q].start, d->s + d->t[key].start,
                          d->t[q].end - d->t[q].start))
            return 0;
          q = d->t[v].next;
        }
      }
      if (!parse_value(d, p, &val))
        return 0;
      if (object) {
        if (!have)
          d->t[id].child = key;
        else
          d->t[previous].next = key;
        d->t[key].next = val;
        previous = val;
      } else {
        if (!have)
          d->t[id].child = val;
        else
          d->t[previous].next = val;
        previous = val;
      }
      have = 1;
      ws(d, p);
      if (*p < d->length && d->s[*p] == ',') {
        ++*p;
        ws(d, p);
        if (*p >= d->length || d->s[*p] == (object ? '}' : ']'))
          return 0;
        continue;
      }
      break;
    }
    if (*p >= d->length || d->s[(*p)++] != (object ? '}' : ']'))
      return 0;
    d->t[id].end = *p;
    --d->depth;
    *out = id;
    return 1;
  }
  if (c >= '0' && c <= '9') {
    if (c == '0' && *p + 1 < d->length && d->s[*p + 1] >= '0' &&
        d->s[*p + 1] <= '9')
      return 0;
    while (*p < d->length && d->s[*p] >= '0' && d->s[*p] <= '9')
      ++*p;
    d->t[id] = (jtoken){J_NUMBER, start, *p, 0, 0};
    *out = id;
    return 1;
  }
  const char *word = c == 't'   ? "true"
                     : c == 'f' ? "false"
                     : c == 'n' ? "null"
                                : NULL;
  if (!word)
    return 0;
  unsigned n = (unsigned)strlen(word);
  if (*p + n > d->length || !bytes_equal(d->s + *p, word, n))
    return 0;
  *p += n;
  d->t[id] = (jtoken){c == 't'   ? J_TRUE
                      : c == 'f' ? J_FALSE
                                 : J_NULL,
                      start, *p, 0, 0};
  *out = id;
  return 1;
}
static int eq(jdoc *d, unsigned id, const char *s) {
  return d->t[id].type == J_STRING &&
         d->t[id].end - d->t[id].start == strlen(s) &&
         bytes_equal(d->s + d->t[id].start, s, strlen(s));
}
static unsigned field(jdoc *d, unsigned object, const char *name) {
  if (object >= d->used || d->t[object].type != J_OBJECT)
    return TOKEN_LIMIT;
  for (unsigned k = d->t[object].child; k;) {
    unsigned v = d->t[k].next;
    if (eq(d, k, name))
      return v;
    k = d->t[v].next;
  }
  return TOKEN_LIMIT;
}
static int number(jdoc *d, unsigned id, unsigned long long *v) {
  if (id >= d->used || d->t[id].type != J_NUMBER)
    return 0;
  unsigned long long n = 0;
  for (unsigned p = d->t[id].start; p < d->t[id].end; p++) {
    unsigned x = (unsigned)(d->s[p] - '0');
    if (n > (~0ull - x) / 10)
      return 0;
    n = n * 10 + x;
  }
  *v = n;
  return 1;
}
static int string_value(jdoc *d, unsigned id, char *out, unsigned cap) {
  if (id >= d->used || d->t[id].type != J_STRING)
    return 0;
  unsigned n = d->t[id].end - d->t[id].start;
  if (n + 1 > cap)
    return 0;
  memcpy(out, d->s + d->t[id].start, n);
  out[n] = 0;
  return 1;
}

/* Windows CNG keeps hashing independent of the proxy's freestanding CRT. */
typedef void *BCRYPT_HANDLE;
typedef long NTSTATUS;
typedef unsigned long ULONG;
extern NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_HANDLE *,
                                                   const wchar_t *,
                                                   const wchar_t *, ULONG);
extern NTSTATUS WINAPI BCryptCreateHash(BCRYPT_HANDLE, BCRYPT_HANDLE *,
                                        unsigned char *, ULONG, unsigned char *,
                                        ULONG, ULONG);
extern NTSTATUS WINAPI BCryptHashData(BCRYPT_HANDLE, unsigned char *, ULONG,
                                      ULONG);
extern NTSTATUS WINAPI BCryptFinishHash(BCRYPT_HANDLE, unsigned char *, ULONG,
                                        ULONG);
extern NTSTATUS WINAPI BCryptDestroyHash(BCRYPT_HANDLE);
extern NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_HANDLE, ULONG);
typedef struct { long long QuadPart; } route_large_integer;
extern BOOL WINAPI SetFilePointerEx(HANDLE, route_large_integer,
                                    route_large_integer *, DWORD);
#define FILE_BEGIN 0u
static int hash_region(const char *path, unsigned long long amount,
                       unsigned long long offset, char hex[65]) {
  static const wchar_t sha[] = {'S', 'H', 'A', '2', '5', '6', 0};
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE)
    return 0;
  DWORD high = 0, low = GetFileSize(f, &high);
  unsigned long long size = ((unsigned long long)high << 32) | low;
  if (amount == ~0ull)
    amount = size;
  if (size == 0xffffffffffffffffull || offset > size ||
      amount > size - offset) {
    CloseHandle(f);
    return 0;
  }
  route_large_integer position = {(long long)offset};
  if (!SetFilePointerEx(f, position, NULL, FILE_BEGIN)) {
    CloseHandle(f);
    return 0;
  }
  BCRYPT_HANDLE alg = 0, h = 0;
  unsigned char object[256], digest[32], buffer[65536];
  int ok = 0;
  if (BCryptOpenAlgorithmProvider(&alg, sha, NULL, 0) < 0)
    goto done;
  if (BCryptCreateHash(alg, &h, object, sizeof object, NULL, 0, 0) < 0)
    goto done;
  while (amount) {
    DWORD want = amount > sizeof buffer ? (DWORD)sizeof buffer : (DWORD)amount,
          got = 0;
    if (!ReadFile(f, buffer, want, &got, NULL) || got != want ||
        BCryptHashData(h, buffer, got, 0) < 0)
      goto done;
    amount -= got;
  }
  if (BCryptFinishHash(h, digest, sizeof digest, 0) < 0)
    goto done;
  static const char a[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; i++) {
    hex[i * 2] = a[digest[i] >> 4];
    hex[i * 2 + 1] = a[digest[i] & 15];
  }
  hex[64] = 0;
  ok = 1;
done:
  if (h)
    BCryptDestroyHash(h);
  if (alg)
    BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(f);
  return ok;
}
static int file_size(const char *path, unsigned long long *out) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE)
    return 0;
  DWORD hi = 0, lo = GetFileSize(f, &hi);
  CloseHandle(f);
  if (lo == 0xffffffffu && hi == 0)
    return 0;
  *out = ((unsigned long long)hi << 32) | lo;
  return 1;
}
static int get_string(jdoc *d, unsigned o, const char *k, char *out,
                      unsigned cap) {
  unsigned t = field(d, o, k);
  return t < TOKEN_LIMIT && string_value(d, t, out, cap);
}
static int get_u32(jdoc *d, unsigned o, const char *k, uint32_t *out) {
  unsigned long long v;
  unsigned t = field(d, o, k);
  if (t >= TOKEN_LIMIT || !number(d, t, &v) || v > 0xffffffffull)
    return 0;
  *out = (uint32_t)v;
  return 1;
}
static int hash_eq(const char *a, const char *b) {
  return strlen(a) == 64 && strlen(b) == 64 && bytes_equal(a, b, 64);
}

hyprlodr_manifest_status
hyprlodr_manifest_validate(const hyprlodr_manifest_inputs *in,
                           hyprlodr_manifest_result *out) {
  if (!in || !out || !in->manifest_path || !in->archive_path ||
      !in->executable_path)
    return HYPRLODR_MANIFEST_IO;
  FILE *f = fopen(in->manifest_path, "rb");
  if (!f)
    return HYPRLODR_MANIFEST_IO;
  jdoc *d = (jdoc *)malloc(sizeof(*d));
  if (!d) {
    fclose(f);
    return HYPRLODR_MANIFEST_LIMIT;
  }
  d->length = 0;
  d->used = 0;
  d->depth = 0;
  char *text = (char *)malloc(JSON_LIMIT);
  if (!text) {
    free(d);
    fclose(f);
    return HYPRLODR_MANIFEST_LIMIT;
  }
  size_t n = fread(text, 1, JSON_LIMIT, f);
  fclose(f);
  if (!n || n >= JSON_LIMIT) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_MALFORMED;
  }
  d->s = text;
  d->length = (unsigned)n;
  d->used = 1;
  d->depth = 0;
  unsigned p = 0, root = 0;
  if (!parse_value(d, &p, &root)) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_MALFORMED;
  }
  ws(d, &p);
  if (p != n || d->t[root].type != J_OBJECT) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_MALFORMED;
  }
  unsigned long long schema = 0;
  uint32_t srcsize = 0, outsize = 0, origoff = 0, origsize = 0, tag = 0,
           reploff = 0, replsize = 0, vc = 0, ic = 0, stride = 0;
  char delivery[64], exe[65], src[65], out_hash[65], name[16], rid[64],
      table[64], association[16], cipher[65];
  unsigned sr = field(d, root, "schema"),
           source = field(d, root, "source_archive"),
           output = field(d, root, "output_archive"),
           record = field(d, root, "record");
  if (sr >= TOKEN_LIMIT || !number(d, sr, &schema) ||
      !get_string(d, root, "delivery", delivery, sizeof delivery) ||
      !get_string(d, root, "executable_sha256", exe, sizeof exe) ||
      source >= TOKEN_LIMIT || output >= TOKEN_LIMIT || record >= TOKEN_LIMIT ||
      !get_string(d, source, "sha256", src, sizeof src) ||
      !get_u32(d, source, "size", &srcsize) ||
      !get_string(d, output, "sha256", out_hash, sizeof out_hash) ||
      !get_u32(d, output, "size", &outsize) ||
      !get_string(d, record, "name", name, sizeof name) ||
      !get_string(d, record, "record_id", rid, sizeof rid) ||
      !get_string(d, record, "resolver_table_id", table, sizeof table) ||
      !get_string(d, record, "association", association, sizeof association) ||
      !get_u32(d, record, "original_offset", &origoff) ||
      !get_u32(d, record, "original_size", &origsize) ||
      !get_u32(d, record, "type_tag", &tag) ||
      !get_u32(d, record, "replacement_offset", &reploff) ||
      !get_u32(d, record, "replacement_size", &replsize) ||
      !get_string(d, record, "replacement_ciphertext_sha256", cipher,
                  sizeof cipher) ||
      !get_u32(d, field(d, record, "mesh"), "vertex_count", &vc) ||
      !get_u32(d, field(d, record, "mesh"), "index_count", &ic) ||
      !get_u32(d, field(d, record, "mesh"), "vertex_stride", &stride)) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_MALFORMED;
  }
  unsigned rt = field(d, root, "runtime_override");
  unsigned impl = field(d, rt, "implemented");
  uint32_t seed = 0;
  char original_cipher[65], plain_hash[65];
  if (!get_u32(d, record, "seed", &seed) ||
      !get_string(d, record, "original_ciphertext_sha256", original_cipher,
                  sizeof original_cipher) ||
      !get_string(d, record, "replacement_plaintext_sha256", plain_hash,
                  sizeof plain_hash) ||
      strlen(original_cipher) != 64 || strlen(plain_hash) != 64 ||
      strlen(exe) != 64 || strlen(src) != 64 || strlen(out_hash) != 64 ||
      strlen(cipher) != 64) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_MALFORMED;
  }
  if (schema != 1 || strcmp(delivery, "append-only-record-override-required") ||
      impl >= TOKEN_LIMIT || d->t[impl].type != J_FALSE) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_UNSUPPORTED;
  }
  if (!in->supported_executable_sha256 ||
      !hash_eq(exe, in->supported_executable_sha256)) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_IDENTITY;
  }
  if (strcmp(name, "hand2") || strcmp(association, "verified") || tag != 1 ||
      seed != 766 || vc != 239 || ic != 1191 || stride != 72 ||
      !in->resolver_table_id || strcmp(table, in->resolver_table_id) ||
      !in->record_id || strcmp(rid, in->record_id) ||
      origoff != in->original_offset || origsize != in->original_size ||
      tag != in->type_tag) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_RESOURCE;
  }
  unsigned long long actual = 0;
  if (outsize < srcsize || origoff > srcsize ||
      origsize > srcsize - origoff || reploff != srcsize || replsize == 0 ||
      replsize >= 0x7fffffffu || reploff > 0xffffffffull - replsize ||
      outsize - srcsize != replsize || !file_size(in->archive_path, &actual) ||
      actual != outsize) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_ARCHIVE;
  }
  char h[65];
  if (!hash_region(in->executable_path, ~0ull, 0, h) || !hash_eq(h, exe)) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_IDENTITY;
  }
  if (!hash_region(in->archive_path, srcsize, 0, h) || !hash_eq(h, src) ||
      !hash_region(in->archive_path, outsize, 0, h) || !hash_eq(h, out_hash) ||
      !hash_region(in->archive_path, origsize, origoff, h) ||
      !hash_eq(h, original_cipher)) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_ARCHIVE;
  }
  if (!hash_region(in->archive_path, replsize, reploff, h) ||
      !hash_eq(h, cipher)) {
    free(text);
    free(d);
    return HYPRLODR_MANIFEST_PAYLOAD;
  }
  memset(out, 0, sizeof(*out));
  memcpy(out->name, name, strlen(name) + 1);
  memcpy(out->executable_sha256, exe, 65);
  memcpy(out->source_sha256, src, 65);
  memcpy(out->output_sha256, out_hash, 65);
  memcpy(out->replacement_sha256, cipher, 65);
  out->vertex_count = vc;
  out->index_count = ic;
  out->vertex_stride = stride;
  out->route = (hyprlodr_record_route){out->name, origoff,  origsize, tag,
                                       reploff,   replsize, outsize};
  free(text);
  free(d);
  return HYPRLODR_MANIFEST_OK;
}
const char *hyprlodr_manifest_status_name(hyprlodr_manifest_status s) {
  switch (s) {
  case HYPRLODR_MANIFEST_OK:
    return "validated";
  case HYPRLODR_MANIFEST_IO:
    return "manifest_missing";
  case HYPRLODR_MANIFEST_MALFORMED:
    return "manifest_malformed";
  case HYPRLODR_MANIFEST_UNSUPPORTED:
    return "manifest_unsupported";
  case HYPRLODR_MANIFEST_IDENTITY:
    return "executable_unsupported";
  case HYPRLODR_MANIFEST_ARCHIVE:
    return "archive_mismatch";
  case HYPRLODR_MANIFEST_RESOURCE:
    return "resource_identity_mismatch";
  case HYPRLODR_MANIFEST_PAYLOAD:
    return "payload_invalid";
  case HYPRLODR_MANIFEST_LIMIT:
    return "manifest_limit";
  default:
    return "manifest_error";
  }
}
