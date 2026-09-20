#include "process_utils.h"

#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace process {

std::string run_capture(const std::vector<std::string> &args) {
    if(args.empty()) return "";
    int fds[2];
    if(pipe(fds) != 0) return "";
    pid_t pid = fork();
    if(pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for(const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[1024];
    ssize_t n = 0;
    while((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fds[0]);
    if(pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
    return out;
}

bool command_exists(const char *name) {
    const char *path = getenv("PATH");
    if(!path || !*path) return access(name, X_OK) == 0;
    std::string paths = path;
    size_t pos = 0;
    while(pos <= paths.size()) {
        size_t end = paths.find(':', pos);
        std::string dir = end == std::string::npos ? paths.substr(pos) : paths.substr(pos, end - pos);
        if(dir.empty()) dir = ".";
        if(access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        if(end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

}
