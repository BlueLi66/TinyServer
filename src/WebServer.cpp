#include "../include/WebServer.h"
#include "../include/Logger.h"
// 1. Constructor and Destructor (Resource Management)
WebServer::WebServer(int port) :port_(port), server_fd_(-1), epoll_fd_(-1), is_running_(false), last_tick_(std::chrono::steady_clock::now()) {}

WebServer::~WebServer() {
    if (server_fd_ != -1) {
        close(server_fd_);
        std::cout << "[System] Server Socket Closed." << std::endl;
    }
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
        std::cout << "[System] Epoll Radar Closed." << std::endl;
    }
}

// 2. Utility: Set a socket to non-blocking mode
void WebServer::SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return ;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 3. Initialization: Socket, Bind, Listen, and Epoll Setup
void WebServer::Init() {
    clients_.resize(65536);
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == -1) {
        throw std::runtime_error("Socket creation failed");
    }

    int opt = 1;
    if ( setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1 ) {
        throw std::runtime_error("Failed to set SO_REUSEADDR");
    }
    

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)& address, sizeof(address)) < 0) {
        throw std::runtime_error("Bind failed! Port might be busy.");
    }
    SetNonBlocking(server_fd_);
    if (listen(server_fd_, SOMAXCONN) < 0) {
        throw std::runtime_error("Listen failed.");
    }

    // Initialize the Epoll Radar
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
        throw std::runtime_error("Epoll creation failed.");
    }

    // Add the Welcome Girl (serve_fd_) to the Radar
    struct epoll_event event;
    event.data.fd = server_fd_;
    event.events = EPOLLIN; // Listen for incoming connections
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &event);

    thread_pool_ = std::make_unique<ThreadPool>(16);
    LOG_INFO("服务器初始化完成，端口 " + std::to_string(port_) + "，线程池 8 线程");
    std::cout << "[System] Server Initialized on port " << port_ << " with Epoll & ThreadPool." << std::endl;
}

void WebServer::CloseConn(int client_fd) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (client_fd < 0 || client_fd >= clients_.size()) {
        return;
    }
    if (clients_[client_fd].fd != -1) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
        close(client_fd);
        clients_[client_fd].fd = -1;
        clients_[client_fd].Reset();
        LOG_INFO("连接关闭 fd=" + std::to_string(client_fd));
        std::cout << "[System] Client FD " << client_fd << " safely cleared by CloseConn.\n" << std::endl;
    }
}

// 4. The Infinate Event Loop (Single Threaded)
void WebServer::Start() {
    is_running_ = true;
    LOG_INFO("服务器启动，开始监听事件...");
    std::cout << "[System] Server Started! Waiting for events in SINGLE THREAD..." << std::endl;

    while (is_running_) {
        // BLOCK until the radar detects activity
        int event_count = epoll_wait(epoll_fd_, events_, MAX_EVENTS, 5000);
        
        if (event_count == -1 && errno != EINTR) {
            LOG_ERROR("epoll_wait 出错 errno=" + std::to_string(errno));
            std::cerr << "[System] Epoll wait error" << std::endl;
        }

        for (int i = 0; i < event_count; ++i) {
            int current_fd = events_[i].data.fd;
            // Branch A: New connection arrives
            if (current_fd == server_fd_) {
                {
                    std::lock_guard<std::recursive_mutex> lock(mtx_);
                    while (true) {
                        struct sockaddr_in client_address;
                        socklen_t client_len = sizeof(client_address);
                        int client_fd = accept(server_fd_, (struct sockaddr*)& client_address, &client_len);

                        if (client_fd <= 0) {
                            break;
                        }

                        if (client_fd >= clients_.size()) {
                            close(client_fd);
                            continue;
                        }

                        SetNonBlocking(client_fd);
                        struct epoll_event client_event;
                        client_event.data.fd = client_fd;
                        client_event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &client_event);

                        clients_[client_fd].Reset();
                        clients_[client_fd].fd = client_fd;
                        LOG_INFO("新连接 fd=" + std::to_string(client_fd));
                        timer_.add(client_fd, 10000);
                    }
                }     
            }
            // Branch B: Existing client sends data
            else {
                if (events_[i].events & EPOLLOUT) {
                    thread_pool_->AddTask([this, current_fd](){
                        HandleWrite(current_fd);
                    });
                } else {
                    thread_pool_->AddTask([this, current_fd](){
                        HandleClient(current_fd);
                    });
                }
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_).count() >= 500) {
            std::lock_guard<std::recursive_mutex> lock(mtx_);
            timer_.tick( [this](int current_fd) { CloseConn(current_fd); } );
            last_tick_ = now;
        }
        
        
        
    }
}



// 5. Read file utility
std::string WebServer::ReadFile(const std::string& path) {
    static std::unordered_map<std::string, std::string> cache;
    static std::mutex cache_mtx;
    {
        std::lock_guard<std::mutex> lock(cache_mtx);
        if (cache.count(path)) {
            return cache[path];
        }
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    {
        std::lock_guard<std::mutex> lock(cache_mtx);
        cache[path] = content;
    }
    
    return content;
}

std::string WebServer::GetMimeType(const std::string& path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "text/html"; //没后缀，默认 html
    }
    std::string ext = path.substr(dot_pos);

    if (ext == ".html" || ext == ".htm")    return "text/html";
    if (ext == ".css")                      return "text/css";
    if (ext == ".js")                       return "application/javascript";
    if (ext == ".png")                       return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")    return "image/jpeg";
    if (ext == ".gif")                      return "image/gif";
    if (ext == ".ico")                       return "image/x-icon";
    if (ext == ".svg")                      return "image/svg+xml";
    if (ext == ".txt")                      return "text/plain";
    if (ext == ".json")                     return "application/json";
    if (ext == ".pdf")                      return "application/pdf";

    return "application/octet-stream";
}

void WebServer::ResetEpollOneshot(int fd, bool write_mode) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    if (write_mode == true) {
        event.events |= EPOLLOUT;
    }
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
}

void WebServer::HandleWrite(int client_fd) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (clients_[client_fd].fd == -1) return;

    HttpConnection& client = clients_[client_fd];
    const char* send_ptr = client.write_buffer.c_str() + client.write_offset;
    ssize_t remaining = client.write_buffer.size() - client.write_offset;

    while (remaining > 0) {
        ssize_t sent = write(client_fd, send_ptr, remaining);
        if (sent > 0) {
            client.write_offset += sent;
            send_ptr += sent;
            remaining -= sent;
        } 
        else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ResetEpollOneshot(client_fd, true);
            return;
        } else {
            LOG_ERROR("写数据出错 fd=" + std::to_string(client_fd) + " errno=" + std::to_string(errno));
            CloseConn(client_fd);
            return;
        }
    }
    client.write_buffer.clear();
    client.write_offset = 0;
    if (client.keep_alive_) {
        client.Reset();
        timer_.add(client_fd, 10000);
        ResetEpollOneshot(client_fd);
    } else {
        CloseConn(client_fd);
    }
}

// 6. Business Logic: Parse HTTP and respond (FSM & ET Mode)
void WebServer::HandleClient(int client_fd) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (clients_[client_fd].fd == -1) {
        return ;
    }
    // Fetch the client's personal profile from the archive
    HttpConnection& client = clients_[client_fd];
    char buffer[4096];

    // Phase 1: The Pumping Machine (Drain the kernal buffer)
    while (true) {
        std::memset(buffer, 0, sizeof(buffer));
        ssize_t byte_read = read(client_fd, buffer, sizeof(buffer) - 1);

        if (byte_read > 0) {
            // Store the newly read data into the client's personal buffer
            client.read_buffer += buffer;
        }
        else if (byte_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Kernel is empty, stop pumping and proceed to parse 
                break;
            } else {
                LOG_ERROR("读出错 fd=" + std::to_string(client_fd) + " errno=" + std::to_string(errno));
                std::cerr << "[Worker] Real read error on FD " << client_fd << std::endl;
                CloseConn(client_fd);
                return ;
            }

        }
        else if (byte_read == 0) {
            LOG_INFO("客户端正常断开 fd=" + std::to_string(client_fd));
            CloseConn(client_fd);
            return ;
        }
    }
    while (true) {
        // Phase 2: The Master Finite State Machine (FSM)
        bool keep_parsing = true;
        while (keep_parsing) {
            // Slave FSM action: Find the next CRLF (\r\n)
            size_t crlf_pos = client.read_buffer.find("\r\n");
            if (crlf_pos == std::string::npos) {
                // Half-packet detected! We haven't received a full line yet.
                // Freeze the FSM, return to Epoll, and wait for the next packet.
                break;
            }

            // Extract the complete line and remove it (with \r\n) from the buffer
            std::string line = client.read_buffer.substr(0, crlf_pos);
            client.read_buffer.erase(0, crlf_pos + 2);

            // Master FSM action: Process the line based on the current state
            switch (client.state) {
                case STATE_REQUEST_LINE: {
                    std::stringstream ss(line);
                    ss >> client.method >> client.url >> client.version;
                    if (client.url == "/") {
                        client.url = "/index.html";
                    }
                    //Transition to next state
                    client.state = STATE_HEADERS;
                    break;
                }
                case STATE_HEADERS: {
                    if (line.empty()) {
                        // An empty line "\r\n\r\n" indicates the end of headers
                        if (client.content_length_ > 0) {
                            client.state = STATE_BODY;
                        } else {
                            client.state = STATE_FINISH;
                        }
                        
                        keep_parsing = false;
                    } else {
                        //Parse the Key: Value header format
                        size_t colon_pos = line.find(':');
                        if (colon_pos != std::string::npos) {
                            std::string key = line.substr(0, colon_pos);
                            std::string value = line.substr(colon_pos + 1);

                            size_t first_not_space = value.find_first_not_of(" \t");
                            if (first_not_space != std::string::npos) {
                                value = value.substr(first_not_space);
                            }

                            // Store the header in our hash map
                            if (key == "Content-Length") {
                                client.content_length_ = std::stoi(value);
                            }
                            client.headers[key] = value;
                            
                        }
                    }
                    break;
                }
                // Note: STATE_BODY logic goes here for POST requests in the future
                default:
                    break;
            }
        }    
        // Phase 2.5: Handle request body (POST)
        if (client.state == STATE_BODY) {
            size_t need = client.content_length_;
            size_t available = client.read_buffer.size();
            if (available >= need) {
                client.body_ = client.read_buffer.substr(0, need);
                client.read_buffer.erase(0, need);
            } else {
                client.body_ = client.read_buffer;
                client.read_buffer.clear();
            }
            client.state = STATE_FINISH;
        }
        // Phase 3: Response Generation && Keep-Alive Logic 
        if (client.state == STATE_FINISH) {
            // std::cout << "[Worker] Successfully parsed Request: " << client.method << " " << client.url << std::endl;
            
            // 1. Determine if Keep-Alive is requested by checking headers
            client.keep_alive_ = false;
            
            if (client.headers.count("Connection")) {
                std::string conn_val = client.headers["Connection"];
                if (conn_val.find("keep-alive") != std::string::npos || conn_val.find("Keep-Alive") != std::string::npos){
                    client.keep_alive_ = true;
                }               
            }
            
            std::string content;
            int status_code = 200;
            std::string status_text = "OK";
            if (client.method.empty() || client.url.empty()) {
                content = "<h1>400 Bad Request</h1>";
                status_code = 400;
                status_text = "Bad Request";
                client.keep_alive_ = false;
            }
            else if (client.method != "GET" && client.method != "POST") {
                content = "<h1>405 Method Not Allowed</h1>";
                status_code = 405;
                status_text = "Method Not Allowed";
                client.keep_alive_ = false;
            }
            else if (client.method == "POST") {
                content = "<h1>POST Received</h1><pre>" + client.body_ + "</pre>";
            } else {
                std::string file_path = "resources" + client.url;
                content = ReadFile(file_path);
                if (content.empty()) {
                    content = "<h1>404 Not Found</h1>";
                    status_code = 404;
                    status_text = "Not Found";
                }
            }
            LOG_INFO(client.method + " " + client.url + " -> " + std::to_string(status_code) + " " + status_text);
            std::string response;
            
            // 2. Construct response headers based on connection type
            std::string connection_header = client.keep_alive_ ? "Connection: keep-alive\r\n" : "Connection: close\r\n";

            response = "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n"
                       "Content-Type: text/html\r\n" 
                       "Content-Length: " + std::to_string(content.size()) + "\r\n"
                       + connection_header + 
                       "\r\n" + content;
            // 3. Non-blocking write loop
            const char* send_ptr = response.c_str();
            ssize_t remaining = response.size();
            while (remaining > 0) {
                ssize_t sent = write(client_fd, send_ptr, remaining);
                if (sent > 0) {
                    send_ptr += sent;
                    remaining -= sent;
                } 
                else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // 内核缓冲区满了，存起来等 EPOLLOUT
                    client.write_buffer.assign(send_ptr, remaining);
                    client.write_offset = 0;
                    ResetEpollOneshot(client_fd, true); // 注册 EPOLLOUT
                    return ;
                } else {
                    LOG_ERROR("响应发送失败 fd=" + std::to_string(client_fd) + " errno=" + std::to_string(errno));
                    CloseConn(client_fd);
                    return ;
                }
            }
            // 4. The Ultimate Fate: Keep Alive vs Close
            if (client.keep_alive_) {
                // Keep-Alive: DO NOT close the socket! Reset the FSM state for the next request.
                client.Reset();
                timer_.add(client_fd, 10000);

                continue;
                
                // std::cout << "[Keep-Alive] Client FD " << client_fd << " renewed and re-armed.\n" << std::endl;
                // Note: In ET mode, the socket remains in the epoll radar automatically.
            } else {
                // Short Connection: Close socket, remove from radar and archive.
                CloseConn(client_fd);
                return;
                // std::cout << "[Epoll] Client FD " << client_fd << " closed (Short Connection).\n" << std::endl;
            }
        } else {
            ResetEpollOneshot(client_fd);
            return;
        }
    } 
   

    

}

