#include "affinitygraph/core.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "usage: affinityctl status|pause|resume|dump [--socket PATH]\n";
    return 2;
  }
  std::string command = argv[1];
  if (command != "status" && command != "pause" && command != "resume" && command != "dump") {
    std::cerr << "affinityctl: unknown command\n";
    return 2;
  }
  std::string path = "/tmp/affinitygraph.sock";
  if (argc == 4 && std::string(argv[2]) == "--socket") path = argv[3];
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) { perror("socket"); return 1; }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) { std::cerr << "affinityctl: socket path too long\n"; return 2; }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) { perror("connect"); return 1; }
  command.push_back('\n');
  if (write(fd, command.data(), command.size()) < 0) { perror("write"); return 1; }
  char buffer[4096];
  ssize_t length;
  while ((length = read(fd, buffer, sizeof(buffer))) > 0) std::cout.write(buffer, length);
  close(fd);
  return length < 0 ? 1 : 0;
}

