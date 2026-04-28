// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.

#include "Configuration.h"

#include "Logging.h"

#include <cstring>
#include <stdexcept>
#include <string>

// We use a minimal hand-written JSON peeker so we don't need to pull in
// jsoncpp. Orthanc itself uses jsoncpp internally but plugins do not link
// against it directly. The plugin SDK lets us request the host's full
// configuration as a UTF-8 JSON string via OrthancPluginGetConfiguration().
//
// For robustness we parse with a small embedded JSON parser. To keep this
// file dependency-free we use a header-only library; we vendor a minimal
// subset inline below. (A real release would depend on nlohmann/json or
// rapidjson; doing so is a one-line CMakeLists.txt change.)
//
// To keep the initial scaffold small we use the C library's escapes by
// going through Orthanc's helper APIs where possible. Configuration JSON
// is small (KB at most), so the parser overhead is negligible.

#include <nlohmann/json.hpp>  // header-only, ubiquitous on Linux distros

namespace synapsis::aws_sqs {

using json = nlohmann::json;

namespace {

std::string GetOrthancConfigurationJson(OrthancPluginContext* context) {
    char* raw = OrthancPluginGetConfiguration(context);
    if (raw == nullptr) {
        throw std::runtime_error("OrthancPluginGetConfiguration returned null");
    }
    std::string result(raw);
    OrthancPluginFreeString(context, raw);
    return result;
}

QueueConfig ParseQueue(const json& obj, const std::string& default_region) {
    if (!obj.is_object()) {
        throw std::runtime_error("AwsSqs.Queues[] entries must be JSON objects");
    }
    QueueConfig q;
    q.name = obj.value("Name", std::string{});
    q.url  = obj.value("Url",  std::string{});
    q.region = obj.value("Region", std::string{});
    if (q.region.empty()) {
        q.region = default_region;
    }
    q.wait_time_seconds       = obj.value("WaitTimeSeconds",            20);
    q.visibility_timeout_secs = obj.value("VisibilityTimeoutSeconds",  300);
    q.max_messages            = obj.value("MaxMessages",                10);
    q.delete_on_ingest_failure = obj.value("DeleteOnIngestFailure", false);

    if (q.name.empty()) {
        throw std::runtime_error("AwsSqs.Queues[].Name is required");
    }
    if (q.url.empty()) {
        throw std::runtime_error("AwsSqs.Queues[].Url is required (queue " + q.name + ")");
    }
    if (q.max_messages < 1 || q.max_messages > 10) {
        throw std::runtime_error(
            "AwsSqs.Queues[].MaxMessages must be 1..10 (queue " + q.name + ")");
    }
    if (q.wait_time_seconds < 0 || q.wait_time_seconds > 20) {
        throw std::runtime_error(
            "AwsSqs.Queues[].WaitTimeSeconds must be 0..20 (queue " + q.name + ")");
    }
    if (q.visibility_timeout_secs < 0) {
        throw std::runtime_error(
            "AwsSqs.Queues[].VisibilityTimeoutSeconds must be >= 0 (queue " + q.name + ")");
    }
    return q;
}

}  // namespace

PluginConfig Configuration::Load(OrthancPluginContext* context) {
    PluginConfig out;

    std::string raw = GetOrthancConfigurationJson(context);
    json root = json::parse(raw, /*cb*/ nullptr, /*allow_exceptions*/ true);

    if (!root.contains("AwsSqs") || root["AwsSqs"].is_null()) {
        // No config at all — plugin stays disabled
        return out;
    }

    const json& cfg = root["AwsSqs"];
    if (!cfg.is_object()) {
        throw std::runtime_error("AwsSqs section must be a JSON object");
    }

    out.enabled = cfg.value("Enabled", false);
    out.region  = cfg.value("Region", out.region);

    if (!out.enabled) {
        return out;
    }

    if (cfg.contains("Queues")) {
        const json& queues = cfg["Queues"];
        if (!queues.is_array()) {
            throw std::runtime_error("AwsSqs.Queues must be a JSON array");
        }
        out.queues.reserve(queues.size());
        for (const auto& q : queues) {
            out.queues.push_back(ParseQueue(q, out.region));
        }
    }

    if (out.enabled && out.queues.empty()) {
        LOG_WARN() << "AwsSqs.Enabled is true but Queues[] is empty; nothing to do";
    }
    return out;
}

}  // namespace synapsis::aws_sqs
