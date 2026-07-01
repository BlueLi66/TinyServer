#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/epoll.h>  //The soul of this demo: Epoll APIs
#include <cstring>
#include <fcntl.h>

const int MAX_EVENTS = 10;

// Helper Function: turn off the "blocking" behavior of a file descriptor
void set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    // 1. Standard setup: socket, reuse port, bind, listen
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 5);

    // The EPOLL MAGIC BEGINS HERE

    // 2. Create the Radar (Monitoring Center)
    int epoll_fd = epoll_create1(0);

    // 3. Add the Welcome Girl(listen_fd) to the Radar
    struct epoll_event event;
    event.events = EPOLLIN; // We care about "Incoming data/connections"
    event.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);

    std::cout << "[Eopoll] Server started. SINGLE THREAD mode." << std::endl;

    // This array will catch the FDs that triggerd the radar
    struct epoll_event events[MAX_EVENTS];
    
    while (true) {
        // 4. Block here and wait for the radar to beep!
        // -1 means "Wait forever until someone talks"
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < num_events; ++i) {
            int current_fd = events[i].data.fd;
            
            // Scenario A: The radar says the Welcome Girl has a new customer
            if (current_fd == listen_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                int client_fd = accept(listen_fd, (struct sockaddr*)& client_addr, &client_len);

                // IMPORTANT: Make the new client non_blocking
                set_non_blocking(client_fd);

                // Add the new customer to the Radar, using Edge Triggered(EPOLLET) mode!   
                struct epoll_event client_event;
                client_event.events = EPOLLIN | EPOLLET;
                client_event.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event);
                
                std::cout << "[Epoll] New Client FD accepted: " << client_fd << std::endl;
            } 
            // Scenario B: The radar says an existing customer is talking
            else {
                char buffer[1024] = {0};
                ssize_t byte_read = read(current_fd, buffer, sizeof(buffer) - 1);

                if (byte_read > 0) {
                    std::cout << "[Epoll] Processed Request from FD: " << current_fd << std::endl;

                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Length: 26\r\n"
                                           "\r\n"
                                           "Hello from Epoll Radar!";
                    write(current_fd, response.c_str(), response.size());
                }

                // Service is done. Remove from radar and hang up
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, nullptr);
                close(current_fd);
                std::cout << "[Epoll] Client FD: " << current_fd << "closed." << std::endl;
            }
        }
    }   
    return 0;
}
