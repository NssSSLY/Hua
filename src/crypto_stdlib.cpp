#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include <array>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fstream>
#include <sys/random.h>
#endif
namespace hua {
namespace {
using I=std::int64_t;
constexpr std::size_t max_random=1024*1024;
// FIPS 180-4 SHA-256.
constexpr std::array<std::uint32_t,64> K{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
std::uint32_t rotr(std::uint32_t x,int n){return x>>n|x<<(32-n);}
void compress(std::array<std::uint32_t,8>& h,const unsigned char* block){
    std::uint32_t w[64];
    for(int i=0;i<16;++i)w[i]=std::uint32_t(block[4*i])<<24|std::uint32_t(block[4*i+1])<<16|std::uint32_t(block[4*i+2])<<8|block[4*i+3];
    for(int i=16;i<64;++i){auto s0=rotr(w[i-15],7)^rotr(w[i-15],18)^w[i-15]>>3,s1=rotr(w[i-2],17)^rotr(w[i-2],19)^w[i-2]>>10;w[i]=w[i-16]+s0+w[i-7]+s1;}
    auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],k=h[7];
    for(int i=0;i<64;++i){
        auto t1=k+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+K[i]+w[i];
        auto t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
        k=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=k;
}
// Hashes the concatenation of parts (HMAC hashes pad||message without copying).
std::string sha256(std::initializer_list<std::string_view> parts,RuntimeContext* context,const SourceSpan& s){
    std::array<std::uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    unsigned char block[64];std::size_t fill=0;std::uint64_t total=0,since_check=0;
    for(auto part:parts){
        auto* p=reinterpret_cast<const unsigned char*>(part.data());auto n=part.size();total+=n;
        while(n){
            if(!fill&&n>=64){compress(h,p);p+=64;n-=64;}
            else{auto take=std::min<std::size_t>(64-fill,n);std::memcpy(block+fill,p,take);fill+=take;p+=take;n-=take;if(fill==64){compress(h,block);fill=0;}}
            if((since_check+=64)>=1024*1024){since_check=0;task_checkpoint(context,s);}
        }
    }
    block[fill++]=0x80;
    if(fill>56){std::memset(block+fill,0,64-fill);compress(h,block);fill=0;}
    std::memset(block+fill,0,56-fill);
    for(int i=0;i<8;++i)block[56+i]=static_cast<unsigned char>(total*8>>(56-8*i));
    compress(h,block);
    std::string out(32,'\0');
    for(int i=0;i<8;++i)for(int j=0;j<4;++j)out[4*i+j]=static_cast<char>(h[i]>>(24-8*j));
    return out;
}
Value bytes(std::string data){return Value(BytesValue{managed<const std::string>(std::move(data))});}
const std::string& bytes_arg(const Value& v){return *std::get<BytesValue>(v.data).data;}
// Operating-system CSPRNG; an empty string with `error` set when unavailable.
std::string os_random(std::size_t n,std::string& error){
    std::string out(n,'\0');if(!n)return out;
#ifdef _WIN32
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(out.data()),static_cast<ULONG>(n),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)error="BCryptGenRandom failed";
#else
    std::size_t done=0;
    while(done<n){auto got=getrandom(out.data()+done,n-done,0);if(got<0){if(errno==EINTR)continue;break;}done+=static_cast<std::size_t>(got);}
    if(done<n){std::ifstream urandom("/dev/urandom",std::ios::binary);if(urandom.read(out.data()+done,static_cast<std::streamsize>(n-done)))done=n;}
    if(done<n)error="operating system random source is unavailable";
#endif
    return error.empty()?out:std::string();
}
}

Value invoke_crypto_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s,RuntimeContext* context){
    const auto& name=f.name;
    if(name=="sha256")return bytes(sha256({bytes_arg(args[0])},context,s));
    if(name=="hmac_sha256"){
        // RFC 2104: keys longer than the 64-byte block are hashed first.
        std::string key=bytes_arg(args[0]);if(key.size()>64)key=sha256({key},context,s);key.resize(64,'\0');
        std::string inner(64,'\0'),outer(64,'\0');
        for(int i=0;i<64;++i){inner[i]=static_cast<char>(key[i]^0x36);outer[i]=static_cast<char>(key[i]^0x5c);}
        auto digest=sha256({inner,bytes_arg(args[1])},context,s);
        return bytes(sha256({outer,digest},context,s));
    }
    if(name=="equal"){
        // Constant time for equal lengths: no early exit on the first difference.
        const auto& a=bytes_arg(args[0]);const auto& b=bytes_arg(args[1]);if(a.size()!=b.size())return Value(false);
        unsigned char diff=0;for(std::size_t i=0;i<a.size();++i)diff|=static_cast<unsigned char>(a[i]^b[i]);
        return Value(diff==0);
    }
    if(name=="random_bytes"||name=="uuid4"){
        std::size_t n=16;
        if(name=="random_bytes"){auto count=std::get<I>(args[0].data);if(count<0)runtime_error(s,"crypto.random_bytes requires n >= 0","E4003");if(static_cast<std::uint64_t>(count)>max_random)runtime_error(s,"crypto.random_bytes exceeds 1 MiB per call","E4099");n=static_cast<std::size_t>(count);}
        std::string error;auto data=os_random(n,error);
        const std::string success=name=="uuid4"?"string":"Bytes";
        if(!error.empty())return standard_result(Value("CRYPTO_UNAVAILABLE: "+error),false,success);
        if(name=="random_bytes")return standard_result(bytes(std::move(data)),true,success);
        // RFC 9562 version 4: version nibble 0100, variant bits 10.
        data[6]=static_cast<char>((data[6]&0x0f)|0x40);data[8]=static_cast<char>((data[8]&0x3f)|0x80);
        static constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(36);
        for(int i=0;i<16;++i){if(i==4||i==6||i==8||i==10)text+='-';auto v=static_cast<unsigned char>(data[i]);text+=digits[v>>4];text+=digits[v&15];}
        return standard_result(Value(std::move(text)),true,success);
    }
    runtime_error(s,"unknown crypto operation","E4003");
}
}
