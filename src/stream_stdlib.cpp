#include "hua/stream.hpp"
#include "hua/file.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
namespace hua {
namespace {
constexpr std::size_t kMaxSingleRead = 4 * 1024 * 1024;
constexpr std::size_t kCopyChunk = 16 * 1024;

ErrorValue make_err(const StreamError& e, std::string origin) {
    auto d = std::make_shared<ErrorData>();
    d->domain = e.domain.empty() ? "io" : e.domain;
    d->code = e.code.empty() ? "io" : e.code;
    d->message = e.message.empty() ? "stream operation failed" : e.message;
    d->origin = std::move(origin);
    d->native_domain = e.native_domain;
    d->native_code = e.native_code;
    return ErrorValue{std::move(d)};
}
Value ok_result(Value payload, std::string success_type) {
    return Value(ResultValue{true, result_box(std::move(payload)), std::move(success_type), "std.error.Value", true, false});
}
Value ok_void() {
    return Value(ResultValue{true, {}, "void", "std.error.Value", true, false});
}

class BytesReaderBackend final : public StreamReaderBackend {
    std::shared_ptr<const std::string> data_;
    std::size_t offset_{0};
public:
    explicit BytesReaderBackend(std::shared_ptr<const std::string> data) : data_(std::move(data)) {}
    StreamReadResult read(std::size_t max_bytes, RuntimeContext*) override {
        if (!data_ || offset_ >= data_->size()) return {std::nullopt, std::nullopt};
        std::size_t n = std::min(max_bytes, data_->size() - offset_);
        std::string out(data_->data() + offset_, n);
        offset_ += n;
        return {std::move(out), std::nullopt};
    }
    std::optional<StreamError> close() override {
        data_.reset();
        return std::nullopt;
    }
};

class FileReaderBackend final : public StreamReaderBackend {
    Value file_value_;
    SourceSpan span_;
public:
    FileReaderBackend(Value file_value, SourceSpan span)
        : file_value_(std::move(file_value)), span_(std::move(span)) {}
    StreamReadResult read(std::size_t max_bytes, RuntimeContext* context) override {
        auto* fn = standard_function("$std$file$read");
        if (!fn) return {std::nullopt, StreamError{"io", "io", "std.file.read unavailable", "", {}}};
        std::size_t capped = std::min<std::size_t>(max_bytes, 1024 * 1024);
        Value res = invoke_file_standard(*fn, {file_value_, Value(static_cast<std::int64_t>(capped))}, span_, context);
        if (!holds<ResultValue>(res.data)) return {std::nullopt, StreamError{"io", "io", "unexpected file read result", "", {}}};
        const auto& rv = cget<ResultValue>(res.data);
        if (!rv.ok) {
            if (rv.payload && holds<IOFailureValue>(rv.payload->data)) {
                const auto& iof = cget<IOFailureValue>(rv.payload->data);
                if (iof.data && iof.data->error.data) {
                    const auto& ed = *iof.data->error.data;
                    return {std::nullopt, StreamError{"io", ed.code, ed.message, ed.native_domain, ed.native_code}};
                }
            }
            return {std::nullopt, StreamError{"io", "io", "file read failed", "", {}}};
        }
        if (!rv.payload || holds<std::monostate>(rv.payload->data)) {
            return {std::nullopt, std::nullopt}; // EOF
        }
        if (auto* b = get_if<BytesValue>(&rv.payload->data)) {
            return {b->data ? *b->data : std::string{}, std::nullopt};
        }
        return {std::nullopt, StreamError{"io", "io", "unexpected file payload", "", {}}};
    }
    std::optional<StreamError> close() override {
        // Non-owning adapter: detaches the reader without closing caller's File.
        file_value_ = Value{};
        return std::nullopt;
    }
};

class BufferWriterBackend final : public StreamWriterBackend {
    BufferValue buffer_;
public:
    explicit BufferWriterBackend(BufferValue buffer) : buffer_(std::move(buffer)) {}
    StreamWriteResult write(std::span<const char> data, RuntimeContext*) override {
        if (!buffer_.writable) {
            return {0, StreamError{"io", "read_only", "buffer is read-only", "", {}}};
        }
        if (!buffer_.data) return {0, StreamError{"io", "closed", "buffer is unavailable", "", {}}};
        constexpr std::size_t kMaxBuffer = 64 * 1024 * 1024;
        if (buffer_.data->size() + data.size() > kMaxBuffer) {
            return {0, StreamError{"io", "limit", "buffer capacity limit exceeded", "", {}}};
        }
        buffer_.data->append(data.data(), data.size());
        return {data.size(), std::nullopt};
    }
    std::optional<StreamError> flush(RuntimeContext*) override { return std::nullopt; }
    std::optional<StreamError> close() override {
        buffer_ = BufferValue{};
        return std::nullopt;
    }
};

class FileWriterBackend final : public StreamWriterBackend {
    Value file_value_;
    SourceSpan span_;
public:
    FileWriterBackend(Value file_value, SourceSpan span)
        : file_value_(std::move(file_value)), span_(std::move(span)) {}
    StreamWriteResult write(std::span<const char> data, RuntimeContext* context) override {
        auto* fn = standard_function("$std$file$write");
        if (!fn) return {0, StreamError{"io", "io", "std.file.write unavailable", "", {}}};
        auto bytes = std::make_shared<std::string>(data.data(), data.size());
        Value res = invoke_file_standard(*fn, {file_value_, Value(BytesValue{std::move(bytes)})}, span_, context);
        if (!holds<ResultValue>(res.data)) return {0, StreamError{"io", "io", "unexpected file write result", "", {}}};
        const auto& rv = cget<ResultValue>(res.data);
        if (!rv.ok) {
            std::size_t transferred = 0;
            StreamError err{"io", "io", "file write failed", "", {}};
            if (rv.payload && holds<IOFailureValue>(rv.payload->data)) {
                const auto& iof = cget<IOFailureValue>(rv.payload->data);
                if (iof.data) {
                    if (iof.data->transferred && *iof.data->transferred > 0)
                        transferred = static_cast<std::size_t>(*iof.data->transferred);
                    if (iof.data->error.data) {
                        const auto& ed = *iof.data->error.data;
                        err = StreamError{"io", ed.code, ed.message, ed.native_domain, ed.native_code};
                    }
                }
            }
            return {transferred, std::move(err)};
        }
        if (rv.payload) {
            if (auto* n = get_if<std::int64_t>(&rv.payload->data)) {
                return {static_cast<std::size_t>(std::max<std::int64_t>(0, *n)), std::nullopt};
            }
        }
        return {0, StreamError{"io", "io", "unexpected file write count", "", {}}};
    }
    std::optional<StreamError> flush(RuntimeContext* context) override {
        auto* fn = standard_function("$std$file$flush");
        if (!fn) return StreamError{"io", "io", "std.file.flush unavailable", "", {}};
        Value res = invoke_file_standard(*fn, {file_value_}, span_, context);
        if (holds<ResultValue>(res.data)) {
            const auto& rv = cget<ResultValue>(res.data);
            if (rv.ok) return std::nullopt;
            if (rv.payload && holds<ErrorValue>(rv.payload->data)) {
                const auto& ed = *cget<ErrorValue>(rv.payload->data).data;
                return StreamError{"io", ed.code, ed.message, ed.native_domain, ed.native_code};
            }
        }
        return StreamError{"io", "io", "file flush failed", "", {}};
    }
    std::optional<StreamError> close() override {
        file_value_ = Value{};
        return std::nullopt;
    }
};

class LimitReaderBackend final : public StreamReaderBackend {
    std::shared_ptr<StreamReaderData> inner_;
    std::size_t remaining_;
public:
    LimitReaderBackend(std::shared_ptr<StreamReaderData> inner, std::size_t max_bytes)
        : inner_(std::move(inner)), remaining_(max_bytes) {}
    StreamReadResult read(std::size_t max_bytes, RuntimeContext* context) override {
        if (!inner_ || remaining_ == 0) return {std::nullopt, std::nullopt};
        std::size_t want = std::min(max_bytes, remaining_);
        auto r = inner_->read(want, context);
        if (r.error) return r;
        if (!r.chunk) return {std::nullopt, std::nullopt};
        if (r.chunk->size() > remaining_) r.chunk->resize(remaining_);
        remaining_ -= r.chunk->size();
        return r;
    }
    std::optional<StreamError> close() override {
        inner_.reset();
        remaining_ = 0;
        return std::nullopt;
    }
};
} // namespace

StreamReaderData::~StreamReaderData() {
    try {
        (void)close();
    } catch (...) {
    }
}
StreamReadResult StreamReaderData::read(std::size_t max_bytes, RuntimeContext* context) {
    std::shared_ptr<StreamReaderBackend> b;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 2) return {std::nullopt, StreamError{"io", "closed", "reader is closed", "", {}}};
        if (state == 3) return {std::nullopt, StreamError{"io", "poisoned", "reader is poisoned", "", {}}};
        b = backend;
    }
    if (!b) return {std::nullopt, StreamError{"io", "closed", "reader is closed", "", {}}};
    return b->read(max_bytes, context);
}
std::optional<StreamError> StreamReaderData::close() {
    std::shared_ptr<StreamReaderBackend> b;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 2) return std::nullopt;
        if (state == 3) return StreamError{"io", "poisoned", "reader is poisoned", "", {}};
        b = std::move(backend);
        state = 2;
    }
    if (!b) return std::nullopt;
    auto err = b->close();
    if (err) {
        std::lock_guard<std::mutex> lock(mutex);
        state = 3;
    }
    return err;
}

StreamWriterData::~StreamWriterData() {
    try {
        (void)close();
    } catch (...) {
    }
}
StreamWriteResult StreamWriterData::write(std::span<const char> data, RuntimeContext* context) {
    std::shared_ptr<StreamWriterBackend> b;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 2) return {0, StreamError{"io", "closed", "writer is closed", "", {}}};
        if (state == 3) return {0, StreamError{"io", "poisoned", "writer is poisoned", "", {}}};
        b = backend;
    }
    if (!b) return {0, StreamError{"io", "closed", "writer is closed", "", {}}};
    return b->write(data, context);
}
std::optional<StreamError> StreamWriterData::flush(RuntimeContext* context) {
    std::shared_ptr<StreamWriterBackend> b;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 2) return StreamError{"io", "closed", "writer is closed", "", {}};
        if (state == 3) return StreamError{"io", "poisoned", "writer is poisoned", "", {}};
        b = backend;
    }
    if (!b) return StreamError{"io", "closed", "writer is closed", "", {}};
    return b->flush(context);
}
std::optional<StreamError> StreamWriterData::close() {
    std::shared_ptr<StreamWriterBackend> b;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 2) return std::nullopt;
        if (state == 3) return StreamError{"io", "poisoned", "writer is poisoned", "", {}};
        b = std::move(backend);
        state = 2;
    }
    if (!b) return std::nullopt;
    auto err = b->close();
    if (err) {
        std::lock_guard<std::mutex> lock(mutex);
        state = 3;
    }
    return err;
}

Value make_reader_value(std::shared_ptr<StreamReaderBackend> backend) {
    return Value(ReaderValue{std::make_shared<StreamReaderData>(std::move(backend))});
}
Value make_writer_value(std::shared_ptr<StreamWriterBackend> backend) {
    return Value(WriterValue{std::make_shared<StreamWriterData>(std::move(backend))});
}
Value stream_error_result(const StreamError& err, std::string success_type, std::string origin) {
    return Value(ResultValue{false, result_box(Value(make_err(err, std::move(origin)))),
                             std::move(success_type), "std.error.Value", true, false});
}

Value invoke_stream_standard(const StandardFunction& function, const std::vector<Value>& args,
                             const SourceSpan& span, RuntimeContext* context) {
    const auto& op = function.name;
    std::string origin = "std.stream." + op;
    if (op == "from_bytes") {
        auto* b = get_if<BytesValue>(&args.at(0).data);
        if (!b) runtime_error(span, "std.stream.from_bytes expects Bytes", "E4003");
        return make_reader_value(std::make_shared<BytesReaderBackend>(b->data));
    }
    if (op == "from_file") {
        if (!holds<FileValue>(args.at(0).data)) runtime_error(span, "std.stream.from_file expects std.file.File", "E4003");
        auto* st_fn = standard_function("$std$file$state");
        if (st_fn) {
            Value st = invoke_file_standard(*st_fn, {args.at(0)}, span, context);
            if (holds<ResultValue>(st.data)) {
                const auto& rv = cget<ResultValue>(st.data);
                if (!rv.ok && rv.payload && holds<ErrorValue>(rv.payload->data)) {
                    const auto& ed = *cget<ErrorValue>(rv.payload->data).data;
                    return stream_error_result({"io", ed.code, ed.message, ed.native_domain, ed.native_code}, "std.stream.Reader", origin);
                }
                if (rv.ok && rv.payload) {
                    if (auto* s = string_data(&rv.payload->data); s && *s != "open") {
                        return stream_error_result({"io", *s, "file is not open", "", {}}, "std.stream.Reader", origin);
                    }
                }
            }
        }
        return ok_result(make_reader_value(std::make_shared<FileReaderBackend>(args.at(0), span)), "std.stream.Reader");
    }
    if (op == "to_buffer") {
        auto* b = get_if<BufferValue>(&args.at(0).data);
        if (!b) runtime_error(span, "std.stream.to_buffer expects Buffer", "E4003");
        if (!b->writable) runtime_error(span, "std.stream.to_buffer requires writable Buffer", "E4003");
        return make_writer_value(std::make_shared<BufferWriterBackend>(*b));
    }
    if (op == "to_file") {
        if (!holds<FileValue>(args.at(0).data)) runtime_error(span, "std.stream.to_file expects std.file.File", "E4003");
        auto* st_fn = standard_function("$std$file$state");
        if (st_fn) {
            Value st = invoke_file_standard(*st_fn, {args.at(0)}, span, context);
            if (holds<ResultValue>(st.data)) {
                const auto& rv = cget<ResultValue>(st.data);
                if (!rv.ok && rv.payload && holds<ErrorValue>(rv.payload->data)) {
                    const auto& ed = *cget<ErrorValue>(rv.payload->data).data;
                    return stream_error_result({"io", ed.code, ed.message, ed.native_domain, ed.native_code}, "std.stream.Writer", origin);
                }
                if (rv.ok && rv.payload) {
                    if (auto* s = string_data(&rv.payload->data); s && *s != "open") {
                        return stream_error_result({"io", *s, "file is not open", "", {}}, "std.stream.Writer", origin);
                    }
                }
            }
        }
        return ok_result(make_writer_value(std::make_shared<FileWriterBackend>(args.at(0), span)), "std.stream.Writer");
    }
    if (op == "limit") {
        auto* r = get_if<ReaderValue>(&args.at(0).data);
        auto* lim = get_if<std::int64_t>(&args.at(1).data);
        if (!r || !lim) runtime_error(span, "std.stream.limit expects (Reader, int)", "E4003");
        if (*lim < 0) return stream_error_result({"io", "invalid_argument", "limit must be non-negative", "", {}}, "std.stream.Reader", origin);
        return ok_result(make_reader_value(std::make_shared<LimitReaderBackend>(r->data, static_cast<std::size_t>(*lim))), "std.stream.Reader");
    }
    if (op == "read") {
        auto* r = get_if<ReaderValue>(&args.at(0).data);
        auto* max_b = get_if<std::int64_t>(&args.at(1).data);
        if (!r || !max_b) runtime_error(span, "std.stream.read expects (Reader, int)", "E4003");
        if (*max_b <= 0) return stream_error_result({"io", "invalid_argument", "max_bytes must be positive", "", {}}, "Bytes?", origin);
        std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(*max_b), kMaxSingleRead);
        auto res = r->data->read(want, context);
        if (res.error) return stream_error_result(*res.error, "Bytes?", origin);
        if (!res.chunk) return ok_result(Value{}, "Bytes?");
        return ok_result(Value(BytesValue{std::make_shared<std::string>(std::move(*res.chunk))}), "Bytes?");
    }
    if (op == "read_exact") {
        auto* r = get_if<ReaderValue>(&args.at(0).data);
        auto* need = get_if<std::int64_t>(&args.at(1).data);
        if (!r || !need) runtime_error(span, "std.stream.read_exact expects (Reader, int)", "E4003");
        if (*need < 0) return stream_error_result({"io", "invalid_argument", "byte count must be non-negative", "", {}}, "Bytes", origin);
        if (*need > 64 * 1024 * 1024) return stream_error_result({"io", "limit", "read_exact exceeds 64 MiB limit", "", {}}, "Bytes", origin);
        std::string out;
        out.reserve(static_cast<std::size_t>(*need));
        while (out.size() < static_cast<std::size_t>(*need)) {
            if (context && !context->cleanup && context->cancellation && context->cancellation->stopped()) {
                return stream_error_result({"io", "canceled", "read_exact canceled", "", {}}, "Bytes", origin);
            }
            std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(*need) - out.size(), kCopyChunk);
            auto res = r->data->read(want, context);
            if (res.error) return stream_error_result(*res.error, "Bytes", origin);
            if (!res.chunk || res.chunk->empty()) {
                return stream_error_result({"io", "unexpected_eof", "unexpected EOF before reading requested bytes", "", {}}, "Bytes", origin);
            }
            out.append(*res.chunk);
        }
        return ok_result(Value(BytesValue{std::make_shared<std::string>(std::move(out))}), "Bytes");
    }
    if (op == "read_all") {
        auto* r = get_if<ReaderValue>(&args.at(0).data);
        auto* max_b = get_if<std::int64_t>(&args.at(1).data);
        if (!r || !max_b) runtime_error(span, "std.stream.read_all expects (Reader, int)", "E4003");
        if (*max_b < 0) return stream_error_result({"io", "invalid_argument", "max_bytes must be non-negative", "", {}}, "Bytes", origin);
        std::size_t limit = static_cast<std::size_t>(*max_b);
        std::string out;
        for (;;) {
            if (context && !context->cleanup && context->cancellation && context->cancellation->stopped()) {
                return stream_error_result({"io", "canceled", "read_all canceled", "", {}}, "Bytes", origin);
            }
            std::size_t want = std::min<std::size_t>(kCopyChunk, (limit - out.size()) + 1);
            auto res = r->data->read(want, context);
            if (res.error) return stream_error_result(*res.error, "Bytes", origin);
            if (!res.chunk || res.chunk->empty()) break;
            if (out.size() + res.chunk->size() > limit) {
                return stream_error_result({"io", "limit", "stream exceeded max_bytes", "", {}}, "Bytes", origin);
            }
            out.append(*res.chunk);
        }
        return ok_result(Value(BytesValue{std::make_shared<std::string>(std::move(out))}), "Bytes");
    }
    if (op == "write") {
        auto* w = get_if<WriterValue>(&args.at(0).data);
        auto* b = get_if<BytesValue>(&args.at(1).data);
        if (!w || !b) runtime_error(span, "std.stream.write expects (Writer, Bytes)", "E4003");
        std::string_view sv = b->data ? std::string_view(*b->data) : std::string_view{};
        auto res = w->data->write(std::span<const char>(sv.data(), sv.size()), context);
        if (res.error) return stream_error_result(*res.error, "int", origin);
        return ok_result(Value(static_cast<std::int64_t>(res.written)), "int");
    }
    if (op == "write_all") {
        auto* w = get_if<WriterValue>(&args.at(0).data);
        auto* b = get_if<BytesValue>(&args.at(1).data);
        if (!w || !b) runtime_error(span, "std.stream.write_all expects (Writer, Bytes)", "E4003");
        std::string_view sv = b->data ? std::string_view(*b->data) : std::string_view{};
        std::size_t sent = 0;
        while (sent < sv.size()) {
            if (context && !context->cleanup && context->cancellation && context->cancellation->stopped()) {
                return stream_error_result({"io", "canceled", "write_all canceled", "", {}}, "void", origin);
            }
            auto res = w->data->write(std::span<const char>(sv.data() + sent, sv.size() - sent), context);
            if (res.error) return stream_error_result(*res.error, "void", origin);
            if (res.written == 0) {
                return stream_error_result({"io", "io", "writer made zero progress", "", {}}, "void", origin);
            }
            sent += res.written;
        }
        return ok_void();
    }
    if (op == "flush") {
        auto* w = get_if<WriterValue>(&args.at(0).data);
        if (!w) runtime_error(span, "std.stream.flush expects Writer", "E4003");
        auto err = w->data->flush(context);
        if (err) return stream_error_result(*err, "void", origin);
        return ok_void();
    }
    if (op == "close_reader") {
        auto* r = get_if<ReaderValue>(&args.at(0).data);
        if (!r) runtime_error(span, "std.stream.close_reader expects Reader", "E4003");
        auto err = r->data->close();
        if (err) return stream_error_result(*err, "void", origin);
        return ok_void();
    }
    if (op == "close_writer") {
        auto* w = get_if<WriterValue>(&args.at(0).data);
        if (!w) runtime_error(span, "std.stream.close_writer expects Writer", "E4003");
        auto err = w->data->close();
        if (err) return stream_error_result(*err, "void", origin);
        return ok_void();
    }
    if (op == "copy") {
        auto* dst = get_if<WriterValue>(&args.at(0).data);
        auto* src = get_if<ReaderValue>(&args.at(1).data);
        auto* max_b = get_if<std::int64_t>(&args.at(2).data);
        if (!dst || !src || !max_b) runtime_error(span, "std.stream.copy expects (Writer, Reader, int)", "E4003");
        if (*max_b < 0) return stream_error_result({"io", "invalid_argument", "max_bytes must be non-negative", "", {}}, "int", origin);
        std::size_t limit = static_cast<std::size_t>(*max_b);
        std::size_t total = 0;
        for (;;) {
            if (context && !context->cleanup && context->cancellation && context->cancellation->stopped()) {
                return stream_error_result({"io", "canceled", "copy canceled", "", {}}, "int", origin);
            }
            std::size_t want = std::min<std::size_t>(kCopyChunk, (limit - total) + 1);
            auto rd = src->data->read(want, context);
            if (rd.error) return stream_error_result(*rd.error, "int", origin);
            if (!rd.chunk || rd.chunk->empty()) break;
            if (total + rd.chunk->size() > limit) {
                return stream_error_result({"io", "limit", "copy exceeded max_bytes", "", {}}, "int", origin);
            }
            std::size_t sent = 0;
            while (sent < rd.chunk->size()) {
                auto wr = dst->data->write(std::span<const char>(rd.chunk->data() + sent, rd.chunk->size() - sent), context);
                if (wr.error) return stream_error_result(*wr.error, "int", origin);
                if (wr.written == 0) return stream_error_result({"io", "io", "writer made zero progress", "", {}}, "int", origin);
                sent += wr.written;
            }
            total += rd.chunk->size();
        }
        return ok_result(Value(static_cast<std::int64_t>(total)), "int");
    }
    runtime_error(span, "unknown std.stream function `" + op + "`", "E4001");
}
} // namespace hua
