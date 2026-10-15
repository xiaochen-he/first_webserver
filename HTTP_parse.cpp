#include"HTTP.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#include <unordered_map>
#include <vector>
#include<unordered_set>


namespace{
    const std::unordered_map<std::string,METHOD>
method_map={
    {"GET",METHOD::GET},
    {"POST",METHOD::POST},
    {"PUT",METHOD::PUT},
    {"HEAD",METHOD::HEAD},
    {"DELETE",METHOD::DELETE}
};
//根据解析结果来判断赋予对应状态

//合法version
std::vector<std::string> VERSION{
    "HTTP/1.0","HTTP/1.1"
};

//字母、数字、! # $ % & ' * + - . ^ _ ` | ~(header_name只能包含这些)

}



//当请求解析完成，响应构建完成后，就可以还原存储的信息，因为一个解析器对象不一定只解析一个请求。
void Httpparser::reset() {
    state = RequestState::REQUEST_LINE;
    read_buffer.clear();
    check_index = 0;
    content_length = 0;
    request_ = HTTP_REQUEST{};
}

//一个转小写的封装函数,只是在关于key(header)的时候转小写
std::string to_lower(std::string s){
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

//检查是否是请求中有关闭连接的信号
void header_value_is_close(std::string& raw,HTTP_REQUEST& request_){
    int start = 0;
    int end = static_cast<int>(raw.size()) - 1;

    while (start <= end &&
        (raw[start] == ' ' || raw[start] == '\t')) {
        ++start;
    }

    while (start <= end &&  
        (raw[end] == ' ' || raw[end] == '\t')) {
        --end;
    }

    std::string value = raw.substr(start, end - start + 1);
    if (to_lower(value) == "close") {
        request_.is_close = true;
    }
    return;
}

/*
以下是五个封装的读取函数，返回值就是对应的值，比如method，path，header等等
*/
std::string read_method(const std::string& data,std::size_t& check_index){
    std::string method1;
    std::size_t left=check_index;
    std::size_t right=0;
    for(std::size_t i=left;i<data.size();i++){
        if(data[i]==' '){
            right=i-1;
            check_index=i+1;
            break;
        }
    }
    if(right==0){
        return "";
    }
    method1=data.substr(left,right-left+1);
    return method1;
}

//提取其中的request_target（包括path和query）
std::string read_request_target(const std::string& data,std::size_t& check_index){
    std::string path;
    std::size_t left=check_index;
    std::size_t right=0;
    for(std::size_t i=left;i<data.size();i++){
        if(data[i]==' '){
            right=i-1;
            check_index=i+1;
            break;
        }
    }
    if(right==0){
        return "";
    }
    path=data.substr(left,right-left+1);
    return path;
}
//校验规则,request_target的校验规则

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool is_correct_request_target(const std::string& request_target){
    if(request_target.empty()){
        return false;
    }
    if(request_target[0]!='/'){
        return false;
    }
    for(int i=0;i<static_cast<int>(request_target.size());i++){
        char c=request_target[i];
        if(c=='#'||c <= 0x20 || c == 0x7F || c >= 0x80 ){
            return false;
        }
        if(c=='%'){
            if(i+2>=static_cast<int>(request_target.size())){
                return false;
            }
            if(!is_hex_digit(request_target[i+1])||!is_hex_digit(request_target[i+2])){
                return false;
            }
            i+=2;
        }
    }
    return true;
}


//提起请求行中的版本
std::string read_version(const std::string& data,std::size_t& check_index){
    std::string version;
    std::size_t left=check_index;
    std::size_t right=0;
    for(std::size_t i=left;i<data.size();i++){
        if(data[i]=='\n'&&data[i-1]=='\r'){
            right=i-2;
            check_index=i+1;
            break;
        }
    }
    if(right==0){
        return "";
    }
    version=data.substr(left,right-left+1);
    return version;
}


bool is_correct(std::string& version){
    for(const auto& i:VERSION){
        if(i==version){
            return true;
        }
    }
    return false;
}
//提取其中每一对header
std::vector<std::string> read_headers(const std::string& data,std::size_t& check_index){
     const std::size_t line_end =
        data.find("\r\n", check_index);//find返回所处位置，也就是最后的\r\n所在的前一个下标
    if (line_end == std::string::npos) {
        return {};
    }
    const std::size_t colon =
        data.find(':', check_index);
    if (colon == std::string::npos || colon >= line_end) {
        return {};
    }
    std::string name =
        data.substr(check_index, colon - check_index);
    std::size_t value_begin = colon + 1;
    while (value_begin < line_end &&
           (data[value_begin] == ' ' ||
            data[value_begin] == '\t')) {
        ++value_begin;
    }
    std::string value =
        data.substr(value_begin, line_end - value_begin);
    check_index = line_end + 2;
    return {name, value};
}
//判断是否header已经结束
bool header_end(const std::string& data,std::size_t& check_index){
    if(check_index+1>=data.size()){
        return false;
    }
    std::size_t i=check_index;
    if(data[i]=='\r'&&data[i+1]=='\n'){
        check_index=i+2;
        return true;
    }
    return false;
}

//字母、数字、! # $ % & ' * + - . ^ _ ` | ~(header_name的校验)

bool is_correct_char(char c){
    return c=='!'||c=='#'||c=='$'
            ||c=='&'||c=='\''||c=='*'
            ||c=='+'||c=='-'||c=='.'||
            c=='^'||c=='_'||c=='`'
            ||c=='|'||c=='~'||std::isalnum(c);
}


bool is_correct_header_name(const std::string& header_name)
{
    if(header_name.empty()){
        return false;
    }
    for(const auto& i:header_name){
        if(!is_correct_char(i)){
            return false;
        }
    }
    return true;
}

/*空值允许，例如X-Test:
普通可见字符允许
空格和Tab允许
其他控制字符禁止
非ASCII字符暂时禁止*/
bool is_correct_header_value(const std::string& header_value){
    for(char i:header_value){
        const unsigned char ch=static_cast<unsigned char>(i);
        const bool is_tab = ch == '\t';
        const bool is_space = ch == ' ';
        const bool is_visible_ascii =
            ch >= 0x21 && ch <= 0x7E;
            if(!is_tab&&!is_space&&!is_visible_ascii){
                return false;
            }
    }
    return true;
}

//校验如果是content_length的value
bool is_correct_contentlength(const std::string& contentlength,std::size_t& result){
    constexpr std::size_t MAX_BODY_SIZE =
        1024 * 1024; 
    if(contentlength.empty()){
        return false;
    }
    for(auto c:contentlength){
        if(c<'0'||c>'9'){
            return false;
        }
    }
    std::size_t length=0;
    const char* begin=contentlength.data();
    const char* end=begin+contentlength.size();
    auto[ptr,ec]=std::from_chars(begin,end,length);
    if(ptr!=end||ec!=std::errc{}){
        return false;
    }
    if (length > MAX_BODY_SIZE) {
        return false;
    }
    result=length;
    return true;
}


//读取数据，根据content_length的长度看是否读完
std::string read_body(const std::string& data,std::size_t& check_index,std::size_t& content_length){
    std::string body;
    std::size_t left=check_index;
    std::size_t right=left+content_length-1;
    if(right>=data.size()){
        return "";
    }else{
        body=data.substr(left,right-left+1);
        check_index=right+1;
        return body;
    }
}

LineStatus parse_line(std::string& data,RequestState& state,HTTP_REQUEST& request_,std::size_t& check_index,std::size_t& content_length){
  
        switch(state){
            case RequestState::REQUEST_LINE:
                {
                    if(request_.method==METHOD::UNKOWN){
                        //读取传入数据中的data
                        std::string method=read_method(data,check_index);
                        //如果没有完整读取就会返回空，所以这个用空返回说明还需要数据
                        if(method==""){
                            return LineStatus::LINE_OPEN;
                        }
                        auto it=method_map.find(method);
                        //这里说明不是合法的方法，所以就返回bad，然后转换成error，之后输出或者什么处理都是后面的事情
                        if(it==method_map.end()){
                            return LineStatus::LINE_BAD;
                        }else{
                            request_.method=it->second;
                        }
                    }
                    //如果还没有读取path就读取
                    if(request_.path.empty()){
                        std::string request_target=read_request_target(data,check_index);
                        //同样的格式
                        if(request_target==""){
                            return LineStatus::LINE_OPEN;
                        }
                        //校验是不是正常格式的requst_target
                        if(!is_correct_request_target(request_target)){
                            return LineStatus::LINE_BAD;
                        }
                        //合法性验证以及对于这个信息的分解
                        std::size_t right=request_target.find('?');
                        if(right==std::string::npos){
                            request_.path=request_target;
                            request_.query.clear();
                        }else{
                            request_.path=request_target.substr(0,right);
                            int n=static_cast<int>(request_target.size());
                            request_.query=request_target.substr(right+1,n-right-1);
                        }
                   }   
                    //与前面相同
                    if(request_.version.empty()){
                        std::string version=read_version(data,check_index);
                        if(version==""){
                            return LineStatus::LINE_OPEN;
                        }
                        if(!is_correct(version)){
                            return LineStatus::LINE_BAD;
                        }
                        request_.version=version;
                    }
                    return LineStatus::LINE_OK;
                }
            //在外层中状态转换，因为肯定会返回所以不用break
            case RequestState::REQUEST_HEADER:
                {
                    //这里使用while是因为只要一直没有到空行就一直读取(目前不在意是否会遇到数据丢失)
                    while(!header_end(data,check_index)){
                        std::vector<std::string> header=read_headers(data,check_index);
                        if(header.empty()){
                            return LineStatus::LINE_OPEN;
                        }
                        //每次加入都检验
                        if(!is_correct_header_name(header[0])||!is_correct_header_value(header[1])){
                            return LineStatus::LINE_BAD;
                        }
                        std::string name=to_lower(header[0]);
                        if(name=="connection"){
                            std::string raw=header[1];
                            while(raw.find(',')!=std::string::npos){
                                auto it=raw.find(',');
                                std::string left=raw.substr(0,it);
                                raw=raw.substr(it+1);
                                header_value_is_close(left,request_);
                            }
                            header_value_is_close(raw,request_);
                        }
                        if(name=="content-length"){
                            if(!is_correct_contentlength(header[1],content_length)){
                                return LineStatus::LINE_BAD;
                            }
                        }
                        if(name=="host"){
                            if(header[1].empty()){
                                return LineStatus::LINE_BAD;
                            }
                        }
                        if(name=="transfer-encoding"){
                            return LineStatus::LINE_BAD;
                        }
                        /*  true     原来没有这个键，插入成功
                            false    已经存在相同键，发生重复*/
                        auto [i,issert]=request_.headers.emplace(name,header[1]);//成功就成功插入，失败就返回重复
                        if(!issert){
                            return LineStatus::LINE_BAD;
                        }
                    }
                    if (request_.version == "HTTP/1.1" &&
                            request_.headers.find("host") ==
                            request_.headers.end()) {
                        return LineStatus::LINE_BAD;
                    }
                    return LineStatus::LINE_OK;
                }
            //这个阶段不一定能到
            case RequestState::BODY:
                {
                    //这个阶段很简单，只需要一直读取就可以，如果大于content_length就读，然后没有就返回需要继续读取
                    std::string body=read_body(data,check_index,content_length);
                    if(body==""){
                        return LineStatus::LINE_OPEN;
                    }else{
                        request_.body=body;
                        return LineStatus::LINE_OK;
                    }

                }
            case RequestState::COMPLETE:
                return LineStatus::LINE_OK;
        }
        return LineStatus::LINE_BAD;
    } 

ParseResult Httpparser::parse_request(const std::string& recvdata)
{
    read_buffer+=recvdata;
    while(true){
        switch(state){
            case RequestState::REQUEST_LINE:
                switch(parse_line(read_buffer,state,request_,check_index,content_length))
                {
                    case LineStatus::LINE_OK:
                        state=RequestState::REQUEST_HEADER;
                        break;
                    case LineStatus::LINE_BAD:
                        return ParseResult::ERROR;
                    case LineStatus::LINE_OPEN:
                        return ParseResult::NEED_MORE_DATA;
                }
                break;
            case RequestState::REQUEST_HEADER:
                switch(parse_line(read_buffer,state,request_,check_index,content_length))
                {
                    case LineStatus::LINE_OK:
                        if(content_length!=0){
                            state=RequestState::BODY;
                            break;
                        }else{
                            state=RequestState::COMPLETE;
                            break;
                        }
                    case LineStatus::LINE_BAD:
                        return ParseResult::ERROR;
                    case LineStatus::LINE_OPEN:
                        return ParseResult::NEED_MORE_DATA;
                }
                break;
            case RequestState::BODY:
                switch(parse_line(read_buffer,state,request_,check_index,content_length))
                {
                    case LineStatus::LINE_OK:
                        state=RequestState::COMPLETE;
                        return ParseResult::COMPLETE;
                    case LineStatus::LINE_BAD:
                        return ParseResult::ERROR;
                    case LineStatus::LINE_OPEN:
                        return ParseResult::NEED_MORE_DATA;
                    break;
                }
                break;
            case RequestState::COMPLETE:
                return ParseResult::COMPLETE;
        }   
        
    }
}