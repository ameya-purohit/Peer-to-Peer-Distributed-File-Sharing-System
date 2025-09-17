#include <iostream>
#include <cstring> // strtok, strcpy
#include <cstdio>  // sscanf
#include <vector>
#include <string>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

vector<pair<string, int>> trackerPeers;

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

void loadTrackerInfo(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        perror("open trackerinfo.txt");
        exit(1);
    }

    char buffer[1024];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    if (n <= 0)
    {
        perror("read trackerinfo.txt");
        close(fd);
        exit(1);
    }
    buffer[n] = '\0';
    close(fd);

    // Parse lines "ip port"
    char *line = strtok(buffer, "\n");
    while (line)
    {
        char ipbuf[64];
        int port;
        if (sscanf(line, "%s %d", ipbuf, &port) == 2)
        {
            trackerPeers.push_back({string(ipbuf), port});
        }
        line = strtok(NULL, "\n");
    }

    if (trackerPeers.empty())
    {
        cerr << "Error: trackerinfo.txt is empty\n";
        exit(1);
    }
}

int connectToTracker(int trackerIndex)
{
    if (trackerIndex >= (int)trackerPeers.size())
    {
        cerr << "Error: tracker index out of range\n";
        return -1;
    }

    string ip = trackerPeers[trackerIndex].first;
    int port = trackerPeers[trackerIndex].second;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &serv.sin_addr) <= 0)
    {
        cerr << "Invalid IP: " << ip << endl;
        close(sockfd);
        return -1;
    }

    cout << "Trying to connect to " << ip << ":" << port << " ..." << endl;
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
