#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>

#include <sys/time.h>

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

struct NALUnit {
  // std::vector 是 C++ 标准库中最常用的动态数组容器，它在内存中连续存储元素，支持随机访问和动态调整大小，非常适合存储 NALU 数据
  // 每个元素是 1 字节的无符号整数，常用于存储二进制数据
  // vector 拷贝很昂贵，所以 push 时用右值引用和 std::move 来避免不必要的复制
  // std::vector<uint8_t> a = {1,2,3,4,5};
  // std::vector<uint8_t> b = a;              // 深拷贝：分配新内存，复制5个字节
  // std::vector<uint8_t> b = std::move(a);   // 移动：只交换三个指针（指向堆内存、大小、容量）,现在 a 为空（合法但未指定状态），b 接管了原来的数组
  std::vector<uint8_t> data;  // includes Annex-B start code
  // u.pts 类型是 timeval，包含：
  // tv_sec：秒
  // tv_usec：微秒
  timeval pts{};              // 该 NALU 的时间戳
};

// 环形队列
// 内部 std::deque<NALUnit> + std::mutex
class RingBuffer {
public:
  explicit RingBuffer(size_t capacity) : cap_(capacity) {}

  // 入队，满时丢最旧（低延迟优先）
  // 右值引用，临时对象、std::move 转换后的对象、函数返回的局部对象等都可以直接传进来，避免不必要的复制
  void push(NALUnit&& u) {
    std::lock_guard<std::mutex> lk(mux);
    if (queue.size() >= cap_) queue.pop_front(); // drop oldest for low latency
    queue.push_back(std::move(u));
  }
  // 出队，无数据返回 false
  bool pop(NALUnit& out) {
    std::lock_guard<std::mutex> lk(mux);
    if (queue.empty()) return false;
    out = std::move(queue.front());
    queue.pop_front();
    return true;
  }

private:
  std::mutex mux;
  // 双端队列
  std::deque<NALUnit> queue;
  size_t cap_;
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

// 自定义 source
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

  // live555 需要一帧时会调用
  void doGetNextFrame() override { deliver(); }

  void deliver() {
    if (!isCurrentlyAwaitingData()) return;

    NALUnit u;
    if (!rb_.pop(u)) {
      pollTask_ = envir().taskScheduler().scheduleDelayedTask(5000, pollTask, this); // 5ms
      return;
    }

    unsigned n = static_cast<unsigned>(u.data.size());
    if (n > fMaxSize) {
      fFrameSize = fMaxSize;
      fNumTruncatedBytes = n - fMaxSize;
    } else {
      fFrameSize = n;
      fNumTruncatedBytes = 0;
    }

    std::memcpy(fTo, u.data.data(), fFrameSize);
    fPresentationTime = u.pts;
    FramedSource::afterGetting(this);
  }

private:
  RingBuffer& rb_;
  TaskToken pollTask_ = nullptr;
};

// 实时 source 挂到 RTSP 会话里
// 客户端发 SETUP/PLAY 时，live555 会来这个类要 source 和 sink
// OnDemandServerMediaSubsession 是 live555 里“按需给客户端创建媒体发送链路”的基类
// 常要重写两个函数
// createNewStreamSource(...)：数据从哪里来（文件/摄像头/队列）
// createNewRTPSink(...)：怎么打 RTP 包（H264/H265/音频等）
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
    // 把 raw source 变成 H265 sink 可接受的“离散 H265 输入
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
// 把读到的数据放进 cache，调用 parseNALUs(cache)
// parseNALUs 从缓存中找“当前起始码 -> 下一个起始码”，切出一个完整 NALU
// 每个 NALU 入队前 gettimeofday 打 pts
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
      uint8_t tmp[4096];            // 临时读缓冲区

      while (running_.load()) {
        // 每个数据元素的大小为 1 字节，尝试读 4096 个元素
        size_t n = std::fread(tmp, 1, sizeof(tmp), fp);
        if (n > 0) {
          // cache.end() 表示向量末尾的“迭代器位置”——也就是最后一个元素之后的位置
          cache.insert(cache.end(), tmp, tmp + n);
          parseNALUs(cache);
        }

        if (n < sizeof(tmp)) {
          if (std::feof(fp)) {
            std::clearerr(fp); // FIFO: wait for more bytes
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
      // 找不到时，保留最后 8 字节（可能是半个起始码），其余丢掉再等新数据
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

int main(int argc, char** argv) {
  char const* input = (argc > 1) ? argv[1] : "/tmp/live.h265";
  char const* streamName = (argc > 2) ? argv[2] : "h265Ring";

  RingBuffer ring(512);
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
  env->taskScheduler().doEventLoop();
  producer.stop();
  return 0;
}
