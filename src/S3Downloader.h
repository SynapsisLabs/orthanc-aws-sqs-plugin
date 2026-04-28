// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.
//
// Thin wrapper around the AWS SDK S3 client, optimised for "give me the
// bytes of this object" — the only S3 operation this plugin needs.

#pragma once

#include <aws/s3/S3Client.h>

#include <memory>
#include <string>
#include <vector>

namespace synapsis::aws_sqs {

class S3Downloader {
public:
    explicit S3Downloader(std::shared_ptr<Aws::S3::S3Client> client);

    // Downloads the full body of s3://<bucket>/<key>.
    //
    // Throws std::runtime_error on AWS errors (NotFound, AccessDenied, etc).
    // The error message includes the S3 error code so the caller can decide
    // whether to retry vs fail permanently.
    std::vector<uint8_t> Download(const std::string& bucket, const std::string& key);

private:
    std::shared_ptr<Aws::S3::S3Client> client_;
};

}  // namespace synapsis::aws_sqs
