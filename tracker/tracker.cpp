#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

map<string, string> users;       // userid -> password
map<string, set<string>> groups; // groupid -> members
vector<pair<string, int>> trackerPeers;
int tracker_no;

void loadTrackerInfo(const char *filename)
{
    ifstream in(filename);
    string ip;
    int port;
    while (in >> ip >> port)
    {
        trackerPeers.push_back({ip, port});
    }
}

// Simple sync placeholder: just prints state
void syncWithPeer()
{
    int peerIndex = (tracker_no == 1 ? 1 : 0); // other tracker
    cout << "[SYNC] Sending current state to Tracker " << peerIndex + 1 << endl;
    for (auto &u : users)
    {
        cout << "   USER: " << u.first << endl;
    }
    for (auto &g : groups)
    {
        cout << "   GROUP: " << g.first << " members:";
        for (auto &m : g.second)
            cout << " " << m;
        cout << endl;
    }
}

// Parse and handle client commands
string handleCommand(const string &cmd)
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
            return "Login success\n";
        return "Login failed\n";
    }
    else if (token == "create_group")
    {
        string gid, uid;
        ss >> gid >> uid;
        if (groups.count(gid))
            return "Group already exists\n";
        groups[gid].insert(uid);
        syncWithPeer();
        return "Group created\n";
    }
    else if (token == "join_group")
    {
        string gid, uid;
        ss >> gid >> uid;
        if (!groups.count(gid))
            return "Group not found\n";
        groups[gid].insert(uid);
        syncWithPeer();
        return "Joined group\n";
    }
    return "Unknown command\n";
}

// Handle each client connection
void clientHandler(int clientSock)
{
    char buffer[1024];
    while (true)
    {
        int n = read(clientSock, buffer, sizeof(buffer) - 1);
        if (n <= 0)
            break;
        buffer[n] = '\0';
        string response = handleCommand(string(buffer));
        write(clientSock, response.c_str(), response.size());
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

    string ip = trackerPeers[tracker_no - 1].first;
    int port = trackerPeers[tracker_no - 1].second;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    sockaddr_in addr;
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

    cout << "Tracker " << tracker_no << " running at " << ip << ":" << port << endl;

    while (true)
    {
        sockaddr_in cli;
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
