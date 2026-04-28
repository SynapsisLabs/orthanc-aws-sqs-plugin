// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.

#include "S3Downloader.h"

#include "Logging.h"

#include <aws/core/utils/Outcome.h>
#include <aws/s3/model/GetObjectRequest.h>

#include <iterator>
#include <sstream>
#include <stdexcept>

namespace synapsis::aws_sqs {

S3Downloader::S3Downloader(std::shared_ptr<Aws::S3::S3Client> client)
    : client_(std::move(client)) {}

std::vector<uint8_t> S3Downloader::Download(const std::string& bucket,
                                            const std::string& key) {
    Aws::S3::Model::GetObjectRequest req;
    req.SetBucket(bucket.c_str());
    req.SetKey(key.c_str());

    auto outcome = client_->GetObject(req);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        std::ostringstream oss;
        oss << "S3 GetObject failed for s3://" << bucket << "/" << key
            << " — " << err.GetExceptionName() << ": " << err.GetMessage();
        throw std::runtime_error(oss.str());
    }

    auto& result = outcome.GetResult();
    auto& body   = result.GetBody();

    // Copy the response stream into a vector. Object size is small enough
    // that a single allocation is fine; for very large objects you'd want
    // chunked streaming, but DICOM SR / SC files are typically <100MB.
    std::vector<uint8_t> bytes;
    body.seekg(0, std::ios::end);
    auto size = body.tellg();
    body.seekg(0, std::ios::beg);

    if (size > 0) {
        bytes.resize(static_cast<size_t>(size));
        body.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!body) {
            throw std::runtime_error(
                "Short read on S3 GetObject body for s3://" + bucket + "/" + key);
        }
    } else {
        // Stream may not support tellg reliably; fall back to istreambuf iter
        bytes.assign(std::istreambuf_iterator<char>(body),
                     std::istreambuf_iterator<char>());
    }
    return bytes;
}

}  // namespace synapsis::aws_sqs
