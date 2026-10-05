#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace hua {
struct SourceSpan {
    std::string file_id;
    std::size_t start_offset{}, end_offset{}, line{1}, column{1};
};
class Source {
public:
    Source(std::string filename, std::string text);
    const std::string& filename() const { return filename_; }
    const std::string& text() const { return text_; }
    SourceSpan span(std::size_t start, std::size_t end) const;
    std::string_view line_text(std::size_t number) const;
private:
    std::string filename_, text_;
    std::vector<std::size_t> line_starts_;
};
class Diagnostic final : public std::runtime_error {
public:
    Diagnostic(std::string code, SourceSpan span, std::string message, std::string help = {});
    const std::string& code() const { return code_; }
    const SourceSpan& span() const { return span_; }
    std::string render(const Source& source) const;
private:
    std::string code_, help_;
    SourceSpan span_;
};
}
