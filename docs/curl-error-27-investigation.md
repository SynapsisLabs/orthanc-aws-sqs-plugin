# curlCode: 27, Out of memory — Investigation

> Status: **root cause identified; fix path 1 invalidated; fix path 2 (rebuild SDK with `USE_CRT_HTTP_CLIENT=ON`) under test**
> Date: 2026-05-06
> Affected: AWS SDK CPP 1.11.376 + libcurl + OpenSSL 3.x (every version we tested)

## Symptom

On a freshly deployed `synapsis/orthanc-client-vm:v0.4.1` container, the
plugin fails to receive any SQS messages. Every `ReceiveMessage` call fails
immediately with the same low-level error:

```
[aws-sqs] [results-papacharalampous] ReceiveMessage failed: : curlCode: 27, Out of memory
```

The host has plenty of memory; the same error fires regardless of queue
depth, network conditions, or AWS IAM state. `curl error 27` is libcurl's
`CURLE_OUT_OF_MEMORY`.

## TL;DR root cause

The AWS SDK for C++ pulls in **s2n-tls** via aws-c-io as part of the AWS
CRT. When `Aws::InitAPI()` runs, aws-c-io calls `s2n_init()`, which
mutates global OpenSSL state in a way that causes every subsequent
`SSL_CTX_new(TLS_client_method())` call to return `NULL`. libcurl needs
that call for every HTTPS request, so when the plugin's libcurl-backed
SQS client tries to handshake against `sqs.eu-north-1.amazonaws.com:443`,
libcurl fails the SSL setup and surfaces it as `CURLE_OUT_OF_MEMORY`.

It is **not** an OOM. It is **not** a libcurl/AWS SDK init-flag problem.
It is **not** a credential, IAM, or network problem. It is a global
OpenSSL state corruption from `s2n_init`.

**The bug reproduces on every OpenSSL 3.x we tested** — Ubuntu 25.10
(OpenSSL 3.5.3), Ubuntu 24.04 (OpenSSL 3.0.13), and Debian Bookworm
(OpenSSL 3.0.19). The error reason text differs across versions
(`reason(36)` vs `malloc failure`), but the failure mode is identical.
Changing the runtime base OS does NOT fix this.

The fix path now under test is **rebuilding the AWS SDK with
`-DUSE_CRT_HTTP_CLIENT=ON`**, which routes all SDK HTTP through the AWS
CRT (s2n) and never touches libcurl — so the corrupted libcurl/OpenSSL
state is irrelevant.

## Diagnostic journey

We went down two wrong paths before the right one.

### Wrong path 1 — `httpOptions.initAndCleanupCurl = false`

Initial reading of the symptom matched several known AWS SDK CPP issues
([#931](https://github.com/aws/aws-sdk-cpp/issues/931),
[#2574](https://github.com/aws/aws-sdk-cpp/issues/2574)) where the SDK's
own `curl_global_init` / `curl_global_cleanup` calls clash with another
libcurl consumer in the same process — Orthanc itself uses libcurl, so
this seemed plausible. The canonical fix in those reports is to set
`options.httpOptions.initAndCleanupCurl = false` and let the host process
manage libcurl globals.

We patched `Plugin.cpp`, rebuilt, redeployed, restarted Orthanc — same
failure. The patch was a no-op for our problem.

### Wrong path 2 — `forward_s3.py` / `RTLD_GLOBAL` / Python OpenSSL

The Orthanc Python plugin force-loads `libpython3.13.so.1.0` with
`RTLD_GLOBAL` (it's logged at plugin init), and `forward_s3.py` imports
`boto3` which transitively initialises Python's `_ssl` module, which links
OpenSSL. So we now had two different OpenSSL clients in one process —
libcurl's and Python's — and a globally-visible libpython. That looked
like a textbook OpenSSL state-clash recipe.

We disabled the Python plugin entirely (moved `libOrthancPython.so` out of
`/usr/share/orthanc/plugins-available/`), restarted, and the SQS error was
unchanged. Python plugin is innocent.

### Right path — trace logging in the SDK, then isolated probes

Enabled SDK trace logging:

```cpp
g_aws_options.loggingOptions.logLevel  = Aws::Utils::Logging::LogLevel::Trace;
g_aws_options.loggingOptions.defaultLogPrefix = "/tmp/aws-sdk-";
```

The trace log made the actual failure visible:

```
[DEBUG] CURL (Text) SSL: could not create a context: error:0A080024:SSL routines::reason(36)
[ERROR] CurlHttpClient Curl returned error code 27 - Out of memory
```

So `CURLE_OUT_OF_MEMORY` is libcurl's translation of "OpenSSL refused to
make me an SSL context." The OpenSSL error is `0A080024`, lib=SSL,
reason=36 (a generic `ERR_R_*` reason — meaning some required algorithm
fetch failed when initializing the context).

Also visible at startup:

```
[INFO] tls-handler static: Initializing TLS using s2n.
```

That single line was the real lead.

## The conclusive test

Built a tiny C++ probe that calls `SSL_CTX_new(TLS_client_method())`
before and after `Aws::InitAPI()`, both linked against the AWS SDK
shared libs already inside the orthanc container:

```cpp
// ssl_ctx_probe.cpp (run inside the orthanc container)
#include <cstdio>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <aws/core/Aws.h>

static void try_ssl_ctx(const char *label) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        unsigned long e = ERR_get_error();
        char buf[256]; ERR_error_string_n(e, buf, sizeof(buf));
        printf("%s: FAIL err=0x%08lx %s\n", label, e, buf);
    } else {
        printf("%s: OK\n", label);
        SSL_CTX_free(ctx);
    }
}

int main() {
    try_ssl_ctx("before InitAPI");
    Aws::SDKOptions o;
    o.httpOptions.initAndCleanupCurl  = false;
    o.cryptoOptions.initAndCleanupOpenSSL = false;
    Aws::InitAPI(o);
    try_ssl_ctx("after  InitAPI (curl/openssl init disabled)");
    Aws::ShutdownAPI(o);
    try_ssl_ctx("after  ShutdownAPI");
}
```

Output:

```
before InitAPI: OK
after  InitAPI (curl/openssl init disabled): FAIL err=0x0a080024 error:0A080024:SSL routines::reason(36)
after  ShutdownAPI: OK
```

So `Aws::InitAPI` poisons OpenSSL even with both
`initAndCleanupCurl = false` and `cryptoOptions.initAndCleanupOpenSSL = false`.
`Aws::ShutdownAPI` undoes the poisoning.

### Narrowing further — is it really s2n?

Stripped out the AWS SDK and called `s2n_init()` alone:

```cpp
extern "C" int s2n_init(void);
extern "C" int s2n_cleanup(void);

int main() {
    void *h = dlopen("libs2n.so", RTLD_NOW|RTLD_GLOBAL);
    auto init = (int(*)())dlsym(h, "s2n_init");
    auto cleanup = (int(*)())dlsym(h, "s2n_cleanup");

    try_("before s2n_init");      // SSL_CTX_new
    init();
    try_("after s2n_init");
    cleanup();
    try_("after s2n_cleanup");
}
```

Output:

```
before s2n_init: OK
s2n_init -> 0
after s2n_init: FAIL
after s2n_cleanup: FAIL
```

`s2n_init()` alone — no AWS SDK code at all — breaks `SSL_CTX_new`. This
is the entire root cause in two lines of output.

`s2n_cleanup()` does **not** restore the OpenSSL state. (It only does
per-thread cleanup; full library teardown happens via
`s2n_cleanup_final()`, which is what `Aws::ShutdownAPI` reaches.)

### Other things ruled out

| Hypothesis | Test | Result |
|---|---|---|
| FD exhaustion in Orthanc process | `cat /proc/$pid/limits`, count `/proc/$pid/fd` | 17 of 524288 — fine |
| Two libssl/libcrypto loaded into the process | `ldd` of probe and SDK | one shared `libssl.so.3` / `libcrypto.so.3` |
| s2n statically linking libcrypto | `nm -D` of `libs2n.so` | 0 defined `EVP_*`, 80 undefined — uses shared libcrypto |
| OpenSSL providers missing after init | `OSSL_PROVIDER_do_all` | "default" provider still loaded |
| FIPS mode enabled by s2n | `EVP_default_properties_is_fips_enabled` | 0 (not enabled) |
| Specific SSL method broken | tried `TLS_method`, `TLS_client_method`, `TLS_server_method`, `TLSv1_2_client_method` | all fail equally |
| EVP fetches generally broken | `EVP_CIPHER_fetch("AES-256-GCM")`, `EVP_MD_fetch("SHA256")` | both still work — only SSL_CTX_new path is broken |
| Reload default provider after InitAPI | `OSSL_PROVIDER_load(NULL, "default")` | returns non-null but `SSL_CTX_new` still fails |
| Use a private `OSSL_LIB_CTX` | `OSSL_LIB_CTX_new` + `SSL_CTX_new_ex(my_libctx, …)` | still fails |
| `OPENSSL_CONF=/dev/null` (skip openssl.cnf) | env var on the probe | unchanged |
| Subprocess libcurl works | `docker exec orthanc curl https://sqs…` | OK — confirms the breakage is in-process only |
| Subprocess `openssl s_client` works | TLS handshake against SQS endpoint | OK — confirms OpenSSL itself is fine in a clean process |

The only remaining narrowing was s2n itself, hence the dlopen probe.

## Fix path 1 — rebase runtime on older OpenSSL (INVALIDATED)

Initial hypothesis: OpenSSL 3.5.3 was the problem (it is recent enough
that the AWS SDK + s2n combo may not have been battle-tested against
it). Built a one-shot test image on `ubuntu:24.04` (OpenSSL 3.0.13)
with the same AWS SDK 1.11.376 from source, and ran the probe binary.

Result:

```
=== Ubuntu 24.04 / OpenSSL 3.0.13 ===
before InitAPI: OK
after  InitAPI (curl/openssl init disabled): FAIL err=0x0a0c0100 error:0A0C0100:SSL routines::malloc failure
after  ShutdownAPI: OK
```

Different OpenSSL error string (`malloc failure` instead of
`reason(36)`), same effect. Then re-ran the probe in the existing
`docker/Dockerfile.build` image (Debian Bookworm, OpenSSL 3.0.19) for
completeness — also failed identically.

**Conclusion:** the bug is not OpenSSL-3.5-specific. Every OpenSSL 3.x
we have tested reproduces it. Rebasing the runtime image is not a fix.

## Fix path 2 — rebuild SDK with `-DUSE_CRT_HTTP_CLIENT=ON` (UNDER TEST)

The AWS SDK CPP CMake option `-DUSE_CRT_HTTP_CLIENT=ON` makes the SDK
use the **AWS CRT HTTP client** instead of the libcurl-backed
`CurlHttpClient` for all HTTP traffic. CRT HTTP goes straight through
s2n; libcurl is not built or invoked by SDK code.

This sidesteps the bug entirely:

- `s2n_init()` still runs and still corrupts the libcurl/OpenSSL path.
- But the SDK never calls `SSL_CTX_new` (or anything else in libcurl)
  because libcurl is not in the SDK's HTTP path anymore.
- Orthanc's libcurl is still loaded in the process and still broken,
  but in this deployment Orthanc itself never makes outbound HTTPS
  calls (no peers, no DICOMweb HTTPS, no LUA HTTPS callbacks). The
  Python plugin's `boto3` uses Python's own `_ssl` stack (separate
  from libcurl, untouched by s2n) so it continues working.

So as long as nothing in this process needs libcurl HTTPS, the bug is
moot. That is true today.

### Build change

`docker/Dockerfile.build` Stage 1 — added
`-DUSE_CRT_HTTP_CLIENT=ON` to the AWS SDK build:

```dockerfile
cmake .. \
    -DBUILD_ONLY="sqs;s3" \
    -DENABLE_TESTING=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DUSE_CRT_HTTP_CLIENT=ON
```

### Validation plan

1. Rebuild plugin image `orthanc-aws-sqs-plugin:dev-crt`.
2. Extract the new `libaws-cpp-sdk-*.so` and `libOrthancAwsSqs.so` to
   `~/build-output/`.
3. Replace `/usr/local/lib/libaws-cpp-sdk-{core,sqs,s3}.so` and
   `/usr/share/orthanc/plugins/libOrthancAwsSqs.so` in the running
   orthanc container.
4. Restart Orthanc and watch logs for actual SQS receive cycles.

Success criteria:

- No `curlCode: 27, Out of memory` lines.
- Either `[results-papacharalampous] received N messages` if the queue
  has any, or just clean long-poll cycles (no errors) if it's empty.

### Known constraints / caveats

- **Anything in this process that needs libcurl HTTPS will break.**
  This includes any future Orthanc plugin that uses libcurl directly.
  The Python plugin's boto3 is fine (uses Python `_ssl`, not libcurl).
- The CRT HTTP client has slightly different connection-pooling and
  timeout semantics than libcurl. Orthanc's existing tuning of SQS
  poll/visibility timeouts should still apply because those are
  request-level options, not transport-level.

## Fallback options if path 2 fails

1. **Disable s2n in aws-c-io.** Build `aws-c-io` with `-DUSE_S2N=OFF` so
   it uses an OpenSSL TLS implementation directly. Requires modifying
   the SDK build to construct CRT components separately with that flag.
   Risk: parts of AWS CRT may hard-require s2n; build may fail.

2. **Replace the C++ plugin with a Python or Go sidecar** that uses
   boto3 / aws-sdk-go (neither has the s2n-in-libcurl conflict because
   neither ships a bundled TLS stack the way aws-sdk-cpp does). The
   plugin's README explicitly mentions a sidecar as a fallback. Higher
   ops cost (extra container, extra service-management surface).

3. **Pin AWS SDK CPP to 1.10.x.** The 1.10 series predates the
   aggressive CRT/s2n integration. Requires verifying that all SDK
   APIs we use exist in 1.10. We'd be moving away from the actively-
   supported 1.11 line.

4. **File upstream.** The minimal `dlopen("libs2n.so") + s2n_init() +
   SSL_CTX_new(TLS_client_method())` reproducer is small enough to file
   at `aws/s2n-tls` and `openssl/openssl`. Independent of which
   workaround we take, the upstream issue should be filed so this stops
   being everyone-rediscovers-it.

## Re-running the probe

The probe is inlined above. To re-run it on any host: drop it in a
container that has the AWS SDK CPP libs and OpenSSL dev headers, build
with:

```bash
g++ -std=c++17 ssl_ctx_probe.cpp -o probe \
    -I/usr/local/include \
    -L/usr/local/lib -laws-cpp-sdk-core -lssl -lcrypto \
    -Wl,-rpath,/usr/local/lib
./probe
```

If `before InitAPI: OK / after InitAPI: FAIL` shows up, the bug is
present and `-DUSE_CRT_HTTP_CLIENT=ON` is the workaround.

## Reference: error-code decoding

OpenSSL 3.x packs error codes as:

```
bits 23..30  library          (8 bits)
bits 18..22  reason flags     (5 bits)  — bit 0 FATAL, bit 1 COMMON
bits  0..17  reason number    (18 bits)
```

For `0x0A080024`:

| field | value | meaning |
|---|---|---|
| library | `0x14` (20) | `ERR_LIB_SSL` |
| rflags  | `0x02`      | `ERR_RFLAG_COMMON` (reason from `ERR_R_*`, not SSL-specific) |
| reason  | `0x24` (36) | a generic crypto reason — most likely "fetch failed" / no algorithm available |

The "reason(36)" in the printed error string just means OpenSSL doesn't
have a registered name for that reason in the SSL library namespace —
which is consistent with it being an `ERR_R_*` cross-library reason
rather than an `SSL_R_*` library-specific one.
