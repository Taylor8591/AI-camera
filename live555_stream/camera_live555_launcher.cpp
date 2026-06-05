#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int v4l2_preview_sdl_entry(int argc, char *argv[]);
extern int live555_server_entry(int argc, char **argv);

static pid_t g_capture_pid = -1;
static pid_t g_server_pid = -1;

static void forward_signal(int sig) {
  if (g_capture_pid > 0) kill(g_capture_pid, sig);
  if (g_server_pid > 0) kill(g_server_pid, sig);
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
  const char *fifo = (argc > 4) ? argv[4] : "/tmp/live.h265";
  const char *stream_name = (argc > 5) ? argv[5] : "h265Ring";

  signal(SIGINT, forward_signal);
  signal(SIGTERM, forward_signal);

  g_capture_pid = fork();
  if (g_capture_pid < 0) {
    std::perror("fork capture failed");
    return 1;
  }

  if (g_capture_pid == 0) {
    char *capture_argv[] = {
        const_cast<char *>("v4l2_preview_sdl"),
        const_cast<char *>(device),
        const_cast<char *>(width),
        const_cast<char *>(height),
        nullptr,
    };
    std::exit(v4l2_preview_sdl_entry(4, capture_argv));
  }

  g_server_pid = fork();
  if (g_server_pid < 0) {
    std::perror("fork server failed");
    kill(g_capture_pid, SIGTERM);
    waitpid(g_capture_pid, nullptr, 0);
    return 1;
  }

  if (g_server_pid == 0) {
    char *server_argv[] = {
        const_cast<char *>("live555_h265_ring_unicast_server"),
        const_cast<char *>(fifo),
        const_cast<char *>(stream_name),
        nullptr,
    };
    std::exit(live555_server_entry(3, server_argv));
  }

  std::fprintf(stderr,
               "launcher started capture pid=%d server pid=%d fifo=%s stream=%s\n",
               g_capture_pid, g_server_pid, fifo, stream_name);

  int first_status = 0;
  pid_t first_done = waitpid(-1, &first_status, 0);
  if (first_done < 0) {
    std::perror("waitpid failed");
    forward_signal(SIGTERM);
    return 1;
  }

  if (first_done == g_capture_pid) {
    std::fprintf(stderr, "capture process exited first, stopping server\n");
    if (g_server_pid > 0) kill(g_server_pid, SIGTERM);
    wait_and_report(g_server_pid, "server");
  } else if (first_done == g_server_pid) {
    std::fprintf(stderr, "server process exited first, stopping capture\n");
    if (g_capture_pid > 0) kill(g_capture_pid, SIGTERM);
    wait_and_report(g_capture_pid, "capture");
  }

  if (WIFEXITED(first_status)) return WEXITSTATUS(first_status);
  if (WIFSIGNALED(first_status)) return 128 + WTERMSIG(first_status);
  return 1;
}
