// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.
//
// Plugin configuration parsed from the host Orthanc's configuration JSON.
// We expect a top-level "AwsSqs" section; see README.md for the schema.

#pragma once

#include <orthanc/OrthancCPlugin.h>

#include <string>
#include <vector>

namespace synapsis::aws_sqs {

struct QueueConfig {
    std::string name;
    std::string url;
    std::string region;            // empty = inherit from PluginConfig::region
    int wait_time_seconds        = 20;
    int visibility_timeout_secs  = 300;
    int max_messages             = 10;   // valid range 1..10
    bool delete_on_ingest_failure = false;
};

struct PluginConfig {
    bool enabled = false;
    // Region used for SQS / S3 clients unless a queue sets its own
    // override. Required (non-empty) when enabled=true; Configuration::Load
    // throws if it isn't.
    std::string region;
    std::vector<QueueConfig> queues;
};

class Configuration {
public:
    // Reads the host Orthanc configuration JSON, looks for the "AwsSqs"
    // section, and returns a parsed PluginConfig.
    //
    // Throws std::runtime_error on malformed config (a fatal startup error).
    static PluginConfig Load(OrthancPluginContext* context);
};

}  // namespace synapsis::aws_sqs
