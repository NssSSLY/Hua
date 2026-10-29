#include "hua/stdlib.hpp"
#include <array>
namespace hua {
namespace {
constexpr std::size_t max_text=16*1024*1024;
constexpr char hex_digits[]="0123456789abcdef";
constexpr char standard_alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char url_alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr unsigned char invalid=255;
constexpr std::array<unsigned char,256> reverse(const char* alphabet){std::array<unsigned char,256> out{};out.fill(invalid);for(unsigned i=0;i<64;++i)out[static_cast<unsigned char>(alphabet[i])]=static_cast<unsigned char>(i);return out;}
constexpr auto standard_values=reverse(standard_alphabet),url_values=reverse(url_alphabet);
constexpr std::array<unsigned char,256> hex_values=[]{std::array<unsigned char,256> out{};out.fill(invalid);for(unsigned i=0;i<10;++i)out['0'+i]=static_cast<unsigned char>(i);for(unsigned i=0;i<6;++i){out['a'+i]=static_cast<unsigned char>(10+i);out['A'+i]=static_cast<unsigned char>(10+i);}return out;}();
Value bytes_ok(std::string data){return standard_result(Value(BytesValue{managed<const std::string>(std::move(data))}),true,"Bytes");}
Value syntax(const std::string& what){return standard_result(Value("ENC_SYNTAX: "+what),false,"Bytes");}
void text_limit(std::size_t size,const SourceSpan& s){if(size>max_text)runtime_error(s,"encoded text exceeds 16 MiB","E4099");}

std::string base64_encode(const std::string& in,const char* alphabet,bool pad){
    std::string out;out.reserve((in.size()+2)/3*4);std::size_t i=0;
    for(;i+3<=in.size();i+=3){
        unsigned n=static_cast<unsigned char>(in[i])<<16|static_cast<unsigned char>(in[i+1])<<8|static_cast<unsigned char>(in[i+2]);
        out+=alphabet[n>>18];out+=alphabet[n>>12&63];out+=alphabet[n>>6&63];out+=alphabet[n&63];
    }
    if(auto rest=in.size()-i){
        unsigned n=static_cast<unsigned char>(in[i])<<16|(rest==2?static_cast<unsigned char>(in[i+1])<<8:0u);
        out+=alphabet[n>>18];out+=alphabet[n>>12&63];
        if(rest==2)out+=alphabet[n>>6&63];else if(pad)out+='=';
        if(pad)out+='=';
    }
    return out;
}
// Strict RFC 4648 decoding. `padded` requires full `=` padding; otherwise padding
// is optional but, when present, must be complete. Non-zero trailing bits are rejected.
Value base64_decode(const std::string& in,const std::array<unsigned char,256>& values,bool padded){
    auto size=in.size();std::size_t pad=0;
    while(pad<2&&pad<size&&in[size-1-pad]=='=')++pad;
    if(padded||pad){if(size%4)return syntax("invalid length");}
    else if(size%4==1)return syntax("invalid length");
    auto body=size-pad;
    if(pad&&body%4+pad!=4)return syntax("invalid padding");
    std::string out;out.reserve(body/4*3+2);unsigned acc=0;int bits=0;
    for(std::size_t i=0;i<body;++i){
        auto v=values[static_cast<unsigned char>(in[i])];
        if(v==invalid)return syntax(in[i]=='='?"invalid padding":"invalid character at byte "+std::to_string(i));
        acc=(acc<<6|v)&0xffffff;bits+=6;
        if(bits>=8){bits-=8;out+=static_cast<char>(acc>>bits&255);}
    }
    if(bits&&(acc&((1u<<bits)-1)))return syntax("non-zero trailing bits");
    return bytes_ok(std::move(out));
}
}

Value invoke_encoding_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    const auto& name=f.name;
    if(name=="hex_encode"||name=="base64_encode"||name=="base64url_encode"){
        const auto& in=*std::get<BytesValue>(args[0].data).data;
        if(name=="hex_encode"){
            text_limit(in.size()*2,s);std::string out(in.size()*2,'\0');
            for(std::size_t i=0;i<in.size();++i){auto b=static_cast<unsigned char>(in[i]);out[2*i]=hex_digits[b>>4];out[2*i+1]=hex_digits[b&15];}
            return Value(std::move(out));
        }
        const bool url=name=="base64url_encode";
        text_limit(url?(in.size()*4+2)/3:(in.size()+2)/3*4,s);
        return Value(base64_encode(in,url?url_alphabet:standard_alphabet,!url));
    }
    const auto& text=hua::string_value(args[0].data);
    if(name=="hex_decode"){
        if(text.size()%2)return syntax("odd length");
        std::string out(text.size()/2,'\0');
        for(std::size_t i=0;i<out.size();++i){
            auto hi=hex_values[static_cast<unsigned char>(text[2*i])],lo=hex_values[static_cast<unsigned char>(text[2*i+1])];
            if(hi==invalid)return syntax("invalid character at byte "+std::to_string(2*i));
            if(lo==invalid)return syntax("invalid character at byte "+std::to_string(2*i+1));
            out[i]=static_cast<char>(hi<<4|lo);
        }
        return bytes_ok(std::move(out));
    }
    if(name=="base64_decode")return base64_decode(text,standard_values,true);
    if(name=="base64url_decode")return base64_decode(text,url_values,false);
    runtime_error(s,"unknown encoding operation","E4003");
}
}
