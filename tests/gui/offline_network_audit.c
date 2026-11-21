/* Development-only network audit. Never linked into or shipped with the app.
 * LD_PRELOAD records and denies Internet socket attempts; Unix-domain IPC
 * remains available to GTK, the display server and accessibility services.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static void record_attempt(const char *operation, const char *details)
{
    const char *path = getenv("SLA_NETWORK_AUDIT_FILE");
    if (!path || !*path) return;
    int file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (file < 0) return;
    char line[512];
    int length = snprintf(line, sizeof(line), "pid=%ld %s %s\n", (long)getpid(), operation, details);
    if (length > 0) write(file, line, (size_t)length < sizeof(line) ? (size_t)length : sizeof(line)-1);
    close(file);
}
int socket(int domain, int type, int protocol)
{
    if (domain == AF_INET || domain == AF_INET6) {
        record_attempt("socket", domain == AF_INET ? "AF_INET" : "AF_INET6");
        errno = EACCES; return -1;
    }
    int (*original)(int,int,int) = dlsym(RTLD_NEXT, "socket");
    return original(domain,type,protocol);
}
int connect(int file, const struct sockaddr *address, socklen_t length)
{
    if (address && (address->sa_family == AF_INET || address->sa_family == AF_INET6)) {
        record_attempt("connect", address->sa_family == AF_INET ? "AF_INET" : "AF_INET6");
        errno = EACCES; return -1;
    }
    int (*original)(int,const struct sockaddr *,socklen_t) = dlsym(RTLD_NEXT,"connect");
    return original(file,address,length);
}
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
    if (node) {
        record_attempt("getaddrinfo",node);
        return EAI_NONAME;
    }
    int (*original)(const char *,const char *,const struct addrinfo *,struct addrinfo **) = dlsym(RTLD_NEXT,"getaddrinfo");
    return original(node,service,hints,result);
}
