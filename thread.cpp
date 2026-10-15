#include<thread>
#include<string>
#include<vector>
#include<queue>
#include<condition_variable>
#include<memory>
#include<mutex>
#include<iostream>

std::mutex now;
std::queue<int> pending;//提交队列
std::condition_variable cv;


std::vector<std::thread> threads;


void worker(){
    while(true){

    }
}

void initailize(int n){
    for(int i=0;i<n;i++){
        threads.emplace_back(worker);
    }
    return;
}

int main(){
    initailize(10);
    return 0;
}