#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fifo_h265_to_mp4.h"
#include "live555_h265_ring_unicast_server.h"
#include "v4l2_preview_sdl.h"

static pid_t capture_pid = -1;
static pid_t server_pid = -1;
static pid_t tomp4_pid = -1;

// 静态函数：仅当前文件可见
static int create_FIFO(const char *path) {
    struct stat st;
    // 文件存在时，检查是否为 FIFO
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            std::fprintf(stderr, "path exists but is not FIFO: %s\n", path);
            return -1;
        }
        return 0;
    }
    // 文件不存在时，创建 FIFO
    if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkfifo(%s) failed: %s\n", path, std::strerror(errno));
        return -1;
    }
    return 0;
}

// 静态函数：仅当前文件可见
static void forward_signal(int sig) {
  // PID > 0 表示进程有效存在
    if (capture_pid > 0) kill(capture_pid, sig);
    if (server_pid > 0) kill(server_pid, sig);
    if (tomp4_pid > 0) kill(tomp4_pid, sig);
}

static int wait_and_report(pid_t pid, const char *name) {
    int status = 0;
    pid_t ret = waitpid(pid, &status, 0);
    if (ret < 0) {
        std::fprintf(stderr, "waitpid(%s) failed: %s\n", name, std::strerror(errno));
        return 1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        std::fprintf(stderr, "%s exited with code %d\n", name, code);
        return code;
    }

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        std::fprintf(stderr, "%s killed by signal %d\n", name, sig);
        return 128 + sig;
    }

    return 1;
}

int main(int argc, char **argv) {
    const char *device = (argc > 1) ? argv[1] : "/dev/video11";
    const char *width = (argc > 2) ? argv[2] : "3264";
    const char *height = (argc > 3) ? argv[3] : "2448";
    const char *fifo_rtsp = (argc > 4) ? argv[4] : "/tmp/live.h265";
    const char *stream_name = (argc > 5) ? argv[5] : "h265Ring";
    const char *fifo_mp4 = (argc > 6) ? argv[6] : "/tmp/live_mp4.h265";
    const char *output_mp4 = (argc > 7) ? argv[7] : "/tmp/live_output.mp4";

    signal(SIGINT, forward_signal);     // 注册 Ctrl+C 信号处理
    signal(SIGTERM, forward_signal);    // 注册终止信号处理

    if (create_FIFO(fifo_rtsp) < 0) return 1;
    if (create_FIFO(fifo_mp4) < 0) return 1;

    // 子进程1：摄像头采集和编码
    capture_pid = fork();
    if (capture_pid < 0) {
        std::perror("fork capture failed");
        return 1;
    } else if (capture_pid == 0) {
        char *capture_argv[] = {
            const_cast<char *>("v4l2_preview_sdl"),
            const_cast<char *>(device),
            const_cast<char *>(width),
            const_cast<char *>(height),
            const_cast<char *>(fifo_rtsp),
            const_cast<char *>(fifo_mp4),
            nullptr,
        };
        int result = v4l2_preview_sdl_entry(6, capture_argv);
        std::exit(result);
    }

    // 子进程2：RTSP 服务器
    server_pid = fork();
    if (server_pid < 0) {
        std::perror("fork server failed");
        kill(capture_pid, SIGTERM);
        waitpid(capture_pid, nullptr, 0);
        return 1;
    } else if (server_pid == 0) {
        char *server_argv[] = {
            const_cast<char *>("live555_h265_ring_unicast_server"),
            const_cast<char *>(fifo_rtsp),
            const_cast<char *>(stream_name),
            nullptr,
        };
        int result = live555_server_entry(3, server_argv);
        std::exit(result);
    }

    // 子进程3：MP4 转封装
    tomp4_pid = fork();
    if (tomp4_pid < 0) {
        std::perror("fork mp4 muxer failed");
        forward_signal(SIGTERM);
        waitpid(capture_pid, nullptr, 0);
        waitpid(server_pid, nullptr, 0);
        return 1;
    } else if (tomp4_pid == 0) {
        char *mp4_argv[] = {
            const_cast<char *>("fifo_h265_to_mp4"),
            const_cast<char *>(fifo_mp4),
            const_cast<char *>(output_mp4),
            const_cast<char *>("15"),
            nullptr,
        };
        int result = mp4_muxer_entry(4, mp4_argv);
        std::exit(result);
    }

    std::fprintf(stderr,
        "launcher started capture pid=%d server pid=%d mp4 pid=%d rtsp_fifo=%s mp4_fifo=%s stream=%s mp4=%s\n",
        capture_pid, server_pid, tomp4_pid,
        fifo_rtsp, fifo_mp4, stream_name, output_mp4);

    int first_status = 0;
    // -1：等待任一子进程退出，并获取其状态
    pid_t first_done = waitpid(-1, &first_status, 0);
    if (first_done < 0) {
        std::perror("waitpid failed");
        forward_signal(SIGTERM);
        return 1;
    }

    if (first_done == capture_pid) {
        std::fprintf(stderr, "capture process exited first, stopping server/mp4\n");
        if (server_pid > 0) kill(server_pid, SIGTERM);
        if (tomp4_pid > 0) kill(tomp4_pid, SIGTERM);
        wait_and_report(server_pid, "server");
        wait_and_report(tomp4_pid, "mp4");
    } else if (first_done == server_pid) {
        std::fprintf(stderr, "server process exited first, stopping capture/mp4\n");
        if (capture_pid > 0) kill(capture_pid, SIGTERM);
        if (tomp4_pid > 0) kill(tomp4_pid, SIGTERM);
        wait_and_report(capture_pid, "capture");
        wait_and_report(tomp4_pid, "mp4");
    } else if (first_done == tomp4_pid) {
        std::fprintf(stderr, "mp4 process exited first, stopping capture/server\n");
        if (capture_pid > 0) kill(capture_pid, SIGTERM);
        if (server_pid > 0) kill(server_pid, SIGTERM);
        wait_and_report(capture_pid, "capture");
        wait_and_report(server_pid, "server");
    }

    if (WIFEXITED(first_status)) return WEXITSTATUS(first_status);
    if (WIFSIGNALED(first_status)) return 128 + WTERMSIG(first_status);
    return 1;
}
