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

    // The AWS SDK response body is generally not seekable in practice
    // (tellg returns -1 for many configurations). Use Content-Length to
    // size the allocation up front, then drain with an istreambuf_iterator.
    // Falls back gracefully if Content-Length is missing or zero.
    std::vector<uint8_t> bytes;
    const auto content_length = result.GetContentLength();
    if (content_length > 0) {
        bytes.reserve(static_cast<size_t>(content_length));
    }
    bytes.assign(std::istreambuf_iterator<char>(body),
                 std::istreambuf_iterator<char>());

    if (content_length > 0
        && static_cast<long long>(bytes.size()) != content_length) {
        throw std::runtime_error(
            "Short read on S3 GetObject body for s3://" + bucket + "/" + key
            + " (got " + std::to_string(bytes.size())
            + " bytes, expected " + std::to_string(content_length) + ")");
    }
    return bytes;
}

}  // namespace synapsis::aws_sqs
