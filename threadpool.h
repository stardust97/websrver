#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<pthread.h>
#include<cstdio>

#include<list>
#include <exception>

#include"lock.h"


// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_numer=8,int max_requests=10000);
    ~threadpool(); 
    //将任务添加到线程池处理
    bool append(T* request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    //C++的类成员函数都有一个默认参数 this 指针，而线程调用的时候，限制了只能有一个参数 void* arg，
    //如果worker不设置成静态在调用的时候会出现this 和arg都给worker 导致错误
    static void* worker(void* arg);
    void run();

private:
    //线程池中线程池的数量
    int m_thread_numer;
    
    //线程池数组，大小为m_thread_numer
    pthread_t *m_threads;
    
    //请求队列中最多允许的，等待的请求数量
    int m_max_requests;
    
    //请求队列
    std::list<T*> m_workqueue;
    
    //信号量，用于线程间同步请求，表示是否有请求需要处理
    sem m_queuestat;
    
    //互斥锁,保护请求队列
    locker m_queuelocker;
    
    //是否结束线程
    bool m_stop;
};

template<typename T>
threadpool<T>::threadpool(int thread_numer,int max_requests):
    m_thread_numer(thread_numer),m_max_requests(max_requests),
    m_stop(false),m_threads(NULL){
    
    if(m_thread_numer<=0||m_max_requests<=0){
        throw std::exception();
    }

    m_threads=new pthread_t[m_thread_numer];
    if(!m_threads){
        throw std::exception();
    }

    //创建m_thread_numer个线程，并设置线程分离
    for(int i=0;i<m_thread_numer;++i){
        printf( "create the %dth thread\n", i);
        if( pthread_create(m_threads+i,NULL,worker,this) !=0){
            delete [] m_threads;
            throw std::exception();
        }

        if( pthread_detach( m_threads[i]) ){
            delete [] m_threads;
            throw std::exception(); 
        }
    }

}

template<typename T>
threadpool<T>::~threadpool(){
    delete [] m_threads;
    m_stop=true;
}

template<typename T>
bool threadpool<T>::append(T* request){
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if(m_workqueue.size()>=m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg){
    threadpool* pool=(threadpool*)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>:: run(){

    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();

        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* request= m_workqueue.front();
        m_workqueue.pop_front();

        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        request->process();
    }
    

}




#endif