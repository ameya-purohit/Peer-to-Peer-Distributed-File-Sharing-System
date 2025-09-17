#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring> // strtok, strcpy
#include <cstdio>  // sscanf
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

map<string, string> users;                // userid -> password
map<string, set<string>> groups;          // groupid -> members
map<string, string> groupOwners;          // groupid -> owner
map<string, set<string>> pendingRequests; // groupid -> pending join requests
map<int, string> activeSessions;          // clientSock -> userid

vector<pair<string, int>> trackerPeers;
int tracker_no;

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

// ...

void loadTrackerInfo(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        perror("open trackerinfo.txt");
        exit(1);
    }

    char filebuf[1024];
    ssize_t n = read(fd, filebuf, sizeof(filebuf) - 1);
    if (n <= 0)
    {
        perror("read trackerinfo.txt");
        close(fd);
        exit(1);
    }
    filebuf[n] = '\0';
    close(fd);

    // Parse lines "ip port"
    char *line = strtok(filebuf, "\n");
    while (line)
    {
        char ipbuf[64]; // temporary buffer for IP
        int port;
        if (sscanf(line, "%63s %d", ipbuf, &port) == 2)
        {
            trackerPeers.push_back({string(ipbuf), port});
        }
        line = strtok(NULL, "\n");
    }

    if (trackerPeers.empty())
    {
        cerr << "Error: trackerinfo.txt is empty or invalid\n";
        exit(1);
    }
}

// Sync state with peer tracker
void syncWithPeer()
{
    int peerIndex = (tracker_no == 1 ? 1 : 0);
    if (peerIndex >= (int)trackerPeers.size())
        return;

    string ip = trackerPeers[peerIndex].first;
    int port = trackerPeers[peerIndex].second;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return;

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port + 100);
    inet_pton(AF_INET, ip.c_str(), &serv.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0)
    {
        close(sockfd);
        return; // peer might be down
    }

    stringstream data;
    data << "SYNC\n";
    for (auto &u : users)
    {
        data << "USER " << u.first << " " << u.second << "\n";
    }
    for (auto &g : groups)
    {
        data << "GROUP " << g.first;
        for (auto &m : g.second)
            data << " " << m;
        data << "\n";
    }
    for (auto &o : groupOwners)
    {
        data << "OWNER " << o.first << " " << o.second << "\n";
    }
    for (auto &pr : pendingRequests)
    {
        data << "PENDING " << pr.first;
        for (auto &u : pr.second)
            data << " " << u;
        data << "\n";
    }

    string msg = data.str();
    write(sockfd, msg.c_str(), msg.size());
    close(sockfd);
}

// Handle sync messages
void syncHandler(int sockfd)
{
    char buffer[4096];
    int n = read(sockfd, buffer, sizeof(buffer) - 1);
    if (n <= 0)
    {
        close(sockfd);
        return;
    }
    buffer[n] = '\0';

    stringstream ss(buffer);
    string type;
    while (ss >> type)
    {
        if (type == "USER")
        {
            string uid, pass;
            ss >> uid >> pass;
            users[uid] = pass;
        }
        else if (type == "GROUP")
        {
            string gid;
            ss >> gid;
            string member;
            set<string> mems;
            while (ss.peek() != '\n' && ss >> member)
            {
                mems.insert(member);
            }
            groups[gid] = mems;
        }
        else if (type == "OWNER")
        {
            string gid, owner;
            ss >> gid >> owner;
            groupOwners[gid] = owner;
        }
        else if (type == "PENDING")
        {
            string gid;
            ss >> gid;
            string uid;
            set<string> reqs;
            while (ss.peek() != '\n' && ss >> uid)
            {
                reqs.insert(uid);
            }
            pendingRequests[gid] = reqs;
        }
    }
    close(sockfd);
}

// Background sync server
void syncServer(string ip, int basePort)
{
    int port = basePort + 100;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind sync");
        return;
    }
    listen(sockfd, 5);
    cout << "Tracker sync server on port " << port << endl;

    while (true)
    {
        sockaddr_in cli{};
        socklen_t clilen = sizeof(cli);
        int newsock = accept(sockfd, (struct sockaddr *)&cli, &clilen);
        if (newsock < 0)
        {
            perror("accept");
            continue;
        }
        thread(syncHandler, newsock).detach();
    }
}

// Command handling
string handleCommand(int clientSock, const string &cmd)
{
    stringstream ss(cmd);
    string token;
    ss >> token;

    if (token == "create_user")
    {
        string uid, pass;
        ss >> uid >> pass;
        if (users.count(uid))
            return "User already exists\n";
        users[uid] = pass;
        syncWithPeer();
        return "User created\n";
    }
    else if (token == "login")
    {
        string uid, pass;
        ss >> uid >> pass;
        if (users.count(uid) && users[uid] == pass)
        {
            activeSessions[clientSock] = uid;
            syncWithPeer();
            return "Login success\n";
        }
        return "Login failed\n";
    }
    else if (token == "create_group")
    {
        string gid;
        ss >> gid;
        if (!activeSessions.count(clientSock))
            return "Not logged in\n";
        string uid = activeSessions[clientSock];
        if (groups.count(gid))
            return "Group already exists\n";
        groups[gid].insert(uid);
        groupOwners[gid] = uid;
        syncWithPeer();
        return "Group created\n";
    }
    else if (token == "join_group")
    {
        string gid;
        ss >> gid;
        if (!activeSessions.count(clientSock))
            return "Not logged in\n";
        string uid = activeSessions[clientSock];
        if (!groups.count(gid))
            return "Group not found\n";
        if (groups[gid].count(uid))
            return "Already a member\n";
        pendingRequests[gid].insert(uid);
        syncWithPeer();
        return "Join request sent\n";
    }
    else if (token == "list_requests")
    {
        string gid;
        ss >> gid;
        if (!groups.count(gid))
            return "Group not found\n";
        if (!activeSessions.count(clientSock))
            return "Not logged in\n";
        string actor = activeSessions[clientSock];
        if (groupOwners[gid] != actor)
            return "Permission denied\n";

        string result = "Pending requests for " + gid + ":\n";
        for (auto &u : pendingRequests[gid])
            result += "  " + u + "\n";
        return result;
    }
    else if (token == "accept_request")
    {
        string gid, uid;
        ss >> gid >> uid;
        if (!groups.count(gid))
            return "Group not found\n";
        if (!pendingRequests[gid].count(uid))
            return "No such pending request\n";
        if (!activeSessions.count(clientSock))
            return "Not logged in\n";
        string actor = activeSessions[clientSock];
        if (groupOwners[gid] != actor)
            return "Permission denied\n";

        groups[gid].insert(uid);
        pendingRequests[gid].erase(uid);
        syncWithPeer();
        return "Request accepted\n";
    }
    else if (token == "list_groups")
    {
        string result = "Groups:\n";
        for (auto &g : groups)
        {
            result += "  " + g.first + " (";
            for (auto &m : g.second)
                result += m + " ";
            result += ")\n";
        }
        return result;
    }
    return "Unknown command\n";
}

// Handle client connections
void clientHandler(int clientSock)
{
    char buffer[1024];
    while (true)
    {
        int n = read(clientSock, buffer, sizeof(buffer) - 1);
        if (n <= 0)
            break;
        buffer[n] = '\0';
        string response = handleCommand(clientSock, string(buffer));
        write(clientSock, response.c_str(), response.size());
    }
    if (activeSessions.count(clientSock))
    {
        activeSessions.erase(clientSock);
    }
    close(clientSock);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cerr << "Usage: ./tracker trackerinfo.txt <tracker_no>\n";
        return 1;
    }

    tracker_no = stoi(argv[2]);
    loadTrackerInfo(argv[1]);

    if (tracker_no < 1 || tracker_no > (int)trackerPeers.size())
    {
        cerr << "Error: tracker_no must be between 1 and "
             << trackerPeers.size() << endl;
        return 1;
    }

    string ip = trackerPeers[tracker_no - 1].first;
    int port = trackerPeers[tracker_no - 1].second;

    thread(syncServer, ip, port).detach();

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(sockfd, 5) < 0)
    {
        perror("listen");
        return 1;
    }

    cout << "Tracker " << tracker_no
         << " running at " << ip << ":" << port << endl;

    while (true)
    {
        sockaddr_in cli{};
        socklen_t clilen = sizeof(cli);
        int clientSock = accept(sockfd, (struct sockaddr *)&cli, &clilen);
        if (clientSock < 0)
        {
            perror("accept");
            continue;
        }
        thread(clientHandler, clientSock).detach();
    }

    close(sockfd);
    return 0;
}
