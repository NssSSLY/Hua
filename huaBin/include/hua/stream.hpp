#pragma once
#include "hua/stdlib.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
namespace hua {
struct StreamError {
    std::string domain{"io"};
    std::string code{"io"};
    std::string message;
    std::string native_domain;
    std::optional<std::int64_t> native_code;
};
struct StreamReadResult {
    std::optional<std::string> chunk; // nullopt => clean EOF
    std::optional<StreamError> error;
};
struct StreamWriteResult {
    std::size_t written{};
    std::optional<StreamError> error;
};
class StreamReaderBackend {
public:
    virtual ~StreamReaderBackend() = default;
    virtual StreamReadResult read(std::size_t max_bytes, RuntimeContext* context) = 0;
    virtual std::optional<StreamError> close() = 0;
};
class StreamWriterBackend {
public:
    virtual ~StreamWriterBackend() = default;
    virtual StreamWriteResult write(std::span<const char> data, RuntimeContext* context) = 0;
    virtual std::optional<StreamError> flush(RuntimeContext* context) = 0;
    virtual std::optional<StreamError> close() = 0;
};
struct StreamReaderData {
    std::mutex mutex;
    std::uint8_t state{1}; // 1=Open, 2=Closed, 3=Poisoned
    std::shared_ptr<StreamReaderBackend> backend;
    explicit StreamReaderData(std::shared_ptr<StreamReaderBackend> b) : backend(std::move(b)) {}
    ~StreamReaderData();
    StreamReadResult read(std::size_t max_bytes, RuntimeContext* context);
    std::optional<StreamError> close();
};
struct StreamWriterData {
    std::mutex mutex;
    std::uint8_t state{1}; // 1=Open, 2=Closed, 3=Poisoned
    std::shared_ptr<StreamWriterBackend> backend;
    explicit StreamWriterData(std::shared_ptr<StreamWriterBackend> b) : backend(std::move(b)) {}
    ~StreamWriterData();
    StreamWriteResult write(std::span<const char> data, RuntimeContext* context);
    std::optional<StreamError> flush(RuntimeContext* context);
    std::optional<StreamError> close();
};
Value make_reader_value(std::shared_ptr<StreamReaderBackend> backend);
Value make_writer_value(std::shared_ptr<StreamWriterBackend> backend);
Value stream_error_result(const StreamError& err, std::string success_type, std::string origin);
Value invoke_stream_standard(const StandardFunction& function, const std::vector<Value>& arguments,
                             const SourceSpan& span, RuntimeContext* context);
} // namespace hua
