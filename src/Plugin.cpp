// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.
//
// Plugin entry points called by the Orthanc host. The exported functions
// are the C ABI required by the Orthanc plugin SDK.

#include "Configuration.h"
#include "Logging.h"
#include "SqsPoller.h"

#include <orthanc/OrthancCPlugin.h>

#include <aws/core/Aws.h>

#include <memory>
#include <vector>

namespace {

OrthancPluginContext*                                       g_context = nullptr;
Aws::SDKOptions                                             g_aws_options;
std::vector<std::unique_ptr<synapsis::aws_sqs::SqsPoller>>  g_pollers;

bool CheckOrthancVersion(OrthancPluginContext* context) {
    if (OrthancPluginCheckVersion(context) == 0) {
        OrthancPluginLogError(
            context,
            "[aws-sqs] this plugin was built against an incompatible Orthanc version; "
            "rebuild against the same SDK as the host.");
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context) {
    g_context = context;
    synapsis::aws_sqs::SetContext(context);

    if (!CheckOrthancVersion(context)) {
        return -1;
    }

    OrthancPluginSetDescription(
        context,
        "Polls AWS SQS queues, downloads referenced S3 objects, and ingests them "
        "into Orthanc via /instances. Configure via the AwsSqs section of orthanc.json.");

    LOG_INFO() << "version " << ORTHANC_AWS_SQS_VERSION << " initialising";

    synapsis::aws_sqs::PluginConfig cfg;
    try {
        cfg = synapsis::aws_sqs::Configuration::Load(context);
    } catch (const std::exception& e) {
        LOG_ERROR() << "Configuration error: " << e.what();
        return -1;
    }

    if (!cfg.enabled) {
        LOG_INFO() << "AwsSqs.Enabled is false (or section missing); plugin idle";
        return 0;
    }

    Aws::InitAPI(g_aws_options);

    for (const auto& queue : cfg.queues) {
        try {
            auto poller = std::make_unique<synapsis::aws_sqs::SqsPoller>(context, queue);
            poller->Start();
            g_pollers.push_back(std::move(poller));
        } catch (const std::exception& e) {
            LOG_ERROR() << "Failed to start poller for queue '"
                        << queue.name << "': " << e.what();
            // Continue with the remaining queues — partial liveness beats
            // failing the whole plugin.
        }
    }

    LOG_INFO() << "started " << g_pollers.size() << " poller(s)";
    return 0;
}

ORTHANC_PLUGINS_API void OrthancPluginFinalize() {
    if (g_context == nullptr) {
        return;
    }
    LOG_INFO() << "finalising; stopping " << g_pollers.size() << " poller(s)";
    for (auto& poller : g_pollers) {
        poller->Stop();
    }
    g_pollers.clear();

    Aws::ShutdownAPI(g_aws_options);

    LOG_INFO() << "finalised";
    synapsis::aws_sqs::SetContext(nullptr);
    g_context = nullptr;
}

ORTHANC_PLUGINS_API const char* OrthancPluginGetName() {
    return "aws-sqs";
}

ORTHANC_PLUGINS_API const char* OrthancPluginGetVersion() {
    return ORTHANC_AWS_SQS_VERSION;
}

}  // extern "C"
