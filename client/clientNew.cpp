#include <iostream>
#include <cstring>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

using namespace std;

const int BUFFER_SIZE = 1024;
const int LISTEN_BACKLOG = 10;

int opt = 1;

struct ThreadArgs
{
    int socketFd;
    struct sockaddr_in address;
    socklen_t addrlen;
    string port;
};

void *handleListen(void *args)
{
    ThreadArgs *threadArgs = (ThreadArgs *)args;
    int clientSocketFd = threadArgs->socketFd;
    struct sockaddr_in address = threadArgs->address;
    socklen_t addrlen = threadArgs->addrlen;
    string clientPort = threadArgs->port;

    // To make accept non-blocking
    fcntl(clientSocketFd, F_SETFL, O_NONBLOCK);

    while (true)
    {
        struct sockaddr_in peerAddress;
        socklen_t peerAddrLen = sizeof(peerAddress);
        int newSocketFd = accept(clientSocketFd, (struct sockaddr *)&peerAddress, &peerAddrLen);
        if (newSocketFd < 0)
        {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
            {
                cerr << "Error in accept: " << strerror(errno) << endl;
            }
            usleep(100000); // Sleep to avoid tight CPU loop
            continue;
        }

        cout << "Client connected on port " << clientPort << "\n";

        while (true)
        {
            char message[BUFFER_SIZE] = {0};

            // Receive message
            int valread = recv(newSocketFd, message, BUFFER_SIZE - 1, 0);
            if (valread > 0)
            {
                cout << "Message received: " << message << "\n";
            }
            else if (valread == 0)
            {
                cout << "Client disconnected.\n";
                close(newSocketFd);
                break;
            }
            else
            {
                if (errno != EWOULDBLOCK && errno != EAGAIN)
                {
                    cerr << "Error in receiving messages: " << strerror(errno) << endl;
                    close(newSocketFd);
                    break;
                }
            }

            if (strcmp(message, "end") == 0)
            {
                break;
            }
        }
    }

    pthread_exit(NULL);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cout << "Usage: " << argv[0] << " <ip>:<port> <tracker_info.txt>\n";
        return 1;
    }

    // Get the tracker details
    char *trackerInfoPath = argv[2];
    int inputFd = open(trackerInfoPath, O_RDWR);
    if (inputFd < 0)
    {
        cerr << "Error: Could not open " << trackerInfoPath << " file.\n";
        return 1;
    }

    char trackerInfo[BUFFER_SIZE];
    ssize_t inputSize = read(inputFd, trackerInfo, BUFFER_SIZE - 1);
    if (inputSize <= 0)
    {
        cerr << "Error: Could not read " << trackerInfoPath << " file.\n";
        close(inputFd);
        return 1;
    }
    trackerInfo[inputSize] = '\0';

    vector<string> trackersList;
    char *token = strtok(trackerInfo, "\n");
    while (token != NULL)
    {
        trackersList.push_back(token);
        token = strtok(NULL, "\n");
    }

    close(inputFd);

    // Get the IP Address and Port of the client
    char *temp = argv[1];
    char *clientIpAddress = strtok(temp, ":");
    char *clientPort = strtok(NULL, ":");

    // Socket creation
    int clientSockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSockFd == -1)
    {
        cerr << "An error occurred in creating socket\n";
        return 1;
    }

    if (setsockopt(clientSockFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        cerr << "Error: Sockopt\n";
        return 1;
    }

    // Binding
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(clientIpAddress);
    address.sin_port = htons(atoi(clientPort));

    if (bind(clientSockFd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        cerr << "Error: Could not bind to " << clientPort << " port.\n";
        return 1;
    }

    // Listening
    if (listen(clientSockFd, LISTEN_BACKLOG) < 0)
    {
        cerr << "Error in listen\n";
        return 1;
    }
    else
    {
        cout << clientPort << "\n";
        cout << "Client listening for other peers on port " << clientPort << "...\n";
    }

    ThreadArgs *threadArgs = new ThreadArgs{clientSockFd, address, addrlen, clientPort};
    pthread_t listenThread;
    if (pthread_create(&listenThread, NULL, handleListen, (void *)threadArgs) != 0)
    {
        cerr << "Failed to create listening thread" << endl;
        delete threadArgs;
        return 1;
    }
    pthread_detach(listenThread);

    struct sockaddr_in trackerAddress;
    trackerAddress.sin_family = AF_INET;

    bool connected = false;
    for (const auto &tracker : trackersList)
    {
        char *trackerIpAddress = strtok(const_cast<char *>(tracker.c_str()), ";");
        char *trackerPort = strtok(NULL, ";");

        trackerAddress.sin_addr.s_addr = inet_addr(trackerIpAddress);
        trackerAddress.sin_port = htons(atoi(trackerPort));

        // Set socket to non-blocking mode
        int flags = fcntl(clientSockFd, F_GETFL, 0);
        fcntl(clientSockFd, F_SETFL, flags | O_NONBLOCK);

        int status = connect(clientSockFd, (struct sockaddr *)&trackerAddress, sizeof(trackerAddress));
        if (status == 0)
        {
            connected = true;
            break;
        }
        else if (status < 0)
        {
            if (errno == EINPROGRESS)
            {
                fd_set fdset;
                struct timeval tv;
                FD_ZERO(&fdset);
                FD_SET(clientSockFd, &fdset);
                tv.tv_sec = 5;
                tv.tv_usec = 0;

                status = select(clientSockFd + 1, NULL, &fdset, NULL, &tv);
                if (status > 0)
                {
                    int so_error;
                    socklen_t len = sizeof(so_error);
                    getsockopt(clientSockFd, SOL_SOCKET, SO_ERROR, &so_error, &len);

                    if (so_error == 0)
                    {
                        connected = true;
                        cout << clientPort << "\n";
                        cout << "Connected to server...\n";
                        break;
                    }
                    else
                    {
                        cerr << "Error in delayed connection: " << strerror(so_error) << endl;
                    }
                }
                else if (status == 0)
                {
                    cerr << "Connection timed out" << endl;
                }
                else
                {
                    cerr << "Error in select(): " << strerror(errno) << endl;
                }
            }
            else
            {
                cerr << "Error connecting: " << strerror(errno) << endl;
            }
        }
        // Set socket back to blocking mode
        flags = fcntl(clientSockFd, F_GETFL, 0);
        fcntl(clientSockFd, F_SETFL, flags & ~O_NONBLOCK);

        if (!connected)
        {
            cout << "Failed to connect to any tracker server.\n";
            close(clientSockFd);
            delete threadArgs;
            return 1;
        }
    }

    cout << "Entering main loop...\n"; // Debug print

    while (true)
    {
        string input;
        cout << "Enter command: "; // Prompt for input
        getline(cin, input);

        if (input.empty())
        {
            continue;
        }

        string add_port = clientIpAddress;
        add_port += ':';
        add_port += clientPort;
        add_port += '+';
        input = add_port + input;
        char totalCommand[BUFFER_SIZE];
        strcpy(totalCommand, input.c_str());

        if (send(clientSockFd, totalCommand, strlen(totalCommand), 0) < 0)
        {
            cout << "Error: Failed to send message to server.\n";
            continue;
        }

        char responseMessage[BUFFER_SIZE] = {0};
        int valread = recv(clientSockFd, responseMessage, BUFFER_SIZE - 1, 0);
        if (valread > 0)
        {
            cout << responseMessage << "\n";
        }
        else if (valread == 0)
        {
            cout << "Connection closed by the server.\n";
            break;
        }
        else
        {
            cerr << "Error in receiving: " << strerror(errno) << endl;
        }

        if (input == "exit" || input == "quit")
        {
            break;
        }
    }

    // Clean up resources
    close(clientSockFd);
    delete threadArgs;

    return 0;
}
