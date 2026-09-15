#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <sstream>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <openssl/sha.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <random>
#include <memory>

using namespace std;

const size_t PIECE_SIZE = 512 * 1024;
const int MAX_DOWNLOAD_THREADS = 4;
const int SOCKET_TIMEOUT_SEC = 5;
const int MAX_RETRIES = 3;

struct FileInfo
{
    string filename, filepath;
    size_t filesize;
    int total_pieces;
    vector<string> piece_hashes;
    vector<bool> pieces_available;
};

struct PeerInfo
{
    string ip, ip_port_str;
    int port;
};

struct DownloadTask
{
    string group_id, filename, dest_path;
    vector<PeerInfo> peers;
    FileInfo file_info;
    vector<bool> downloaded_pieces;
    mutex piece_mutex;
    int pieces_downloaded;
    bool completed;
    bool should_stop;

    ~DownloadTask() = default;
};

// Global State
vector<pair<string, int>> trackerPeers;
map<string, FileInfo> shared_files;
map<string, shared_ptr<DownloadTask>> active_downloads;
mutex downloads_mutex, shared_files_mutex, queue_mutex, tracker_mutex;
condition_variable queue_cv;
queue<pair<shared_ptr<DownloadTask>, int>> download_queue;
bool shutdown_workers = false;

string client_ip;
int client_port, tracker_sockfd = -1, p2p_listen_fd = -1;
bool is_logged_in = false;

// Utility Functions
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

    if (trackerPeers.empty())
    {
        cerr << "Error: trackerinfo.txt is empty or invalid\n";
        exit(1);
    }
}

string sha1_hash(const unsigned char *data, size_t len)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(data, len, hash);
    char hex[SHA_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
    {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }
    return string(hex);
}

FileInfo calculate_file_hashes(const string &filepath)
{
    FileInfo info;
    info.filepath = filepath;
    size_t last_slash = filepath.find_last_of('/');
    info.filename = (last_slash != string::npos) ? filepath.substr(last_slash + 1) : filepath;

    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0)
        throw runtime_error("Cannot open file: " + string(strerror(errno)));

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        close(fd);
        throw runtime_error("Error getting file status");
    }

    info.filesize = st.st_size;
    info.total_pieces = (info.filesize + PIECE_SIZE - 1) / PIECE_SIZE;

    unsigned char *buffer = new unsigned char[PIECE_SIZE];
    for (int i = 0; i < info.total_pieces; ++i)
    {
        lseek(fd, i * PIECE_SIZE, SEEK_SET);
        ssize_t to_read = (i == info.total_pieces - 1) ? (info.filesize - (i * PIECE_SIZE)) : PIECE_SIZE;
        ssize_t n = read(fd, buffer, to_read);
        if (n != to_read)
        {
            close(fd);
            delete[] buffer;
            throw runtime_error("Error reading file");
        }
        info.piece_hashes.push_back(sha1_hash(buffer, n));
        info.pieces_available.push_back(true);
    }
    close(fd);
    delete[] buffer;
    return info;
}

bool parseIpPort(const string &arg, string &ip, int &port)
{
    size_t colon = arg.find(':');
    if (colon == string::npos)
        return false;
    ip = arg.substr(0, colon);
    try
    {
        port = stoi(arg.substr(colon + 1));
    }
    catch (...)
    {
        return false;
    }
    return true;
}

bool parse_peer_info(const string &ip_port_str, PeerInfo &peer)
{
    size_t colon = ip_port_str.find(':');
    if (colon == string::npos)
        return false;
    peer.ip = ip_port_str.substr(0, colon);
    peer.ip_port_str = ip_port_str;
    try
    {
        peer.port = stoi(ip_port_str.substr(colon + 1));
    }
    catch (...)
    {
        return false;
    }
    return true;
}

// Network Communication
int connectTo(const string &ip, int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return -1;

    struct timeval timeout;
    timeout.tv_sec = SOCKET_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &serv.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0)
    {
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int findAndConnectToTracker()
{
    for (auto &tracker : trackerPeers)
    {
        int new_sockfd = connectTo(tracker.first, tracker.second);
        if (new_sockfd >= 0)
        {
            cout << "Connected to tracker at " << tracker.first << ":" << tracker.second << endl;
            return new_sockfd;
        }
    }
    return -1;
}

string send_tracker_command(const string &cmd)
{
    lock_guard<mutex> lk(tracker_mutex);

    for (int i = 0; i < MAX_RETRIES; ++i)
    {
        if (tracker_sockfd < 0)
        {
            tracker_sockfd = findAndConnectToTracker();
            if (tracker_sockfd < 0)
            {
                return "ERROR: Could not connect to any tracker.\n";
            }
        }

        string sendline = cmd + "\n";
        size_t total_sent = 0;
        while (total_sent < sendline.size())
        {
            ssize_t sent = write(tracker_sockfd, sendline.c_str() + total_sent,
                                 sendline.size() - total_sent);
            if (sent < 0)
            {
                close(tracker_sockfd);
                tracker_sockfd = -1;
                break;
            }
            total_sent += sent;
        }
        if (tracker_sockfd < 0)
            continue;

        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(tracker_sockfd, &readfds);
        timeout.tv_sec = SOCKET_TIMEOUT_SEC;
        timeout.tv_usec = 0;

        int activity = select(tracker_sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (activity <= 0)
        {
            close(tracker_sockfd);
            tracker_sockfd = -1;
            continue;
        }

        char buffer[262144];
        int n = read(tracker_sockfd, buffer, sizeof(buffer) - 1);
        if (n > 0)
        {
            buffer[n] = '\0';
            return string(buffer);
        }
        else
        {
            close(tracker_sockfd);
            tracker_sockfd = -1;
        }
    }
    return "ERROR: Failed to execute command after retries.\n";
}

// P2P and Download Logic
void handle_peer_request(int peer_sock)
{
    char buffer[1024];
    int n = read(peer_sock, buffer, sizeof(buffer) - 1);
    if (n <= 0)
    {
        close(peer_sock);
        return;
    }
    buffer[n] = '\0';

    stringstream ss(buffer);
    string cmd, group_id, filename;
    int piece_num;
    ss >> cmd >> group_id >> filename >> piece_num;

    if (cmd == "GET_PIECE")
    {
        string key = group_id + "_" + filename;
        lock_guard<mutex> lk(shared_files_mutex);

        if (shared_files.count(key) &&
            piece_num >= 0 &&
            piece_num < (int)shared_files[key].pieces_available.size() &&
            shared_files[key].pieces_available[piece_num])
        {

            FileInfo &finfo = shared_files[key];
            int fd = open(finfo.filepath.c_str(), O_RDONLY);
            if (fd >= 0)
            {
                off_t offset = (off_t)piece_num * PIECE_SIZE;
                size_t to_read = (piece_num == finfo.total_pieces - 1) ? (finfo.filesize - offset) : PIECE_SIZE;
                unsigned char *piece_data = new unsigned char[to_read];

                lseek(fd, offset, SEEK_SET);
                ssize_t bytes_read = read(fd, piece_data, to_read);
                close(fd);

                if (bytes_read == (ssize_t)to_read)
                {
                    write(peer_sock, piece_data, bytes_read);
                }
                delete[] piece_data;
            }
        }
    }
    close(peer_sock);
}

void p2p_accept_loop()
{
    while (!shutdown_workers)
    {
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(p2p_listen_fd, &readfds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(p2p_listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (activity > 0)
        {
            int peer_sock = accept(p2p_listen_fd, NULL, NULL);
            if (peer_sock >= 0)
            {
                thread(handle_peer_request, peer_sock).detach();
            }
        }
    }
}

void download_piece_worker()
{
    while (true)
    {
        pair<shared_ptr<DownloadTask>, int> task;
        {
            unique_lock<mutex> lk(queue_mutex);
            queue_cv.wait(lk, []
                          { return !download_queue.empty() || shutdown_workers; });

            if (shutdown_workers && download_queue.empty())
                break;
            if (download_queue.empty())
                continue;

            task = download_queue.front();
            download_queue.pop();
        }

        auto dtask = task.first;
        int piece_num = task.second;

        if (!dtask || dtask->should_stop)
            continue;

        bool success = false;

        // Check if already downloaded
        {
            lock_guard<mutex> lk(dtask->piece_mutex);
            if (dtask->downloaded_pieces[piece_num])
            {
                success = true;
                continue;
            }
        }

        vector<PeerInfo> shuffled_peers = dtask->peers;
        random_device rd;
        mt19937 g(rd());
        shuffle(shuffled_peers.begin(), shuffled_peers.end(), g);

        for (auto &peer : shuffled_peers)
        {
            if (dtask->should_stop)
                break;

            {
                lock_guard<mutex> lk(dtask->piece_mutex);
                if (dtask->downloaded_pieces[piece_num])
                {
                    success = true;
                    break;
                }
            }

            int peer_sock = connectTo(peer.ip, peer.port);
            if (peer_sock < 0)
                continue;

            string request = "GET_PIECE " + dtask->group_id + " " +
                             dtask->filename + " " + to_string(piece_num) + "\n";

            if (write(peer_sock, request.c_str(), request.size()) < 0)
            {
                close(peer_sock);
                continue;
            }

            size_t piece_size = (piece_num == dtask->file_info.total_pieces - 1) ? (dtask->file_info.filesize - ((off_t)piece_num * PIECE_SIZE)) : PIECE_SIZE;

            unsigned char *piece_data = new unsigned char[piece_size];
            ssize_t total_read = 0;

            while (total_read < (ssize_t)piece_size)
            {
                ssize_t r = read(peer_sock, piece_data + total_read, piece_size - total_read);
                if (r <= 0)
                    break;
                total_read += r;
            }

            if (sha1_hash(piece_data, piece_size) == dtask->file_info.piece_hashes[piece_num])
            {
                int fd = open(dtask->dest_path.c_str(), O_WRONLY);
                if (fd >= 0)
                {
                    lseek(fd, (off_t)piece_num * PIECE_SIZE, SEEK_SET);
                    write(fd, piece_data, piece_size);
                    close(fd);

                    {
                        lock_guard<mutex> lk(dtask->piece_mutex);
                        if (!dtask->downloaded_pieces[piece_num])
                        {
                            dtask->downloaded_pieces[piece_num] = true;
                            dtask->pieces_downloaded++;

                            int progress = (dtask->pieces_downloaded * 100) / dtask->file_info.total_pieces;
                            cout << "\n[DOWNLOAD] Piece " << piece_num << "/"
                                 << (dtask->file_info.total_pieces - 1)
                                 << " of " << dtask->filename
                                 << " from peer " << peer.ip << ":" << peer.port
                                 << " [" << progress << "% complete]" << endl;

                            if (dtask->pieces_downloaded == dtask->file_info.total_pieces)
                            {
                                dtask->completed = true;
                                cout << "\n"
                                     << string(60, '=') << endl;
                                cout << "[C] [" << dtask->group_id << "] "
                                     << dtask->filename << " - Download Complete!" << endl;
                                cout << string(60, '=') << endl;

                                // Register as seeder after successful download
                                string key = dtask->group_id + "_" + dtask->filename;
                                FileInfo seeder_info;
                                seeder_info.filename = dtask->filename;
                                seeder_info.filepath = dtask->dest_path;
                                seeder_info.filesize = dtask->file_info.filesize;
                                seeder_info.total_pieces = dtask->file_info.total_pieces;
                                seeder_info.piece_hashes = dtask->file_info.piece_hashes;
                                seeder_info.pieces_available.resize(dtask->file_info.total_pieces, true);

                                {
                                    lock_guard<mutex> lk(shared_files_mutex);
                                    shared_files[key] = seeder_info;
                                }

                                // Notify tracker
                                string tracker_cmd = "upload_file " + dtask->group_id + " " + dtask->filename + " " +
                                                     client_ip + ":" + to_string(client_port) + " " +
                                                     to_string(seeder_info.filesize) + " " + to_string(seeder_info.total_pieces);

                                for (auto &h : seeder_info.piece_hashes)
                                {
                                    tracker_cmd += " " + h;
                                }

                                string register_response = send_tracker_command(tracker_cmd);
                                if (register_response.find("success") != string::npos)
                                {
                                    cout << "Registered as seeder for " << dtask->filename << endl;
                                }
                            }
                            cout << "client> " << flush;
                        }
                    }
                    success = true;
                }
            }

            delete[] piece_data;
            close(peer_sock);

            if (success)
                break;
        }

        if (!success && !dtask->should_stop)
        {
            lock_guard<mutex> lk(queue_mutex);
            download_queue.push({dtask, piece_num});
            queue_cv.notify_one();
        }
    }
}

void start_download(const string &group_id, const string &filename, const string &dest_path)
{
    string cmd = "get_file_info " + group_id + " " + filename;
    string response = send_tracker_command(cmd);
    cout << "DEBUG: Tracker response:\n"
         << response << "\nEND DEBUG\n"
         << flush;
    if (response.find("ERROR") != string::npos ||
        response.find("File not found") != string::npos ||
        response.find("Not a member") != string::npos)
    {
        cout << response;
        return;
    }

    auto dtask = make_shared<DownloadTask>();
    dtask->group_id = group_id;
    dtask->filename = filename;
    dtask->dest_path = dest_path;
    dtask->pieces_downloaded = 0;
    dtask->completed = false;
    dtask->should_stop = false;

    stringstream ss(response);
    string line;
    bool found_fileinfo = false;
    bool found_peers = false;

    // Parses response line by line
    while (getline(ss, line))
    {
        if (line.find("FILEINFO") != string::npos)
        {
            found_fileinfo = true;
            stringstream line_ss(line);
            string token;
            line_ss >> token >> dtask->file_info.filesize >> dtask->file_info.total_pieces;
            dtask->file_info.filename = filename;
            dtask->downloaded_pieces.resize(dtask->file_info.total_pieces, false);

            string hash;
            while (line_ss >> hash)
            {
                dtask->file_info.piece_hashes.push_back(hash);
            }
        }
        else if (line.find("PEERS") != string::npos)
        {
            found_peers = true;
            stringstream line_ss(line);
            string token, peer_str;
            line_ss >> token;

            while (line_ss >> peer_str)
            {
                PeerInfo p;
                if (parse_peer_info(peer_str, p))
                {
                    dtask->peers.push_back(p);
                }
            }
        }
    }

    if (!found_fileinfo || !found_peers)
    {
        cout << "ERROR: Invalid tracker response format\n";
        return;
    }

    if (dtask->peers.empty())
    {
        cout << "ERROR: No seeders found\n";
        return;
    }

    // Create destination file
    int fd = open(dest_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
    {
        perror("open dest file");
        return;
    }

    if (ftruncate(fd, dtask->file_info.filesize) < 0)
    {
        perror("ftruncate");
        close(fd);
        return;
    }
    close(fd);

    string key = group_id + "_" + filename;
    {
        lock_guard<mutex> lk(downloads_mutex);
        active_downloads[key] = dtask;
    }

    // Queue all pieces for download
    {
        lock_guard<mutex> lk(queue_mutex);
        for (int i = 0; i < dtask->file_info.total_pieces; i++)
        {
            download_queue.push({dtask, i});
        }
    }
    queue_cv.notify_all();

    cout << "Download started for " << filename << endl;
}

// Main Loop
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cerr << "Usage: ./client <IP>:<PORT> trackerinfo.txt\n";
        return 1;
    }

    if (!parseIpPort(argv[1], client_ip, client_port))
    {
        cerr << "Invalid client address. Use <IP>:<PORT>\n";
        return 1;
    }

    loadTrackerInfo(argv[2]);

    // Setup P2P listening
    p2p_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(p2p_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(client_port);
    addr.sin_addr.s_addr = inet_addr(client_ip.c_str());

    if (bind(p2p_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind p2p");
        return 1;
    }

    listen(p2p_listen_fd, 10);

    // Start worker threads
    thread(p2p_accept_loop).detach();
    for (int i = 0; i < MAX_DOWNLOAD_THREADS; i++)
    {
        thread(download_piece_worker).detach();
    }

    cout << "P2P client listening on " << client_ip << ":" << client_port << endl;

    // Connect to tracker
    tracker_sockfd = findAndConnectToTracker();
    if (tracker_sockfd < 0)
    {
        cerr << "Could not connect to any tracker\n";
        return 1;
    }

    // Main command loop
    while (true)
    {
        cout << "client> ";
        string line;
        if (!getline(cin, line))
            break;

        if (line.empty())
            continue;
        if (line == "quit")
            break;

        istringstream iss(line);
        string cmd_token;
        iss >> cmd_token;

        string response;

        if (cmd_token == "login")
        {
            response = send_tracker_command(line);
            if (response.find("success") != string::npos)
            {
                is_logged_in = true;
            }
        }
        else if (cmd_token == "logout")
        {
            response = send_tracker_command(line);
            is_logged_in = false;
        }
        else if (cmd_token == "upload_file")
        {
            if (!is_logged_in)
            {
                cout << "Please login first.\n";
                continue;
            }

            string group_id, filepath;
            if (!(iss >> group_id >> filepath))
            {
                cout << "Usage: upload_file <group_id> <filepath>\n";
                continue;
            }

            try
            {
                FileInfo finfo = calculate_file_hashes(filepath);
                string key = group_id + "_" + finfo.filename;

                {
                    lock_guard<mutex> lk(shared_files_mutex);
                    shared_files[key] = finfo;
                }

                string tracker_cmd = "upload_file " + group_id + " " + finfo.filename + " " +
                                     client_ip + ":" + to_string(client_port) + " " +
                                     to_string(finfo.filesize) + " " + to_string(finfo.total_pieces);

                for (auto &h : finfo.piece_hashes)
                {
                    tracker_cmd += " " + h;
                }

                response = send_tracker_command(tracker_cmd);
            }
            catch (const exception &e)
            {
                cout << "Error: " << e.what() << endl;
                continue;
            }
        }
        else if (cmd_token == "download_file")
        {
            if (!is_logged_in)
            {
                cout << "Please login first.\n";
                continue;
            }

            string group_id, filename, dest_path;
            if (!(iss >> group_id >> filename >> dest_path))
            {
                cout << "Usage: download_file <group_id> <filename> <dest_path>\n";
                continue;
            }

            start_download(group_id, filename, dest_path);
            continue;
        }
        else if (cmd_token == "show_downloads")
        {
            lock_guard<mutex> lk(downloads_mutex);

            if (active_downloads.empty())
            {
                cout << "No active downloads\n";
                continue;
            }

            cout << "\nActive Downloads:\n";
            cout << string(60, '-') << endl;

            for (auto const &[key, dt] : active_downloads)
            {
                int progress = (dt->file_info.total_pieces == 0) ? 100 : (dt->pieces_downloaded * 100) / dt->file_info.total_pieces;

                cout << "[" << (dt->completed ? "C" : "D") << "] "
                     << dt->filename << " (" << progress << "%)" << endl;
                cout << "    Group: " << dt->group_id << " | Progress: "
                     << dt->pieces_downloaded << "/" << dt->file_info.total_pieces
                     << " pieces" << endl;
            }
            cout << string(60, '-') << endl;
            continue;
        }
        else if (cmd_token == "stop_share")
        {
            if (!is_logged_in)
            {
                cout << "Please login first.\n";
                continue;
            }

            string group_id, filename;
            if (!(iss >> group_id >> filename))
            {
                cout << "Usage: stop_share <group_id> <filename>\n";
                continue;
            }

            string key = group_id + "_" + filename;
            {
                lock_guard<mutex> lk(shared_files_mutex);
                if (shared_files.count(key))
                {
                    shared_files.erase(key);
                }
            }

            string tracker_cmd = "stop_share " + group_id + " " + filename + " " +
                                 client_ip + ":" + to_string(client_port);
            response = send_tracker_command(tracker_cmd);
        }
        else
        {
            response = send_tracker_command(line);
        }

        cout << response;
    }

    // Cleanup
    cout << "Goodbye!\n";
    shutdown_workers = true;
    queue_cv.notify_all();

    if (tracker_sockfd >= 0)
        close(tracker_sockfd);
    if (p2p_listen_fd >= 0)
        close(p2p_listen_fd);

    return 0;
}