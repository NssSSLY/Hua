#include "hua/source.hpp"
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <vector>
namespace hua {
Source::Source(std::string filename, std::string text) : filename_(std::move(filename)) {
    std::size_t start = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        if (text[i] == '\r') {
            text_ += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else text_ += text[i];
    }
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i)
        if (text_[i] == '\n') line_starts_.push_back(i + 1);
}
SourceSpan Source::span(std::size_t start, std::size_t end) const {
    start = std::min(start, text_.size());
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), start);
    const auto line = static_cast<std::size_t>(it - line_starts_.begin());
    return {filename_, start, std::max(start,std::min(end, text_.size())), line, start - line_starts_[line - 1] + 1};
}
std::string_view Source::line_text(std::size_t number) const {
    if (number == 0 || number > line_starts_.size()) return {};
    const auto start = line_starts_[number - 1];
    auto end = text_.find('\n', start);
    if (end == std::string::npos) end = text_.size();
    return std::string_view(text_).substr(start, end - start);
}
void SourceTrace::add(const SourceSpan& span,std::string message){
    if(span.file_id.empty())return;
    for(const auto& site:sites)if(site.span==span)return;
    if(sites.size()==32){truncated=true;return;}
    sites.push_back({span,std::move(message)});
}
std::string render_source_trace(const SourceTrace& trace){
    std::ostringstream out;
    for(const auto& site:trace.sites)out<<"note: "<<site.message<<" at "<<site.span.file_id<<':'<<site.span.line<<':'<<site.span.column<<'\n';
    if(trace.truncated)out<<"note: further source context omitted (limit 32)\n";
    return out.str();
}
Diagnostic Diagnostic::located(const SourceSpan& span) const{
    auto out=*this;if(out.span_.file_id.empty())out.span_=span;return out;
}
void Diagnostic::add_context(const SourceSpan& span,std::string message){if(span!=span_)trace_.add(span,std::move(message));}
void Diagnostic::add_trace(const SourceTrace& trace){for(const auto& site:trace.sites)add_context(site.span,site.message);trace_.truncated=trace_.truncated||trace.truncated;}
Diagnostic::Diagnostic(std::string code, SourceSpan span, std::string message, std::string help)
    : std::runtime_error(std::move(message)), code_(std::move(code)), help_(std::move(help)), span_(std::move(span)) {}
std::string Diagnostic::render(const Source& source) const {
    std::ostringstream out;
    out << "error[" << code_ << "]: " << what() << '\n'
        << " --> " << span_.file_id << ':' << span_.line << ':' << span_.column << '\n';
    auto expected=source.span(span_.start_offset,span_.end_offset);
    if(span_.file_id!=source.filename()||span_.start_offset>source.text().size()||
       expected.line!=span_.line||expected.column!=span_.column){
        if(!help_.empty())out<<"help: "<<help_<<'\n';
        out<<render_source_trace(trace_);return out.str();
    }
    auto line = source.line_text(span_.line);
    const auto digits = std::to_string(span_.line).size();
    std::string displayed, underline;
    static constexpr char hex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < line.size(); ++i) {
        auto byte = static_cast<unsigned char>(line[i]);
        std::string part;
        // Invalid UTF-8 is escaped, so even an encoding error produces valid UTF-8 diagnostics.
        if ((code_ == "E1009" && byte >= 0x80) || (byte < 0x20 && byte != '\t')) {
            part = "\\x";
            part += hex[byte >> 4]; part += hex[byte & 15];
        } else part += line[i];
        displayed += part;
        if (i + 1 < span_.column) {
            if (part == "\t") underline += '\t';
            else if (part.size() == 4) underline += "    ";
            else if (byte < 0x80) underline += ' ';
            else if (byte >= 0xC0) {
                std::uint32_t codepoint = byte & (byte < 0xE0 ? 0x1F : byte < 0xF0 ? 0x0F : 0x07);
                auto count = byte < 0xE0 ? 1u : byte < 0xF0 ? 2u : 3u;
                for (unsigned j = 1; j <= count && i + j < line.size(); ++j)
                    codepoint = (codepoint << 6) | (static_cast<unsigned char>(line[i + j]) & 0x3F);
                const bool combining = (codepoint >= 0x300 && codepoint <= 0x36F);
                const bool wide = (codepoint >= 0x1100 && codepoint <= 0x115F) ||
                    (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
                    (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
                    (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
                    (codepoint >= 0xFF01 && codepoint <= 0xFF60) || codepoint >= 0x1F300;
                if (!combining) underline += wide ? "  " : " ";
            }
        }
    }
    out << std::string(digits + 1, ' ') << "|\n" << span_.line << " | " << displayed << '\n'
        << std::string(digits + 1, ' ') << "| " << underline;
    out << '^' << '\n';
    if (!help_.empty()) out << "help: " << help_ << '\n';
    out<<render_source_trace(trace_);
    return out.str();
}
}
