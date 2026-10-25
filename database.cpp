#include"database.h"
//这个就是为了对应的方法
#include"HTTP.h"

namespace{
using sh=std::unique_ptr<MYSQL,decltype(&mysql_close)>;
//这个是连接池存储的东西
//这些连接池也是共享的，所有线程都可以访问
std::vector<sh> connect_pool;//连接池存储对于数据库的连接
std::queue<MYSQL*> free_pool;//空闲的连接
std::mutex connect_mutex;
std::condition_variable cv_pool;

}


int initialize_connect_pool(int n){
    const char* user=std::getenv("MYSQL_USER");
    const char* password=std::getenv("MYSQL_PASSWORD");
    if(!user||!password){
        std::cerr<<"MYSQL_USER MISSING OR MYSQL_PASSWORD MISSING";
        return 1;
    }
    for(int i=0;i<n;i++){
        sh db(mysql_init(nullptr),mysql_close);
        if(!db){
            std::cerr<<"连接初始化出现错误"<<"\n";
            return 1;
        }
        if(mysql_real_connect(db.get(),"127.0.0.1",user,password,"webserver",3306,nullptr,0)==nullptr){
            std::cerr<<"建立连接失败"<<mysql_error(db.get())<<"\n";
            return 1;
        }
        connect_pool.emplace_back(std::move(db));
        MYSQL* con=connect_pool[i].get();
        free_pool.push(con);
    }
    return 0;
}

//关闭连接池，释放连接池，不需要锁，因为是在所有工作线程完成之后释放，所以就可以直接释放不用得到锁
void close_connect_pool(){
    while(!free_pool.empty()){
        free_pool.pop();
    }  
    while(!connect_pool.empty()){
        connect_pool.pop_back();
    }
    return;
} 

//借用到连接
MYSQL* get_connect(){
    std::unique_lock<std::mutex> lock(connect_mutex);
    //条件变量，如果为空就等待为空，并且等待期间释放锁_pool，如果不为空就直接取
    cv_pool.wait(lock,[]
        {return  !free_pool.empty();});
    MYSQL* result=free_pool.front();
    free_pool.pop();
    return result;
}

//释放连接，并通知
void free_connect(MYSQL* now){
    {   
        std::lock_guard<std::mutex> lock(connect_mutex);
        free_pool.push(now);
    }
    cv_pool.notify_one();//释放并提醒
    return;
}


void record_error(bool is_stmt,MYSQL* now,response& res,MYSQL_STMT* stmt){
    res.is_failed=true;
    res.failed_data+="错误编号=";
    if(is_stmt){
        res.failed_data+=std::to_string(mysql_stmt_errno(stmt));
    }else{
        res.failed_data+=std::to_string(mysql_errno(now));
    }
    res.failed_data+="\t";
    res.failed_data+="错误记录:";
    if(is_stmt){
        res.failed_data+=mysql_stmt_error(stmt);
    }else{
        res.failed_data+=mysql_error(now);
    }
    if(stmt!=nullptr){
        mysql_stmt_close(stmt);
    }
    return ;
}

//GET SELETE 列 FROM 表名 WHERE id=?
//只需要填写id
response execute_get(MYSQL* now,enternode& node){
    response res{};
    //转换为longlong
    long long input=std::stoll(node.first[0]);
    //创建语句对象（在借来的连接上）
    MYSQL_STMT* stmt=mysql_stmt_init(now);
    if(!stmt){
        std::cerr<<mysql_error(now)<<"\n";
        res.failed_data+="创建语句对象失败";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    //返回三列
    const char sql[]="SELECT id,name,age FROM student WHERE id=?";
    if(mysql_stmt_prepare(stmt,sql,sizeof(sql)-1)!=0){
        res.failed_data+="模块化语句准备失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    MYSQL_BIND params[1]{};
    params[0].buffer_type=MYSQL_TYPE_LONGLONG;
    params[0].buffer=&input;
    params[0].is_unsigned=false;
    if(mysql_stmt_bind_param(stmt,params)!=0){
        res.failed_data+="参数绑定失败";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_execute(stmt)!=0){
        res.failed_data+="语句执行失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    char buffer[3][256]{};
    unsigned long length[3]{};
    bool is_null[3]{};
    bool truncated[3]{};
    MYSQL_BIND out[3]{};
    for (int i = 0; i < 3; ++i) {
        out[i].buffer_type = MYSQL_TYPE_STRING;
        out[i].buffer = buffer[i];
        out[i].buffer_length = sizeof(buffer[i]);
        out[i].length = &length[i];
        out[i].is_null = &is_null[i];
        out[i].error = &truncated[i];
    }
    if(mysql_stmt_bind_result(stmt,out)!=0){
        res.failed_data+="输出缓冲区绑定失败";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    int status=mysql_stmt_fetch(stmt);
    while(true){
        if(status==MYSQL_NO_DATA){
            break;
        }
        if(status==1){
            res.failed_data+="获取行错误";
            res.failed_data+="\t";
            record_error(true,now,res,stmt);
            return res;
        }
        if(status==MYSQL_DATA_TRUNCATED){
            res.failed_data+="当前行有列发生了截断或转换信息丢失";
            res.failed_data+="\t";
            res.is_failed=true;
            mysql_stmt_close(stmt);
            return res;
        }
        std::string now;
        for(int i=0;i<3;i++){
            if(is_null[i]){
                now+="NULL";
                if(i!=2){
                    now+="\t";
                }
                continue;
            }
            now.append(buffer[i],length[i]);
            if(i!=2){
                now+="\t";
            }
        }
        res.data.push_back(now);
        status=mysql_stmt_fetch(stmt);
    }
    if(mysql_stmt_close(stmt)!=0){
        res.failed_data+="关闭语句是发生错误";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    res.is_failed=false;//没有失败
    return res;
}
//head与get就是一样的
response execute_head(MYSQL* now,enternode& node){
    response res{};
    //转换为longlong
    long long input=std::stoll(node.first[0]);
    //创建语句对象（在借来的连接上）
    MYSQL_STMT* stmt=mysql_stmt_init(now);
    if(!stmt){
        std::cerr<<mysql_error(now)<<"\n";
        res.failed_data+="创建语句对象失败";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    //返回三列
    const char sql[]="SELECT id,name,age FROM student WHERE id=?";
    if(mysql_stmt_prepare(stmt,sql,sizeof(sql)-1)!=0){
        res.failed_data+="模块化语句准备失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    MYSQL_BIND params[1]{};
    params[0].buffer_type=MYSQL_TYPE_LONGLONG;
    params[0].buffer=&input;
    params[0].is_unsigned=false;
    if(mysql_stmt_bind_param(stmt,params)!=0){
        res.failed_data+="参数绑定失败";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_execute(stmt)!=0){
        res.failed_data+="语句执行失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    char buffer[3][256]{};
    unsigned long length[3]{};
    bool is_null[3]{};
    bool truncated[3]{};
    MYSQL_BIND out[3]{};
    for (int i = 0; i < 3; ++i) {
        out[i].buffer_type = MYSQL_TYPE_STRING;
        out[i].buffer = buffer[i];
        out[i].buffer_length = sizeof(buffer[i]);
        out[i].length = &length[i];
        out[i].is_null = &is_null[i];
        out[i].error = &truncated[i];
    }
    if(mysql_stmt_bind_result(stmt,out)!=0){
        res.failed_data+="输出缓冲区绑定失败";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    int status=mysql_stmt_fetch(stmt);
    while(true){
        if(status==MYSQL_NO_DATA){
            break;
        }
        if(status==1){
            res.failed_data+="获取行错误";
            res.failed_data+="\t";
            record_error(true,now,res,stmt);
            return res;
        }
        if(status==MYSQL_DATA_TRUNCATED){
            res.failed_data+="当前行有列发生了截断或转换信息丢失";
            res.failed_data+="\t";
            res.is_failed=true;
            mysql_stmt_close(stmt);
            return res;
        }
        std::string now;
        for(int i=0;i<3;i++){
            if(is_null[i]){
                now+="NULL";
                if(i!=2){
                    now+="\t";
                }
                continue;
            }
            now.append(buffer[i],length[i]);
            if(i!=2){
                now+="\t";
            }
        }
        res.data.push_back(now);
        status=mysql_stmt_fetch(stmt);
    }
    if(mysql_stmt_close(stmt)!=0){
        res.failed_data+="关闭语句是发生错误";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    res.is_failed=false;//没有失败
    return res;
}
//更新
//不需要返回结果集
//UPDATE student SET id=,name=,age= WHERE id=?,
response execute_put(MYSQL* now,enternode& node){
    response res{};
    std::string& name=node.first[1];
    long long id1=std::stoll(node.first[0]);
    long long age=std::stoll(node.first[2]);
    long long id2=std::stoll(node.first[3]);
    const char sql[]="UPDATE student SET id=?,name=?,age=? WHERE id=?";
    MYSQL_STMT* stmt=mysql_stmt_init(now);
    if(!stmt){
        res.failed_data+="语句对象初始化失败";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    if(mysql_stmt_prepare(stmt,sql,sizeof(sql)-1)!=0){
        res.failed_data+="模块化语句准备失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    MYSQL_BIND param[4]{};
    param[0].buffer=&id1;
    param[2].buffer=&age;
    param[3].buffer=&id2;
    for(int i=0;i<4;i++){
        if(i==1){
            continue;
        }
        param[i].buffer_type=MYSQL_TYPE_LONGLONG;
        param[i].is_unsigned=false;
    }
    unsigned long name_length=static_cast<unsigned long>(name.size());
    param[1].buffer=name.data();
    param[1].buffer_type=MYSQL_TYPE_STRING;
    param[1].buffer_length=name_length;
    param[1].length=&name_length;
    param[1].is_unsigned=false;
    if(mysql_stmt_bind_param(stmt,param)!=0){
        res.failed_data+="参数绑定失败";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_execute(stmt)!=0){
        res.failed_data+="语句执行失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_close(stmt)!=0){
        res.failed_data+="关闭语句是发生错误";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    res.is_failed=false;
    return res;
}

//INSERT INTO 表名 列名 values 值
//id name age
response execute_post(MYSQL* now,enternode& node){
    response res{};
    unsigned long name_length =
    static_cast<unsigned long>(node.first[1].size());
    long long id=std::stoll(node.first[0]);
    long long age=std::stoll(node.first[2]);
    MYSQL_STMT* stmt=mysql_stmt_init(now);
    const char sql[]="INSERT INTO student (id,name,age) VALUES (?,?,?)";
    if(!stmt){
        res.failed_data+="语句对象初始化失败";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    if(mysql_stmt_prepare(stmt,sql,sizeof(sql)-1)!=0){
        res.failed_data+="模块化语句准备失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    MYSQL_BIND param[3]{};
    param[0].buffer=&id;
    param[1].buffer=node.first[1].data();
    param[2].buffer=&age;
    param[0].buffer_type=MYSQL_TYPE_LONGLONG;
    param[1].buffer_type=MYSQL_TYPE_STRING;
    param[2].buffer_type=MYSQL_TYPE_LONGLONG;
    for(int i=0;i<3;i++){
        param[i].is_unsigned=false;
    }
    param[1].buffer_length=name_length;
    param[1].length=&name_length;
    if(mysql_stmt_bind_param(stmt,param)!=0){
        res.failed_data+="参数绑定失败";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_execute(stmt)!=0){
        res.failed_data+="语句执行失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_close(stmt)!=0){
        res.failed_data+="关闭语句是发生错误";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    res.is_failed=false;
    return res;
}
//DELETE FROM student WHERE id=?
response execute_delete(MYSQL* now,enternode& node){
    response res{};
    long long id=std::stoll(node.first[0]);
    MYSQL_STMT* stmt=mysql_stmt_init(now);
    const char sql[]="DELETE FROM student WHERE id=?";
    if(!stmt){
        res.failed_data+="语句对象初始化失败";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    if(mysql_stmt_prepare(stmt,sql,sizeof(sql)-1)!=0){
        res.failed_data+="模块化语句准备失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    MYSQL_BIND param[1]{};
    param[0].buffer=&id;
    param[0].buffer_type=MYSQL_TYPE_LONGLONG;
    param[0].is_unsigned=false;
    if(mysql_stmt_bind_param(stmt,param)!=0){
        res.failed_data+="参数绑定失败";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_execute(stmt)!=0){
        res.failed_data+="语句执行失败:";
        res.failed_data+="\t";
        record_error(true,now,res,stmt);
        return res;
    }
    if(mysql_stmt_close(stmt)!=0){
        res.failed_data+="关闭语句是发生错误";
        res.failed_data+="\t";
        record_error(false,now,res,nullptr);
        return res;
    }
    res.is_failed=false;
    return res;
}



response sendto_database(enternode& node,METHOD method){
    response res{};
    switch(method){
        case METHOD::GET:
        {
            MYSQL* now=get_connect();
            res=execute_get(now,node);
            free_connect(now);
            break;
        }
        case METHOD::HEAD:
        {
            MYSQL* now=get_connect();
            res=execute_head(now,node);
            free_connect(now);
            break;
        }
        case METHOD::PUT:
        {
            MYSQL* now=get_connect();
            res=execute_put(now,node);
            free_connect(now);
            break;
        }
        case METHOD::POST:
        {
            MYSQL* now=get_connect();
            res=execute_post(now,node);
            free_connect(now);
            break;
        }
        case METHOD::DELETE:
        {
            MYSQL* now=get_connect();
            res=execute_delete(now,node);
            free_connect(now);
            break;;
        }
    }
    return res;
}

