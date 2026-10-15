#include"json_parse.h"


//定义实现
//ent中有一个is_failed用来判断是否合法
//规定:GET和HEAD都只需要id=?所以都从query中提取
//DELETE也是只需要id=？
//POST从body中提取三个,id,name,age,因为Post是提交
//PUT需要提取四个，id1,name,age,id2,其中id2是从query中提取，前面三个是从body中提取


bool is_true_number(std::string& value){
    if (value.empty()) {
        return false;
    }
    for(const auto& i:value){
        if(i<'0'||i>'9'){
            return false;
        }
    }
    return true;
}

void parse_get(HTTP_REQUEST& request,enternode& ent){
    std::string name;
    std::string value;
    std::string& query=request.query;
    auto it=query.find('=');
    if(it==std::string::npos){
        ent.is_failed=true;
        return;
    }
    name=query.substr(0,it);
    if(name!="id"){
        ent.is_failed=true;
        return;
    }
    auto second=query.find('=',it+1);
    if(second!=std::string::npos){
        ent.is_failed=true;
        return;
    }
    value=query.substr(it+1);
    if(!is_true_number(value)){
        ent.is_failed=true;
        return;
    }
    ent.first.push_back(value);
    ent.is_failed=false;
    return;
}
void parse_head(HTTP_REQUEST& request,enternode& ent){
    std::string name;
    std::string value;
    std::string& query=request.query;
    auto it=query.find('=');
    if(it==std::string::npos){
        ent.is_failed=true;
        return;
    }
    name=query.substr(0,it);
    if(name!="id"){
        ent.is_failed=true;
        return;
    }
    auto second=query.find('=',it+1);
    if(second!=std::string::npos){
        ent.is_failed=true;
        return;
    }
    value=query.substr(it+1);
    if(!is_true_number(value)){
        ent.is_failed=true;
        return;
    }
    ent.first.push_back(value);
    ent.is_failed=false;
    return;
}
//更新,id,name,age
void parse_put(HTTP_REQUEST& request,enternode& ent){
    std::string& body=request.body;
    //提取最后一个空
    std::string& query=request.query;//
    //第三个参数就是解析失败标记，第二个参数是表示不使用解析回调
    json data=json::parse(body,nullptr,false);
    //是否为解析失败标记,(true表示解析失败)
    if(data.is_discarded()){
        ent.is_failed=true;
        return;
    }
    //判断有无id,name,age字段
    if(!data.contains("age")||!data.contains("name")||!data.contains("id")){
        ent.is_failed=true;
        return;
    }
    if(!data.at("id").is_number_integer()||
    !data.at("age").is_number_integer()||
    !data.at("name").is_string()){
        ent.is_failed=true;
        return;
    }
    long long id=data.at("id").get<long long>();
    std::string name=data.at("name").get<std::string>();
    long long age=data.at("age").get<long long >();
    auto it=query.find('=');
    if(it==std::string::npos){
        ent.is_failed=true;
        return;
    }
    std::string na=query.substr(0,it);
    if(na!="id"){
        ent.is_failed=true;
        return ;
    }
    auto second=query.find('=',it+1);
    if(second!=std::string::npos){
        ent.is_failed=true;
        return;
    }
    std::string value=query.substr(it+1);
    if(!is_true_number(value)){
        ent.is_failed=true;
        return;
    }
    //顺序填入四个参数
    ent.first.push_back(std::to_string(id));
    ent.first.push_back(name);
    ent.first.push_back(std::to_string(age));
    ent.first.push_back(value);
    ent.is_failed=false;
    return ;
}
void parse_post(HTTP_REQUEST& request,enternode& ent){
    std::string& body=request.body;
    //提取最后一个空
    std::string& query=request.query;//
    //第三个参数就是解析失败标记，第二个参数是表示不使用解析回调
    json data=json::parse(body,nullptr,false);
    //是否为解析失败标记(true表示解析失败)
    if(data.is_discarded()){
        ent.is_failed=true;
        return;
    }
    //判断有无id,name,age字段
    if(!data.contains("age")||!data.contains("name")||!data.contains("id")){
        ent.is_failed=true;
        return;
    }
    if(!data.at("id").is_number_integer()||
    !data.at("age").is_number_integer()||
    !data.at("name").is_string()){
        ent.is_failed=true;
        return;
    }
    long long id=data.at("id").get<long long>();
    std::string name=data.at("name").get<std::string>();
    long long age=data.at("age").get<long long >();
    ent.first.push_back(std::to_string(id));
    ent.first.push_back(name);
    ent.first.push_back(std::to_string(age));
    ent.is_failed = false;
    return;
}
void parse_delete(HTTP_REQUEST& request,enternode& ent){
    std::string name;
    std::string value;
    std::string& query=request.query;
    auto it=query.find('=');
    if(it==std::string::npos){
        ent.is_failed=true;
        return;
    }
    name=query.substr(0,it);
    if(name!="id"){
        ent.is_failed=true;
        return;
    }
    auto second=query.find('=',it+1);
    if(second!=std::string::npos){
        ent.is_failed=true;
        return;
    }
    value=query.substr(it+1);
    if(!is_true_number(value)){
        ent.is_failed=true;
        return;
    }
    ent.first.push_back(value);
    ent.is_failed = false;
    return;
}
//外部接口直接调用即可
enternode json_parse(HTTP_REQUEST request){
    enternode ent{};
    switch(request.method){
        case METHOD::GET:
            parse_get(request,ent);
            break;
        case METHOD::HEAD:
            parse_head(request,ent);
            break;
        case METHOD::PUT:
            parse_put(request,ent);
            break;
        case METHOD::POST:
            parse_post(request,ent);
            break;
        case METHOD::DELETE:
            parse_delete(request,ent);
            break;
    }
    return ent;
}