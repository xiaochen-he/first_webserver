#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
//枚举method（后续构造响应体或者实际连接服务器的时候可以用到）
enum class METHOD{
    GET,
    POST,
    PUT,
    HEAD,
    DELETE,
    UNKOWN
};


enum class RequestState{
    REQUEST_LINE,//请求行
    REQUEST_HEADER,//请求头
    BODY,//数据
    COMPLETE//完成
};//这个表示这个请求解析到达了哪一步

enum class LineStatus{
    LINE_OK,//当前行解析完成
    LINE_OPEN,//当前需要更多数据
    LINE_BAD//当前出现问题
};//这个表示当前请求解析到哪一行，然后这一行的状态是什么

enum class ParseResult{
    NEED_MORE_DATA,
    COMPLETE,
    ERROR
};//这个表示我当前解析返回的结果（也就是外面输入输出函数看到的结果，根据三个结果来继续输入或者调取构造响应函数）



//记录每个请求对应信息的结构体
struct HTTP_REQUEST{
        METHOD method=METHOD::UNKOWN;
        std::string path;
        std::string version;
        std::unordered_map<std::string,std::string> headers;
        bool is_close=false;
        std::string body;
        std::string query;
};



//解析器对象类
class Httpparser{
public:
    ParseResult parse_request(const std::string& recvdata);
    const HTTP_REQUEST& request() const noexcept {
        return request_;
    }
    const std::pair<std::string,std::size_t> read_check() const noexcept{
        return {read_buffer,check_index};
    }
    void reset();
    bool is_close(){
        return request_.is_close;
    }
private:
    RequestState state=RequestState::REQUEST_LINE;
    std::string read_buffer;
    std::size_t check_index=0;
    std::size_t content_length=0;
    HTTP_REQUEST request_;//结构体存储每次解析的数据
};
