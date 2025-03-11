#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <iomanip>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <sys/un.h>
#include <arpa/inet.h>

#define PORT 8080
#define INDEX_PATH "www/index.html"
#define FILE_NOT_FOUND_PATH "www/error_404.html"
#define ERROR_503_PATH "www/error_503.html"
#define MAX_WORKERS 3
#define LOG_MSG_QUEUE_KEY 1234

struct LogMessage {
    long mtype;
    char message[256];
};

void log_event(const std::string &message, int msg_queue_id) {
    LogMessage log_msg{};
    log_msg.mtype = 1;
    strncpy(log_msg.message, message.c_str(), sizeof(log_msg.message) - 1);
    log_msg.message[sizeof(log_msg.message) - 1] = '\0';
    msgsnd(msg_queue_id, &log_msg, sizeof(log_msg.message), 0);
}

void logger_process() {
    const int msg_queue_id = msgget(LOG_MSG_QUEUE_KEY, IPC_CREAT | 0666);
    if (msg_queue_id == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }
    std::ofstream log_file("logs/log.txt", std::ios::app);
    if (!log_file) {
        std::cerr << "Failed to open log file." << std::endl;
        exit(EXIT_FAILURE);
    }
    LogMessage log_msg{};
    while (true) {
        if (msgrcv(msg_queue_id, &log_msg, sizeof(log_msg.message), 0, 0) == -1) {
            perror("msgrcv");
            continue;
        }
        log_file << log_msg.message << std::endl;
    }
}

std::string read_file(const std::string &file_path) {
    std::ifstream file(file_path);
    if (!file) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void handle_client(SSL *ssl, int msg_queue_id) {
    char buffer[1024] = {0};
    SSL_read(ssl, buffer, sizeof(buffer));

    std::string response;
    std::string content = read_file(INDEX_PATH);
    if (content.empty()) {
        content = read_file(FILE_NOT_FOUND_PATH);
        response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" + content;
    } else {
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" + content;
    }
    SSL_write(ssl, response.c_str(), response.length());

    log_event("Worker handled SSL client", msg_queue_id);
}

void load_certificates(SSL_CTX *ctx, const std::string &cert_file, const std::string &key_file) {
    if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        std::cerr << "Private key does not match the public certificate." << std::endl;
        exit(EXIT_FAILURE);
    }
}

void worker_process(const int sock_fd, const int msg_queue_id, SSL_CTX *ctx) {
    while (true) {
        int client_fd;
        struct msghdr msg = {};
        char buf[CMSG_SPACE(sizeof(int))];
        struct iovec io = {.iov_base = &client_fd, .iov_len = sizeof(client_fd)};
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);
        if (recvmsg(sock_fd, &msg, 0) == -1) {
            perror("recvmsg");
            continue;
        }
        const struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int)) && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type ==
            SCM_RIGHTS) {
            memcpy(&client_fd, CMSG_DATA(cmsg), sizeof(client_fd));

            struct sockaddr_in client_addr{};
            socklen_t addrlen = sizeof(client_addr);
            getpeername(client_fd, (struct sockaddr *) &client_addr, &addrlen);

            // Print worker info
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
            const int client_port = ntohs(client_addr.sin_port);
            std::cout << "Worker " << getpid() << " handling connection from " << client_ip << ":" << client_port <<
                    std::endl;

            SSL *ssl = SSL_new(ctx);
            SSL_set_fd(ssl, client_fd);
            if (SSL_accept(ssl) <= 0) {
                ERR_print_errors_fp(stderr);
            } else {
                handle_client(ssl, msg_queue_id);
            }
            SSL_shutdown(ssl);
            SSL_free(ssl);
            close(client_fd);
        }
    }
}

int main() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
    load_certificates(ctx, "server.crt", "server.key");

    const int msg_queue_id = msgget(LOG_MSG_QUEUE_KEY, IPC_CREAT | 0666);
    if (msg_queue_id == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }

    // Start logger process
    if (fork() == 0) {
        logger_process();
        exit(0);
    }

    // Create worker processes
    int worker_sockets[MAX_WORKERS][2];
    for (int i = 0; i < MAX_WORKERS; ++i) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, worker_sockets[i]) == -1) {
            perror("socketpair");
            exit(EXIT_FAILURE);
        }
        if (fork() == 0) {
            close(worker_sockets[i][1]); // Close the parent's end
            worker_process(worker_sockets[i][0], msg_queue_id, ctx);
            exit(0);
        }
        close(worker_sockets[i][0]); // Close the child's end
    }

    struct sockaddr_in address{};
    socklen_t addrlen = sizeof(address);
    const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *) &address, sizeof(address)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // main loop to accept incoming connections
    int round_robin = 0;
    while (true) {
        int client_socket = accept(server_fd, (struct sockaddr *) &address, &addrlen);
        if (client_socket == -1) {
            perror("accept");
            continue;
        }

        std::cout << "Parent handing off connection to worker " << round_robin << std::endl;

        struct msghdr msg = {};
        char buf[CMSG_SPACE(sizeof(int))];
        struct iovec io = {.iov_base = &client_socket, .iov_len = sizeof(client_socket)};
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(cmsg), &client_socket, sizeof(client_socket));
        if (sendmsg(worker_sockets[round_robin][1], &msg, 0) == -1) {
            perror("sendmsg");
        }
        round_robin = (round_robin + 1) % MAX_WORKERS;
        close(client_socket);
    }

    SSL_CTX_free(ctx);
    return 0;
}
