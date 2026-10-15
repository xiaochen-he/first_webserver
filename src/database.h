#pragma once
#include<iostream>
#include<vector>
#include<cstdlib>
#include<unordered_map>
#include<mysql/mysql.h>
#include<utility>
#include<memory>
#include<queue>
#include<mutex>
#include<condition_variable>
#include"HTTP.h"
#include <string>

struct response {
    std::vector<std::string> data;
    bool is_failed = false;
    std::string failed_data;
};

struct enternode {
    std::vector<std::string> first;
    //用来对应HTTPJSON解析是否是正确的（也就是说是否合法）
    bool is_failed;
};


int initialize_connect_pool(int n);

response sendto_database(enternode& node,METHOD method);
