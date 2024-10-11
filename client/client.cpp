#include <bits/stdc++.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <string>
using namespace std;

int BUFFER_SIZE = 1024;
int LISTEN_BACKLOG = 10;
int CHUNK_SIZE = 512 * 1024;

int opt = 1;

struct ThreadArgs
{
    int socketFd;
    struct sockaddr_in address;
    socklen_t addrlen;
    string port;
};

// File Sha: {chunk sha}
unordered_map<string, vector<string>> files;
// File Sha: Path
unordered_map<string, string> fileHash;

// Mutex
pthread_mutex_t fileHashMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t filesMutex = PTHREAD_MUTEX_INITIALIZER;

unsigned char *stringToUnsignedChar(const string &input)
{
    return reinterpret_cast<unsigned char *>(const_cast<char *>(input.c_str()));
}

string sha1ToHexString(const unsigned char sha1Hash[SHA_DIGEST_LENGTH])
{
    stringstream ss;
    ss << hex << setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i)
    {
        ss << setw(2) << static_cast<unsigned int>(sha1Hash[i]);
    }
    return ss.str();
}

void *handleListen(void *args)
{
    ThreadArgs *threadArgs = (ThreadArgs *)args;
    int clientSocketFd = threadArgs->socketFd;
    struct sockaddr_in address = threadArgs->address;
    socklen_t addrlen = threadArgs->addrlen;
    string clientPort = threadArgs->port;

    // To make accept non blocking
    fcntl(clientSocketFd, F_SETFL, O_NONBLOCK);

    while (true)
    {
        struct sockaddr_in peerAddress;
        socklen_t peerAddrLen = sizeof(peerAddress);
        int newSocketFd = accept(clientSocketFd, (struct sockaddr *)&peerAddress, &peerAddrLen);
        if (newSocketFd < 0)
        {
            usleep(100000);
            continue;
        }

        cout << "Client connected on port " << clientPort << "\n";

        while (true)
        {
            char *message = new char[BUFFER_SIZE];
            memset(message, 0, BUFFER_SIZE);

            int valread = recv(newSocketFd, message, BUFFER_SIZE - 1, 0);
            if (valread > 0)
            {
                cout << "Message received:\n";
                cout << message << "\n";
                string str(message);

                if (str.substr(0, 8) == "Download")
                {
                    stringstream ss(str);
                    string downloadType, chunkNumberStr, filePath, chunkHash;
                    getline(ss, downloadType, ':');
                    getline(ss, chunkNumberStr, ':');
                    getline(ss, filePath, ':');
                    getline(ss, chunkHash);
                    int chunkNumber = stoi(chunkNumberStr);

                    cout << "Client wants " << chunkNumber << " chunk from " << filePath << " file\n";

                    int inputFile = open(filePath.c_str(), O_RDONLY);
                    if (inputFile < 0)
                    {
                        cout << "Error opening file: " << filePath << "...\n";
                        continue;
                    }

                    char *fileChunk = new char[CHUNK_SIZE];
                    off_t offset = chunkNumber * CHUNK_SIZE;
                    if (lseek(inputFile, offset, SEEK_SET) == (off_t)-1)
                    {
                        cout << "Error seeking to chunk: " << chunkNumber << "\n";
                        close(inputFile);
                        delete[] fileChunk;
                    }

                    // Read the nth chunk
                    ssize_t bytesRead = read(inputFile, fileChunk, CHUNK_SIZE);
                    if (bytesRead < 0)
                    {
                        cout << "Error reading file: " << filePath << "\n";
                        close(inputFile);
                        delete[] fileChunk;
                    }

                    cout << "Chunk " << chunkNumber << " sent to requested client...\n";
                    send(newSocketFd, fileChunk, strlen(fileChunk), 0);
                    close(inputFile);
                    delete[] fileChunk;
                }
            }
            else if (valread == 0)
            {
                cout << "Peer disconnected.\n\n";
                close(newSocketFd);
                break;
            }
            else
            {
                cout << "Error in receiving messages...\n";
                close(newSocketFd);
                break;
            }

            if (message == "end")
            {
                break;
            }

            delete[] message;
        }
    }

    pthread_exit(NULL);
    return NULL;
}

struct UserDetails
{
    string ipaddress;
    string port;
};

void handleDownloads(const string &response, const string &newFileName, const string &folderPath)
{
    string newFilePath = folderPath + "/" + newFileName;
    int fileDescriptor = open(newFilePath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fileDescriptor < 0)
    {
        cout << "Error creating file at: " << newFilePath << "\n";
        return;
    }

    unordered_map<string, unordered_map<string, pair<int, vector<UserDetails>>>> parsedData;

    stringstream ss(response);
    string filePath, fileSha, chunkShaBlock, chunkShaWithNumber, chunkSha, userBlock;

    getline(ss, filePath, ':');
    getline(ss, fileSha, ':');

    while (getline(ss, chunkShaBlock, '&'))
    {
        stringstream chunkStream(chunkShaBlock);
        if (getline(chunkStream, chunkShaWithNumber, ':') && !chunkShaWithNumber.empty())
        {
            stringstream chunkShaStream(chunkShaWithNumber);
            getline(chunkShaStream, chunkSha, ',');
            string chunkNumberStr;
            getline(chunkShaStream, chunkNumberStr, ',');
            int chunkNumber = stoi(chunkNumberStr);

            vector<UserDetails> userInfo;
            string userBlock;

            while (getline(chunkStream, userBlock, ':'))
            {
                stringstream userStream(userBlock);
                string user, ip, port;

                getline(userStream, user, ';');
                getline(userStream, ip, ';');
                getline(userStream, port, ';');

                if (!user.empty() && !ip.empty() && !port.empty())
                {
                    UserDetails userDetails = {ip, port};
                    userInfo.push_back(userDetails);
                }
            }

            parsedData[fileSha][chunkSha] = {chunkNumber, userInfo};
        }
    }

    // Collect all chunks with their client counts
    vector<pair<string, pair<int, vector<UserDetails>>>> chunkList;

    for (const auto &chunk : parsedData[fileSha])
    {
        chunkList.push_back({chunk.first, chunk.second});
    }

    // Sort chunks by number of clients (rarest first)
    sort(chunkList.begin(), chunkList.end(), [](const auto &a, const auto &b)
         { return a.second.second.size() < b.second.second.size(); });

    // Iterate over sorted chunks and request from clients
    for (const auto &chunkEntry : chunkList)
    {
        const string &chunkHash = chunkEntry.first;
        const int chunkNumber = chunkEntry.second.first;
        const vector<UserDetails> &users = chunkEntry.second.second;

        if (users.empty())
        {
            cout << "No clients available for chunk " << chunkHash << ".\n";
            continue;
        }

        // Randomly select a client
        int randomIndex = rand() % users.size();
        string userIp = users[randomIndex].ipaddress;
        string port = users[randomIndex].port;

        // Connect to the selected client
        cout << "Attempting to connect to " << userIp << ":" << port << " for chunk " << chunkNumber << "...\n";

        struct sockaddr_in seederData;
        seederData.sin_family = AF_INET;
        in_addr_t ipInBinary = inet_addr(userIp.c_str());
        seederData.sin_addr.s_addr = ipInBinary;
        uint16_t PORT = static_cast<uint16_t>(strtoul(port.c_str(), NULL, 10));
        seederData.sin_port = htons(PORT);

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1)
        {
            cout << "Error creating socket.\n";
            continue;
        }

        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
        {
            cout << "Error: Sockopt\n";
            close(fd);
            continue;
        }

        // Set a timeout for connect and recv to avoid getting stuck
        struct timeval timeout;
        timeout.tv_sec = 5; // 5 second timeout
        timeout.tv_usec = 0;

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        int status = connect(fd, (struct sockaddr *)&seederData, sizeof(seederData));
        if (status == -1)
        {
            cout << "Failed to connect to peer for chunk " << chunkHash << ".\n";
            close(fd);
            continue;
        }

        cout << "Connected to peer for chunk " << chunkNumber << ".\n";

        string temp = "Download:" + to_string(chunkNumber) + ":" + filePath + ":" + chunkHash;

        char *message = new char[BUFFER_SIZE];
        strcpy(message, temp.c_str());
        if (send(fd, message, strlen(message), 0) < 0)
        {
            cout << "Error: Failed to send message to peer.\n";
            delete[] message;
            close(fd);
            continue;
        }

        cout << "Message sent to peer for chunk " << chunkNumber << ".\n";

        char *fileChunk = new char[CHUNK_SIZE];
        int valread = recv(fd, fileChunk, CHUNK_SIZE, 0);
        if (valread <= 0)
        {
            cout << "Error: Failed to receive chunk data for chunk " << chunkNumber << ".\n";
            delete[] fileChunk;
            close(fd);
            continue;
        }

        // Write chunk data to file
        off_t offset = chunkNumber * CHUNK_SIZE;

        if (lseek(fileDescriptor, offset, SEEK_SET) == -1)
        {
            cout << "Error seeking to position " << offset << " for chunk " << chunkNumber << ".\n";
            return;
        }

        ssize_t bytesWritten = write(fileDescriptor, fileChunk, valread);
        if (bytesWritten != valread)
        {
            cout << "Error writing chunk " << chunkNumber << " to the file.\n";
            return;
        }

        cout << "Successfully wrote chunk " << chunkNumber << " of size " << valread << " bytes.\n";

        delete[] message;
        delete[] fileChunk;
        close(fd);
    }

    close(fileDescriptor);

    // Now check the file SHA integrity after download
    fileDescriptor = open(newFilePath.c_str(), O_RDONLY);
    if (fileDescriptor < 0)
    {
        cout << "Error opening file for SHA calculation.\n";
        return;
    }

    unsigned char fileShaTemp[SHA_DIGEST_LENGTH];
    char *buffer = new char[BUFFER_SIZE];
    string fullFile;
    ssize_t bytesRead;
    while ((bytesRead = read(fileDescriptor, buffer, BUFFER_SIZE)) > 0)
    {
        fullFile.append(buffer, bytesRead);
    }
    delete[] buffer;

    const unsigned char *totalFile = stringToUnsignedChar(fullFile);
    SHA1(totalFile, fullFile.length(), fileShaTemp);
    string receivedSha = sha1ToHexString(fileShaTemp);

    cout << "Expected SHA: " << fileSha << "\n";
    cout << "Received SHA: " << receivedSha << "\n";

    if (fileSha == receivedSha)
    {
        cout << "Download completed successfully, and SHA matches!\n";
    }
    else
    {
        cout << "SHA mismatch! Download corrupted.\n";
    }

    close(fileDescriptor);
}

// void handleDownloads(const string &response, const string &newFileName, const string &folderPath)
// {
//     string newFilePath = folderPath + "/" + newFileName;
//     int fileDescriptor = open(newFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
//     if (fileDescriptor < 0)
//     {
//         cout << "Error creating file at: " << newFilePath << "\n";
//         return;
//     }

//     unordered_map<string, unordered_map<string, pair<int, vector<UserDetails>>>> parsedData;

//     stringstream ss(response);
//     string filePath, fileSha, chunkShaBlock, chunkShaWithNumber, chunkSha, userBlock;

//     getline(ss, filePath, ':');
//     getline(ss, fileSha, ':');

//     while (getline(ss, chunkShaBlock, '&'))
//     {
//         stringstream chunkStream(chunkShaBlock);
//         if (getline(chunkStream, chunkShaWithNumber, ':') && !chunkShaWithNumber.empty())
//         {
//             stringstream chunkShaStream(chunkShaWithNumber);
//             getline(chunkShaStream, chunkSha, ',');
//             string chunkNumberStr;
//             getline(chunkShaStream, chunkNumberStr, ',');
//             int chunkNumber = stoi(chunkNumberStr);

//             vector<UserDetails> userInfo;
//             string userBlock;

//             while (getline(chunkStream, userBlock, ':'))
//             {
//                 stringstream userStream(userBlock);
//                 string user, ip, port;

//                 getline(userStream, user, ';');
//                 getline(userStream, ip, ';');
//                 getline(userStream, port, ';');

//                 if (!user.empty() && !ip.empty() && !port.empty())
//                 {
//                     UserDetails userDetails = {ip, port};
//                     userInfo.push_back(userDetails);
//                 }
//             }

//             parsedData[fileSha][chunkSha] = {chunkNumber, userInfo};
//         }
//     }

//     // Collect all chunks with their client counts
//     vector<pair<string, pair<int, vector<UserDetails>>>> chunkList;

//     for (const auto &chunk : parsedData[fileSha])
//     {
//         chunkList.push_back({chunk.first, chunk.second}); // chunk.first is the chunkSHA, chunk.second is pair<int, vector<UserDetails>>
//     }

//     // Sort chunks by the number of clients (rarest first)
//     sort(chunkList.begin(), chunkList.end(), [](const auto &a, const auto &b)
//          { return a.second.second.size() < b.second.second.size(); });

//     // Iterate over sorted chunks and request from clients
//     for (const auto &chunkEntry : chunkList)
//     {
//         const string &chunkHash = chunkEntry.first;
//         const int chunkNumber = chunkEntry.second.first;
//         const vector<UserDetails> &users = chunkEntry.second.second;

//         // If no clients are available, continue to the next chunk
//         if (users.empty())
//         {
//             cout << "No clients available for chunk " << chunkHash << ".\n";
//             continue;
//         }

//         // Randomly select a client from the available users
//         int randomIndex = rand() % users.size();
//         string userIp = users[randomIndex].ipaddress;
//         string port = users[randomIndex].port;

//         // Prepare to connect to the selected user
//         string chunkNumberStr = to_string(chunkNumber);

//         // New socket to connect with the selected client
//         struct sockaddr_in seederData;
//         socklen_t clientAddrLen = sizeof(seederData);
//         seederData.sin_family = AF_INET;
//         in_addr_t ipInBinary = inet_addr(userIp.c_str());
//         seederData.sin_addr.s_addr = ipInBinary;
//         uint16_t PORT = static_cast<uint16_t>(strtoul(port.c_str(), NULL, 10));
//         seederData.sin_port = htons(PORT);

//         int fd = socket(AF_INET, SOCK_STREAM, 0);
//         if (fd == -1)
//         {
//             cout << "An error occurred in creating socket\n";
//             continue;
//         }

//         int opt = 1;
//         if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
//         {
//             cout << "Error: Sockopt\n";
//             close(fd);
//             continue;
//         }

//         int status = connect(fd, (struct sockaddr *)&seederData, sizeof(seederData));
//         if (status == -1)
//         {
//             cout << "Failed to connect to peer for chunk " << chunkHash << ".\n";
//             close(fd);
//             continue;
//         }

//         string temp = "Download:";
//         temp += chunkNumberStr;
//         temp += ":";
//         temp += filePath;
//         temp += ":";
//         temp += chunkHash;

//         char *message = new char[BUFFER_SIZE];
//         strcpy(message, temp.c_str());
//         if (send(fd, message, strlen(message), 0) < 0)
//         {
//             cout << "Error: Failed to send message to server.\n";
//         }

//         char *fileChunk = new char[CHUNK_SIZE];
//         int valread = recv(fd, fileChunk, BUFFER_SIZE - 1, 0);
//         if (valread <= 0)
//         {
//             delete[] fileChunk;
//             close(fd);
//             continue;
//         }

//         // off_t offset = chunkNumber * CHUNK_SIZE;

//         // if (lseek(fileDescriptor, offset, SEEK_SET) < 0)
//         // {
//         //     cout << "Error seeking in file\n";
//         //     continue;
//         // }

//         // if (write(fileDescriptor, fileChunk, bytesRead) != bytesRead)
//         // {
//         //     cout << "Error writing chunk to file\n";
//         //     continue;
//         // }

//         int bytesRead;
//         if (bytesRead > 0)
//         {
//             off_t offset = chunkNumber * CHUNK_SIZE;

//             if (lseek(fileDescriptor, offset, SEEK_SET) == -1)
//             {
//                 cout << "Error seeking to position " << offset << " for chunk " << chunkNumber << ".\n";
//                 return;
//             }

//             ssize_t bytesWritten = write(fileDescriptor, fileChunk, bytesRead);
//             if (bytesWritten != bytesRead)
//             {
//                 cout << "Error writing chunk " << chunkNumber << " to the file.\n";
//                 return;
//             }
//             cout << "Successfully wrote chunk " << chunkNumber << " of size " << bytesRead << " bytes to the file.\n";
//         }

//         unsigned char chunkShaTemp[SHA_DIGEST_LENGTH];
//         string str(fileChunk);
//         const unsigned char *PATH = stringToUnsignedChar(str);
//         SHA1(PATH, str.length(), chunkShaTemp);

//         string receivedSha = sha1ToHexString(chunkShaTemp);
//         cout << chunkHash << "\n";
//         cout << receivedSha << "\n";
//         if (chunkHash == receivedSha)
//         {
//             cout << "Successful transfer of chunk " << chunkHash << ".\n";
//         }

//         // Remove the chunk from parsed data after processing
//         parsedData[fileSha].erase(chunkHash);

//         // Clean up
//         delete[] message;
//         delete[] fileChunk;
//         close(fd);
//     }

//     string fullFile;
//     ssize_t bytesRead;
//     char *buffer = new char[BUFFER_SIZE];
//     while ((bytesRead = read(fileDescriptor, buffer, sizeof(buffer) - 1)) > 0)
//     {
//         buffer[bytesRead] = '\0';
//         fullFile += buffer;
//     }

//     unsigned char fileShaTemp[SHA_DIGEST_LENGTH];
//     const unsigned char *totalFile = stringToUnsignedChar(fullFile);
//     SHA1(totalFile, sizeof(totalFile) - 1, fileShaTemp);
//     string receivedSha = sha1ToHexString(fileShaTemp);

//     cout << fileSha << "\n";
//     cout << receivedSha << "\n";

//     if (fileSha == receivedSha)
//     {
//         cout << "Download completed...\n";
//     }
// }

int main(int argc, char *argv[])
{
    if (argc > 3)
    {
        cout << "Please enter the command in this format: ./a.out <ip>:<port> <tracker_info.txt>\n";
        return 1;
    }

    // Get the tracker details
    char *trackerInfoPath = argv[2];
    int inputFd = open(trackerInfoPath, O_RDWR);
    if (inputFd < 0)
    {
        cout << "Error: Could not open " << trackerInfoPath << " file.\n";
        return 1;
    }

    char *trackerInfo = (char *)malloc(BUFFER_SIZE);
    ssize_t inputSize = read(inputFd, trackerInfo, BUFFER_SIZE);
    if (inputSize == 0)
    {
        cout << "Error: Could not read " << trackerInfoPath << " file.\n";
        return 1;
    }

    vector<char *> trackersList;
    char *token = strtok(trackerInfo, "\n");
    while (token != NULL)
    {
        trackersList.push_back(token);
        token = strtok(NULL, "\n");
    }

    // Get the IP Address and Port of the client
    char *temp = argv[1];
    char *clientIpAddress = strtok(temp, ":");
    char *clientPort = strtok(NULL, ":");

    // Socket creation
    int clientSockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSockFd == -1)
    {
        cout << "An error occurred in creating socket\n";
        return 1;
    }

    if (setsockopt(clientSockFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        cout << "Error: Sockopt\n";
        return 1;
    }

    // Binding
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    address.sin_family = AF_INET;
    in_addr_t ipInBinary = inet_addr(clientIpAddress);
    address.sin_addr.s_addr = ipInBinary;
    uint16_t PORT = static_cast<uint16_t>(strtoul(clientPort, NULL, 10));
    address.sin_port = htons(PORT);

    if (bind(clientSockFd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        cout << "Error: Could not bind to " << clientPort << " port.\n";
        return 1;
    }

    // Listening
    if (listen(clientSockFd, LISTEN_BACKLOG) < 0)
    {
        cout << "Error\n";
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
        cout << "Failed to create listening thread...\n";
        return 1;
    }
    // pthread_detach(listenThread);

    struct sockaddr_in trackerAddress;
    trackerAddress.sin_family = AF_INET;
    cout << "Initiating Connection with tracker...\n";

    int clientToTrackerSocketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (clientToTrackerSocketFd == -1)
    {
        cout << "An error occurred in creating socket\n";
        return 1;
    }

    for (int i = 0; i < trackersList.size(); i++)
    {
        char *trackerIpAddress = strtok(trackersList[i], ";");
        char *trackerPort = strtok(NULL, ";");

        in_addr_t ipInBinclientSockFdary = inet_addr(trackerIpAddress);
        trackerAddress.sin_addr.s_addr = ipInBinary;
        uint16_t PORT = static_cast<uint16_t>(strtoul(trackerPort, nullptr, 10));
        trackerAddress.sin_port = htons(PORT);

        int status = connect(clientToTrackerSocketFd, (struct sockaddr *)&trackerAddress, sizeof(trackerAddress));
        if (status != -1)
        {
            // cout << clientPort << "\n";
            cout << "Connected to server...\n";
            break;
        }
    }

    while (true)
    {
        string input;
        getline(cin, input);

        if (input.empty())
        {
            continue;
        }

        string add_port = string(clientIpAddress) + ':' + string(clientPort) + '+' + input;

        if (input.find("upload_file") != string::npos)
        {
            pthread_mutex_lock(&fileHashMutex);
            pthread_mutex_lock(&filesMutex);

            string encodedHash = add_port;
            size_t firstSpace = input.find(' ');
            size_t secondSpace = input.find(' ', firstSpace + 1);
            string path = input.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            encodedHash += ' ';

            int fd = open(path.c_str(), O_RDONLY);
            if (fd == -1)
            {
                cout << "Error: Could not open file...\n";
                pthread_mutex_unlock(&fileHashMutex);
                pthread_mutex_unlock(&filesMutex);
                continue;
            }

            string fullFile;
            ssize_t bytesRead;
            char buffer[BUFFER_SIZE];

            while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0)
            {
                fullFile.append(buffer, bytesRead);
            }
            close(fd);

            unsigned char fileShaTemp[SHA_DIGEST_LENGTH];
            const unsigned char *totalFile = reinterpret_cast<const unsigned char *>(fullFile.c_str());
            SHA1(totalFile, fullFile.size(), fileShaTemp);
            string fileSha = sha1ToHexString(fileShaTemp);

            encodedHash += fileSha + '&';
            fileHash[fileSha] = path;

            struct stat fileInfo;
            stat(path.c_str(), &fileInfo);
            int fileSize = fileInfo.st_size;
            int numChunks = (fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;

            int inputFile = open(path.c_str(), O_RDONLY);
            if (inputFile < 0)
            {
                cout << "Error opening file: " << path << "...\n";
                pthread_mutex_unlock(&fileHashMutex);
                pthread_mutex_unlock(&filesMutex);
                continue;
            }

            for (int i = 0; i < numChunks; i++)
            {
                int currentChunkSize = min(CHUNK_SIZE, fileSize);
                char *fileChunk = new char[currentChunkSize];

                ssize_t chunkRead = read(inputFile, fileChunk, currentChunkSize);
                if (chunkRead < 0)
                {
                    cout << "Error reading file: " << path << "...\n";
                    delete[] fileChunk;
                    break;
                }

                unsigned char chunkShaTemp[SHA_DIGEST_LENGTH];
                SHA1(reinterpret_cast<const unsigned char *>(fileChunk), chunkRead, chunkShaTemp);
                string chunkSha = sha1ToHexString(chunkShaTemp);

                encodedHash += chunkSha + '&';
                files[fileSha].push_back(chunkSha);

                delete[] fileChunk;
                fileSize -= currentChunkSize;
            }
            close(inputFile);

            if (send(clientToTrackerSocketFd, encodedHash.c_str(), encodedHash.size(), 0) < 0)
            {
                cout << "Error: Failed to send message to server.\n";
                pthread_mutex_unlock(&fileHashMutex);
                pthread_mutex_unlock(&filesMutex);
                continue;
            }

            cout << "Data sent to tracker\n"
                 << encodedHash << "\n";

            char responseMessage[BUFFER_SIZE];
            memset(responseMessage, 0, BUFFER_SIZE);
            int valread = recv(clientToTrackerSocketFd, responseMessage, BUFFER_SIZE - 1, 0);
            if (valread > 0)
            {
                cout << responseMessage << "\n";
            }

            pthread_mutex_unlock(&fileHashMutex);
            pthread_mutex_unlock(&filesMutex);
            continue;
        }

        char totalCommand[BUFFER_SIZE];
        strcpy(totalCommand, add_port.c_str());
        if (send(clientToTrackerSocketFd, totalCommand, strlen(totalCommand), 0) < 0)
        {
            cout << "Error: Failed to send message to server.\n";
            continue;
        }

        char responseMessage[BUFFER_SIZE];
        memset(responseMessage, 0, BUFFER_SIZE);
        if (input.find("download_file") != string::npos)
        {
            size_t lastSpace = input.rfind(' ');
            string savePath = input.substr(lastSpace + 1);

            istringstream iss(input);
            string newFileName;
            iss >> newFileName >> newFileName >> newFileName;

            int valread = recv(clientToTrackerSocketFd, responseMessage, BUFFER_SIZE - 1, 0);
            if (valread > 0)
            {
                handleDownloads(responseMessage, newFileName, savePath);
            }
            else if (valread == 0)
            {
                cout << "Connection closed by the server.\n";
                close(clientToTrackerSocketFd);
                break;
            }
        }
        else
        {
            int valread = recv(clientToTrackerSocketFd, responseMessage, BUFFER_SIZE - 1, 0);
            if (valread > 0)
            {
                cout << responseMessage << "\n";
            }
            else if (valread == 0)
            {
                cout << "Connection closed by the server.\n";
                close(clientToTrackerSocketFd);
                break;
            }
        }
    }

    pthread_join(listenThread, NULL);
    close(clientToTrackerSocketFd);
    close(clientSockFd);
    return 0;
}