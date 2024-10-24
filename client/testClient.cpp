#include <bits/stdc++.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
using namespace std;

int BUFFER_SIZE = 1024;
int LISTEN_BACKLOG = 10;

string ip = "127.0.0.1";
int PORT = 5000;

int main()
{
    int status, valread, client_fd;
    struct sockaddr_in serv_addr;
    string hello = "Hello from client";
    char buffer[1024] = {0};
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0)
    {
        printf(
            "\nInvalid address/ Address not supported \n");
        return -1;
    }

    if ((status = connect(client_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) < 0)
    {
        printf("\nConnection Failed \n");
        return -1;
    }

    send(client_fd, hello.c_str(), strlen(hello.c_str()), 0);
    printf("Hello message sent\n");

    close(client_fd);
    return 0;
}