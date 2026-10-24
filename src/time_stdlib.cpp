#include "hua/stdlib.hpp"
#include <cstdio>
namespace hua {
namespace {
using I=std::int64_t;using U=std::uint64_t;
constexpr I day_ms=86400000;
// Proleptic Gregorian calendar, years 0001..9999 (RFC 3339), Unix time without
// leap seconds. Howard Hinnant's days_from_civil / civil_from_days.
I days_from_civil(I y,I m,I d){
    y-=m<=2;const I era=(y>=0?y:y-399)/400;const I yoe=y-era*400;
    const I doy=(153*(m>2?m-3:m+9)+2)/5+d-1;const I doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+doe-719468;
}
struct Civil{I year,month,day;};
Civil civil_from_days(I z){
    z+=719468;const I era=(z>=0?z:z-146096)/146097;const I doe=z-era*146097;
    const I yoe=(doe-doe/1460+doe/36524-doe/146096)/365;const I doy=doe-(365*yoe+yoe/4-yoe/100);
    const I mp=(5*doy+2)/153;const I d=doy-(153*mp+2)/5+1;const I m=mp<10?mp+3:mp-9;
    return {yoe+era*400+(m<=2),m,d};
}
const I min_ms=days_from_civil(1,1,1)*day_ms,max_ms=days_from_civil(10000,1,1)*day_ms-1;
bool leap(I y){return (y%4==0&&y%100!=0)||y%400==0;}
I month_days(I y,I m){static constexpr I table[]{31,28,31,30,31,30,31,31,30,31,30,31};return m==2&&leap(y)?29:table[m-1];}
I floor_div(I a,I b){I q=a/b;if((a%b!=0)&&((a<0)!=(b<0)))--q;return q;}
I integer(const Value& v,const SourceSpan& s){auto p=std::get_if<I>(&v.data);if(!p)runtime_error(s,"expected int","E4003");return *p;}
const std::string& text(const Value& v,const SourceSpan& s){auto p=std::get_if<std::string>(&v.data);if(!p)runtime_error(s,"expected string","E4003");return *p;}
void in_range(I ms,const SourceSpan& s){if(ms<min_ms||ms>max_ms)runtime_error(s,"time outside years 0001..9999","E4003");}
struct Parts{I year,month,day,hour,minute,second,millis;};
Parts parts(I ms){
    auto days=floor_div(ms,day_ms);auto rest=ms-days*day_ms;auto c=civil_from_days(days);
    return {c.year,c.month,c.day,rest/3600000,rest/60000%60,rest/1000%60,rest%1000};
}
std::string rfc3339(I ms,const char* zone){
    auto p=parts(ms);char buffer[40];
    int n=p.millis?std::snprintf(buffer,sizeof buffer,"%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lld%s",(long long)p.year,(long long)p.month,(long long)p.day,(long long)p.hour,(long long)p.minute,(long long)p.second,(long long)p.millis,zone)
                  :std::snprintf(buffer,sizeof buffer,"%04lld-%02lld-%02lldT%02lld:%02lld:%02lld%s",(long long)p.year,(long long)p.month,(long long)p.day,(long long)p.hour,(long long)p.minute,(long long)p.second,zone);
    return std::string(buffer,static_cast<std::size_t>(n));
}
Value err(const std::string& message,const std::string& success){return standard_result(Value(message),false,success);}
// RFC 3339 date-time; the result is UTC milliseconds (fraction truncated to ms).
Value parse_rfc3339(const std::string& t){
    std::size_t at=0;bool ok=true;
    auto digits=[&](int count){I v=0;for(int i=0;i<count;++i){if(at>=t.size()||t[at]<'0'||t[at]>'9'){ok=false;return I{0};}v=v*10+(t[at++]-'0');}return v;};
    auto expect=[&](char a,char b=0){if(at<t.size()&&(t[at]==a||(b&&t[at]==b))){++at;return true;}ok=false;return false;};
    auto syntax=[&]{return err("TIME_SYNTAX: invalid RFC 3339 date-time","int");};
    I year=digits(4);expect('-');I month=digits(2);expect('-');I day=digits(2);expect('T','t');
    I hour=digits(2);expect(':');I minute=digits(2);expect(':');I second=digits(2);
    if(!ok)return syntax();
    I millis=0;
    if(at<t.size()&&t[at]=='.'){
        ++at;int count=0;
        while(at<t.size()&&t[at]>='0'&&t[at]<='9'){if(count<3)millis=millis*10+(t[at]-'0');++count;++at;}
        if(count==0||count>9)return syntax();
        for(int i=count;i<3;++i)millis*=10;
    }
    I offset=0;
    if(at<t.size()&&(t[at]=='Z'||t[at]=='z'))++at;
    else if(at<t.size()&&(t[at]=='+'||t[at]=='-')){
        const I sign=t[at++]=='-'?-1:1;I oh=digits(2);expect(':');I om=digits(2);
        if(!ok)return syntax();
        if(oh>23||om>59)return err("TIME_RANGE: offset outside -23:59..+23:59","int");
        offset=sign*(oh*60+om);
    }
    else return syntax();
    if(at!=t.size())return syntax();
    if(second==60)return err("TIME_RANGE: leap seconds are not representable","int");
    if(year<1||month<1||month>12||day<1||day>month_days(year,month)||hour>23||minute>59||second>59)return err("TIME_RANGE: date-time field out of range","int");
    I ms=(days_from_civil(year,month,day)*24+hour)*3600000+minute*60000+second*1000+millis-offset*60000;
    if(ms<min_ms||ms>max_ms)return err("TIME_RANGE: time outside years 0001..9999","int");
    return standard_result(Value(ms),true,"int");
}
// Go time.Duration.String(), byte for byte.
std::string format_duration(I d){
    char buffer[32];std::size_t w=sizeof buffer;U u=static_cast<U>(d);const bool neg=d<0;if(neg)u=U(0)-u;
    auto fraction=[&](U v,int prec){bool print=false;for(int i=0;i<prec;++i){auto digit=v%10;print=print||digit!=0;if(print)buffer[--w]=static_cast<char>('0'+digit);v/=10;}if(print)buffer[--w]='.';return v;};
    auto integer_part=[&](U v){if(v==0)buffer[--w]='0';else while(v>0){buffer[--w]=static_cast<char>('0'+v%10);v/=10;}};
    if(u<1000000000ull){
        int prec=0;buffer[--w]='s';
        if(u==0)return "0s";
        if(u<1000ull){prec=0;buffer[--w]='n';}
        else if(u<1000000ull){prec=3;buffer[--w]=static_cast<char>(0xb5);buffer[--w]=static_cast<char>(0xc2);}
        else{prec=6;buffer[--w]='m';}
        u=fraction(u,prec);integer_part(u);
    }else{
        buffer[--w]='s';u=fraction(u,9);integer_part(u%60);u/=60;
        if(u>0){buffer[--w]='m';integer_part(u%60);u/=60;if(u>0){buffer[--w]='h';integer_part(u);}}
    }
    if(neg)buffer[--w]='-';
    return std::string(buffer+w,sizeof buffer-w);
}
// Go time.ParseDuration.
Value parse_duration(const std::string& original){
    auto syntax=[&]{return err("TIME_SYNTAX: invalid duration","int");};
    auto range=[&]{return err("TIME_RANGE: duration overflows int64 nanoseconds","int");};
    std::string_view s=original;U total=0;bool neg=false;
    if(!s.empty()&&(s[0]=='-'||s[0]=='+')){neg=s[0]=='-';s.remove_prefix(1);}
    if(s=="0")return standard_result(Value(I{0}),true,"int");
    if(s.empty())return syntax();
    constexpr U limit=U(1)<<63;
    while(!s.empty()){
        if(!(s[0]=='.'||(s[0]>='0'&&s[0]<='9')))return syntax();
        U v=0;std::size_t i=0;
        for(;i<s.size()&&s[i]>='0'&&s[i]<='9';++i){if(v>(limit-1)/10)return range();v=v*10+static_cast<U>(s[i]-'0');if(v>limit)return range();}
        const bool pre=i>0;s.remove_prefix(i);
        U f=0;double scale=1;bool post=false;
        if(!s.empty()&&s[0]=='.'){
            s.remove_prefix(1);std::size_t j=0;bool overflow=false;
            for(;j<s.size()&&s[j]>='0'&&s[j]<='9';++j){if(overflow)continue;if(f>(limit-1)/10){overflow=true;continue;}U y=f*10+static_cast<U>(s[j]-'0');if(y>limit){overflow=true;continue;}f=y;scale*=10;}
            post=j>0;s.remove_prefix(j);
        }
        if(!pre&&!post)return syntax();
        std::size_t k=0;while(k<s.size()&&s[k]!='.'&&!(s[k]>='0'&&s[k]<='9'))++k;
        if(k==0)return syntax();
        auto unit_text=s.substr(0,k);s.remove_prefix(k);U unit=0;
        if(unit_text=="ns")unit=1;else if(unit_text=="us"||unit_text=="\xc2\xb5s"||unit_text=="\xce\xbcs")unit=1000;
        else if(unit_text=="ms")unit=1000000;else if(unit_text=="s")unit=1000000000;
        else if(unit_text=="m")unit=60000000000ull;else if(unit_text=="h")unit=3600000000000ull;
        else return syntax();
        if(v>limit/unit)return range();
        v*=unit;
        if(f>0){v+=static_cast<U>(static_cast<double>(f)*(static_cast<double>(unit)/scale));if(v>limit)return range();}
        total+=v;if(total>limit)return range();
    }
    if(neg)return standard_result(Value(static_cast<I>(U(0)-total)),true,"int");
    if(total>limit-1)return range();
    return standard_result(Value(static_cast<I>(total)),true,"int");
}
}

// std.time additions (since 15): one-call operations dispatched before the
// generic argument checks, so they validate argument kinds themselves.
Value invoke_time_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    if(args.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    const auto& name=f.name;
    if(name=="since_ns")return Value(monotonic_now_ns(s)-integer(args[0],s));
    if(name=="format_duration")return Value(format_duration(integer(args[0],s)));
    if(name=="parse_duration")return parse_duration(text(args[0],s));
    if(name=="is_leap_year")return Value(leap(integer(args[0],s)));
    if(name=="days_in_month"){auto m=integer(args[1],s);if(m<1||m>12)runtime_error(s,"month must be 1..12","E4003");return Value(month_days(integer(args[0],s),m));}
    if(name=="utc_from_parts"){
        I v[7];for(int i=0;i<7;++i)v[i]=integer(args[static_cast<std::size_t>(i)],s);
        if(v[0]<1||v[0]>9999||v[1]<1||v[1]>12||v[2]<1||v[2]>month_days(v[0],v[1])||v[3]<0||v[3]>23||v[4]<0||v[4]>59||v[5]<0||v[5]>59||v[6]<0||v[6]>999)
            return err("TIME_RANGE: date-time field out of range","int");
        return standard_result(Value((days_from_civil(v[0],v[1],v[2])*24+v[3])*3600000+v[4]*60000+v[5]*1000+v[6]),true,"int");
    }
    if(name=="parse_rfc3339")return parse_rfc3339(text(args[0],s));
    auto ms=integer(args[0],s);
    if(name=="format_rfc3339_offset"){
        auto offset=integer(args[1],s);if(offset<-1439||offset>1439)runtime_error(s,"offset must be -1439..1439 minutes","E4003");
        auto local=ms+offset*60000;in_range(local,s);
        char zone[8];auto a=offset<0?-offset:offset;std::snprintf(zone,sizeof zone,"%c%02lld:%02lld",offset<0?'-':'+',(long long)(a/60),(long long)(a%60));
        return Value(rfc3339(local,zone));
    }
    in_range(ms,s);
    if(name=="format_rfc3339")return Value(rfc3339(ms,"Z"));
    if(name=="utc_parts"){auto p=parts(ms);return make_multi({Value(p.year),Value(p.month),Value(p.day),Value(p.hour),Value(p.minute),Value(p.second),Value(p.millis)});}
    auto days=floor_div(ms,day_ms);
    if(name=="weekday")return Value(((days%7+7)%7+3)%7+1);
    if(name=="day_of_year")return Value(days-days_from_civil(civil_from_days(days).year,1,1)+1);
    runtime_error(s,"unknown time operation","E4003");
}
}
