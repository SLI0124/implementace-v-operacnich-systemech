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

#define PORT 8080
#define INDEX_PATH "www/index.html"
#define FILE_NOT_FOUND_PATH "www/error_404.html"

void log(const std::string &message) {
    std::filesystem::create_directories("logs");
    std::ofstream log_file("logs/log.txt", std::ios::app);
    const time_t now = time(nullptr);
    const struct tm *localTime = localtime(&now);

    if (!log_file) {
        std::cerr << "Error creating log file!" << std::endl;
    } else {
        log_file << "[" << std::setw(2) << std::setfill('0') << localTime->tm_hour << ":"
                << std::setw(2) << std::setfill('0') << localTime->tm_min << ":"
                << std::setw(2) << std::setfill('0') << localTime->tm_sec << "] "
                << message << std::endl;
    }
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
};

HttpRequest parse_request(std::istringstream &stream) {
    HttpRequest http_request;
    std::string line;

    std::getline(stream, line);
    std::istringstream line_stream(line);
    line_stream >> http_request.method >> http_request.path >> http_request.version;

    while (std::getline(stream, line) && !line.empty()) {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            value.erase(0, value.find_first_not_of(" \t")); // Trim leading whitespace
            http_request.headers[key] = value;
        }
    }
    return http_request;
}

std::string build_response(const int status_code, const std::string &body, const std::string &mime_type) {
    std::string status;
    switch (status_code) {
        case 200: status = "200 OK";
            break;
        case 404: status = "404 Not Found";
            break;
        case 500: status = "500 Internal Server Error";
            break;
        default: status = "200 OK";
            break;
    }
    return "HTTP/1.1 " + status + "\r\n" +
           "Content-Type: " + mime_type + "\r\n" +
           "Connection: close\r\n\r\n" + body;
}

std::string load_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not open file: " + path);

    std::string content;
    char buffer[4096];

    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        content.append(buffer, file.gcount());
    }

    return content;
}

bool file_exists(const std::string &path) {
    std::ifstream file(path);
    return file.is_open();
}

std::unordered_map<std::string, std::string> mime_types = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"}
};

std::string get_mime_type(const std::string &file_path) {
    const size_t pos = file_path.find_last_of('.');
    if (pos != std::string::npos) {
        const std::string ext = file_path.substr(pos);
        if (mime_types.count(ext)) {
            return mime_types[ext];
        }
    }
    return "text/html";
}

void handle_client(SSL *ssl) {
    char buffer[1024] = {0};
    SSL_read(ssl, buffer, sizeof(buffer));

    std::istringstream request_stream(buffer);
    const HttpRequest http_request = parse_request(request_stream);

    const std::string file_path = "www" + http_request.path;
    const std::string mime_type = get_mime_type(file_path);
    std::string body;

    if (http_request.path == "/") {
        body = load_file(INDEX_PATH);
        log("Index file requested");
    } else if (file_exists(file_path)) {
        body = load_file(file_path);
        log("File found: " + file_path);
    } else {
        body = load_file(FILE_NOT_FOUND_PATH);
        log("File not found: " + http_request.path);
    }

    const std::string response = build_response(200, body, mime_type);
    SSL_write(ssl, response.c_str(), response.length());
    log("Response sent");
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

int main() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
    if (!ctx) {
        perror("Unable to create SSL context");
        exit(EXIT_FAILURE);
    }

    load_certificates(ctx, "server.crt", "server.key");

    int server_fd, new_socket;
    struct sockaddr_in address{};
    const int opt = 1;
    socklen_t addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *) &address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    while (true) {
        new_socket = accept(server_fd, (struct sockaddr *) &address, &addrlen);
        if (new_socket < 0) {
            perror("accept");
            continue;
        }

        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, new_socket);
        if (SSL_accept(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
        } else {
            handle_client(ssl);
        }
        close(new_socket);
    }

    SSL_CTX_free(ctx);
    close(server_fd);
    return 0;
}
