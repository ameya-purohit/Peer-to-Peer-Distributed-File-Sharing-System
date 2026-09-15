#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <mutex>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

using namespace std;

struct FileMetadata
{
    string filename;
    string group_id;
    size_t filesize;
    int total_pieces;
    vector<string> piece_hashes;
    set<string> seeders;
};

map<string, string> users;
map<string, set<string>> groups;
map<string, string> groupOwners;
map<string, set<string>> pendingRequests;
map<int, string> activeSessions;
map<string, FileMetadata> files;

vector<pair<string, int>> trackerPeers;
int tracker_no;

int main_listen_fd = -1;
int sync_listen_fd = -1;

mutex state_mutex;

void loadTrackerInfo(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        perror("open trackerinfo.txt");
        exit(1);
    }

    char filebuf[4096];
    ssize_t n = read(fd, filebuf, sizeof(filebuf) - 1);
    if (n < 0)
    {
        perror("read trackerinfo.txt");
        close(fd);
        exit(1);
    }
    filebuf[n] = '\0';
    close(fd);

    char *line = strtok(filebuf, "\n");
    while (line)
    {
        char ipbuf[128];
        int port;
        if (sscanf(line, "%127s %d", ipbuf, &port) == 2)
        {
            trackerPeers.push_back({string(ipbuf), port});
        }
        line = strtok(NULL, "\n");
    }
}

string serialize_state()
{
    stringstream data;
    data << "SYNC\n";
    for (auto const &[uid, pass] : users)
    {
        data << "USER " << uid << " " << pass << "\n";
    }
    for (auto const &[gid, members] : groups)
    {
        data << "GROUP " << gid;
        for (auto const &member : members)
            data << " " << member;
        data << "\n";
    }
    for (auto const &[gid, owner] : groupOwners)
    {
        data << "OWNER " << gid << " " << owner << "\n";
    }
    for (auto const &[gid, requests] : pendingRequests)
    {
        data << "PENDING " << gid;
        for (auto const &uid : requests)
            data << " " << uid;
        data << "\n";
    }
    for (auto const &[key, fm] : files)
    {
        data << "FILE " << fm.group_id << " " << fm.filename << " "
             << fm.filesize << " " << fm.total_pieces;
        for (auto const &hash : fm.piece_hashes)
            data << " " << hash;
        data << "\n";
        data << "SEEDERS " << key;
        for (auto const &seeder : fm.seeders)
            data << " " << seeder;
        data << "\n";
    }
    return data.str();
}

void syncWithPeer(string msg)
{
    if (trackerPeers.size() < 2)
        return;
    int peerIndex = (tracker_no == 1 ? 1 : 0);

    string ip = trackerPeers[peerIndex].first;
    int port = trackerPeers[peerIndex].second;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return;

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port + 100);
    inet_pton(AF_INET, ip.c_str(), &serv.sin_addr);

    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    if (connect(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0)
    {
        close(sockfd);
        return;
    }

    write(sockfd, msg.c_str(), msg.size());
    close(sockfd);
}

void syncHandler(int sockfd)
{
    char buffer[16384];
    int n = read(sockfd, buffer, sizeof(buffer) - 1);
    if (n <= 0)
    {
        close(sockfd);
        return;
    }
    buffer[n] = '\0';

    stringstream ss(buffer);
    string type;
    vector<string> prints;

    lock_guard<mutex> lk(state_mutex);

    while (ss >> type)
    {
        if (type == "USER")
        {
            string uid, pass;
            ss >> uid >> pass;
            if (users.find(uid) == users.end())
            {
                prints.push_back("Received from tracker: User " + uid + " created");
            }
            users[uid] = pass;
        }
        else if (type == "GROUP")
        {
            string gid;
            ss >> gid;
            string rest;
            getline(ss, rest);
            istringstream iss(rest);
            string member;
            set<string> mems;
            while (iss >> member)
            {
                mems.insert(member);
                if (groups.find(gid) == groups.end() || groups[gid].find(member) == groups[gid].end())
                {
                    prints.push_back("Received from tracker: User " + member + " added to group " + gid);
                }
            }
            groups[gid] = mems;
        }
        else if (type == "OWNER")
        {
            string gid, owner;
            ss >> gid >> owner;
            if (groupOwners.find(gid) == groupOwners.end())
            {
                prints.push_back("Received from tracker: Owner " + owner + " for group " + gid);
            }
            groupOwners[gid] = owner;
        }
        else if (type == "PENDING")
        {
            string gid, rest, uid;
            ss >> gid;
            getline(ss, rest);
            set<string> reqs;
            istringstream iss(rest);
            while (iss >> uid)
            {
                reqs.insert(uid);
                if (pendingRequests.find(gid) == pendingRequests.end() ||
                    pendingRequests[gid].find(uid) == pendingRequests[gid].end())
                {
                    prints.push_back("Received from tracker: Pending request from " + uid + " for group " + gid);
                }
            }
            pendingRequests[gid] = reqs;
        }
        else if (type == "FILE")
        {
            FileMetadata fm;
            ss >> fm.group_id >> fm.filename >> fm.filesize >> fm.total_pieces;
            for (int i = 0; i < fm.total_pieces; i++)
            {
                string hash;
                ss >> hash;
                fm.piece_hashes.push_back(hash);
            }
            string key = fm.group_id + "_" + fm.filename;
            if (files.find(key) == files.end())
            {
                files[key] = fm;
            }
            else
            {
                files[key].filesize = fm.filesize;
                files[key].total_pieces = fm.total_pieces;
                files[key].piece_hashes = fm.piece_hashes;
            }
        }
        else if (type == "SEEDERS")
        {
            string key, rest, seeder;
            ss >> key;
            getline(ss, rest);
            istringstream iss(rest);
            if (files.count(key))
            {
                files[key].seeders.clear();
                while (iss >> seeder)
                {
                    files[key].seeders.insert(seeder);
                }
            }
        }
        else
        {
            string skip;
            getline(ss, skip);
        }
    }

    for (auto &p : prints)
    {
        cout << p << endl;
    }
    cout.flush();
    close(sockfd);
}

void syncAcceptLoop(int listen_fd)
{
    while (true)
    {
        int newsock = accept(listen_fd, NULL, NULL);
        if (newsock < 0)
            break;
        thread(syncHandler, newsock).detach();
    }
}

string handleCommand(int clientSock, const string &cmd)
{
    istringstream iss(cmd);
    string a;
    if (!(iss >> a))
        return "Unknown command\n";

    string response_msg;
    string sync_msg;

    {
        lock_guard<mutex> lk(state_mutex);

        if (a == "create_user")
        {
            string uid, pass;
            if (!(iss >> uid >> pass))
            {
                response_msg = "Usage: create_user <user id> <password>\n";
            }
            else if (users.count(uid))
            {
                response_msg = "User already exists\n";
            }
            else
            {
                users[uid] = pass;
                cout << "User created: " << uid << endl;
                cout.flush();
                response_msg = "User created\n";
                sync_msg = serialize_state();
            }
        }
        else if (a == "login")
        {
            string uid, pass;
            if (!(iss >> uid >> pass))
            {
                response_msg = "Usage: login <user id> <password>\n";
            }
            else if (activeSessions.count(clientSock))
            {
                response_msg = "Already logged in\n";
            }
            else
            {
                bool logged_in_elsewhere = false;
                for (auto &p : activeSessions)
                {
                    if (p.second == uid)
                    {
                        logged_in_elsewhere = true;
                        break;
                    }
                }
                if (logged_in_elsewhere)
                {
                    response_msg = "User already logged in\n";
                }
                else if (users.count(uid) && users[uid] == pass)
                {
                    activeSessions[clientSock] = uid;
                    cout << "User logged in: " << uid << endl;
                    cout.flush();
                    response_msg = "Login success\n";
                }
                else
                {
                    response_msg = "Login failed\n";
                }
            }
        }
        else if (a == "logout")
        {
            if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string uid = activeSessions[clientSock];
                activeSessions.erase(clientSock);
                cout << "User logged out: " << uid << endl;
                cout.flush();
                response_msg = "Logout success\n";
            }
        }
        else if (a == "create_group")
        {
            string gid;
            if (!(iss >> gid))
            {
                response_msg = "Usage: create_group <group id>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else if (groups.count(gid))
            {
                response_msg = "Group already exists\n";
            }
            else
            {
                string uid = activeSessions[clientSock];
                groups[gid].insert(uid);
                groupOwners[gid] = uid;
                cout << "Group created: " << gid << " by " << uid << endl;
                cout.flush();
                response_msg = "Group created\n";
                sync_msg = serialize_state();
            }
        }
        else if (a == "join_group")
        {
            string gid;
            if (!(iss >> gid))
            {
                response_msg = "Usage: join_group <group id>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string uid = activeSessions[clientSock];
                if (!groups.count(gid))
                {
                    response_msg = "Group not found\n";
                }
                else if (groups[gid].count(uid))
                {
                    response_msg = "Already a member\n";
                }
                else if (pendingRequests[gid].count(uid))
                {
                    response_msg = "Request already pending\n";
                }
                else
                {
                    pendingRequests[gid].insert(uid);
                    cout << "Join request: " << uid << " -> " << gid << endl;
                    cout.flush();
                    response_msg = "Join request sent\n";
                    sync_msg = serialize_state();
                }
            }
        }
        else if (a == "leave_group")
        {
            string gid;
            if (!(iss >> gid))
            {
                response_msg = "Usage: leave_group <group id>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string uid = activeSessions[clientSock];
                if (!groups.count(gid))
                {
                    response_msg = "Group not found\n";
                }
                else if (!groups[gid].count(uid))
                {
                    response_msg = "Not a member\n";
                }
                else if (groupOwners[gid] == uid)
                {
                    if (groups[gid].size() == 1)
                    {
                        vector<string> keys_to_remove;
                        for (auto const &[key, fm] : files)
                        {
                            if (fm.group_id == gid)
                            {
                                keys_to_remove.push_back(key);
                            }
                        }
                        for (const string &key : keys_to_remove)
                        {
                            files.erase(key);
                        }
                        groups.erase(gid);
                        groupOwners.erase(gid);
                        pendingRequests.erase(gid);
                        cout << "Group " << gid << " deleted (owner left, no members)" << endl;
                        cout.flush();
                        response_msg = "Group deleted\n";
                    }
                    else
                    {
                        groups[gid].erase(uid);
                        string newOwner = *groups[gid].begin();
                        groupOwners[gid] = newOwner;
                        cout << "Ownership transferred: " << gid << " from " << uid << " to " << newOwner << endl;
                        cout.flush();
                        response_msg = "Left group, ownership transferred to " + newOwner + "\n";
                    }
                    sync_msg = serialize_state();
                }
                else
                {
                    groups[gid].erase(uid);
                    cout << "User " << uid << " left group " << gid << endl;
                    cout.flush();
                    response_msg = "Left group\n";
                    sync_msg = serialize_state();
                }
            }
        }
        else if (a == "list_groups")
        {
            response_msg = "Groups:\n";
            for (auto &g : groups)
            {
                response_msg += "  " + g.first + " (Owner: " + groupOwners[g.first] + ")\n";
            }
        }
        else if (a == "list_requests")
        {
            string gid;
            if (!(iss >> gid))
            {
                response_msg = "Usage: list_requests <group id>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string actor = activeSessions[clientSock];
                if (!groups.count(gid))
                {
                    response_msg = "Group not found\n";
                }
                else if (groupOwners[gid] != actor)
                {
                    response_msg = "Permission denied\n";
                }
                else
                {
                    cout << "Owner " << actor << " checked requests for group " << gid << endl;
                    cout.flush();
                    response_msg = "Pending requests for " + gid + ":\n";
                    for (auto &u : pendingRequests[gid])
                    {
                        response_msg += "  " + u + "\n";
                    }
                }
            }
        }
        else if (a == "accept_request")
        {
            string gid, uid;
            if (!(iss >> gid >> uid))
            {
                response_msg = "Usage: accept_request <group id> <user id>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string actor = activeSessions[clientSock];
                if (!groups.count(gid))
                {
                    response_msg = "Group not found\n";
                }
                else if (groupOwners[gid] != actor)
                {
                    response_msg = "Permission denied\n";
                }
                else if (!pendingRequests[gid].count(uid))
                {
                    response_msg = "No such pending request\n";
                }
                else
                {
                    groups[gid].insert(uid);
                    pendingRequests[gid].erase(uid);
                    cout << "Owner " << actor << " accepted " << uid << " into group " << gid << endl;
                    cout.flush();
                    response_msg = "Request accepted\n";
                    sync_msg = serialize_state();
                }
            }
        }
        else if (a == "upload_file")
        {
            string gid, filename, seeder_addr;
            size_t filesize;
            int total_pieces;
            if (!(iss >> gid >> filename >> seeder_addr >> filesize >> total_pieces))
            {
                response_msg = "Usage: upload_file <group_id> <filename> <ip:port> <filesize> <pieces> <hashes...>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string uid = activeSessions[clientSock];
                if (!groups.count(gid) || !groups[gid].count(uid))
                {
                    response_msg = "Not a member of group\n";
                }
                else
                {
                    FileMetadata fm;
                    fm.group_id = gid;
                    fm.filename = filename;
                    fm.filesize = filesize;
                    fm.total_pieces = total_pieces;

                    bool hash_error = false;
                    for (int i = 0; i < total_pieces; i++)
                    {
                        string hash;
                        if (!(iss >> hash))
                        {
                            response_msg = "Insufficient piece hashes\n";
                            hash_error = true;
                            break;
                        }
                        fm.piece_hashes.push_back(hash);
                    }

                    if (!hash_error)
                    {
                        string key = gid + "_" + filename;
                        if (files.count(key))
                        {
                            files[key].seeders.insert(seeder_addr);
                        }
                        else
                        {
                            fm.seeders.insert(seeder_addr);
                            files[key] = fm;
                        }
                        cout << "File uploaded: " << filename << " to group " << gid
                             << " by " << uid << " (Seeder: " << seeder_addr << ")" << endl;
                        cout.flush();
                        response_msg = "File uploaded successfully\n";
                        sync_msg = serialize_state();
                    }
                }
            }
        }
        else if (a == "list_files")
        {
            string gid;
            if (!(iss >> gid))
            {
                response_msg = "Usage: list_files <group id>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string uid = activeSessions[clientSock];
                if (!groups.count(gid) || !groups[gid].count(uid))
                {
                    response_msg = "Not a member of group\n";
                }
                else
                {
                    response_msg = "Files in group " + gid + ":\n";
                    for (auto &f : files)
                    {
                        if (f.second.group_id == gid)
                        {
                            response_msg += "  " + f.second.filename + "\n";
                        }
                    }
                }
            }
        }
        else if (a == "get_file_info")
        {
            string gid, filename;
            if (!(iss >> gid >> filename))
            {
                response_msg = "Usage: get_file_info <group id> <filename>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string uid = activeSessions[clientSock];
                if (!groups.count(gid) || !groups[gid].count(uid))
                {
                    response_msg = "Not a member of group\n";
                }
                else
                {
                    string key = gid + "_" + filename;
                    if (!files.count(key))
                    {
                        response_msg = "File not found\n";
                    }
                    else
                    {
                        FileMetadata &fm = files[key];
                        stringstream ss_resp;
                        ss_resp << "FILEINFO " << fm.filesize << " " << fm.total_pieces;
                        for (auto &h : fm.piece_hashes)
                        {
                            ss_resp << " " << h;
                        }
                        ss_resp << "\nPEERS";
                        for (auto &s : fm.seeders)
                        {
                            ss_resp << " " << s;
                        }
                        ss_resp << "\n";
                        response_msg = ss_resp.str();
                    }
                }
            }
        }
        else if (a == "stop_share")
        {
            string gid, filename, seeder_addr;
            if (!(iss >> gid >> filename >> seeder_addr))
            {
                response_msg = "Usage: stop_share <group id> <filename> <ip:port>\n";
            }
            else if (!activeSessions.count(clientSock))
            {
                response_msg = "Not logged in\n";
            }
            else
            {
                string key = gid + "_" + filename;
                if (!files.count(key))
                {
                    response_msg = "File not found\n";
                }
                else
                {
                    if (files[key].seeders.count(seeder_addr))
                    {
                        files[key].seeders.erase(seeder_addr);
                        cout << "Stop share success: " << filename << " from group "
                             << gid << " by " << seeder_addr << endl;
                        if (files[key].seeders.empty())
                        {
                            files.erase(key);
                            cout << "File metadata removed: " << filename << " from group "
                                 << gid << " (no seeders left)" << endl;
                        }
                        cout.flush();
                        response_msg = "Stopped sharing file\n";
                        sync_msg = serialize_state();
                    }
                    else
                    {
                        response_msg = "Client is not registered as seeder for this file\n";
                    }
                }
            }
        }
        else
        {
            response_msg = "Unknown command\n";
        }
    }

    if (!sync_msg.empty())
    {
        thread(syncWithPeer, sync_msg).detach();
    }

    return response_msg;
}

void clientAcceptLoop(int listen_fd)
{
    while (true)
    {
        int clientSock = accept(listen_fd, NULL, NULL);
        if (clientSock < 0)
            break;

        thread([clientSock]()
               {
    char buffer[262144];  // 256KB buffer
    string accumulated = "";
    
    while (true) {
        int n = read(clientSock, buffer, sizeof(buffer) - 1);
        if (n <= 0) break;
        buffer[n] = '\0';
        
        accumulated += string(buffer);
        
        size_t pos;
        while ((pos = accumulated.find('\n')) != string::npos) {
            string cmd = accumulated.substr(0, pos);
            accumulated.erase(0, pos + 1);
            
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) {
                cmd.pop_back();
            }
            
            if (cmd.empty()) continue;
            
            string response = handleCommand(clientSock, cmd);
            
            if (!response.empty() && response.back() != '\n') {
                response += "\n";
            }
            
            write(clientSock, response.c_str(), response.size());
        }
    }
            
            {
                lock_guard<mutex> lk(state_mutex);
                if (activeSessions.count(clientSock)) {
                    string uid = activeSessions[clientSock];
                    activeSessions.erase(clientSock);
                    cout << "User disconnected: " << uid << endl;
                    cout.flush();
                }
            }
            close(clientSock); })
            .detach();
    }
}

void consoleLoop()
{
    string line;
    while (true)
    {
        cout << "tracker> ";
        cout.flush();
        if (!getline(cin, line))
            break;
        if (line == "quit")
        {
            cout << "Shutting down tracker " << tracker_no << " ..." << endl;
            if (main_listen_fd >= 0)
                close(main_listen_fd);
            if (sync_listen_fd >= 0)
                close(sync_listen_fd);
            exit(0);
        }
        else
        {
            cout << "Unknown console command (type 'quit' to exit)\n";
        }
    }
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
        cerr << "Error: tracker_no must be between 1 and " << trackerPeers.size() << endl;
        return 1;
    }

    string ip = trackerPeers[tracker_no - 1].first;
    int port = trackerPeers[tracker_no - 1].second;

    sync_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sync_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in sync_addr{};
    sync_addr.sin_family = AF_INET;
    sync_addr.sin_port = htons(port + 100);
    sync_addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(sync_listen_fd, (struct sockaddr *)&sync_addr, sizeof(sync_addr)) < 0)
    {
        perror("bind sync");
        return 1;
    }
    if (listen(sync_listen_fd, 5) < 0)
    {
        perror("listen sync");
        return 1;
    }

    cout << "Tracker sync server on port " << (port + 100) << endl;

    main_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(main_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in main_addr{};
    main_addr.sin_family = AF_INET;
    main_addr.sin_port = htons(port);
    main_addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(main_listen_fd, (struct sockaddr *)&main_addr, sizeof(main_addr)) < 0)
    {
        perror("bind main");
        return 1;
    }
    if (listen(main_listen_fd, 5) < 0)
    {
        perror("listen main");
        return 1;
    }

    cout << "Tracker " << tracker_no << " running at " << ip << ":" << port << endl;

    thread(syncAcceptLoop, sync_listen_fd).detach();
    thread(clientAcceptLoop, main_listen_fd).detach();

    consoleLoop();

    if (main_listen_fd >= 0)
        close(main_listen_fd);
    if (sync_listen_fd >= 0)
        close(sync_listen_fd);
    return 0;
}