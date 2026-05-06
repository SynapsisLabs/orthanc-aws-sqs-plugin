# Architecture notes

## High-level

```
                      ┌──────────────────────────────────────┐
                      │              Orthanc                 │
                      │                                      │
                      │  ┌────────────────────────────────┐  │
   AWS                │  │  aws-sqs plugin (this repo)   │  │
   ┌──────┐  receive  │  │                                │  │
   │ SQS  │ ◀─────────┼──┤  per-queue thread:             │  │
   │      │           │  │   1. ReceiveMessage (long-poll)│  │
   │      │  delete   │  │   2. parse S3 event            │  │
   │      │ ◀─────────┼──┤   3. for each Records[]:       │  │
   └──────┘           │  │      ─ S3 GetObject            │  │
                      │  │      ─ POST /instances         │  │
   ┌──────┐  GetObj   │  │   4. DeleteMessage             │  │
   │ S3   │ ◀─────────┼──┤                                │  │
   └──────┘           │  └────────────────┬───────────────┘  │
                      │                   │                  │
                      │                   ▼                  │
                      │          /instances pipeline         │
                      │      (storage, hooks, etc.)          │
                      └──────────────────────────────────────┘
```

The plugin is an **ingestion-only** component. It does not implement any
DICOM logic, anonymisation, routing, or storage — those concerns are
handled by Orthanc itself or by other plugins (e.g. a Python plugin
implementing on-stored hooks).

## Threading

- One **OS thread per configured queue** (`std::thread`).
- All threads share AWS SDK clients via `Aws::MakeShared`, which is
  internally thread-safe.
- Each thread owns its own state machine; failures in one thread do not
  affect the others.
- On `OrthancPluginFinalize` the main plugin layer signals stop_ and
  joins each thread before unloading.

## Message lifecycle

| Step | Outcome |
|---|---|
| `ReceiveMessage` returns no messages | Loop, long-poll again |
| `ReceiveMessage` returns N messages | Process each in order |
| All `s3.GetObject` + `POST /instances` succeed for one message | `DeleteMessage` |
| Any sub-step fails | Do **not** delete; message becomes visible again after `VisibilityTimeout` |
| Message hits SQS `redrivePolicy.maxReceiveCount` | Goes to DLQ (configured outside this plugin) |

The `DeleteOnIngestFailure: true` knob exists for cases where you'd rather
acknowledge "we tried, move on" than retry — typically you keep it `false`.

## What can fail

| Class | Example | Consequence |
|---|---|---|
| Transient AWS error | `RequestTimeout`, throttling | SDK auto-retries (`DefaultRetryStrategy`). If still failing: thread loops with 5×1s backoff. |
| Auth error | Expired Roles Anywhere session, no creds | Logs error every poll; SDK refreshes creds automatically when caller process refreshes them. |
| Permanent S3 error | `NoSuchKey`, `AccessDenied` | Message is left visible → returns to queue → after `maxReceiveCount` lands in DLQ. |
| Orthanc rejects `/instances` POST | Malformed DICOM, validation failure | Same as above — DLQ catches the poison message. |
| Plugin crash | Unhandled exception | Caught in the message loop; logged; thread continues. |

## What it deliberately does NOT do

- Does not implement an SQS message format other than S3 Event Notification.
  If you want a custom format (with extra fields), wire your producer to
  publish S3-event-shaped JSON, or fork the plugin to teach it your shape.
- Does not act on the result of the `/instances` POST — Orthanc's on-stored
  hooks (Lua, Python, other C++ plugins) handle whatever happens next.
- Does not write anything to S3 (read-only on S3, receive/delete on SQS).
- Does not manage SQS infrastructure (queue creation, redrive policy,
  DLQ) — that's Terraform/CloudFormation territory.

## Config-time validation

`Configuration::Load` validates:

- Each queue has both `Name` and `Url`
- `MaxMessages` is in range 1..10
- `WaitTimeSeconds` is in range 0..20
- `VisibilityTimeoutSeconds` is non-negative

Bad config → plugin fails to initialise (returns -1 from
`OrthancPluginInitialize`) → Orthanc startup fails. This is intentional;
silent misconfiguration is worse than a loud failure.

## Roadmap (if/when there is interest)

- Add a sibling plugin `orthanc-aws-s3-export` that exposes a REST
  endpoint to upload an Orthanc instance to S3 with templated key
  (mirror of this one's ingest path).
- Optional pre-ingest validation: parse the S3 object as DICOM and
  reject before posting to /instances.
- Prometheus metrics endpoint.
- CMake `find_package(OrthancPluginSDK)` once the SDK ships a proper
  CMake config.
- IPv6 / dual-stack endpoint configuration.
