#include "hua/stdlib.hpp"
#include <array>
#include <bit>
#include <cmath>
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

// One strict UTF-8 scalar at `at`, same rules as text_utf8_error; {-1,0} when invalid or out of range.
std::pair<std::int64_t,std::int64_t> utf8_scalar(const std::string& in,std::int64_t offset){
    if(offset<0||static_cast<std::uint64_t>(offset)>=in.size())return {-1,0};
    const auto at=static_cast<std::size_t>(offset);const auto lead=static_cast<unsigned char>(in[at]);
    if(lead<128)return {lead,1};
    std::size_t extra;std::int64_t scalar,least;
    if(lead>=194&&lead<=223){extra=1;scalar=lead&31;least=128;}else if(lead>=224&&lead<=239){extra=2;scalar=lead&15;least=2048;}else if(lead>=240&&lead<=244){extra=3;scalar=lead&7;least=65536;}else return {-1,0};
    if(extra>in.size()-at-1)return {-1,0};
    for(std::size_t i=1;i<=extra;++i){const auto c=static_cast<unsigned char>(in[at+i]);if((c&192)!=128)return {-1,0};scalar=scalar<<6|(c&63);}
    if(scalar<least||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return {-1,0};
    return {scalar,static_cast<std::int64_t>(extra+1)};
}
bool big_endian(const Value& order,const SourceSpan& s){
    const auto& text=string_value(order.data);
    if(text=="be")return true;
    if(text!="le")runtime_error(s,"byte order must be \"le\" or \"be\"","E4003");
    return false;
}
unsigned pack_width(const Value& width,const SourceSpan& s){
    const auto n=as_int(width,s);
    if(n!=1&&n!=2&&n!=4&&n!=8)runtime_error(s,"pack width must be 1, 2, 4 or 8","E4003");
    return static_cast<unsigned>(n);
}
Value packed(std::uint64_t bits,unsigned width,bool big){
    std::string out(width,'\0');
    for(unsigned i=0;i<width;++i)out[big?width-1-i:i]=static_cast<char>(bits>>(8*i)&255);
    return bytes_ok(std::move(out));
}
// Bits of `width` bytes at `offset`; false when they are not all there.
bool unpacked(const std::string& in,std::int64_t offset,unsigned width,bool big,std::uint64_t& bits){
    if(offset<0||static_cast<std::uint64_t>(offset)>in.size()||width>in.size()-static_cast<std::size_t>(offset))return false;
    bits=0;const auto at=static_cast<std::size_t>(offset);
    for(unsigned i=0;i<width;++i)bits|=static_cast<std::uint64_t>(static_cast<unsigned char>(in[at+(big?width-1-i:i)]))<<(8*i);
    return true;
}
Value invoke_pack(const std::string& name,const std::vector<Value>& args,const SourceSpan& s){
    auto failed=[](const char* what,const char* type){return standard_result(Value(std::string(what)),false,type);};
    constexpr const char* range="ENC_RANGE: value out of range for width";constexpr const char* truncated="ENC_TRUNCATED: not enough bytes";constexpr const char* infinite="ENC_RANGE: value is not finite";
    if(name=="pack_uint"||name=="pack_int"){
        const auto v=as_int(args[0],s);const auto width=pack_width(args[1],s);const auto big=big_endian(args[2],s);
        const auto bits=8*width;
        const bool fits=name=="pack_uint"?v>=0&&(width==8||v<(std::int64_t{1}<<bits)):width==8||(v>=-(std::int64_t{1}<<(bits-1))&&v<(std::int64_t{1}<<(bits-1)));
        if(!fits)return failed(range,"Bytes");
        return packed(static_cast<std::uint64_t>(v),width,big);
    }
    if(name=="unpack_uint"||name=="unpack_int"){
        const auto& in=*std::get<BytesValue>(args[0].data).data;const auto offset=as_int(args[1],s);const auto width=pack_width(args[2],s);const auto big=big_endian(args[3],s);
        std::uint64_t bits;
        if(!unpacked(in,offset,width,big,bits))return failed(truncated,"int");
        if(name=="unpack_uint"){
            if(width==8&&bits>>63)return failed("ENC_RANGE: value does not fit in int","int");
            return standard_result(Value(static_cast<std::int64_t>(bits)),true,"int");
        }
        if(width<8&&(bits>>(8*width-1)&1))bits|=~std::uint64_t{0}<<(8*width);
        return standard_result(Value(std::bit_cast<std::int64_t>(bits)),true,"int");
    }
    if(name=="pack_f64"||name=="pack_f32"){
        const auto x=std::get<double>(args[0].data);const auto big=big_endian(args[1],s);
        if(name=="pack_f64")return packed(std::bit_cast<std::uint64_t>(x),8,big);
        const auto narrow=static_cast<float>(x);
        if(!std::isfinite(narrow))return failed(range,"Bytes");
        return packed(std::bit_cast<std::uint32_t>(narrow),4,big);
    }
    const auto& in=*std::get<BytesValue>(args[0].data).data;const auto offset=as_int(args[1],s);const auto big=big_endian(args[2],s);
    const auto wide=name=="unpack_f64";std::uint64_t bits;
    if(!unpacked(in,offset,wide?8:4,big,bits))return failed(truncated,"float");
    const double x=wide?std::bit_cast<double>(bits):static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
    if(!std::isfinite(x))return failed(infinite,"float");
    return standard_result(Value(x),true,"float");
}
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

bool append_utf8(std::string& out,std::int64_t scalar){
    if(scalar<0||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return false;
    const auto v=static_cast<std::uint32_t>(scalar);
    if(v<0x80)out+=static_cast<char>(v);
    else if(v<0x800){out+=static_cast<char>(0xC0|v>>6);out+=static_cast<char>(0x80|(v&63));}
    else if(v<0x10000){out+=static_cast<char>(0xE0|v>>12);out+=static_cast<char>(0x80|(v>>6&63));out+=static_cast<char>(0x80|(v&63));}
    else{out+=static_cast<char>(0xF0|v>>18);out+=static_cast<char>(0x80|(v>>12&63));out+=static_cast<char>(0x80|(v>>6&63));out+=static_cast<char>(0x80|(v&63));}
    return true;
}
Value invoke_encoding_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    const auto& name=f.name;
    if(f.since==24)return invoke_pack(name,args,s);
    if(name=="utf8_valid")return Value(text_utf8_error(*std::get<BytesValue>(args[0].data).data).empty());
    if(name=="utf8_encode"){
        std::string out;
        if(!append_utf8(out,as_int(args[0],s)))return standard_result(Value(std::string("ENC_RANGE: invalid Unicode scalar")),false,"Bytes");
        return bytes_ok(std::move(out));
    }
    if(name=="utf8_decode"){
        const auto [scalar,size]=utf8_scalar(*std::get<BytesValue>(args[0].data).data,as_int(args[1],s));
        return make_multi({Value(scalar),Value(size)});
    }
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
