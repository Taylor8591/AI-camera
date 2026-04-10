#ifndef JCDATADEFINE_H
#define JCDATADEFINE_H
#include <QList>
#include <QMutex>

template <class T>
class JCTSQueue: public QList<T>
{
public:
    JCTSQueue() {

    }

    ~JCTSQueue() {
        
    }

    void enqueue(const T &t) {

        m_mutex.lock();
        QList<T>::append(t);
        m_mutex.unlock();
    }

    T dequeue() {

        m_mutex.lock();
        T t = NULL;
        if(!QList<T>::isEmpty()) {
            t = QList<T>::takeFirst();
        }
        m_mutex.unlock();
        return t;
    }

    bool isEmpty() {

        m_mutex.lock();
        bool ret = QList<T>::isEmpty();
        m_mutex.unlock();
        return ret;
    }

private:
    QMutex m_mutex;
};


#endif // JCDATADEFINE_H
