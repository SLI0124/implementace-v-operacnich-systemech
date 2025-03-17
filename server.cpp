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
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <ctime>
#include <csignal>

#define PORT 8080
#define INDEX_PATH "www/index.html"
#define FILE_NOT_FOUND_PATH "www/error_404.html"
#define ERROR_503_PATH "www/error_503.html"
#define MAX_WORKERS 3
#define LOG_MSG_QUEUE_KEY 1234
#define UPLOAD_DIR "www/uploads"

struct LogMessage {
    long mtype;
    char message[512];
};

std::string get_timestamp() {
    std::time_t now = std::time(nullptr);
    char buf[100];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return std::string(buf);
}

void log_event(const std::string &message, int msg_queue_id, const std::string &client_ip = "", int client_port = 0) {
    LogMessage log_msg{};
    log_msg.mtype = 1;

    std::ostringstream log_stream;
    log_stream << "[" << get_timestamp() << "] [PID: " << getpid() << "]";
    if (!client_ip.empty()) {
        log_stream << " [Client: " << client_ip << ":" << client_port << "]";
    }
    log_stream << " " << message;

    strncpy(log_msg.message, log_stream.str().c_str(), sizeof(log_msg.message) - 1);
    log_msg.message[sizeof(log_msg.message) - 1] = '\0';
    printf("%s\n", log_msg.message);
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

std::string parse_http_request(const std::string &request) {
    std::istringstream request_stream(request);
    std::string method, path, protocol;
    request_stream >> method >> path >> protocol;
    return path;
}

std::string get_content_type(const std::string &path) {
    const std::string extension = path.substr(path.find_last_of('.') + 1);
    if (extension == "html" || extension == "htm") return "text/html";
    if (extension == "css") return "text/css";
    if (extension == "js") return "application/javascript";
    if (extension == "json") return "application/json";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "png") return "image/png";
    if (extension == "gif") return "image/gif";
    if (extension == "pdf") return "application/pdf";
    return "text/plain";
}

void handle_php_request(const std::string &php_path, std::string &response) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        char *args[] = {(char *) "/usr/bin/php", (char *) php_path.c_str(), nullptr};
        execvp("/usr/bin/php", args);
        perror("execvp");
        exit(EXIT_FAILURE);
    } else {
        close(pipefd[1]);
        char php_output[4096];
        ssize_t n = read(pipefd[0], php_output, sizeof(php_output));
        close(pipefd[0]);
        waitpid(pid, nullptr, 0);

        if (n > 0) {
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
            response.append(php_output, n);
        } else {
            response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
        }
    }
}

void handle_post_request(const std::string &body, const std::string &client_ip, int client_port, int msg_queue_id) {
    std::istringstream body_stream(body);
    std::string line;
    std::string boundary;

    // Extract boundary from Content-Type header
    while (std::getline(body_stream, line) && line != "\r") {
        if (line.find("Content-Type: multipart/form-data; boundary=") != std::string::npos) {
            boundary = line.substr(line.find("boundary=") + 9);
            boundary.pop_back(); // Remove trailing \r
        }
    }

    if (boundary.empty()) {
        log_event("Invalid POST request: Missing boundary", msg_queue_id, client_ip, client_port);
        return;
    }

    // Find the boundary in the body
    size_t pos = body.find("--" + boundary);
    if (pos == std::string::npos) {
        log_event("Boundary not found in POST request", msg_queue_id, client_ip, client_port);
        return;
    }

    // Find the filename in the Content-Disposition header
    pos = body.find("filename=\"", pos);
    if (pos == std::string::npos) {
        log_event("Filename not found in POST request", msg_queue_id, client_ip, client_port);
        return;
    }

    size_t filename_start = pos + 10; // "filename=\"" is 10 characters
    size_t filename_end = body.find('\"', filename_start);
    std::string filename = body.substr(filename_start, filename_end - filename_start);

    // Find the start of the file content
    pos = body.find("\r\n\r\n", filename_end);
    if (pos == std::string::npos) {
        log_event("File content not found in POST request", msg_queue_id, client_ip, client_port);
        return;
    }

    size_t file_content_start = pos + 4; // Skip "\r\n\r\n"
    size_t file_content_end = body.find("--" + boundary, file_content_start);
    std::string file_content = body.substr(file_content_start, file_content_end - file_content_start);

    // Debug: Print the file path
    std::string file_path = std::string(UPLOAD_DIR) + "/" + filename;
    std::cout << "Saving file to: " << file_path << std::endl;

    // Save the file
    std::ofstream out_file(file_path, std::ios::binary);
    if (!out_file) {
        log_event("Failed to create file on server: " + file_path, msg_queue_id, client_ip, client_port);
        return;
    }

    out_file.write(file_content.c_str(), file_content.size());
    out_file.close();

    log_event("File uploaded: " + filename, msg_queue_id, client_ip, client_port);
}

void handle_client(SSL *ssl, int msg_queue_id, const std::string &client_ip, int client_port) {
    char buffer[1024] = {0};
    const int bytes = SSL_read(ssl, buffer, sizeof(buffer));

    if (bytes <= 0) {
        const int err = SSL_get_error(ssl, bytes);
        if (err == SSL_ERROR_ZERO_RETURN) {
            log_event("Client closed the connection gracefully", msg_queue_id, client_ip, client_port);
        } else if (err == SSL_ERROR_SYSCALL || err == SSL_ERROR_SSL) {
            log_event("Client disconnected abruptly or SSL error", msg_queue_id, client_ip, client_port);
            ERR_print_errors_fp(stderr);
        } else {
            log_event("SSL read error", msg_queue_id, client_ip, client_port);
            ERR_print_errors_fp(stderr);
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        return;
    }

    const std::string request(buffer, bytes);

    // Extract Content-Length from the request headers
    size_t content_length = 0;
    size_t content_length_pos = request.find("Content-Length: ");
    if (content_length_pos != std::string::npos) {
        size_t content_length_end = request.find("\r\n", content_length_pos);
        std::string content_length_str = request.substr(content_length_pos + 16,
                                                        content_length_end - (content_length_pos + 16));
        content_length = std::stoul(content_length_str);
    }

    // Read the remaining request body if Content-Length is larger than the initial buffer
    std::string body;
    if (content_length > 0) {
        body = request;
        while (body.size() < content_length) {
            char additional_buffer[1024] = {0};
            int additional_bytes = SSL_read(ssl, additional_buffer, sizeof(additional_buffer));
            if (additional_bytes <= 0) {
                log_event("Failed to read request body", msg_queue_id, client_ip, client_port);
                SSL_shutdown(ssl);
                SSL_free(ssl);
                return;
            }
            body.append(additional_buffer, additional_bytes);
        }
    }

    // Handle the request
    const std::string method = request.substr(0, request.find(' '));
    const std::string path = parse_http_request(request);

    std::string response;
    if (method == "POST" && path == "/upload") {
        handle_post_request(body, client_ip, client_port, msg_queue_id);
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\nFile uploaded successfully.";
    } else {
        std::string file_path = "www" + path;
        if (file_path == "www/") {
            file_path = INDEX_PATH;
        }

        // Handle PHP files
        if (file_path.find(".php") != std::string::npos) {
            handle_php_request(file_path, response);
        }
        // Serve static files
        else {
            std::string content = read_file(file_path);
            if (!content.empty()) {
                std::string content_type = get_content_type(file_path);
                response = "HTTP/1.1 200 OK\r\nContent-Type: " + content_type +
                           "\r\nConnection: close\r\n\r\n" + content;
            } else {
                std::string content = read_file(FILE_NOT_FOUND_PATH);
                response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" + content;
            }
        }
    }

    if (SSL_write(ssl, response.c_str(), response.length()) <= 0) {
        log_event("SSL write error", msg_queue_id, client_ip, client_port);
        ERR_print_errors_fp(stderr);
    }

    log_event("Worker handled SSL client", msg_queue_id, client_ip, client_port);

    SSL_shutdown(ssl);
    SSL_free(ssl);
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
            getpeername(client_fd, (sockaddr *) &client_addr, &addrlen);

            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
            const int client_port = ntohs(client_addr.sin_port);

            log_event("Worker handling connection", msg_queue_id, client_ip, client_port);

            SSL *ssl = SSL_new(ctx);
            SSL_set_fd(ssl, client_fd);

            if (SSL_accept(ssl) <= 0) {
                log_event("SSL handshake failed", msg_queue_id, client_ip, client_port);
                ERR_print_errors_fp(stderr);
                SSL_shutdown(ssl);
                SSL_free(ssl);
                close(client_fd);
                continue;
            }

            handle_client(ssl, msg_queue_id, client_ip, client_port);
            close(client_fd);
        }
    }
}

int main() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    load_certificates(ctx, "server.crt", "server.key");

    const int msg_queue_id = msgget(LOG_MSG_QUEUE_KEY, IPC_CREAT | 0666);
    if (msg_queue_id == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }

    // Create upload directory if it doesn't exist
    if (!std::filesystem::exists(UPLOAD_DIR)) {
        std::filesystem::create_directory(UPLOAD_DIR);
    }

    // Start logger process
    pid_t logger_pid = fork();
    if (logger_pid == 0) {
        logger_process();
        exit(0);
    }

    // Create worker processes
    int worker_sockets[MAX_WORKERS][2];
    pid_t worker_pids[MAX_WORKERS];
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, worker_sockets[i]) == -1) {
            perror("socketpair");
            exit(EXIT_FAILURE);
        }
        if ((worker_pids[i] = fork()) == 0) {
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
    if (bind(server_fd, (sockaddr *) &address, sizeof(address)) == -1) {
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

        log_event("Parent handing off connection to worker " + std::to_string(round_robin), msg_queue_id);

        // Check if the worker process is still alive before sending the socket
        if (waitpid(worker_pids[round_robin], nullptr, WNOHANG) == 0) {
            // Worker is still alive, send the socket
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
                close(client_socket);
            }
            round_robin = (round_robin + 1) % MAX_WORKERS;
        } else {
            log_event("Worker " + std::to_string(round_robin) + " is no longer alive, restarting.", msg_queue_id);
            close(worker_sockets[round_robin][1]);
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, worker_sockets[round_robin]) == -1) {
                perror("socketpair");
                close(client_socket);
                continue;
            }
            if ((worker_pids[round_robin] = fork()) == 0) {
                close(worker_sockets[round_robin][1]); // Close the parent's end
                worker_process(worker_sockets[round_robin][0], msg_queue_id, ctx);
                exit(0);
            }
            close(worker_sockets[round_robin][0]); // Close the child's end
            close(client_socket);
        }
    }

    // Clean up
    close(server_fd);
    SSL_CTX_free(ctx);

    // Terminate worker processes
    for (int i = 0; i < MAX_WORKERS; i++) {
        close(worker_sockets[i][1]);
        waitpid(worker_pids[i], nullptr, 0);
    }

    // Terminate logger process
    kill(logger_pid, SIGTERM);
    waitpid(logger_pid, nullptr, 0);

    return 0;
}
