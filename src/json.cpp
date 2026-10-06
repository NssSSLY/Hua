#include "hua/json.hpp"
#include "hua/stdlib.hpp"
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace hua {
namespace {
using I=std::int64_t;using P=JsonData::Pointer;using A=JsonData::Array;using O=JsonData::Object;
constexpr std::size_t max_text=16*1024*1024,max_nodes=1000000,max_depth=64;
struct Failure:std::runtime_error {using std::runtime_error::runtime_error;};
template<class T>P node(T value){return managed<const JsonData>(std::move(value));}
void append_utf8(std::string& s,unsigned c){if(c<128)s+=static_cast<char>(c);else if(c<2048){s+=static_cast<char>(192|(c>>6));s+=static_cast<char>(128|(c&63));}else if(c<65536){s+=static_cast<char>(224|(c>>12));s+=static_cast<char>(128|((c>>6)&63));s+=static_cast<char>(128|(c&63));}else{s+=static_cast<char>(240|(c>>18));s+=static_cast<char>(128|((c>>12)&63));s+=static_cast<char>(128|((c>>6)&63));s+=static_cast<char>(128|(c&63));}}
class Parser {
    const std::string& text;std::size_t at{},nodes{};
    [[noreturn]]void fail(const std::string& reason)const{throw Failure("JSON byte "+std::to_string(at+1)+": "+reason);}
    void space(){while(at<text.size()&&(text[at]==' '||text[at]=='\t'||text[at]=='\r'||text[at]=='\n'))++at;}
    bool take(char c){if(at<text.size()&&text[at]==c){++at;return true;}return false;}
    unsigned hex(){if(text.size()-at<4)fail("incomplete Unicode escape");unsigned out=0;for(unsigned i=0;i<4;++i){unsigned char c=text[at++];unsigned v;if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;else if(c>='A'&&c<='F')v=c-'A'+10;else fail("invalid Unicode escape");out=out*16+v;}return out;}
    std::string string(){if(!take('"'))fail("expected string");std::string out;
        while(at<text.size()){unsigned char c=text[at++];if(c=='"')return out;if(c<32)fail("unescaped control character");if(c!='\\'){out+=static_cast<char>(c);continue;}if(at==text.size())fail("incomplete escape");char e=text[at++];
            switch(e){case '"':case '\\':case '/':out+=e;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{unsigned u=hex();if(u>=0xd800&&u<=0xdbff){if(!take('\\')||!take('u'))fail("missing low surrogate");unsigned low=hex();if(low<0xdc00||low>0xdfff)fail("invalid low surrogate");u=0x10000+((u-0xd800)<<10)+(low-0xdc00);}else if(u>=0xdc00&&u<=0xdfff)fail("unpaired low surrogate");append_utf8(out,u);break;}
            default:fail("invalid escape");}}
        fail("unterminated string");
    }
    P number(){auto start=at;take('-');if(at==text.size())fail("incomplete number");if(take('0')){if(at<text.size()&&text[at]>='0'&&text[at]<='9')fail("leading zero");}else{if(text[at]<'1'||text[at]>'9')fail("expected number");while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;}
        bool floating=false;if(take('.')){floating=true;auto first=at;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;if(at==first)fail("expected fraction digits");}
        if(take('e')||take('E')){floating=true;if(!take('+'))take('-');auto first=at;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;if(at==first)fail("expected exponent digits");}
        if(floating){double v;auto [end,ec]=std::from_chars(text.data()+start,text.data()+at,v);if(ec!=std::errc{}||end!=text.data()+at||!std::isfinite(v))fail("number outside finite float64 range");return node(v);}
        I v;auto [end,ec]=std::from_chars(text.data()+start,text.data()+at,v);if(ec!=std::errc{}||end!=text.data()+at)fail("integer outside int64 range");return node(v);
    }
    P value(std::size_t depth){if(depth>max_depth)fail("nesting exceeds 64");if(++nodes>max_nodes)fail("node limit exceeded");space();if(at==text.size())fail("expected value");char c=text[at];
        if(c=='"')return node(string());
        if(take('[')){A values;space();if(take(']'))return node(values);while(true){values.push_back(value(depth+1));space();if(take(']'))return node(std::move(values));if(!take(','))fail("expected comma or ]");}}
        if(take('{')){O values;space();if(take('}'))return node(values);while(true){space();auto key=string();space();if(!take(':'))fail("expected colon");values.insert_or_assign(std::move(key),value(depth+1));space();if(take('}'))return node(std::move(values));if(!take(','))fail("expected comma or }");}}
        for(const auto& literal:{std::string("true"),std::string("false"),std::string("null")})if(text.compare(at,literal.size(),literal)==0){at+=literal.size();if(literal=="null")return node(std::monostate{});return node(literal=="true");}
        if(c=='-'||(c>='0'&&c<='9'))return number();fail("unexpected token");
    }
public:
    explicit Parser(const std::string& s):text(s){}
    P parse(){if(text.size()>max_text)fail("text exceeds 16 MiB");auto invalid=text_utf8_error(text);if(!invalid.empty())fail(invalid);auto out=value(0);space();if(at!=text.size())fail("trailing content");return out;}
};
struct Writer {
    std::string out;std::size_t nodes{};
    void add(std::string_view s){if(s.size()>max_text-out.size())throw Failure("JSON text exceeds 16 MiB");out+=s;}
    void quote(const std::string& s){auto invalid=text_utf8_error(s);if(!invalid.empty())throw Failure(invalid);add("\"");constexpr char hex[]="0123456789abcdef";for(unsigned char c:s){if(c=='"')add("\\\"");else if(c=='\\')add("\\\\");else if(c<32){char escaped[]={'\\','u','0','0',hex[c>>4],hex[c&15]};add(std::string_view(escaped,6));}else {char ch=static_cast<char>(c);add(std::string_view(&ch,1));}}add("\"");}
    void value(const P& p,std::size_t depth=0){if(!p||depth>max_depth||++nodes>max_nodes)throw Failure("JSON nesting/node limit exceeded");const auto& d=p->data;
        if(std::holds_alternative<std::monostate>(d))add("null");else if(auto b=std::get_if<bool>(&d))add(*b?"true":"false");else if(auto i=std::get_if<I>(&d))add(std::to_string(*i));
        else if(auto f=std::get_if<double>(&d)){if(!std::isfinite(*f))throw Failure("non-finite JSON number");char buffer[64];auto [end,ec]=std::to_chars(buffer,buffer+64,*f,std::chars_format::general,std::numeric_limits<double>::max_digits10);if(ec!=std::errc{})throw Failure("JSON number conversion failed");std::string n(buffer,end);if(n.find_first_of(".eE")==std::string::npos)n+=".0";add(n);}
        else if(auto s=std::get_if<std::string>(&d))quote(*s);
        else if(auto a=std::get_if<A>(&d)){add("[");for(std::size_t i=0;i<a->size();++i){if(i)add(",");value((*a)[i],depth+1);}add("]");}
        else {add("{");bool first=true;for(const auto& [k,v]:std::get<O>(d)){if(!first)add(",");first=false;quote(k);add(":");value(v,depth+1);}add("}");}
    }
};
P convert(const Value& v,std::size_t depth,std::size_t& nodes){if(depth>max_depth||++nodes>max_nodes)throw Failure("JSON nesting/node limit exceeded");
    if(auto numeric=std::get_if<NumericValue>(&v.data)){try{auto number=convert_numeric(v,numeric_spec(numeric->type)->category=='f'?"float":"int",{},true);if(auto x=std::get_if<I>(&number.data))return node(*x);return node(std::get<double>(number.data));}catch(const Diagnostic&){throw Failure("JSON numeric value is outside int64/finite float64 range");}}
    if(auto p=std::get_if<JsonValue>(&v.data))return p->data;
    if(v.data.index()==0)return node(std::monostate{});if(auto p=std::get_if<bool>(&v.data))return node(*p);if(auto p=std::get_if<I>(&v.data))return node(*p);if(auto p=std::get_if<double>(&v.data))return node(*p);if(auto p=std::get_if<std::string>(&v.data))return node(*p);
    if(auto p=std::get_if<SliceValue>(&v.data)){A out;for(std::size_t i=0;i<p->length;++i)out.push_back(convert(sequence_read(*p,i),depth+1,nodes));return node(std::move(out));}
    if(auto p=std::get_if<MapValue>(&v.data)){if(p->data->key_type!="string")throw Failure("JSON objects require string Map keys");O out;for(const auto& [k,x]:p->data->entries)out.emplace(std::get<std::string>(k),convert(x,depth+1,nodes));return node(std::move(out));}
    if(auto p=std::get_if<StructValue>(&v.data)){if(p->data->fields.contains("$case"))throw Failure("enum JSON encoding requires an explicit representation");O out;for(const auto& [k,x]:p->data->fields)out.emplace(k,convert(x,depth+1,nodes));return node(std::move(out));}
    throw Failure("unsupported JSON value type: "+display_type(value_type(v)));
}
Value list(const std::vector<std::string>& xs){auto values=managed<std::vector<Value>>();for(const auto& x:xs)values->emplace_back(x);return Value(SliceValue{values,0,values->size(),false,"string"});}
}
std::string json_display(const JsonValue& v){Writer writer;writer.value(v.data);return writer.out;}
Value json_decode(const std::string& text){try{return standard_result(Value(JsonValue{Parser(text).parse()}),true,"Json");}catch(const Failure& e){return standard_result(Value(std::string(e.what())),false,"Json");}}
Value json_encode(const Value& value){try{std::size_t nodes=0;Writer writer;writer.value(convert(value,0,nodes));return standard_result(Value(std::move(writer.out)),true,"string");}catch(const Failure& e){return standard_result(Value(std::string(e.what())),false,"string");}}
Value json_access(const std::string& op,const std::vector<Value>& args){const auto& data=std::get<JsonValue>(args[0].data).data->data;
    std::string type=op=="get"||op=="at"?"Json":op=="as_string"?"string":op=="as_bool"?"bool":op=="as_float"?"float":op=="keys"?"[]string":"int";
    auto ok=[&](Value v){return standard_result(std::move(v),true,type);};auto err=[&](std::string e){return standard_result(Value(std::move(e)),false,type);};
    if(op=="kind"){static const char* kinds[]={"null","bool","number","number","string","array","object"};return Value(std::string(kinds[data.index()]));}
    if(op=="is_null")return Value(std::holds_alternative<std::monostate>(data));
    if(op=="get"){auto object=std::get_if<O>(&data);if(!object)return err("JSON value is not an object");auto found=object->find(std::get<std::string>(args[1].data));if(found==object->end())return err("JSON object key is missing");return ok(Value(JsonValue{found->second}));}
    if(op=="at"){auto array=std::get_if<A>(&data);if(!array)return err("JSON value is not an array");auto at=std::get<I>(args[1].data);if(at<0||static_cast<std::size_t>(at)>=array->size())return err("JSON array index out of bounds");return ok(Value(JsonValue{(*array)[static_cast<std::size_t>(at)]}));}
    if(op=="len"){if(auto p=std::get_if<A>(&data))return ok(Value(static_cast<I>(p->size())));if(auto p=std::get_if<O>(&data))return ok(Value(static_cast<I>(p->size())));return err("JSON len requires an array or object");}
    if(op=="keys"){auto p=std::get_if<O>(&data);if(!p)return err("JSON keys requires an object");std::vector<std::string> out;for(const auto& [k,_]:*p)out.push_back(k);return ok(list(out));}
    if(op=="as_string"){if(auto p=std::get_if<std::string>(&data))return ok(Value(*p));}
    if(op=="as_bool"){if(auto p=std::get_if<bool>(&data))return ok(Value(*p));}
    if(op=="as_int"){if(auto p=std::get_if<I>(&data))return ok(Value(*p));}
    if(op=="as_float"){if(auto p=std::get_if<double>(&data))return ok(Value(*p));if(auto p=std::get_if<I>(&data)){if(*p>9007199254740992LL||*p< -9007199254740992LL)return err("JSON integer exceeds exact float conversion range");return ok(Value(static_cast<double>(*p)));}}
    return err("JSON value has the wrong type");
}
}
