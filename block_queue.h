#pragma once

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "lock.h"

using namespace std;

//阻塞队列类
template<class T>
class block_queue{
public:
    block_queue(int max_size=1000);
    ~block_queue();

    bool push(const T &item);
    bool pop(T &item);
    bool pop(T &item, int ms_timeout);

    void clear();
    bool full();
    bool empty();
    bool front(T &value);
    bool back(T &value);
    int size();
    int max_size();

private:
    //互斥量和条件变量用于保护阻塞队列的线程安全
    locker m_mutex;
    cond m_cond;

    T *m_array;//用于模拟阻塞队列的数组
    int m_size;//队列当前大小
    int m_max_size;//队列的总大小
    int m_front;//队头元素索引
    int m_back;//队尾元素索引
};


template<class T>
block_queue<T>::block_queue(int max_size=1000){

    if (max_size <= 0)
    {
        exit(-1);
    }

    m_max_size = max_size;
    m_array = new T[max_size];
    m_size = 0;
    m_front = -1;
    m_back = -1;
}

template<class T>
block_queue<T>::~block_queue(){
    m_mutex.lock();
    if (m_array != NULL)
        delete [] m_array;

    m_mutex.unlock();
}

template<class T>
//往队列添加元素，需要将所有使用队列的线程先唤醒
//当有元素push进队列,相当于生产者生产了一个元素
//若当前没有线程等待条件变量,则唤醒无意义
bool block_queue<T>::push(const T &item)
{
    m_mutex.lock();
    if (m_size >= m_max_size)
    {
        m_cond.broadcast();
        m_mutex.unlock();
        return false;
    }
    //插入队尾
    m_back = (m_back + 1) % m_max_size;
    m_array[m_back] = item;

    m_size++;

    m_cond.broadcast();
    m_mutex.unlock();
    return true;
}


template<class T>
//pop时,如果当前队列没有元素,将会等待条件变量
bool block_queue<T>::pop(T &item)
{
    m_mutex.lock();
    //多个消费者的时候，这里要是用while而不是if
    //因为消费者线程还会争抢资源，如果资源不够，还需继续阻塞等待条件变量
    while (m_size <= 0)
    {
        if (!m_cond.wait(m_mutex.getlocker()))
        {
            m_mutex.unlock();
            return false;
        }
    }
    //取出队首元素
    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];
    m_size--;
    m_mutex.unlock();
    return true;
}

template<class T>
//增加了超时处理
bool block_queue<T>::pop(T &item, int ms_timeout)
{
    struct timespec t = {0, 0};
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    m_mutex.lock();
    if (m_size <= 0)
    {
        t.tv_sec = now.tv_sec + ms_timeout / 1000;
        t.tv_nsec = (ms_timeout % 1000) * 1000;
        if (!m_cond.timedwait(m_mutex.getlocker(), &t))
        {
            m_mutex.unlock();
            return false;
        }
    }

    if (m_size <= 0)
    {
        m_mutex.unlock();
        return false;
    }

    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];
    m_size--;
    m_mutex.unlock();
    return true;
}

template<class T>
//清空队列
void block_queue<T>::clear()
{
    m_mutex.lock();
    m_size = 0;
    m_front = -1;
    m_back = -1;
    m_mutex.unlock();
}

template<class T>
//判断队列是否满了
bool block_queue<T>:: full() 
{
    m_mutex.lock();
    if (m_size >= m_max_size)
    {

        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

template<class T>
//判断队列是否为空
bool block_queue<T>::empty() 
{
    m_mutex.lock();
    if (0 == m_size)
    {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

template<class T>
//返回队首元素
bool block_queue<T>::front(T &value) 
{
    m_mutex.lock();
    if (0 == m_size)
    {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_front];
    m_mutex.unlock();
    return true;
}

template<class T>
//返回队尾元素
bool block_queue<T>::back(T &value) 
{
    m_mutex.lock();
    if (0 == m_size)
    {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_back];
    m_mutex.unlock();
    return true;
}

template<class T>
//获取当前队列数量
int block_queue<T>::size() 
{
    int tmp = 0;

    m_mutex.lock();
    tmp = m_size;

    m_mutex.unlock();
    return tmp;
}

template<class T>
//获取队列数量最大值
int block_queue<T>::max_size()
{
    int tmp = 0;

    m_mutex.lock();
    tmp = m_max_size;

    m_mutex.unlock();
    return tmp;
}

