# orthanc-aws-sqs-plugin

A native C++ plugin for the [Orthanc DICOM server](https://orthanc.uclouvain.be/)
that polls one or more **AWS SQS queues**, treats each message as an
**S3 Event Notification**, downloads the referenced DICOM object from S3,
and ingests it into Orthanc via the standard `POST /instances` path.

This lets you wire Orthanc into S3-event-driven pipelines without writing
any glue code in Python or running a separate sidecar service.

```
                                   ┌─────────────────────┐
                                   │   Orthanc           │
        s3:ObjectCreated  ───┐     │  ┌───────────────┐  │
   AWS SQS  ──[long-poll]────┼────▶│  │ aws-sqs       │  │
                             │     │  │ plugin        │  │
   AWS S3   ──[GetObject]────┘     │  │ (this repo)   │  │
                                   │  └───────┬───────┘  │
                                   │          │          │
                                   │          ▼          │
                                   │   /instances        │
                                   │   (native Orthanc   │
                                   │    ingestion)       │
                                   └─────────────────────┘
```

## What it does

For each configured queue, a dedicated background thread:

1. **Long-polls** the queue (`WaitTimeSeconds`, default 20s)
2. For each received message:
   - Parses the message body as an **S3 Event Notification** (the format AWS S3 emits when the bucket has a `QueueConfiguration`)
   - For each `Records[].s3` entry, downloads the object from S3
   - Ingests the bytes into Orthanc via the plugin SDK (equivalent to `POST /instances`)
   - Deletes the SQS message on success
3. Failures: the message is **not deleted**, returns to the queue after the
   configured `VisibilityTimeout`. After `maxReceiveCount` retries it lands
   in the configured DLQ (configured on the SQS side, not by this plugin).

## Why this plugin

If you've used Orthanc to build an event-driven imaging pipeline you've
probably written one of these in Python or shipped a sidecar process. The
patterns are well-trodden but there's no canonical native plugin for them.
This plugin fills that gap so a single Orthanc instance can be wired to
SQS-driven workloads with **just a JSON config block**.

## Status

Early stage. Built against:

- Orthanc Plugin SDK ≥ 1.12
- AWS SDK for C++ ≥ 1.11
- C++17

Tested manually; no integration test suite yet. PRs welcome.

## Configuration

Add to your `orthanc.json`:

```json
{
  "Plugins": ["/usr/share/orthanc/plugins"],

  "AwsSqs": {
    "Enabled": true,
    "Region": "eu-north-1",
    "Queues": [
      {
        "Name": "results-sakarellos",
        "Url": "https://sqs.eu-north-1.amazonaws.com/919099343976/results-sakarellos",
        "WaitTimeSeconds": 20,
        "VisibilityTimeoutSeconds": 300,
        "MaxMessages": 10,
        "DeleteOnIngestFailure": false
      }
    ]
  }
}
```

| Field | Default | Notes |
|---|---|---|
| `Enabled` | `false` | Master switch |
| `Region` | (none, required) | Default region used for SQS + S3 clients unless overridden per-queue |
| `Queues[]` | `[]` | One or more queues to poll |
| `Queues[].Name` | (required) | Used in log lines |
| `Queues[].Url` | (required) | Full SQS queue URL |
| `Queues[].Region` | inherit | Per-queue region override (rare) |
| `Queues[].WaitTimeSeconds` | `20` | SQS long-poll wait |
| `Queues[].VisibilityTimeoutSeconds` | `300` | Time before a non-deleted message becomes visible again |
| `Queues[].MaxMessages` | `10` | Per-receive batch (1–10) |
| `Queues[].DeleteOnIngestFailure` | `false` | Set `true` to delete messages even after Orthanc ingestion failure (typically you want this to be `false` so the DLQ catches poisons) |

## AWS authentication

Uses the standard AWS SDK credential chain. In order:

1. Environment variables (`AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / `AWS_SESSION_TOKEN`)
2. `~/.aws/credentials` and `~/.aws/config`
3. **`credential_process`** in `~/.aws/config` — works seamlessly with
   [AWS IAM Roles Anywhere](https://aws.amazon.com/iam/identity-center/)
   via `aws_signing_helper`
4. ECS / EC2 instance metadata if applicable

The plugin does not handle AWS auth itself — it relies on the SDK picking
up your environment.

## Required IAM permissions

For each queue:

```json
{
  "Effect": "Allow",
  "Action": [
    "sqs:ReceiveMessage",
    "sqs:DeleteMessage",
    "sqs:ChangeMessageVisibility",
    "sqs:GetQueueAttributes"
  ],
  "Resource": "arn:aws:sqs:eu-north-1:<account>:<queue-name>"
}
```

Plus `s3:GetObject` on whatever buckets the events reference.

## Building

### Dependencies

- CMake ≥ 3.16
- A C++17 compiler (gcc 9+, clang 10+)
- Orthanc Plugin SDK headers (downloaded at configure time, see `cmake/`)
- AWS SDK for C++ with the `sqs` and `s3` components

On Debian / Ubuntu:

```bash
sudo apt-get install -y \
  build-essential cmake git \
  libcurl4-openssl-dev libssl-dev zlib1g-dev \
  libaws-cpp-sdk-core libaws-cpp-sdk-sqs libaws-cpp-sdk-s3
```

If `libaws-cpp-sdk-*` is too old (< 1.11), build aws-sdk-cpp from source:

```bash
git clone --recursive https://github.com/aws/aws-sdk-cpp.git
cd aws-sdk-cpp && mkdir build && cd build
cmake .. -DBUILD_ONLY="sqs;s3" -DENABLE_TESTING=OFF \
  -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) && sudo make install
```

### Build the plugin

```bash
git clone https://github.com/<your-org>/orthanc-aws-sqs-plugin.git
cd orthanc-aws-sqs-plugin
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Output: `libOrthancAwsSqs.so` (Linux) / `OrthancAwsSqs.dylib` (macOS).

### Install into Orthanc

Either copy the `.so` into Orthanc's plugin directory:

```bash
sudo cp libOrthancAwsSqs.so /usr/share/orthanc/plugins/
```

…or reference its full path in `orthanc.json`:

```json
{ "Plugins": ["/path/to/libOrthancAwsSqs.so"] }
```

### Docker build

```bash
docker build -f docker/Dockerfile.build -t orthanc-aws-sqs-plugin:dev .
docker run --rm -v $(pwd)/build-output:/out orthanc-aws-sqs-plugin:dev
# .so lands in ./build-output/libOrthancAwsSqs.so
```

## Logging

The plugin uses Orthanc's standard logging API, so all messages appear
in Orthanc's log alongside the rest:

```
W0429 10:15:22.123456 SqsPoller.cpp:78] [results-sakarellos] received 3 messages
I0429 10:15:22.345678 SqsPoller.cpp:142] [results-sakarellos] ingested s3://synapsis-dicom-sakarellos/results/mg/2026/4/29/x.y.z/a.b.c.dcm (47821 bytes)
```

If Orthanc is configured for verbose JSON logging the plugin's lines
include the same fields.

## Why not just use a Python plugin / sidecar?

You can. Both work. This plugin exists because:

- Native code = no Python runtime in the loop, lower memory footprint
- Configuration is part of `orthanc.json`, not a separate process to manage
- Lifecycle is tied to Orthanc — start / stop / restart together
- One less container in your compose file
- Same operational model as Orthanc's other plugins (Postgres, DICOMweb, etc.)

Use a sidecar instead when you want independent scaling or want the
poller in a different language (e.g. Go).

## License

Apache 2.0 — see [LICENSE](LICENSE).

## Author / contact

Initially developed by [Synapsis Labs](https://github.com/SynapsisLabs)
for clinical-AI imaging pipelines. Issues and pull requests welcome.
