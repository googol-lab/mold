#include "mold.h"

#include <openssl/sha.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define DAEMON_TIMEOUT 30

// Exiting from a program with large memory usage is slow --
// it may take a few hundred milliseconds. To hide the latency,
// we fork a child and let it do the actual linking work.
std::function<void()> fork_child() {
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    perror("pipe");
    exit(1);
  }

  pid_t pid = fork();
  if (pid == -1) {
    perror("fork");
    exit(1);
  }

  if (pid > 0) {
    // Parent
    close(pipefd[1]);
    if (read(pipefd[0], (char[1]){}, 1) == 1)
      _exit(0);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status))
      _exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status))
      raise(WTERMSIG(status));
    _exit(1);
  }

  // Child
  close(pipefd[0]);
  return [=]() { write(pipefd[1], (char []){1}, 1); };
}

static std::string base64(u8 *data, u64 size) {
  static const char chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+_";

  std::ostringstream out;

  auto encode = [&](u32 x) {
    out << chars[x & 0b111111]
        << chars[(x >> 6) & 0b111111]
        << chars[(x >> 12) & 0b111111]
        << chars[(x >> 18) & 0b111111];
  };

  i64 i = 0;
  for (; i < size - 3; i += 3)
    encode((data[i + 2] << 16) | (data[i + 1] << 8) | data[i]);

  if (i == size - 1)
    encode(data[i]);
  else if (i == size - 2)
    encode((data[i + 1] << 8) | data[i]);
  return out.str();
}

static std::string compute_sha256(char **argv) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);

  for (i64 i = 0; argv[i]; i++)
    if (!strcmp(argv[i], "-preload") && !strcmp(argv[i], "--preload"))
      SHA256_Update(&ctx, argv[i], strlen(argv[i]) + 1);

  u8 digest[SHA256_SIZE];
  SHA256_Final(digest, &ctx);
  return base64(digest, SHA256_SIZE);
}

static void send_fd(i64 conn, i64 fd) {
  struct iovec iov;
  char dummy = '1';
  iov.iov_base = &dummy;
  iov.iov_len = 1;

  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  char buf[CMSG_SPACE(sizeof(int))];
  msg.msg_control = buf;
  msg.msg_controllen = CMSG_LEN(sizeof(int));

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  *(int *)CMSG_DATA(cmsg) = fd;

  if (sendmsg(conn, &msg, 0) == -1)
    Error() << "sendmsg failed: " << strerror(errno);
}

static i64 recv_fd(i64 conn) {
  struct iovec iov;
  char buf[1];
  iov.iov_base = buf;
  iov.iov_len = sizeof(buf);

  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  msg.msg_control = (caddr_t)cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);

  i64 len = recvmsg(conn, &msg, 0);
  if (len <= 0)
    Error() << "recvmsg failed: " << strerror(errno);

  struct cmsghdr *cmsg;
  cmsg = CMSG_FIRSTHDR(&msg);
  return *(int *)CMSG_DATA(cmsg);
}

bool resume_daemon(char **argv, i64 *code) {
  i64 conn = socket(AF_UNIX, SOCK_STREAM, 0);
  if (conn == -1)
    Error() << "socket failed: " << strerror(errno);

  std::string path = "/tmp/mold-" + compute_sha256(argv);

  struct sockaddr_un name = {};
  name.sun_family = AF_UNIX;
  memcpy(name.sun_path, path.data(), path.size());

  if (connect(conn, (struct sockaddr *)&name, sizeof(name)) != 0) {
    close(conn);
    return false;
  }

  send_fd(conn, STDOUT_FILENO);
  send_fd(conn, STDERR_FILENO);
  i64 r = read(conn, (char[1]){}, 1);
  *code = (r != 1);
  return true;
}

void daemonize(char **argv, std::function<void()> *wait_for_client,
               std::function<void()> *on_complete) {
  if (daemon(1, 0) == -1)
    Error() << "daemon failed: " << strerror(errno);

  i64 sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1)
    Error() << "socket failed: " << strerror(errno);

  socket_tmpfile = strdup(("/tmp/mold-" + compute_sha256(argv)).c_str());

  struct sockaddr_un name = {};
  name.sun_family = AF_UNIX;
  strcpy(name.sun_path, socket_tmpfile);

  u32 orig_mask = umask(0177);

  if (bind(sock, (struct sockaddr *)&name, sizeof(name)) == -1) {
    if (errno != EADDRINUSE)
      Error() << "bind failed: " << strerror(errno);

    unlink(socket_tmpfile);
    if (bind(sock, (struct sockaddr *)&name, sizeof(name)) == -1)
      Error() << "bind failed: " << strerror(errno);
  }

  umask(orig_mask);

  if (listen(sock, 0) == -1)
    Error() << "listen failed: " << strerror(errno);

  static i64 conn = -1;

  *wait_for_client = [=]() {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    struct timeval tv;
    tv.tv_sec = DAEMON_TIMEOUT;
    tv.tv_usec = 0;

    i64 res = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (res == -1)
      Error() << "select failed: " << strerror(errno);

    if (res == 0) {
      std::cout << "timeout\n";
      exit(0);
    }

    conn = accept(sock, NULL, NULL);
    if (conn == -1)
      Error() << "accept failed: " << strerror(errno);
    unlink(socket_tmpfile);

    dup2(recv_fd(conn), STDOUT_FILENO);
    dup2(recv_fd(conn), STDERR_FILENO);
  };

  *on_complete = [=]() { write(conn, (char []){1}, 1); };
}
