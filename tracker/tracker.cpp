#include <bits/stdc++.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <sys/stat.h>
using namespace std;

int BUFFER_SIZE = 1024;
int LISTEN_BACKLOG = 10;
int opt = 1;
int CHUNK_SIZE = 512 * 1024;

struct userInfo
{
    string userId;
    string password;
    bool active = false;
    unordered_set<int> groupId;
    unordered_set<int> groupOwner;
    string ipaddress;
    string port;
    int socketFd;
    // Paths
    unordered_set<string> filesOwned;
};

struct groupInfo
{
    int groupId;
    string owner;
    unordered_set<string> members;
    unordered_set<string> requests;
    // File Name: Sha
    unordered_map<string, string> filePaths;
    // To get the file path
    // Sha: Filepath
    unordered_map<string, string> getPath;
};

// Shared data
unordered_map<string, userInfo *> userDetails;
unordered_map<int, groupInfo *> groups;
unordered_map<int, userInfo *> groupOwners;
unordered_set<string> activeUsers;
// To store {file_sha: {chunks sha1}}
unordered_map<string, vector<string>> torrent;
// To store which user has which chunk {chunk_sha: {users list}}
unordered_map<string, vector<string>> userChunks;

// Mutex
pthread_mutex_t userDetailsMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t groupsMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t groupOwnersMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t activeUsersMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t torrentMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t userChunksMutex = PTHREAD_MUTEX_INITIALIZER;

struct ThreadArgs
{
    int socketFd;
    struct sockaddr_in address;
    socklen_t addrlen;
};

unsigned char *stringToUnsignedChar(const string &input)
{
    return reinterpret_cast<unsigned char *>(const_cast<char *>(input.c_str()));
}

string sha1ToHexString(const unsigned char sha1Hash[SHA_DIGEST_LENGTH])
{
    stringstream ss;
    ss << hex << setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
    {
        ss << setw(2) << static_cast<unsigned int>(sha1Hash[i]);
    }
    return ss.str();
}

bool filePresent(string path)
{
    // Check if file is directory (if yes return false)
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0)
    {
        return false;
    }

    if (S_ISDIR(buffer.st_mode))
    {
        return false;
    }

    int fd = open(path.c_str(), O_RDONLY);
    if (fd != -1)
    {
        close(fd);
        return true;
    }
    else
    {
        return false;
    }
}

void *handleClientRequest(void *args)
{
    ThreadArgs *threadArgs = (ThreadArgs *)args;
    int socketFd = threadArgs->socketFd;
    struct sockaddr_in address = threadArgs->address;
    socklen_t addrlen = threadArgs->addrlen;

    userInfo *currUser = NULL;
    while (true)
    {
        char *clientMessage = new char[BUFFER_SIZE];
        // Correction
        memset(clientMessage, 0, BUFFER_SIZE);

        int valread = recv(socketFd, clientMessage, BUFFER_SIZE - 1, 0);
        if (valread <= 0)
        {
            delete[] clientMessage;
            delete threadArgs;
            pthread_exit(NULL);
        }

        // Parse total message
        char *clientDetails = strtok(clientMessage, "+");
        char *totalCommand = strtok(NULL, "+");

        if (!clientDetails || !totalCommand)
        {
            cout << "Invalid message format\n";
            delete threadArgs;
            pthread_exit(NULL);
        }

        // Parse client details
        char *ipToken = strtok(clientDetails, ":");
        char *portToken = strtok(NULL, ":");

        if (!ipToken || !portToken)
        {
            cout << "Invalid client details format\n";
            delete threadArgs;
            pthread_exit(NULL);
        }

        string clientIpAddress(ipToken);
        string clientPort(portToken);

        // Parse commands
        vector<string> commands;
        char *token = strtok(totalCommand, " ");
        while (token != NULL)
        {
            commands.push_back(string(token));
            token = strtok(NULL, " ");
        }

        if (commands.empty())
        {
            cout << "No command received\n";
            delete threadArgs;
            pthread_exit(NULL);
        }

        if (commands[0] == "logout")
        {
            pthread_mutex_lock(&activeUsersMutex);

            if (currUser != NULL && currUser->active == true)
            {
                currUser->active = false;
                activeUsers.erase(currUser->userId);

                cout << "User " << currUser->userId << " logged out successfully.\n";
                string response = "User logged out successfully.\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
            else
            {
                cout << "Logout Failed...\n";
                string response = "User needs to logged in first in order to logout.\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }

            pthread_mutex_unlock(&activeUsersMutex);
        }
        else if (commands[0] == "create_user")
        {
            if (commands.size() < 3)
            {
                string response = "Invalid create_user command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            string user_id = commands[1];
            string passwd = commands[2];

            pthread_mutex_lock(&userDetailsMutex);

            if (userDetails.find(user_id) != userDetails.end())
            {
                // Write code to send this back to client. And let client display this message
                cout << "User with the same user id " << user_id << " already exists...\n";
                string response = "User Id already exists.\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
            else
            {
                userInfo *newUser = new userInfo();
                newUser->userId = user_id;
                newUser->password = passwd;
                newUser->ipaddress = clientIpAddress;
                newUser->port = clientPort;

                userDetails[user_id] = newUser;
                cout << "User " + user_id + " created successfully.\n";

                string response = "User created successfully...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }

            pthread_mutex_unlock(&userDetailsMutex);
        }
        else if (commands[0] == "login")
        {
            if (commands.size() < 3)
            {
                string response = "Invalid login command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            string user_id = commands[1];
            string passwd = commands[2];

            pthread_mutex_lock(&userDetailsMutex);
            pthread_mutex_lock(&activeUsersMutex);

            if (userDetails.find(user_id) == userDetails.end())
            {
                cout << "No such user with this user id exists..\n";
                string response = "No such user with this user id exists. Please retry with new credentials...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
            else if (currUser != NULL)
            {
                if (currUser->userId != user_id)
                {
                    cout << "Login Unsuccessful...\n";
                    string response = "Login Unsuccessful. Already logged in with another user...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else
                {
                    string response = "Already logged in...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
            }
            else if (activeUsers.find(user_id) != activeUsers.end())
            {
                cout << "User " << user_id << " already logged in...\n";
                string response = "User already logged in.\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
            else
            {
                currUser = userDetails[user_id];
                if (currUser->password == passwd)
                {
                    currUser->active = true;
                    currUser->socketFd = socketFd;
                    activeUsers.insert(user_id);
                    userDetails[user_id] = currUser;
                    cout << "User with userId " + user_id + " is successfully logged in...\n";
                    string response = "Login Successful...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else
                {
                    cout << "Login Unsuccessful...\n";
                    string response = "Login Unsuccessful. Please retry with correct credentials...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
            }

            pthread_mutex_unlock(&userDetailsMutex);
            pthread_mutex_unlock(&activeUsersMutex);
        }
        else if (commands[0] == "create_group")
        {
            if (commands.size() < 2)
            {
                string response = "Invalid create_group command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            pthread_mutex_lock(&groupsMutex);
            pthread_mutex_lock(&groupOwnersMutex);
            pthread_mutex_lock(&activeUsersMutex);

            if (currUser != NULL && currUser->active == true)
            {
                string temp = commands[1];
                int groupNumber = stoi(temp);

                // If group number already exists
                if (groups.find(groupNumber) != groups.end())
                {
                    cout << "Group " << temp << " already exists...\n";
                    string response = "Group already exists. Try creating a different group...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else
                {
                    groupInfo *newGroup = new groupInfo();
                    newGroup->groupId = groupNumber;
                    newGroup->owner = currUser->userId;
                    newGroup->members.insert(currUser->userId);

                    currUser->groupId.insert(groupNumber);
                    currUser->groupOwner.insert(groupNumber);

                    userDetails[currUser->userId] = currUser;
                    groups[groupNumber] = newGroup;
                    groupOwners[groupNumber] = currUser;

                    cout << "Group " << groupNumber << " created successfully...\n";
                    string response = "Group created successfully...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }

            pthread_mutex_unlock(&groupsMutex);
            pthread_mutex_unlock(&groupOwnersMutex);
            pthread_mutex_unlock(&activeUsersMutex);
        }
        else if (commands[0] == "join_group")
        {
            if (commands.size() < 2)
            {
                string response = "Invalid join_group command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            pthread_mutex_lock(&groupsMutex);
            pthread_mutex_lock(&groupOwnersMutex);

            if (currUser != NULL && currUser->active == true)
            {
                string temp = commands[1];
                int groupNumber = stoi(temp);

                if (groups.find(groupNumber) == groups.end())
                {
                    cout << "User tried to join a group that doesn't exist...\n";
                    string response = "No such group exists...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else
                {
                    groupInfo *group = groups[groupNumber];

                    if (group->members.find(currUser->userId) != group->members.end())
                    {
                        cout << "User attempted to join a group which he is already a part of...\n";
                        string response = "You are already a member...\n";
                        send(socketFd, response.c_str(), response.length(), 0);
                    }
                    else
                    {
                        group->requests.insert(currUser->userId);
                        cout << "Request to join " << groupNumber << " created...\n";
                        string response = "Request to join sent to group owner...\n";
                        send(socketFd, response.c_str(), response.length(), 0);
                    }

                    groups[groupNumber] = group;
                }
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }

            pthread_mutex_unlock(&groupsMutex);
            pthread_mutex_unlock(&groupOwnersMutex);
        }
        else if (commands[0] == "leave_group")
        {
            if (commands.size() < 2)
            {
                string response = "Invalid leave_group command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            string temp = commands[1];
            int groupNumber = stoi(temp);

            pthread_mutex_lock(&groupsMutex);
            pthread_mutex_lock(&groupOwnersMutex);
            pthread_mutex_lock(&userDetailsMutex);

            if (currUser != NULL && currUser->active == true)
            {
                if (groups.find(groupNumber) == groups.end())
                {
                    string response = "Group does not exist.\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else
                {
                    groupInfo *currGroup = groups[groupNumber];

                    // Check if requested user is part of the group
                    if (currUser->groupId.find(groupNumber) != currUser->groupId.end())
                    {
                        currUser->groupId.erase(groupNumber);

                        // If current user is owner of the group
                        if (currGroup->owner == currUser->userId)
                        {
                            currUser->groupOwner.erase(groupNumber);
                            currGroup->members.erase(currUser->userId);

                            cout << "User is the owner of group " << groupNumber << ". Updating ownership.\n";

                            if (!currGroup->members.empty())
                            {
                                string newOwner = *currGroup->members.begin();
                                currGroup->owner = newOwner;

                                // Assigning new owner
                                userInfo *newOwnerUser = userDetails[newOwner];
                                newOwnerUser->groupOwner.insert(groupNumber);

                                cout << "New owner of group " << groupNumber << " is now " << newOwner << ".\n";

                                groups[groupNumber] = currGroup;
                                groupOwners[groupNumber] = newOwnerUser;
                                userDetails[newOwnerUser->userId] = newOwnerUser;
                            }
                            // If the owner was the only member
                            else
                            {
                                cout << "No other members in group " << groupNumber << ". Deleting the group.\n";
                                groups.erase(groupNumber);
                                groupOwners.erase(groupNumber);
                                // delete currGroup;
                            }

                            currUser->groupOwner.erase(groupNumber);
                            userDetails[currUser->userId] = currUser;

                            string response = "You have left group " + temp + ".\n";
                            send(socketFd, response.c_str(), response.length(), 0);
                        }
                        else
                        {
                            currGroup->members.erase(currUser->userId);
                            // groups[groupNumber] = currGroup;

                            currUser->groupId.erase(groupNumber);
                            userDetails[currUser->userId] = currUser;

                            string response = "You have left group " + temp + ".\n";
                            send(socketFd, response.c_str(), response.length(), 0);

                            cout << "User " << currUser->userId << " has left group " << groupNumber << ".\n";
                            groups[groupNumber] = currGroup;
                        }
                    }
                    else
                    {
                        string response = "You are not a member of group " + temp + ".\n";
                        send(socketFd, response.c_str(), response.length(), 0);
                    }
                }
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), strlen(response.c_str()), 0);
            }

            pthread_mutex_unlock(&groupsMutex);
            pthread_mutex_unlock(&groupOwnersMutex);
            pthread_mutex_unlock(&userDetailsMutex);
        }
        else if (commands[0] == "list_requests")
        {
            if (currUser != NULL && currUser->active == true)
            {
                string temp = commands[1];
                int groupNumber = stoi(temp);

                cout << "Pending requests were displayed.\n";

                string response = "Requests:\n";
                groupInfo *currGroup = groups[groupNumber];
                for (auto it : currGroup->requests)
                {
                    response += "Join request from: ";
                    response += it;
                    response += "\n";
                }
                send(socketFd, response.c_str(), response.length(), 0);
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
        }
        else if (commands[0] == "accept_request")
        {
            if (commands.size() < 3)
            {
                string response = "Invalid accept_request command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            string temp = commands[1];
            int groupNumber = stoi(temp);
            string user_id = commands[2];

            pthread_mutex_lock(&groupsMutex);
            pthread_mutex_lock(&userDetailsMutex);

            if (currUser != NULL && currUser->active == true)
            {
                // if current user is the owner of the group
                if (groups.find(groupNumber) == groups.end())
                {
                    string response = "Group does not exist.\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else if (groupOwners[groupNumber] != currUser)
                {
                    string response = "You are not the owner of this group.\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else
                {
                    groupInfo *currGroup = groups[groupNumber];

                    // if user requesting is in the pending requests
                    if (currGroup->requests.find(user_id) != currGroup->requests.end())
                    {
                        currGroup->requests.erase(user_id);
                        currGroup->members.insert(user_id);
                        userDetails[user_id]->groupId.insert(groupNumber);

                        cout << "Group owner " << currGroup->owner << " has accepted the request of " << user_id
                             << " to join group " << groupNumber << "\n";

                        groups[groupNumber] = currGroup;
                        // userInfo *reqUser = userDetails[user_id];
                        // reqUser->groupId.insert(groupNumber);

                        string ownerResponse = "User " + user_id + " has been added to the group.\n";
                        send(socketFd, ownerResponse.c_str(), ownerResponse.length(), 0);
                    }
                    else
                    {
                        cout << "No join request from user " << user_id << " for group " << groupNumber << ".\n";
                        string response = "No such join request exists for user " + user_id + ".\n";
                        send(socketFd, response.c_str(), response.length(), 0);
                    }
                    // else
                    // {
                    //     cout << "User is not the owner of the group " << groupNumber << ".\n";
                    //     string response = "You are not the owner of the group.\n";
                    //     send(socketFd, response.c_str(), response.length(), 0);
                    // }
                }
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), strlen(response.c_str()), 0);
            }

            pthread_mutex_unlock(&groupsMutex);
            pthread_mutex_unlock(&userDetailsMutex);
        }
        else if (commands[0] == "list_groups")
        {
            if (currUser != NULL && currUser->active == true)
            {
                cout << "All groups were displayed.\n";

                string response = "Groups list:\n";
                for (auto it : groups)
                {
                    response += to_string(it.first);
                    response += "\n";
                }
                send(socketFd, response.c_str(), response.length(), 0);
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
        }
        else if (commands[0] == "upload_file")
        {
            if (commands.size() < 4)
            {
                string response = "Invalid upload_file command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            string path = commands[1];
            int groupNumber = stoi(commands[2]);

            pthread_mutex_lock(&groupsMutex);
            pthread_mutex_lock(&userDetailsMutex);
            pthread_mutex_lock(&torrentMutex);
            pthread_mutex_lock(&userChunksMutex);

            // Checks whether user is logged in or not
            if (currUser != NULL && currUser->active == true)
            {
                // Check is group number is valid or not
                if (groups.find(groupNumber) == groups.end())
                {
                    cout << "Invalid group number entered...\n";
                    string response = "Invalid group number entered. Please retry...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                // Checks if user is part of the group
                else if (currUser->groupId.find(groupNumber) == currUser->groupId.end())
                {
                    cout << "User is not part of this group...\n";
                    string response = "You are not part of this group.\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                // Checks if file is already part of group
                else
                {
                    // Check if file is actually proper
                    if (filePresent(path) == false)
                    {
                        cout << "File mentioned by client doesn't exist...\n";
                        string response = "No such file exists.\n";
                        send(socketFd, response.c_str(), response.length(), 0);
                        continue;
                    }

                    groupInfo *group = groups[groupNumber];
                    if (group->filePaths.find(path) != group->filePaths.end())
                    {
                        cout << "File " << path << " is already part of this group...\n";
                        string response = "File is already part of this group.\n";
                        send(socketFd, response.c_str(), response.length(), 0);
                        continue;
                    }

                    string hashCodes = commands[3];
                    vector<string> parts;
                    string delimiter = "&";

                    size_t start = 0;
                    size_t end = hashCodes.find(delimiter);

                    // cout << hashCodes << "\n";

                    while (end != string::npos)
                    {
                        parts.push_back(hashCodes.substr(start, end - start));
                        start = end + delimiter.length();
                        end = hashCodes.find(delimiter, start);
                    }
                    parts.push_back(hashCodes.substr(start, end));

                    string fileSha = parts[0];
                    currUser->filesOwned.insert(fileSha);

                    for (int i = 1; i < parts.size(); i++)
                    {
                        torrent[fileSha].push_back(parts[i]);
                    }

                    cout << "File's path\n";
                    cout << fileSha << "\n";
                    cout << "Chunk sha:\n";
                    for (auto it : torrent[fileSha])
                    {
                        cout << it << "\n";
                    }

                    size_t lastSlashPos = path.find_last_of('/');
                    string fileName;
                    if (lastSlashPos != string::npos)
                    {
                        fileName = path.substr(lastSlashPos + 1);
                    }
                    group->filePaths.insert({fileName, fileSha});
                    group->getPath.insert({fileSha, path});

                    for (auto it : group->filePaths)
                    {
                        cout << it.first << " = " << it.second << "\n";
                    }

                    for (int i = 1; i < parts.size() - 1; i++)
                    {
                        userChunks[parts[i]].push_back(currUser->userId);
                    }

                    cout << "File " << fileName << " successfully uploaded...\n";
                    string response = "Done uploading...\n";
                    send(socketFd, response.c_str(), response.length(), 0);

                    groups[groupNumber] = group;
                }
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }

            pthread_mutex_unlock(&groupsMutex);
            pthread_mutex_unlock(&userDetailsMutex);
            pthread_mutex_unlock(&torrentMutex);
            pthread_mutex_unlock(&userChunksMutex);
        }
        else if (commands[0] == "download_file")
        {
            if (commands.size() < 4)
            {
                string response = "Invalid upload_file command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            int groupNumber = stoi(commands[1]);
            string fileName = commands[2];
            string reqFilePath = commands[3];

            // Checks whether user is logged in or not
            if (currUser != NULL && currUser->active == true)
            {
                // Check is group number is valid or not
                if (groups.find(groupNumber) == groups.end())
                {
                    cout << "Invalid group number entered...\n";
                    string response = "Invalid group number entered. Please retry...\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                // Checks if user is part of the group
                else if (currUser->groupId.find(groupNumber) == currUser->groupId.end())
                {
                    cout << "User is not part of this group...\n";
                    string response = "You are not part of this group.\n";
                    send(socketFd, response.c_str(), response.length(), 0);
                }
                else
                {
                    // Check if this file is present in group or not
                    groupInfo *group = groups[groupNumber];
                    // for (auto it : group->filePaths)
                    // {
                    //     cout << it.first << " = " << it.second << "\n";
                    // }

                    if (group->filePaths.find(fileName) == group->filePaths.end())
                    {
                        cout << "Requested file is not part of this group...\n";
                        string response = "File not part of this group.\n";
                        send(socketFd, response.c_str(), response.length(), 0);
                    }
                    else
                    {
                        cout << "Requested file is part of this group...\n";
                        string fileSha = group->filePaths[fileName];
                        // for (auto chunkSha : torrent[fileSha])
                        // {
                        //     for (auto user : userChunks[chunkSha])
                        //     {
                        //         response += user;
                        //         response += "\n";
                        //     }
                        // }

                        // Send file path also here
                        string response = group->getPath[fileSha];
                        response += ':';
                        response += fileSha;
                        response += ':';

                        // vector<pair<string, vector<string>>> userChunksVec;

                        // for (auto chunkSha : torrent[fileSha])
                        // {
                        //     userChunksVec.push_back({chunkSha, userChunks[chunkSha]});
                        // }

                        // sort(userChunksVec.begin(), userChunksVec.end(), [](const pair<string, vector<string>> &a, const pair<string, vector<string>> &b)
                        //      { return a.second.size() < b.second.size(); });

                        vector<tuple<string, vector<string>, int>> userChunksVec;

                        int index = 0;
                        for (const auto &chunkSha : torrent[fileSha])
                        {
                            userChunksVec.push_back(make_tuple(chunkSha, userChunks[chunkSha], index));
                            ++index;
                        }

                        sort(userChunksVec.begin(), userChunksVec.end(), [](const tuple<string, vector<string>, int> &a, const tuple<string, vector<string>, int> &b)
                             { return get<1>(a).size() < get<1>(b).size(); });

                        for (const auto &entry : userChunksVec)
                        {
                            cout << "Chunk: " << get<0>(entry)
                                 << ", Users: " << get<1>(entry).size()
                                 << ", Original Index: " << get<2>(entry) << "\n";
                        }

                        // for (auto chunkSha : torrent[fileSha])
                        // {
                        //     if (chunkSha != "")
                        //     {
                        //         response += chunkSha;
                        //         response += ':';
                        //         for (auto user : userChunks[chunkSha])
                        //         {
                        //             response += user;
                        //             response += ';';
                        //             response += userDetails[user]->ipaddress;
                        //             response += ';';
                        //             response += userDetails[user]->port;
                        //             response += '&';
                        //         }
                        //     }
                        // }

                        for (const auto &entry : userChunksVec)
                        {
                            string chunkSha = get<0>(entry);
                            int chunkNumber = get<2>(entry);

                            if (chunkSha != "")
                            {
                                response += chunkSha;
                                response += ',';
                                response += to_string(chunkNumber);
                                response += ':';

                                for (const auto &user : get<1>(entry))
                                {
                                    response += user;
                                    response += ';';
                                    response += userDetails[user]->ipaddress;
                                    response += ';';
                                    response += userDetails[user]->port;
                                    response += '&';
                                }
                            }
                        }
                        send(socketFd, response.c_str(), response.length(), 0);
                    }
                }
            }
            else
            {
                cout << "Client is not logged in.\n";
                string response = "User is not logged in. Please log in and retry...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
        }
        else if (commands[0] == "list_files")
        {
            if (commands.size() < 2)
            {
                string response = "Invalid upload_file command.\n";
                send(socketFd, response.c_str(), response.length(), 0);
                continue;
            }

            int groupNumber = stoi(commands[1]);

            if (groups.find(groupNumber) == groups.end())
            {
                cout << "Invalid group number entered...\n";
                string response = "Invalid group number entered. Please retry...\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
            // Checks if user is part of the group
            else if (currUser->groupId.find(groupNumber) == currUser->groupId.end())
            {
                cout << "User is not part of this group...\n";
                string response = "You are not part of this group.\n";
                send(socketFd, response.c_str(), response.length(), 0);
            }
            else
            {
                cout << "All files of group " << groupNumber << " were displayed.\n";
                string response = "Files list:\n";
                for (auto it : groups[groupNumber]->filePaths)
                {
                    response += it.first;
                    response += "\n";
                }
                send(socketFd, response.c_str(), response.length(), 0);
            }
        }
        else
        {
            cout << "Wrong command entered...\n";
            string response = "Wrong command entered. Please check...\n";
            send(socketFd, response.c_str(), response.length(), 0);
        }
    }

    delete threadArgs;
    pthread_exit(NULL);
}

bool stopExecution = false;

void *handleInput(void *args)
{
    string input;
    while (stopExecution == false)
    {
        cin >> input;
        if (input == "quit")
        {
            cout << "Closing...\n";
            stopExecution = true;
            break;
        }
    }

    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
    if (argc > 3)
    {
        cout << "Please enter the command in this format: ./a.out <tracker_info.txt> <tracker_no>\n";
        return 1;
    }

    int tracker_no = stoi(string(argv[2])) - 1;

    char *trackerInfoPath = argv[1];
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

    char *trackerIpAddress = strtok(trackersList[tracker_no], ";");
    char *trackerPort = strtok(NULL, ";");

    // Socket creation
    int trackerSocketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (trackerSocketFd == -1)
    {
        cout << "An error occurred in creating socket\n";
        return 1;
    }

    // To make accept non blocking
    fcntl(trackerSocketFd, F_SETFL, O_NONBLOCK);

    // Sock Optserver_fd
    if (setsockopt(trackerSocketFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        cout << "Error: Sockopt\n";
        return 1;
    }

    // Binding
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    address.sin_family = AF_INET;
    in_addr_t ipInBinary = inet_addr(trackerIpAddress);
    address.sin_addr.s_addr = ipInBinary;
    uint16_t PORT = static_cast<uint16_t>(strtoul(trackerPort, NULL, 10));
    address.sin_port = htons(PORT);

    if (bind(trackerSocketFd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        cout << "Error: Could not bind to port " << trackerPort << ".\n";
        return 1;
    }

    // Listening
    if (listen(trackerSocketFd, LISTEN_BACKLOG) < 0)
    {
        cout << "Error\n";
        return 1;
    }
    else
    {
        cout << trackerPort << "\n";
        cout << "Tracker listening on port " << trackerPort << "...\n";
    }

    // For getting inputs from user (quit case)
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, handleInput, NULL) != 0)
    {
        cout << "Failed to create thread" << endl;
        return 1;
    }

    vector<pthread_t> clientThreads;
    while (stopExecution == false)
    {
        // Accept messages
        struct sockaddr_in clientAddress;
        socklen_t clientAddrLen = sizeof(clientAddress);

        int clientSocketFd = accept(trackerSocketFd, (struct sockaddr *)&clientAddress, &clientAddrLen);
        if (clientSocketFd < 0)
        {
            if (stopExecution == true)
            {
                break;
            }
            usleep(100000);
            continue;
        }

        ThreadArgs *threadArgs = new ThreadArgs;
        threadArgs->socketFd = clientSocketFd;
        threadArgs->address = clientAddress;
        threadArgs->addrlen = clientAddrLen;

        pthread_t clientThreadId;
        if (pthread_create(&clientThreadId, NULL, handleClientRequest, (void *)threadArgs) != 0)
        {
            cout << "Failed to create thread" << endl;
            delete threadArgs;
        }
        else
        {
            clientThreads.push_back(clientThreadId);
        }
    }

    for (pthread_t thread : clientThreads)
    {
        pthread_join(thread, NULL);
    }
    pthread_join(thread_id, NULL);
    close(trackerSocketFd);
    return 0;
}