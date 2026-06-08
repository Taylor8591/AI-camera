#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>

#include <sys/time.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "live555_h265_ring_unicast_server.h"

static constexpr int kSourcePollDelayUs = 1000;
static constexpr size_t kRingBufferCapacity = 64;

struct NALUnit {
    std::vector<uint8_t> data;  // includes Annex-B start code
    timeval pts{};              // 该 NALU 的时间戳
    int64_t enqueue_us = 0;     // 进入 ring buffer 的时刻
};

static int64_t now_monotonic_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

// 环形队列
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : cap_(capacity) {}

    // 入队
    void push(NALUnit&& u) {
        std::lock_guard<std::mutex> lk(mux);
        if (queue.size() >= cap_) queue.pop_front();    // 满了丢最旧
        queue.push_back(std::move(u));
    }
    // 出队
    bool pop(NALUnit& out) {
        std::lock_guard<std::mutex> lk(mux);
        if (queue.empty()) return false;
        out = std::move(queue.front());
        queue.pop_front();
        return true;
    }
private:
    std::mutex mux;
    std::deque<NALUnit> queue;      // 双端队列，头尾操作 O(1)
    size_t cap_;                    // 队列容量
};

// 识别 00 00 01 或 00 00 00 01，用于从 byte-stream 找 NALU 边界（Annex-B 的核心）
static bool findStartCode(const std::vector<uint8_t>& b, size_t from, size_t& pos, size_t& len) {
    for (size_t i = from; i + 3 < b.size(); ++i) {
        if (b[i] == 0x00 && b[i + 1] == 0x00) {
            if (b[i + 2] == 0x01) {
                // 找到 00 00 01
                pos = i; len = 3; return true;
            }
            if (i + 3 < b.size() && b[i + 2] == 0x00 && b[i + 3] == 0x01) {
                // 找到 00 00 00 01
                pos = i; len = 4; return true;
            }
        }
    }
    return false;
}

// Live555 自定义实时数据源
class LiveH265Source : public FramedSource {
public:
    static LiveH265Source* createNew(UsageEnvironment& env, RingBuffer& rb) {
        return new LiveH265Source(env, rb);
    }

protected:
    LiveH265Source(UsageEnvironment& env, RingBuffer& rb) : FramedSource(env), rb_(rb) {}
    ~LiveH265Source() override {
        if (pollTask_ != nullptr) envir().taskScheduler().unscheduleDelayedTask(pollTask_);
    }

private:
    static void pollTask(void* clientData) {
        static_cast<LiveH265Source*>(clientData)->deliver();
    }

    // Live555 核心钩子
    void doGetNextFrame() override { deliver(); }

    void deliver() {
        static int64_t rtsp_queue_total_us = 0;
        static int64_t rtsp_samples = 0;
        static int64_t last_log_us = 0;

        if (!isCurrentlyAwaitingData()) return;

        NALUnit u;
        // 没数据 → 5ms 后再试
        if (!rb_.pop(u)) {
            pollTask_ = envir().taskScheduler().scheduleDelayedTask(kSourcePollDelayUs, pollTask, this);
            return;
        }

        // 拷贝数据到 Live555 提供的缓冲区 fTo
        unsigned n = static_cast<unsigned>(u.data.size());
        if (n > fMaxSize) {
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = n - fMaxSize;
        } else {
            fFrameSize = n;
            fNumTruncatedBytes = 0;
        }
        std::memcpy(fTo, u.data.data(), fFrameSize);

        // 设置时间戳 PTS
        fPresentationTime = u.pts;

        if (u.enqueue_us > 0) {
            rtsp_queue_total_us += now_monotonic_us() - u.enqueue_us;
            rtsp_samples++;
        }
        if (last_log_us == 0) last_log_us = now_monotonic_us();
        if (rtsp_samples > 0 && now_monotonic_us() - last_log_us >= 1000000) {
            std::fprintf(stderr, "latency: rtsp_queue->send=%.2f ms samples=%lld\n",
                         (double)rtsp_queue_total_us / rtsp_samples / 1000.0,
                         (long long)rtsp_samples);
            rtsp_queue_total_us = 0;
            rtsp_samples = 0;
            last_log_us = now_monotonic_us();
        }

        // 告诉 Live555：帧已准备好，可以发走
        FramedSource::afterGetting(this);
    }

private:
    RingBuffer& rb_;
    TaskToken pollTask_ = nullptr;
};

// Live555 自定义媒体子会话，负责创建数据源和 RTP sink
class LiveH265Subsession : public OnDemandServerMediaSubsession {
public:
    // reuseFirstSource = True：多个客户端共享同一个实时源（实时流常用）
    static LiveH265Subsession* createNew(UsageEnvironment& env, RingBuffer& rb, Boolean reuseFirstSource = True) {
        return new LiveH265Subsession(env, rb, reuseFirstSource);
    }

protected:
    LiveH265Subsession(UsageEnvironment& env, RingBuffer& rb, Boolean reuseFirstSource)
        : OnDemandServerMediaSubsession(env, reuseFirstSource), rb_(rb) {}
    ~LiveH265Subsession() override = default;

    FramedSource* createNewStreamSource(unsigned, unsigned& estBitrate) override {
        estBitrate = 4000; // kbps
        FramedSource* raw = LiveH265Source::createNew(envir(), rb_);
        return H265VideoStreamDiscreteFramer::createNew(envir(), raw);
    }

    RTPSink* createNewRTPSink(Groupsock* gs, unsigned char pt, FramedSource*) override {
        OutPacketBuffer::maxSize = 2000000;
        return H265VideoRTPSink::createNew(envir(), gs, pt);
    }

private:
    RingBuffer& rb_;
};

// 生产者线程类，持续 fopen/fread 输入
class AnnexBProducer {
public:
    AnnexBProducer(RingBuffer& rb, std::string path) : rb_(rb), path_(std::move(path)) {}

    void start() {
        running_.store(true);
        th_ = std::thread(&AnnexBProducer::loop, this);
    }

    void stop() {
        running_.store(false);
        if (th_.joinable()) th_.join();
    }

private:
    void loop() {
        while (running_.load()) {
            FILE* fp = std::fopen(path_.c_str(), "rb");
            if (!fp) {
                std::perror("open input");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            std::fprintf(stderr, "[producer] opened: %s\n", path_.c_str());
            std::vector<uint8_t> cache;   // cache 用于累积从文件读到的数据
            cache.reserve(1 << 20);       // 预分配 1 MB，减少反复扩容
            uint8_t tmp[4096];            // 4KB 读取块

            while (running_.load()) {
                size_t n = std::fread(tmp, 1, sizeof(tmp), fp);     // 块读取
                if (n > 0) {
                    cache.insert(cache.end(), tmp, tmp + n);        // 写入缓存
                    parseNALUs(cache);                              // 实时切帧
                }
                if (n < sizeof(tmp)) {
                    if (std::feof(fp)) {
                        std::clearerr(fp);
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    } else if (std::ferror(fp)) {
                        std::perror("read input");
                        break;
                    }
                }
            }

            std::fclose(fp);
            std::fprintf(stderr, "[producer] closed, reopen...\n");
        }
    }

    void parseNALUs(std::vector<uint8_t>& b) {
        size_t firstPos = 0, firstLen = 0;
        if (!findStartCode(b, 0, firstPos, firstLen)) {
            if (b.size() > 8) b.erase(b.begin(), b.end() - 8);
            return;
        }

        // 把第一个起始码前的垃圾字节丢掉
        if (firstPos > 0) b.erase(b.begin(), b.begin() + firstPos);

        while (true) {
            if (b.size() < 8) return;

            size_t curPos = 0, curLen = 0;
            if (!findStartCode(b, 0, curPos, curLen)) return;
            if (curPos != 0) {
                b.erase(b.begin(), b.begin() + curPos);
                continue;
            }

            size_t nextPos = 0, nextLen = 0;
            if (!findStartCode(b, curLen, nextPos, nextLen)) {
                if (b.size() > (1 << 20)) b.erase(b.begin(), b.end() - 65536);
                return;
            }

            NALUnit u;
            // Discrete framer expects a discrete NAL unit payload (without Annex-B start code).
            u.data.assign(b.begin() + curLen, b.begin() + nextPos);
            // nullptr 表示不需要时区信息（现代代码通常都这么传）
            gettimeofday(&u.pts, nullptr);
            u.enqueue_us = now_monotonic_us();
            rb_.push(std::move(u));

            b.erase(b.begin(), b.begin() + nextPos);
        }
    }

private:
    RingBuffer& rb_;
    std::string path_;
    std::atomic<bool> running_{false};
    std::thread th_;
};

UsageEnvironment* env;
static volatile sig_atomic_t g_stopRequested = 0;
static EventLoopWatchVariable g_stopEventLoop(0);

static void handleSignal(int sig) {
    (void)sig;
    g_stopRequested = 1;
    g_stopEventLoop = 1;
}

int live555_server_entry(int argc, char** argv) {
    char const* input = (argc > 1) ? argv[1] : "/tmp/live.h265";
    char const* streamName = (argc > 2) ? argv[2] : "h265Ring";

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    RingBuffer ring(kRingBufferCapacity);
    AnnexBProducer producer(ring, input);

    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*scheduler);

    RTSPServer* rtspServer = RTSPServer::createNew(*env, 8554);
    if (!rtspServer) {
        *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
        return 1;
    }

    ServerMediaSession* sms = ServerMediaSession::createNew(
        *env, streamName, streamName,
        "Live H265 unicast (ring buffer + custom FramedSource)");
    sms->addSubsession(LiveH265Subsession::createNew(*env, ring, True));
    rtspServer->addServerMediaSession(sms);

    *env << "Input: " << input << "\n";
    char* url = rtspServer->rtspURL(sms);
    *env << "Play this stream using the URL \"" << url << "\"\n";
    delete[] url;

    producer.start();
    env->taskScheduler().doEventLoop(&g_stopEventLoop);
    producer.stop();
    return 0;
}
