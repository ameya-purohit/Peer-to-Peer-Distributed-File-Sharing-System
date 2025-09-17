#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

vector<pair<string, int>> trackerPeers;

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

int connectToTracker(int trackerIndex)
{
    string ip = trackerPeers[trackerIndex].first;
    int port = trackerPeers[trackerIndex].second;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }

    sockaddr_in serv;
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    serv.sin_addr.s_addr = inet_addr(ip.c_str());

    if (connect(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0)
    {
        perror("connect");
        close(sockfd);
        return -1;
    }

    cout << "Connected to tracker at " << ip << ":" << port << endl;
    return sockfd;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        cerr << "Usage: ./client trackerinfo.txt\n";
        return 1;
    }

    loadTrackerInfo(argv[1]);

    int sockfd = connectToTracker(0); // connect to first tracker
    if (sockfd < 0)
        return 1;

    while (true)
    {
        cout << "client> ";
        string line;
        if (!getline(cin, line))
            break;

        if (line == "quit")
            break;

        write(sockfd, line.c_str(), line.size());

        char buffer[1024];
        int n = read(sockfd, buffer, sizeof(buffer) - 1);
        if (n > 0)
        {
            buffer[n] = '\0';
            cout << buffer;
        }
    }

    close(sockfd);
    return 0;
}
