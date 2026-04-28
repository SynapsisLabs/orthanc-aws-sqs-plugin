// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.
//
// One SqsPoller instance == one background thread polling one SQS queue.
//
// On each successfully-received message, the poller parses the body as an
// S3 Event Notification, downloads each referenced object, and ingests it
// into Orthanc via the plugin SDK's REST adapter (effectively the same
// path as POST /instances).

#pragma once

#include "Configuration.h"

#include <orthanc/OrthancCPlugin.h>

#include <aws/s3/S3Client.h>
#include <aws/sqs/SQSClient.h>
#include <aws/sqs/model/Message.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace synapsis::aws_sqs {

class S3Downloader;

class SqsPoller {
public:
    SqsPoller(OrthancPluginContext* context, QueueConfig config);
    ~SqsPoller();

    // Non-copyable, non-movable (owns a thread).
    SqsPoller(const SqsPoller&)            = delete;
    SqsPoller& operator=(const SqsPoller&) = delete;

    void Start();
    void Stop();

private:
    OrthancPluginContext* context_;
    QueueConfig           config_;
    std::atomic<bool>     stop_{false};
    std::thread           thread_;

    std::shared_ptr<Aws::SQS::SQSClient> sqs_client_;
    std::shared_ptr<Aws::S3::S3Client>   s3_client_;
    std::unique_ptr<S3Downloader>        downloader_;

    void Run();

    // Returns true if the message was fully processed and can be deleted
    // from SQS. Returns false to leave the message visible (will retry
    // after VisibilityTimeout).
    bool ProcessMessage(const Aws::SQS::Model::Message& message);

    // Ingest a single S3 object's bytes into Orthanc.
    // Throws on any failure.
    void IngestS3Object(const std::string& bucket, const std::string& key);

    // POST raw DICOM bytes to Orthanc's /instances. Returns true if Orthanc
    // accepted the instance.
    bool PostInstance(const std::vector<uint8_t>& dicom_bytes);
};

}  // namespace synapsis::aws_sqs
