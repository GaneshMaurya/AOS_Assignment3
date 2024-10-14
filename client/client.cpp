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

string calculateFileSHA1(const string &filePath)
{
    const int bufferSize = 8 * 1024;
    unsigned char buffer[bufferSize];
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA_CTX shaContext;

    SHA1_Init(&shaContext);

    int fileDescriptor = open(filePath.c_str(), O_RDONLY);
    if (fileDescriptor < 0)
    {
        cerr << "Error opening file: " << filePath << endl;
        return "";
    }

    ssize_t bytesRead;
    while ((bytesRead = read(fileDescriptor, buffer, bufferSize)) > 0)
    {
        SHA1_Update(&shaContext, buffer, bytesRead);
    }

    SHA1_Final(hash, &shaContext);
    close(fileDescriptor);

    stringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i)
    {
        ss << hex << setw(2) << setfill('0') << static_cast<int>(hash[i]);
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
                // cout << message << "\n";
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
                        continue;
                    }

                    // Read the nth chunk
                    ssize_t bytesRead = read(inputFile, fileChunk, CHUNK_SIZE);
                    if (bytesRead < 0)
                    {
                        cout << "Error reading file: " << filePath << "\n";
                        close(inputFile);
                        delete[] fileChunk;
                        continue;
                    }

                    cout << "Chunk " << chunkNumber << " sent to requested client...\n";
                    cout << "Chunk size: " << bytesRead << "\n";

                    if (send(newSocketFd, fileChunk, bytesRead, 0) < 0)
                    {
                        cout << "Error: Failed to send message to peer.\n";
                        close(newSocketFd);
                        delete[] fileChunk;
                        close(inputFile);
                        continue;
                    }

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
    // cout << "File Path to download = " << filePath << "\n";
    getline(ss, fileSha, ':');
    // cout << "File Sha of the above = " << fileSha << "\n";

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
    // sort(chunkList.begin(), chunkList.end(), [](const auto &a, const auto &b)
    //      { return a.second.second.size() < b.second.second.size(); });

    // Sort based on chunk number
    sort(chunkList.begin(), chunkList.end(), [](const auto &a, const auto &b)
         { return a.second.first < b.second.first; });

    struct stat fileStat;
    string tempPath = filePath;
    if (stat(tempPath.c_str(), &fileStat) == -1)
    {
        cout << "Stat error\n";
    }
    int fileSize = fileStat.st_size;
    cout << "File path = " << filePath << "\n";
    cout << "File Size calculated during download " << fileSize << "\n";

    int sizeReceived = 0;

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

        // size_t bytesRead = 0;
        // string receivedData = "";
        // size_t bytesRemaining = min(CHUNK_SIZE, fileSize - (chunkNumber * CHUNK_SIZE));

        // while (bytesRead < bytesRemaining)
        // {
        //     char *chunkSubPiece = new char[bytesRemaining];
        //     int valread = recv(fd, chunkSubPiece, bytesRemaining - bytesRead, 0);
        //     if (valread < 0)
        //     {
        //         cout << "Error: Failed to receive chunk data.\n";
        //         delete[] chunkSubPiece;
        //         close(fd);
        //         break;
        //     }
        //     else if (valread == 0)
        //     {
        //         cout << "Connection closed by peer.\n";
        //         delete[] chunkSubPiece;
        //         close(fd);
        //         break;
        //     }

        //     receivedData.append(chunkSubPiece, valread);
        //     bytesRead += valread;
        //     delete[] chunkSubPiece;

        //     if (bytesRead >= bytesRemaining)
        //     {
        //         break;
        //     }
        // }

        // sizeReceived += bytesRead;

        // char *fileChunk = new char[bytesRead];
        // memcpy(fileChunk, receivedData.c_str(), bytesRead);

        // // Write chunk data to file
        // off_t offset = chunkNumber * CHUNK_SIZE;

        // if (lseek(fileDescriptor, offset, SEEK_SET) == -1)
        // {
        //     cout << "Error seeking to position " << offset << " for chunk " << chunkNumber << ".\n";
        //     delete[] fileChunk;
        //     return;
        // }

        // ssize_t bytesWritten = write(fileDescriptor, fileChunk, bytesRead);
        // if (bytesWritten != bytesRead)
        // {
        //     cout << "Error writing chunk " << chunkNumber << " to the file.\n";
        //     delete[] fileChunk;
        //     return;
        // }

        size_t bytesRead = 0;
        vector<char> receivedData;
        receivedData.reserve(CHUNK_SIZE);
        size_t bytesRemaining = min(CHUNK_SIZE, fileSize - (chunkNumber * CHUNK_SIZE));

        char *chunkSubPiece = new char[CHUNK_SIZE];

        while (bytesRead < bytesRemaining)
        {
            int valread = recv(fd, chunkSubPiece, bytesRemaining - bytesRead, 0);
            if (valread < 0)
            {
                cout << "Error: Failed to receive chunk data.\n";
                delete[] chunkSubPiece;
                close(fd);
                return;
            }
            else if (valread == 0)
            {
                cout << "Connection closed by peer.\n";
                delete[] chunkSubPiece;
                close(fd);
                return;
            }

            receivedData.insert(receivedData.end(), chunkSubPiece, chunkSubPiece + valread);
            bytesRead += valread;

            if (bytesRead >= bytesRemaining)
            {
                break;
            }
        }

        delete[] chunkSubPiece;

        off_t offset = chunkNumber * CHUNK_SIZE;
        if (lseek(fileDescriptor, offset, SEEK_SET) == -1)
        {
            cout << "Error seeking to position " << offset << " for chunk " << chunkNumber << ".\n";
            continue;
        }

        ssize_t bytesWritten = write(fileDescriptor, receivedData.data(), bytesRead);
        if (bytesWritten != bytesRead)
        {
            cout << "Error writing chunk " << chunkNumber << " to the file.\n";
            continue;
        }

        cout << "Successfully wrote chunk " << chunkNumber << " of size " << bytesWritten << " bytes.\n";

        // delete[] fileChunk;
        close(fd);
    }

    close(fileDescriptor);

    int newFileFd = open(newFilePath.c_str(), O_RDONLY);
    if (newFileFd < 0)
    {
        cout << "Error opening file for SHA calculation.\n";
        return;
    }
    cout << "Expected Size " << fileSize << "\n";
    cout << "Received Size " << sizeReceived << "\n";

    string receivedSha = calculateFileSHA1(newFilePath);
    cout << "Expected SHA: " << fileSha << "\n";
    cout << "Received SHA: " << receivedSha << "\n";

    if (fileSha == receivedSha)
    {
        cout << "Download completed successfully!\n";
    }
    else
    {
        cout << "SHA mismatch! Download corrupted.\n";
    }

    close(newFileFd);
}

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
            string fileSha = calculateFileSHA1(path);

            encodedHash += fileSha + '&';
            fileHash[fileSha] = path;

            struct stat fileInfo;
            stat(path.c_str(), &fileInfo);
            int fileSize = fileInfo.st_size;
            int numChunks = (fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
            // cout << "File Size calculated during upload " << fileSize << "\n";

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

            // cout << "Data sent to tracker\n"
            //      << encodedHash << "\n";

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
                // cout << "Response from tracker : " << responseMessage << "\n";
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