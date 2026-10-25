#include<iostream>
#include<cstring>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<string>
#include<cerrno>
#include"HTTP.h"
#include<vector>
#include<sys/epoll.h>
#include<sys/timerfd.h>
#include <cstdint>
#include<thread>
#include<memory>
#include<functional>
#include<sys/eventfd.h>
#include <cstdint>
#include<queue>
#include<mutex>
#include <condition_variable>
#include<fstream>
//这两个来获取当前的时间
#include <ctime>
#include <iomanip>
#include <sstream>
#include"database.h"
#include"json_parse.h"
#include<signal.h>
#include<sys/signalfd.h>

constexpr int port=8080;
constexpr int logcount=4096;
constexpr int thread_number=15;
constexpr int timewait=5;
constexpr int connect_number=15;
/*
- INFO：正常信息，例如服务器启动、请求完成。
- WARN：异常提醒，但仍可以继续运行。
- ERROR：某项操作失败，例如请求解析失败。
- FATAL：不可恢复的问题，程序无法正常继续运行。*/
constexpr int INFO=1;
constexpr int WARN=2;
constexpr int ERROR=3;
constexpr int FATAL=4;

//枚举监听fd和客户端fd的模式ET(ONESHOT)/LT/ET
enum class Mode_fd{
    ET,
    LT,
    ET_ONESHOT
};

struct location{
    Mode_fd client_fd=Mode_fd::ET_ONESHOT;
    Mode_fd listen_fd=Mode_fd::LT;
};

location now;


class Connection{
public:
    bool is_close=false;
    Httpparser parser;
    std::string response;
    std::string recv_data;//没有解析的数据
    bool client_status=true;//表示连接存在，不用close清理clear
    bool recv_status=true;//表示接收正常.,false表示接收数据过程出现错误需要清理
    std::string last_data;
};

//pending_log中应该存储的东西
struct lo{
    std::string pending;//提交的记录
    int client_fd;
    std::thread::id thread_id;
    std::time_t pending_time;
};

//专用于日志模块的结构体
struct log_thread{
    std::mutex log_mutex;//队列锁
    std::queue<std::pair<int,lo>> pending_log;//需要提交记录的日志队列，级别和提交的记录
    std::condition_variable cv;//通过这个来短暂释放锁和等待通知
    int loglevel=WARN;//初始化为3
    bool failed=false;//判断是否还是写入日志
    //这个stopping专门用于日志线程的退出
    bool log_stopping=false;
};

log_thread log_state;//初始化全局日志结构体

//这个是需要共享的，就不能有线程独有的东西
struct chirld_thread{
    std::queue<int> newclient;//存储输入的client
    int wake_fd=-1;//这个是新加入的，然后主线程会调用每次选择的工作线程
    std::mutex one;
    int close_fd=-1;
    //这些都是要独有的
};
//这些是每个线程独享的，不会被其他所以知晓的
struct thread_self{
    std::unordered_map<int,Connection> map;//client对应他自己的Connection，如果已经确认完成就删除map中对应的.
    std::vector<std::pair<int,int>> list;//每个的到期时间以及对应client_fd
    std::unordered_map<int,int> timer_index;//client_fd对应list中的index
    std::unordered_map<int,bool> old_client;//client_fd是否已经过期
    int tfd;
    int deadline=0;
};

//共享的输入队列，下标对应threads，然后每次就可以修改里面的newclient，其余的交给工作线程操作
std::vector<std::unique_ptr<chirld_thread>> pending_client;
//线程池，每个线程所在的下标与输入队列的下标的对应的
//主线程负责初始化工作线程，pending_client由每个的事件循环初始化
std::vector<std::thread> threads;//这是工作线程池



//统一调用关闭连接，并且清除关注和map中的函数,第三个参数是决定是否在这里需要
//清除map哈希表中的映射
void client_clear(int client_fd, int epfd, bool map_is_clear,thread_self& myself);
//声明定时器更新函数,堆空就停止定时器，堆不为空就更新定时器
void update_timer(thread_self& myself);
//这是写入日志队列的函数，调用level代表等级，st代表内容,在需要记录的地方调用这个
void log_message_write(int level, const std::string& st, int client_fd);
//errno来自头文件：
//#include <cerrno>
//它用于保存 C/POSIX 函数最近一次失败的错误编号。可以先把它理解成由系统库维护的一个整数。
//错误不同对应编号不同，可通过std::strerror(saved_errno)将错误编号转换为错误信息（具体因为什么错误）。
void print_error(const char* operation,int client_fd,int level=ERROR);

/*list重排序函数*/
/*左孩子：2 * index + 1
右孩子：2 * index + 2
父节点：(index - 1) / 2，仅适用于 index > 0*/
void resort(int index,int client_fd,thread_self& myself){
    int n=static_cast<int>(myself.list.size());
    int father=(index-1)/2;
    while(father>=0&&myself.list[father].first>myself.list[index].first){
        auto temp=myself.list[father];
        myself.list[father]=myself.list[index];
        myself.list[index]=temp;
        myself.timer_index[myself.list[index].second]=index;
        myself.timer_index[myself.list[father].second]=father;
        index=father;
        if(index==0){
            break;
        }
        father=(index-1)/2;
    }
    while(true){
        int left=index*2+1;
        if(left>=n){
            return;
        }
        int right=left+1;
        int change = left;
        if (right < n && myself.list[right].first < myself.list[left].first) {
            change = right;
        }
        if(myself.list[index].first<=myself.list[change].first){
            break;
        }
        int change_client=myself.list[change].second;
        auto temp=myself.list[index];
        myself.list[index]=myself.list[change];
        myself.list[change]=temp;
        myself.timer_index[change_client]=index;
        myself.timer_index[client_fd]=change;
        index=change;
    }
    return;
}   


/*到期删除函数list*/
void remove_list(int nowtime,int epfd,thread_self& myself){
    while(!myself.list.empty()&&myself.list[0].first<=nowtime){
        int n=static_cast<int>(myself.list.size());
        auto temp=myself.list[n-1];
        myself.list[n-1]=myself.list[0];
        myself.list[0]=temp;
        myself.timer_index[myself.list[n-1].second]=n-1;
        myself.timer_index[myself.list[0].second]=0;
        int client_fd=myself.list[n-1].second;
        //记录本批次的过期client_fd
        myself.old_client[client_fd]=true;
        myself.timer_index.erase(client_fd);
        myself.list.pop_back();
        if(!myself.list.empty()){
            resort(0,myself.list[0].second,myself);//重排第一位的
        }
        //不能跳过后续的关闭连接部分
        auto it=myself.map.find(client_fd);
        if(it==myself.map.end()){
            continue;
        }else{
            client_clear(client_fd,epfd,true,myself);
        }
        //这样就全部清除完成
    }
    //更新定时器，不管是否为空，因为其内部有对应处理
    update_timer(myself);
    return;
}


//堆空则关闭定时器，堆不空就根据堆顶更新定时器和deadline
void update_timer(thread_self& myself){
    if(myself.list.empty()){
        itimerspec stop{};
        if (timerfd_settime(myself.tfd, 0, &stop, nullptr) == -1) {
            print_error("timerfd_settime",-1);
        }
        myself.deadline=0;
        return;
    }else{
        myself.deadline=myself.list[0].first;
        itimerspec spec{};
        spec.it_value.tv_sec=myself.deadline;
        timerfd_settime(myself.tfd,TFD_TIMER_ABSTIME,&spec,nullptr);
        return;
    }
}


/*
封装监听client_fd的函数，根据事件是EPOLLIN|EPOLLOUT这些种类，还有根据是MOD|ADD这两种因为DEL有专门的删除函数
还有根据传入参数也就是Mode_fd来判断是否用ONESHOT和ET还是只是默认LT
返回值是bool，true代表成功，false代表失败*/
//io_events代表IN还是OUT，operation代表是MOD还是ADD，mode代表什么模式，
bool set_client_event(thread_self& myself,
    int epfd,int client_fd,
    int operation,Mode_fd mode,
      uint32_t io_events){
    std::string record;
    auto it=myself.map.find(client_fd);
    if(it==myself.map.end()){
        Connection i;
        myself.map[client_fd]=i;
    }
    epoll_event client_event{};
    client_event.data.fd=client_fd;
    switch(mode){
        case Mode_fd::ET:
        {
            client_event.events=io_events|EPOLLET;
        }
        break;
        case Mode_fd::LT:{
            client_event.events=io_events;
        }
            break;
        case Mode_fd::ET_ONESHOT:{
            client_event.events=io_events|EPOLLET|EPOLLONESHOT;
        }
            break;
    }
    if(operation==EPOLL_CTL_ADD){
        record+="EPOLL_CTL_ADD";
    }else if(operation==EPOLL_CTL_MOD){
        record+="EPOLL_CTL_MOD";
    }
    record+="\t";
    if(io_events==EPOLLIN){
        record+="EPOLLIN";
    }else{
        record+="EPOLLOUT";
    }
    if(epoll_ctl(epfd,operation,client_fd,&client_event)<0){
        const int saved_error=errno;
        record+="错误码=";
        record+=std::to_string(saved_error);
        record+="\t";
        record+=std::strerror(saved_error);
        record+="\t失败";
        log_message_write(ERROR,record,client_fd);
        myself.map.erase(client_fd);
        return false;
    }
    return true;
}


//errno来自头文件：
//#include <cerrno>
//它用于保存 C/POSIX 函数最近一次失败的错误编号。可以先把它理解成由系统库维护的一个整数。
//错误不同对应编号不同，可通过std::strerror(saved_errno)将错误编号转换为错误信息（具体因为什么错误）。
void print_error(const char* operation,int client_fd,int level){
    const int saved_errno=errno;
    std::string record;
    record+=operation;
    record+="\t";
    record+="错误码=";
    record+=std::to_string(saved_errno);
    record+="\t";
    record+=std::strerror(saved_errno);
    log_message_write(level,record,client_fd);
    std::cerr<<operation<<"失败:"<<std::strerror(saved_errno)<<'\n';
}

//发送所有数据
bool send_all(const int epfd,const int client_fd,const std::string& response,thread_self& myself){
    //提取上次的数据
    std::string lastdata=myself.map[client_fd].last_data;
    std::size_t total_sent = 0;
    std::string send_to=lastdata+response;
    while(total_sent<send_to.size()){
        ssize_t i=send(client_fd,send_to.data()+total_sent,send_to.size()-total_sent,MSG_NOSIGNAL);
        if(i>0){
            total_sent+=static_cast<std::size_t>(i);
            continue;
        }
        if(i==0){
            std::string record;
            log_message_write(WARN,"发送未取得进展",client_fd);
            std::cout<<"发送的数据为0";
            return false;
        }
        //要么被其他所终止
        if(errno == EINTR){
            continue;
        }
        //说明当前是因为异常是EAGAIN（代表发送缓冲区可能满或者rwnd或者cwnd限制导致）
        if(errno==EAGAIN){
            //从还没有发送的开始记录
            myself.map[client_fd].last_data=send_to.substr(total_sent);
            if(!set_client_event(myself,epfd,client_fd,EPOLL_CTL_MOD,now.client_fd,EPOLLOUT)){
                return false;
            }
            return true;
        }
        print_error("send()",client_fd);
        return false;
    }
    myself.map[client_fd].last_data="";
    std::string record;
    record+="响应发送成功:";
    record+="\t";
    log_message_write(INFO,record,client_fd);
    return true;
}

//现在在这里接入刚刚写好的两个接口，一个是构造输入参数，一个是输入SQL语句到数据库
//数据库接入简单部分已经完成，然后现在就应该是接入真实的数据库了
std::string build_response(const HTTP_REQUEST& request,bool is_error,int client_fd){
    //表明是错误，所以构造400响应体
    response res{};
    std::string body;
    if(is_error){
        const std::string body = "请求解析阶段出现问题";

        std::string r = "HTTP/1.1 400 Bad Request\r\n";
        r += "Content-Type: text/plain; charset=utf-8\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        r += "\r\n";

        if (request.method != METHOD::HEAD) {
            r += body;
        }
        return r;
    }
    //说明解析阶段没有问题
    enternode now=json_parse(request);//调用自己写好的接口
    //说明这次解析的请求其中的参数不对,用对应的错误码表示
    if(now.is_failed){
        log_message_write(WARN,"本次请求的参数不对",client_fd);
        //然后构造对应响应
        std::string body="本次请求的参数不对";
        std::string r="HTTP/1.1 400 Bad Request\r\n";
        r += "Content-Type: text/plain; charset=utf-8\r\n";
        r+= "Content-Length: " + std::to_string(body.size()) + "\r\n";
        r+="\r\n";
        if(request.method!=METHOD::HEAD){
            r+=body;
        }
        return r;
    }else{
        //说明参数正确
        res=sendto_database(now,request.method);
        //说明这次sql语句执行错误,就用对应的错误码
        if(res.is_failed){
            log_message_write(
                ERROR, "数据库执行失败: " + res.failed_data, client_fd);

            std::string body = "本次服务器执行失败";
            std::string r="HTTP/1.1 500 Internal Server Error\r\n";
           r += "Content-Type: text/plain; charset=utf-8\r\n";
            r+= "Content-Length: " + std::to_string(body.size()) + "\r\n";
            r+="\r\n";
            if(request.method!=METHOD::HEAD){
                r+=body;
            }
            return r;
        }else{
            //执行成功
            std::string body="本次执行成功";
            body+="\t";
            for(int i=0;i<static_cast<int>(res.data.size());i++){
                body+=res.data[i];
                if(i!=static_cast<int>(res.data.size())-1){
                    body+="\r\n";
                }
            }
            std::string r="HTTP/1.1 200 OK\r\n";
            r += "Content-Type: text/plain; charset=utf-8\r\n";
            r+= "Content-Length: " + std::to_string(body.size()) + "\r\n";
            r+="\r\n";
            if(request.method!=METHOD::HEAD){
                r+=body;
            }
            return r;
        }
    }
}



//需要一个单独的封装解析函数
std::vector<std::string> parse_data(thread_self& myself,
    std::string& result,Httpparser& parser,
    int epfd,int client_fd,bool is_error){
    switch(parser.parse_request(result)){
        case ParseResult::NEED_MORE_DATA:{
            //需要更多的数据，所以就应该复用监听
            if(!myself.map[client_fd].client_status){
                client_clear(client_fd, epfd, false,myself);
                return {};
            }
            if(!set_client_event(myself,epfd,client_fd,EPOLL_CTL_MOD,now.client_fd,EPOLLIN)){
                client_clear(client_fd,epfd,false,myself);
                myself.map[client_fd].client_status=false;
            }
            //复用监听,返回
            return {};
        }
        case ParseResult::COMPLETE:
            //因为解析完成，所以就直接break，进行下一步构造响应
            //然后把当前剩余的部分的data保存下来,查看是否有无，有就保存在connect对象里面的recv_data中
        /*const std::pair<std::string,std::size_t> read_check() const noexcept{
                           return {read_buffer,check_index};
                     }*/
            {
                std::pair<std::string,std::size_t> rc=parser.read_check();
                if(rc.first.size()!=rc.second){
                    myself.map[client_fd].recv_data=rc.first.substr(rc.second);//将剩余部分保存
                }else{
                    myself.map[client_fd].recv_data="";//表示当前无后续
                }
                std::string record;
                HTTP_REQUEST request=parser.request();//获取结构体
                record+="method=";
                switch(request.method){
                    case METHOD::GET:
                        record+="GET\t";
                        break;
                    case METHOD::POST:
                        record+="POST\t";
                        break;
                    case METHOD::PUT:
                        record+="PUT\t";
                        break;
                    case METHOD::HEAD:
                        record+="HEAD\t";
                        break;
                    case METHOD::DELETE:
                        record+="DELETE\t";
                        break;
                }
                record+=request.path;
                record+="\t";
                record+=request.version;
                record+="\t";
                log_message_write(INFO,record,client_fd);
            }
            break;
        case ParseResult::ERROR:{
            is_error=true;
            std::string record;
            record+="HTTP解析阶段出现问题,将返回400:";
            log_message_write(WARN,record,client_fd);
            myself.map[client_fd].client_status=false;//说明解析出现问题，需要进行清理
        }
        break;
    }
    HTTP_REQUEST request=parser.request();
    //根据解析的请求来构造响应
    //method，path，vesion，body，但是现在只是构造固定响应
    //构造固定响应不需要根据结果来,因为现在没有数据库
    std::string response=build_response(request,is_error,client_fd);
    myself.map[client_fd].is_close=request.is_close;//将解析器中的是否关闭连接存储到当前对应的连接中
    parser.reset();//清除旧数据
    return {response,result};
}   


//现在是HTTP服务器了，因为我写好了基础的HTTP有限状态机
std::vector<std::string> recv_data(const int client_fd,const int epfd,thread_self& myself){
    Httpparser& parser=myself.map[client_fd].parser;//根据client_fd，来提取对应的解析器
    char buffer[1024];
    std::string result;//这是新加入的部分，因为解析器只要存在就可以一直根据解析里面内部有一个read_buffer然后一直加入，然后也会有check_index一直介入
    bool is_error=false;
    int count=0;
    while(true){
        ssize_t recv_length=recv(client_fd,buffer,sizeof(buffer),0);
        if(recv_length>0){
            result.append(buffer,static_cast<std::size_t>(recv_length));
            if(count==0){
                timespec nowtime{};
                clock_gettime(CLOCK_MONOTONIC,&nowtime);
                int starttime=nowtime.tv_sec;
                int index=myself.timer_index[client_fd];//list中的地址
                myself.list[index]={starttime+timewait,client_fd};//更新当前client_fd的到期时间
                resort(index,client_fd,myself);//调用重排序函数
                //如果当前的下一次到期时间大于当前的到期时间就更新并且重新设置下次到期
                //也就是说看之前的到期时间是否小于当前重新计算的到期时间，如果是就更新
                //并且更新deadline
                update_timer(myself);//更新定时器
                count++;
            }
        }
        if(recv_length==0){
            //对端关闭连接
            myself.map[client_fd].client_status=false;//表示响应结束后应该关闭连接
            break;
        }
        if(recv_length<0){
            if(errno==EINTR){
                continue;
            }
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }else{
                //其他错误,当前一致按关闭client连接处理
                print_error("recv()",client_fd);
                client_clear(client_fd,epfd,false,myself);
                myself.map[client_fd].recv_status=false;//外部根据这个来删除map中的client
                return {};
            }
        }
    }
    //调用解析函数进行解析
    return parse_data(myself,result,parser,epfd,client_fd,is_error);
}
//接收监听是否出现别的错误，如果没有就返回true,如果有就返回false
bool listen_fd_ready(const int listen_fd,int epfd,int& index){
    int n=thread_number;
    do{
        sockaddr_in client_address{};
        socklen_t client_address_len=sizeof(client_address);
        int client_fd=accept4(listen_fd,
                            reinterpret_cast<sockaddr*>(&client_address),
                            &client_address_len,
                            SOCK_NONBLOCK|SOCK_CLOEXEC);
        // LT尝试接受一次，ET持续接受直到队列暂时为空
        if(client_fd<0){
            auto err=errno;
            if(err == EAGAIN || err == EWOULDBLOCK){
                break;
            }
            if (err == EWOULDBLOCK ||
            err == ECONNABORTED ||err==EINTR||
            err == ENETDOWN || err == EPROTO ||
            err == ENOPROTOOPT || err == EHOSTDOWN ||
            err == ENONET || err == EHOSTUNREACH ||
            err == EOPNOTSUPP || err == ENETUNREACH) {
                continue; 
            }
            print_error("accept4()",client_fd,FATAL);
            return false;
        }
        index=(index+1)%n;
        {
            std::lock_guard<std::mutex> guard(pending_client[index]->one);
            pending_client[index]->newclient.push(client_fd);
        }
        //解锁后通知加入
        std::uint64_t value = 1;
        write(pending_client[index]->wake_fd,&value,sizeof(value));//写入1;
        //定时器也是工作线程独有（每个独有,所以也需要放入事件循环中）
    }while(now.listen_fd==Mode_fd::ET||now.listen_fd==Mode_fd::ET_ONESHOT);
    if(now.listen_fd==Mode_fd::ET_ONESHOT){
        //复用监听:
        epoll_event event{};
        event.events=EPOLLIN|EPOLLET|EPOLLONESHOT;
        event.data.fd=listen_fd;
        if(epoll_ctl(epfd,EPOLL_CTL_MOD,listen_fd,&event)<0){
            print_error("epoll_ctl()",-1,FATAL);
            return false;
        }
    }
    return true;
}


//当tfd可读，先提取当前到期次数（但是不需要使用），然后进行到期处理
void tfd_ready_EPOLLIN(const int tfd,int epfd,thread_self& myself){
    std::uint64_t count = 0;
    ssize_t bytes;
    do {
        bytes = read(tfd, &count, sizeof(count));
    } while (bytes == -1 && errno == EINTR);
    //消耗当前的到期,不需要提取
    timespec nowtime{};
    clock_gettime(CLOCK_MONOTONIC,&nowtime);//获取当前时间，进行删除
    int nowtimes=nowtime.tv_sec;
    if(myself.list.empty()){
        update_timer(myself);//如果为空调用进行停止
    }else{
        remove_list(nowtimes,epfd,myself);//其内部有定时处理
    }
    return;
}


//当监听到client_fd并且事件伴随err的时候就调用这个函数,因为出现了错误所以直接删除当前监听并跳过
void client_fd_ready_EPOLLERR(int client_fd,int epfd,thread_self& myself){
    //socket出现错误，打印当前错误，并且关闭连接
    int socket_error = 0;
    socklen_t len = sizeof(socket_error);
    /*这里用 SO_ERROR 获取 socket 的错误编号，
    不能直接用你现有的 print_error("EPOLLERR")：
    事件标志不会替你把错误原因写进当前 errno
    但是会把对应的编号写入socket_error*/
    int getstatus=getsockopt(client_fd,SOL_SOCKET, SO_ERROR,&socket_error,&len);
    if(getstatus<0){
        print_error("getsockopt()",client_fd);
    }else{
        if(socket_error!=0){
            std::string record;
            record+="错误码=";
            record+=std::to_string(socket_error);
            record+="\t";
            record+="socket error:";
            record+=std::strerror(socket_error);
            log_message_write(ERROR,record,client_fd);
            std::cerr << "socket error: "<< std::strerror(socket_error) << '\n';
        }else{
            log_message_write(WARN,"出现了EPOLLERR但是没有获取到待处理的socket错误",client_fd);
        }
    }
    client_clear(client_fd,epfd,true,myself);
    return;
}


//监听到client_fd可读就是传来数据，封装后不需要返回值，无论怎么样都需要continue，所以只需要提前终止即可
void client_fd_ready_EPOLLIN(int client_fd,int epfd,thread_self& myself){
    auto it =myself.map.find(client_fd);
    if(it==myself.map.end()){
        Connection i;
        myself.map[client_fd]=i;
    }
    std::vector<std::string> response_request=recv_data(client_fd,epfd,myself);
    if(response_request.empty()){
        if(!myself.map[client_fd].recv_status||!myself.map[client_fd].client_status){
            myself.map.erase(client_fd);//当前连接的数据接收阶段出现问题，关闭这个连接
            return;
        }else {
            //说明需要更多的数据
            return;//进行下一个current_fd的步骤了
        }
    }
    //说明当前不为空，收到了response和request;
    //也需要更新定时器
    std::string response=response_request[0];
    std::string request=response_request[1];
    myself.map[client_fd].response+=response;
    if(!set_client_event(myself,epfd,client_fd,EPOLL_CTL_MOD,now.client_fd,EPOLLOUT)){
        client_clear(client_fd,epfd,true,myself);
        return;
    }
    return;
}


//client_fd可写,根据可写规则进行封装函数,同样不需要返回值，只需要提前终止，继续continue即可
void client_fd_ready_EPOLLOUT(int client_fd,int epfd,uint32_t ready,thread_self& myself){
    if(ready&EPOLLHUP){
        myself.map[client_fd].client_status=false;//因为对面连接关闭需要将这个信号修改
        log_message_write(INFO,"客户端关闭连接",client_fd);
    }
    std::string response=myself.map[client_fd].response;
    if(send_all(epfd,client_fd,response,myself)){
        myself.map[client_fd].response="";
        if(!myself.map[client_fd].last_data.empty()){
            //这里直接返回，因为当前响应还没有发送完成，所以直接返回（send_all已经重启对epollout的监听）
            return;
        }
    }else{
        //要么是没有发完遇到其他错误（除开EAGAIN）,或者是因为EAGAIN然后在重启监听写的时候遇到错误
        client_clear(client_fd,epfd,true,myself);
        return;//提前终止即可
    }
    //已经发送完成
    if(myself.map[client_fd].is_close){
        client_clear(client_fd,epfd,true,myself);//直接关闭连接清理，因为请求中有连接关闭connecttion
        log_message_write(INFO,"请求中有关闭连接请求头",client_fd);
        return;
    }
    //写事件完成，检查是否有剩余数据，如果有就复用解析，然后根据解析结果进行
    //如果没有就关闭连接
    //按理论来说应该是看服务器响应结构决定是否需要连接
    //如果不需要就直接关闭连接
    //如果需要就检查是否有数据，有数据就先解析再复用，如果没有就直接复用
    //如果对面的连接关闭和收到之前的数据解析有问题就关闭
    if(!myself.map[client_fd].client_status){
        client_clear(client_fd,epfd,true,myself);
        return;
    }
    //这里是根据里面是否为空，如果不为空说明还有数据
    //需要解析
    if(myself.map[client_fd].recv_data.empty()){
        //如果为空就继续
        //重新监听读时间
         set_client_event(myself,epfd, client_fd, EPOLL_CTL_MOD,
                     now.client_fd, EPOLLIN);
        return;//关闭就交给定时器解决
    }else{
        //没有问题就继续解析剩余的数据
        std::string recv_data=myself.map[client_fd].recv_data;
        Httpparser& parser=myself.map[client_fd].parser;
        //直接调用专门的封装解析函数进行解析
        //因为不用读取数据，所以直接调用就行
        std::vector<std::string> response_request=parse_data(myself,recv_data,parser,
            epfd,client_fd,false);
        if(response_request.empty()){
            if(!myself.map[client_fd].recv_status||!myself.map[client_fd].client_status){
                myself.map.erase(client_fd);//当前连接的数据接收阶段出现问题，关闭这个连接
            }
            return;//进行下一个current_fd的步骤了
        }
        //说明当前不为空，收到了response和request;
        std::string response=response_request[0];
        std::string request=response_request[1];
        myself.map[client_fd].response+=response;
        if(!set_client_event(myself,epfd,client_fd,EPOLL_CTL_MOD,now.client_fd,EPOLLOUT)){
            client_clear(client_fd,epfd,true,myself);
            return;
        }
    }
}



//统一调用关闭连接，并且清除关注和map中的函数,第三个参数是决定是否在这里需要
//清除map哈希表中的映射
void client_clear(int client_fd,int epfd,bool map_is_clear,thread_self& myself){
    epoll_ctl(epfd,EPOLL_CTL_DEL,client_fd,nullptr);
    close(client_fd);
    //根据map_is_clear来判断是否进行第三个,需要就是true，
    if(map_is_clear){
        myself.map.erase(client_fd);
    }
    auto it=myself.timer_index.find(client_fd);
    int n=static_cast<int>(myself.list.size());
    if(it!=myself.timer_index.end()){
        int index=myself.timer_index[client_fd];
        if(index==n-1){
            myself.list.pop_back();
            myself.timer_index.erase(client_fd);
        }else{
            auto temp=myself.list[n-1];
            myself.list[n-1]=myself.list[index];
            myself.list[index]=temp;
            myself.timer_index[myself.list[index].second]=index;
            myself.timer_index[myself.list[n-1].second]=n-1;
            myself.list.pop_back();
            myself.timer_index.erase(client_fd);
            resort(index,temp.second,myself);
        }
    }
    update_timer(myself);
    log_message_write(INFO,"连接清理成功",client_fd);
    return;
}

/*传入当前发生需要记录的地方和级别,以及是否为异常
INFO:正常信息（比如启动完成什么什么）
WRON:警告，但不会影响运行
ERROR:异常
FATAL：不可恢复的问题
这个是用来写入输入日志队列的，并且有对应的可读性封装*/
void log_message_write(int level,const std::string& st,int client_fd){
    //在这里直接过滤不需要的日志级别
    if(level<log_state.loglevel){
        return;
    }
    std::string record=st;
    record+="\t";  
    if(level==ERROR){
        record+="ERROR";
    }
    if(level==INFO){
        record+="INFO:";
    }
    if(level==WARN){
        record+="WARN:";
    }
    if(level==FATAL){
        record+="FATAL:";
    }
    record+="\t";
    //将失败记录入日志
    bool add=false;
    {   
        std::lock_guard<std::mutex> lock(log_state.log_mutex);
        if(!log_state.failed){
            add=true;
            lo push{};
            push.client_fd=client_fd;
            std::thread::id tid = std::this_thread::get_id();
            push.thread_id=tid;
            push.pending_time=std::time(nullptr);
            push.pending=record;
            log_state.pending_log.push({level,push});
        }
    }
    //提醒已经把错误记录
    if(add){
        log_state.cv.notify_one();
    }
    return ;
}

//日志模块，也就是日志线程的事件循环
void worker_log(){
    std::ofstream file("src/file.log",std::ios::app);//追加输入而不是覆盖输入
    if (!file.is_open()) {
    // 报告日志文件打开失败
        std::cerr<<std::strerror(errno)<<'\n';
        {
            std::lock_guard<std::mutex> lock(log_state.log_mutex);
            log_state.failed=true;
            while(!log_state.pending_log.empty()){
                log_state.pending_log.pop();
            }
        }
        return;
    }
    while(true){
        std::unique_lock<std::mutex> lock(log_state.log_mutex);
        //队列为空时释放锁并等待，被唤醒后重新取得锁，再检查队列是否非空。
        log_state.cv.wait(lock,[]{
            //三种情况，有日志，打开失败，有停止通知
            return !log_state.pending_log.empty()||log_state.failed||log_state.log_stopping;
        });
        //空的同时，要么有错，要么有停止通知，就直接return
        if((log_state.failed||log_state.log_stopping)&&log_state.pending_log.empty()){
            //跳出循环
            return;
        }
        //进行到这里说明已经不为空并且现在有锁
        //提取提交队列里面的东西
        std::pair<int,lo> now_log=log_state.pending_log.front();
        log_state.pending_log.pop();
        //获取之后解锁
        lock.unlock();
        int level=now_log.first;
        std::string& node=now_log.second.pending;  
        //在log_message_write中已经修剪了level(在worker_log中就不需要完成)
        //获取当前时间
        std::time_t nowtime=now_log.second.pending_time;
        std::tm local{};
        if (localtime_r(&nowtime, &local) == nullptr) {
            std::cerr<<std::strerror(errno)<<'\n';
            {
                std::lock_guard<std::mutex> lock(log_state.log_mutex);
                log_state.failed=true;
                while(!log_state.pending_log.empty()){
                    log_state.pending_log.pop();
                }
            }
            return;
        }
        //-1代表是主线程写入日志
        if(now_log.second.client_fd!=-1){
            node+="client_fd: ";
            node+=std::to_string(now_log.second.client_fd);
            node+="\t";
        }
        node+="线程id: ";
        std::ostringstream out;
        out <<now_log.second.thread_id;
        std::string thread_id = out.str();
        node+=thread_id;
        node+="\t";
        if(level==ERROR){
            file<<std::put_time(&local,"%Y-%m-%d %H:%M:%S")<<"\t"<<node+"\n";
        }else if(level==FATAL){
            file<<std::put_time(&local,"%Y-%m-%d %H:%M:%S")<<"\t"<<node+"\n";
        }else if(level==INFO){
            file<<std::put_time(&local,"%Y-%m-%d %H:%M:%S")<<"\t"<<node+"\n";
        }else{
            file<<std::put_time(&local,"%Y-%m-%d %H:%M:%S")<<"\t"<<node+"\n";
        }
        file.flush();
        if (!file) {
            // 报告日志文件打开失败
            std::cerr<<std::strerror(errno)<<'\n';
            {
                std::lock_guard<std::mutex> lock(log_state.log_mutex);
                log_state.failed=true;
                while(!log_state.pending_log.empty()){
                    log_state.pending_log.pop();
                }
            }
            return;
        }
    }
    return;
}



//工作线程的事件循环,目前还需要对应的epfd，之后就不需要了，因为在每个工作线程独有
void worker(chirld_thread& nowthread){
    //事件循环第一件事情，创建对应epfd
    thread_self myself{};
    int epfd=epoll_create1(EPOLL_CLOEXEC);
    //创建对应的唤醒fd
    int wake_fd=nowthread.wake_fd;
    //这就是在初始化的时候将wake_fd写入了输入列表中。
    //这样就可以让主线程初始化的时候知道这个wake_fd
    //写入结构体中
    int tfd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    myself.tfd=tfd;
    //不需要在意，但是这个tfd需要每次传参数，因为当前已经不是全局变量了。
    itimerspec spec{};
    //当前不需要加入监听，原本的单线程模式是在listen_fd处理的时候加入监听
    //现在只需要在处理wake_fd的时候处理监听就可以了。
    timerfd_settime(tfd,0,&spec,nullptr);
    epoll_event event{};
    event.events=EPOLLIN;
    event.data.fd=wake_fd;
    epoll_ctl(epfd,EPOLL_CTL_ADD,wake_fd,&event);
    epoll_event clevent{};
    clevent.events=EPOLLIN;
    clevent.data.fd=nowthread.close_fd;
    epoll_ctl(epfd,EPOLL_CTL_ADD,nowthread.close_fd,&clevent);
    epoll_event tevent{};
    tevent.events=EPOLLIN;
    tevent.data.fd=tfd;
    epoll_ctl(epfd,EPOLL_CTL_ADD,tfd,&tevent);
    bool is_continue=true;
    while(is_continue){
        epoll_event recv_event[1024];
        int recv_size=epoll_wait(epfd,recv_event,1024,-1);
        myself.old_client.clear();//调用结构体的
        for(int i=0;i<recv_size;i++){
            int current_fd=recv_event[i].data.fd;
            if(current_fd==wake_fd){
                //被唤醒，需要读取输入队列里面的值然后进行操作，
                //当前先不封装函数，先完成功能，最后再封装
                //先根据wake_fd读取其中的计数器
                std::uint64_t count=0;
                ssize_t n=read(wake_fd,&count,sizeof(count));
                if(n!=8){
                    log_message_write(WARN,"唤醒后读取计数器错误",-1);
                    continue;
                }
                //n读取成功就8字节，然后count中存储的就是数量
                //先不考虑异常处理
                for(int i=0;i<static_cast<int>(count);i++){
                    //先得到锁
                    int client_fd=-1;
                    {   
                        std::lock_guard<std::mutex> guard(nowthread.one);
                        client_fd=nowthread.newclient.front();
                        nowthread.newclient.pop();
                    }//RAII（资源获取即初始化）
                    if(client_fd!=-1){
                        //完成创建(监听)
                        set_client_event(myself,epfd,
                            client_fd,EPOLL_CTL_ADD,
                            now.client_fd,EPOLLIN);
                    }
                    //下面部分可以直接复用
                    timespec nowtime{};
                    clock_gettime(CLOCK_MONOTONIC,&nowtime);
                    int starttime=nowtime.tv_sec;//读取当前计时器里面的当前时间
                    myself.list.push_back({starttime+timewait,client_fd});//记录是记录到期时间
                    int index=static_cast<int>(myself.list.size())-1;
                    myself.timer_index[client_fd]=index;
                    update_timer(myself);//更新定时器,因为每次新记录一定是最长的,加入当前tfd
                    continue;
                }
            }else if(current_fd==tfd&&recv_event[i].events&EPOLLIN){
                //可以直接复用之前的，执行需要传入参数而已，
                //将之前的全局变量转换为当前的线程独有变量
                tfd_ready_EPOLLIN(tfd,epfd,myself);
                continue;
            }else if(current_fd==nowthread.close_fd){
                //检测到关闭信号
                while(!nowthread.newclient.empty()){
                    close(nowthread.newclient.front());
                    nowthread.newclient.pop();
                }
                while (!myself.map.empty()) {
                    int client_fd = myself.map.begin()->first;
                    client_clear(client_fd, epfd, true, myself);
                }
                close(tfd);
                is_continue=false;
                break;//退出循环
            }else{
                //或者是client_fd可读，说明当前有可读或者可写
                //以下函数进行封装
                if(myself.old_client[current_fd]){
                    continue;
                }
                auto ready=recv_event[i].events;
                if(ready&EPOLLERR){
                    //封装函数,处理事件监听到EPOLLERR
                    client_fd_ready_EPOLLERR(current_fd,epfd,myself);
                    continue;
                }else if(ready&EPOLLIN){
                    //封装函数，没有返回值，continue即可，监听可读
                    int client_fd=current_fd;
                    client_fd_ready_EPOLLIN(client_fd,epfd,myself);
                    continue;
                }else if(ready&EPOLLOUT){
                    //当前可写，调用封装函数进行操作
                    client_fd_ready_EPOLLOUT(current_fd,epfd,ready,myself);
                }else if(ready&EPOLLHUP){
                    //对面连接关闭，只有这一个事件，所以直接删除关注并关闭连接，并且中哈希表中清除
                    int client_fd=current_fd;
                    client_clear(client_fd,epfd,true,myself);
                    continue;
                }
            }
        }
    }
    close(epfd);
    return;
}

//初始化线程池的函数，参数就是线程池数量
void initialize(int n){
    for(int i=0;i<n;i++){
        //在这里创建wake_fd可以避免初始化线程的时候wake_fd还没有初始化主线程就已经开始运行了
        std::unique_ptr<chirld_thread> now=std::make_unique<chirld_thread>();
        now->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        //注册关闭通知方式
        now->close_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        pending_client.push_back(std::move(now));
    }
    //初始化
    for(int i=0;i<n;i++){
        //这样就可以对应起来，将工作线程和对应的pending_client对应
        threads.emplace_back(worker,std::ref(*pending_client[i]));
        //初始化对应线程池
    }
    return;
}

//工作线程的通知关闭函数
void stop_worker(){
    for(int i=0;i<thread_number;i++){
        std::uint64_t value = 1;
        //唤醒每个线程的close事件，
        write(pending_client[i]->close_fd,&value,sizeof(value));
    }
    //等待所有的关闭
    for(int i=0;i<thread_number;i++){
        threads[i].join();
    }
    return;
}

//日志线程的通知关闭函数,并且等待关闭
void stop_log(std::thread& mylog_thread){
    {
        std::lock_guard<std::mutex> lock(log_state.log_mutex);
        log_state.log_stopping=true;
    }
    log_state.cv.notify_one();
    mylog_thread.join();
    return;
}

//clear 当前剩下的资源,如果当前资源没有创建就填-1
void clear_all(int epfd,int listen_fd,int signal_fd){
    if(epfd!=-1){
        close(epfd);
    }
    if(listen_fd!=-1){
        close(listen_fd);
    }
    if(signal_fd!=-1){
        close(signal_fd);
    }
    if(!pending_client.empty()){
        for(int i=0;i<thread_number;i++){
            close(pending_client[i]->wake_fd);
            close(pending_client[i]->close_fd);
        }
        for(int i=0;i<thread_number;i++){
            pending_client.pop_back();
        }
    }
    return ;
}


int main(int argc, char* argv[]){
    if(argc>1){
        //规定启动程序之后的参数 就是client_mode;
        std::string client_mode=argv[1];
        if(client_mode=="et"||client_mode=="ET"){
            now.client_fd=Mode_fd::ET;
        }else if(client_mode=="ET_ONESHOT"||client_mode=="et_oneshot"){
            now.client_fd=Mode_fd::ET_ONESHOT;
        }else{
            now.client_fd=Mode_fd::LT;
        }
    }
    //初始化线程池之前需要先进行退出信号的注册（）
    sigset_t mask;
    sigemptyset(&mask);//创建空信号集合
    sigaddset(&mask,SIGINT);//ctrl+c信号
    sigaddset(&mask,SIGTERM);//终止请求信号
    int rc=pthread_sigmask(SIG_BLOCK,&mask,nullptr);
    if(rc!=0){
        print_error("pthread_sigmask",-1,ERROR);
        return 1;
    }
    int signal_fd=signalfd(-1,&mask,SFD_NONBLOCK|SFD_CLOEXEC);
    if(signal_fd==-1){
        print_error("signalfd",-1,ERROR);   
        return 1;
    }
    //在启动之前初始化线程池(工作线程池)
    //根据最上面的函数来确定
    int n=thread_number;
    initialize(n);
    //初始化连接池，调用外部写好的接口部分
    if (initialize_connect_pool(connect_number) != 0){
        //线程池初始化失败
        stop_worker();
        close_connect_pool();
        clear_all(-1,-1,signal_fd);
        return 1;
    }
    //后面就只需要调用对应的操作，就是在解析好HTTP请求之后，根据方法的不同来获取输入
    //初始化日志线程,不需要全局因为都不会有直接访问这个线程，只需要加入队列即可
    std::thread mylog_thread(worker_log);
    //没必要封装
    const int listen_fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(listen_fd<0){
        print_error("socket()",-1,FATAL);
        stop_worker();
        stop_log(mylog_thread);
        close_connect_pool();
        clear_all(-1,-1,signal_fd);
        return 1;
    }
    const int reuse=1;
    //可复用端口
    if(setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))<0){
        print_error("setsockopt()",-1,FATAL);
        stop_worker();
        stop_log(mylog_thread);
        close_connect_pool();
        clear_all(-1,listen_fd,signal_fd);
        return 1;
    }
    //bind可封装
    sockaddr_in server_address{};
    server_address.sin_family=AF_INET;
    server_address.sin_port=htons(port);
    server_address.sin_addr.s_addr=htonl(INADDR_ANY);//绑定所有网卡的对应端口
    int bind_status=bind(listen_fd,
                    reinterpret_cast<const sockaddr*>(&server_address),
                    sizeof(server_address));
    if(bind_status<0){
        print_error("bind()",-1,FATAL);
        stop_worker();
        stop_log(mylog_thread);
        close_connect_pool();
        clear_all(-1,listen_fd,signal_fd);
        return 1;
    }
    int listen_status=listen(listen_fd,logcount);
    if(listen_status<0){
        print_error("listen()",-1,FATAL);
        stop_worker();
        stop_log(mylog_thread);
        close_connect_pool();
        clear_all(-1,listen_fd,signal_fd);
        return 1;
    }
    int epfd=epoll_create1(EPOLL_CLOEXEC);
    if(epfd<0){
        print_error("epoll_create1()",-1,FATAL);
        stop_worker();
        stop_log(mylog_thread);
        close_connect_pool();
        clear_all(-1,listen_fd,signal_fd);
        return 1;
    }
    log_message_write(INFO,"epoll实例创建成功",-1);
    epoll_event event{};
    switch(now.listen_fd){
        case Mode_fd::ET:
            event.events=EPOLLIN|EPOLLET;
            break;
        case Mode_fd::LT:
            event.events=EPOLLIN;
            break;
        case Mode_fd::ET_ONESHOT:
            event.events=EPOLLIN|EPOLLET|EPOLLONESHOT;
            break;
    }
    //监听根据配置修改
    event.data.fd=listen_fd;
    int epoll_ctl_status=epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&event);
    if(epoll_ctl_status<0){ 
        print_error("epoll_ctl()",-1,FATAL);
        stop_worker();
        stop_log(mylog_thread);
        close_connect_pool();
        clear_all(epfd,listen_fd,signal_fd);
        return 1;
    }
    epoll_event signal_event{};
    signal_event.data.fd=signal_fd;
    signal_event.events=EPOLLIN;//可读
    if(epoll_ctl(epfd,EPOLL_CTL_ADD,signal_fd,&signal_event)==-1){
        log_message_write(FATAL,"监听signal_fd失败",-1);
        stop_worker();
        stop_log(mylog_thread);
        close_connect_pool();
        clear_all(epfd,listen_fd,signal_fd);
        return 1;
    }
    log_message_write(INFO,"epoll初始监听listen_fd成功",-1);
    int index=0;
    //这个的初始下标，不会影响当前的，但是可以多批次维持顺序
    bool is_continue=true;
    while(is_continue){
        epoll_event recv_event[1024]{};
        int recvsize=epoll_wait(epfd,recv_event,1024,-1);
        if(recvsize<0){
            if(errno==EINTR){
                continue;
            }
            print_error("epoll_wait()",-1,FATAL);
            stop_worker();
            stop_log(mylog_thread);
            close_connect_pool();
            clear_all(epfd,listen_fd,signal_fd);
            return 1;
        }
        //因为主线程只监听了listen_fd所以就只有listen_fd可读，
        for(int i=0;i<recvsize;i++){
            int current=recv_event[i].data.fd;
            if(current==listen_fd){
                //因为只监听listen_fd所以一定是listen_fd
                if(!listen_fd_ready(listen_fd,epfd,index)){
                    stop_worker();
                    stop_log(mylog_thread);
                    close_connect_pool();
                    clear_all(epfd,listen_fd,signal_fd);
                    return 1;
                }else{
                    continue;
                }    
            }else if(current==signal_fd){
                stop_worker();
                is_continue=false;
                break;
                //所有唤醒之后，然后里面会对应的清理独立资源
            }
        }
    }
    //关闭日志线程,并且等待关闭成功
    stop_log(mylog_thread);
    //清理资源
    clear_all(epfd,listen_fd,signal_fd);
    //清理连接池·,工作线程都结束后开始释放
    close_connect_pool();
    return 0;
}
