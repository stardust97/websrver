#pragma once

#include<exception>
#include<pthread.h>
#include<semaphore.h>

//线程同步机制封装类

//互斥锁类
class locker{
public:
    locker(){
        //使用动态方式创建一个互斥锁结构体变量
        if(pthread_mutex_init(&m_mutex,NULL)!=0){
            throw std::exception();
        }
    }

    ~locker(){
        //动态方式创建的互斥锁需要手动销毁
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock(){
        return pthread_mutex_lock(&m_mutex)==0;
    }

    bool unlock(){
        return pthread_mutex_unlock(&m_mutex)==0;
    }

    pthread_mutex_t* getlocker(){
        return &m_mutex;
    }

private:
    pthread_mutex_t  m_mutex;
};


//条件变量类
class cond{
public:
    cond(){
        if(pthread_cond_init(&m_cond,NULL)!=0){
            throw std::exception();
        }
    }

    ~cond(){
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t* m_mutex){
        return pthread_cond_wait(&m_cond,m_mutex)==0;
    }
    
    bool timedwait(pthread_mutex_t* m_mutex,struct timespec* t){
        return pthread_cond_timedwait(&m_cond,m_mutex,t)==0;
    }

    //唤醒线程
    bool signal(){
        return pthread_cond_signal(&m_cond)==0;
    }

    //唤醒所有线程
    bool broadcast(){
        return pthread_cond_broadcast(&m_cond)==0;
    }

private:
    pthread_cond_t m_cond;
};


//信号量类
class sem{
public:
    //第二个参数非0为进程间共享，0表示进程内的线程共享
    sem(){
        if(sem_init(&m_sem,0,0)!=0){
            throw std::exception();
        }
    }

    //指定信号量的初始值为num
    sem(int num){
        if(sem_init(&m_sem,0,num)!=0){
            throw std::exception();
        }
    }

    ~sem(){
        sem_destroy(&m_sem);
    }

    bool wait(){
        return sem_wait(&m_sem)==0;
    }

    bool post(){
        return sem_post(&m_sem)==0;
    }

private:
    sem_t m_sem;
};


