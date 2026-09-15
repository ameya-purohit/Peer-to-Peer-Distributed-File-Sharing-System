# P2P File Sharing System with Group Management

A distributed peer-to-peer file sharing system with synchronized tracker servers, supporting multi-threaded downloads, automatic seeding, and group-based access control.

## Overview

This project implements a P2P file sharing ecosystem where multiple tracker servers maintain synchronized state while clients can create user accounts, form groups, and share files within those groups. Files are transferred using a piece-based protocol similar to BitTorrent, with automatic integrity verification and parallel downloads from multiple seeders.

**System Capabilities:**
- Multi-threaded parallel downloads (4 worker threads)
- 512KB piece-based file transfers with SHA-1 verification
- Automatic post-download seeding registration
- Fault-tolerant tracker synchronization
- Group-based access control and permissions
- Real-time state synchronization between trackers
- Tested and verified with files up to 1GB in size

## Project Structure

```
Peer-to-Peer-Distributed-File-Sharing-System/
├── client/
│   ├── client.cpp          # P2P client implementation
│   ├── Makefile           # Client build configuration
│   └── trackerinfo.txt    # Tracker server addresses
├── tracker/
│   ├── tracker.cpp        # Tracker server implementation
│   ├── Makefile          # Tracker build configuration
│   └── trackerinfo.txt   # Tracker server addresses
└── README.md             # This file
```

## Prerequisites

- Linux environment (Ubuntu 20.04+ recommended)
- g++ compiler with C++11 support
- OpenSSL development libraries
- Make build system

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install build-essential libssl-dev
```

## Building the System

### Compile Tracker
```bash
cd tracker
make clean && make
```

### Compile Client
```bash
cd client
make clean && make
```

## Configuration

Create `trackerinfo.txt` in both `client/` and `tracker/` directories:

```
127.0.0.1 5000
127.0.0.1 5001
```

Format: `<IP_ADDRESS> <PORT>` (one tracker per line)

The tracker will listen on:
- Main port (specified): Client connections
- Sync port (main + 100): Inter-tracker synchronization

## Running the System

### Start Tracker Servers

```bash
# Terminal 1: Start first tracker
cd tracker
./tracker trackerinfo.txt 1

# Terminal 2: Start second tracker
cd tracker
./tracker trackerinfo.txt 2
```

The tracker number (1, 2, etc.) corresponds to the line number in `trackerinfo.txt`.

### Start Clients

```bash
# Terminal 3: Start first client
cd client
./client 127.0.0.1:6000 trackerinfo.txt

# Terminal 4: Start second client (for P2P testing)
cd client
./client 127.0.0.1:6001 trackerinfo.txt
```

Format: `./client <CLIENT_IP>:<CLIENT_PORT> trackerinfo.txt`

The IP:PORT combination specifies where this client will listen for incoming P2P connections. The client automatically connects to the first available tracker from `trackerinfo.txt`.

## Commands

### User Management

| Command | Description | Example |
|---------|-------------|---------|
| `create_user <userid> <password>` | Register new user account | `create_user alice pass123` |
| `login <userid> <password>` | Authenticate and start session | `login alice pass123` |
| `logout` | End current session | `logout` |

### Group Management

| Command | Description | Example |
|---------|-------------|---------|
| `create_group <groupid>` | Create new group (you become owner) | `create_group mygroup` |
| `join_group <groupid>` | Request to join existing group | `join_group mygroup` |
| `leave_group <groupid>` | Leave a group you're member of | `leave_group mygroup` |
| `list_groups` | Display all available groups | `list_groups` |

### Group Owner Commands

| Command | Description | Example |
|---------|-------------|---------|
| `list_requests <groupid>` | Show pending join requests (owner only) | `list_requests mygroup` |
| `accept_request <groupid> <userid>` | Accept user's join request (owner only) | `accept_request mygroup bob` |

### File Operations

| Command | Description | Example |
|---------|-------------|---------|
| `upload_file <groupid> <filepath>` | Share file in group (automatic seeding) | `upload_file mygroup /tmp/file.txt` |
| `list_files <groupid>` | List all files in group | `list_files mygroup` |
| `download_file <groupid> <filename> <dest>` | Download file to destination | `download_file mygroup file.txt /tmp/downloaded.txt` |
| `show_downloads` | Show active/completed downloads | `show_downloads` |
| `stop_share <groupid> <filename>` | Stop seeding a file | `stop_share mygroup file.txt` |

### System Commands

| Command | Description |
|---------|-------------|
| `quit` | Exit client |

## Example Usage

### Scenario: Two users sharing a file

**Client 1 (Alice - Uploader):**
```bash
client> create_user alice pass123
User created

client> login alice pass123
Login success

client> create_group research
Group created

client> upload_file research /home/alice/dataset.csv
File uploaded successfully

client> list_files research
Files in group research:
  dataset.csv
```

**Client 2 (Bob - Downloader):**
```bash
client> create_user bob secret456
User created

client> login bob secret456
Login success

client> join_group research
Join request sent
```

**Alice accepts Bob's request:**
```bash
client> list_requests research
Pending requests for research:
  bob

client> accept_request research bob
Request accepted
```

**Bob downloads the file:**
```bash
client> download_file research dataset.csv /home/bob/data.csv
Download started for dataset.csv

[DOWNLOAD] Piece 0/9 of dataset.csv from peer 127.0.0.1:6000 [10% complete]
[DOWNLOAD] Piece 1/9 of dataset.csv from peer 127.0.0.1:6000 [20% complete]
...
============================================================
[C] [research] dataset.csv - Download Complete!
============================================================
Registered as seeder for dataset.csv

client> show_downloads
Active Downloads:
------------------------------------------------------------
[C] dataset.csv (100%)
    Group: research | Progress: 10/10 pieces
------------------------------------------------------------
```

## Technical Architecture

### Components

**Tracker Server (`tracker.cpp`)**
- Maintains user credentials and session state
- Manages group memberships and permissions
- Stores file metadata (hashes, piece counts, seeder addresses)
- Synchronizes state with peer trackers
- Listens on two ports: main (clients) and sync (peer trackers)

**Client Application (`client.cpp`)**
- Dual-mode operation: acts as both downloader and seeder
- Implements P2P file transfer protocol
- Uses worker thread pool for parallel downloads
- Automatically registers as seeder after successful download
- Maintains persistent connection to tracker with automatic failover

### File Transfer Protocol

**Piece-based Transfer:**
- Files divided into 512KB pieces
- Each piece independently hashed with SHA-1
- Pieces downloaded in parallel from multiple seeders
- Hash verification before writing to disk
- Failed pieces automatically retried

**Download Flow:**
1. Client requests file metadata from tracker
2. Tracker responds with file size, piece count, hashes, and seeder list
3. Client creates destination file and queues all pieces
4. Worker threads request pieces from random seeders
5. Each piece: download → verify hash → write to disk
6. On completion: register as seeder with tracker

### Performance Characteristics

**Throughput** (100Mbps network):
- Small files (<1MB): 50-200ms
- Medium files (1-100MB): 8-12 MB/s
- Large files (100MB-1GB): 10-12 MB/s (sustained)

Testing has been conducted with files ranging from 512KB to 1GB. The system maintains consistent throughput across this range, with transfer times scaling linearly with file size.

**Resource Usage**:
- Memory: 15-25MB per active download, remains constant regardless of file size
- CPU: 15-25% during active transfers
- Network: 80-95% utilization

**Scalability**:
- Linear performance scaling verified from 1MB to 1GB
- Efficiently manages up to 2,048 pieces (1GB file)
- Supports 10+ concurrent downloads
- Performance with multiple seeders limited by network bandwidth rather than implementation

### Design Decisions

**1. Why 512KB pieces?**
- Balances metadata overhead with transfer granularity
- A 1GB file results in 2,048 pieces, which is manageable
- Reduces impact of packet loss on large transfers
- Enables potential for download resumption at piece boundaries

**2. Why SHA-1 instead of SHA-256?**
- Faster computation (~2x speed)
- Collision resistance sufficient for file integrity verification
- Industry standard (BitTorrent protocol uses SHA-1)
- Threat model focuses on accidental corruption, not adversarial attacks

**3. Why 4 worker threads?**
- Empirically optimal for 100Mbps+ networks
- Balances CPU, disk I/O, and network I/O
- Prevents excessive context switching overhead
- Tested configurations with more threads showed diminishing returns

**4. Why full-state tracker sync?**
- Simplicity: No complex differential synchronization logic
- Small state size: User/group metadata typically under 1MB
- Consistency: Guarantees eventual consistency between trackers
- Fault tolerance: New trackers can sync complete state immediately

**5. Why text-based protocol?**
- Human-readable for debugging and development
- Simple parsing using standard string operations
- Easy to implement in any programming language
- Extensible without versioning concerns

### Network Protocol

**Client to Tracker:**
```
Command: login alice pass123\n
Response: Login success\n
```

**Client to Peer (P2P):**
```
Request: GET_PIECE group1 file.txt 5\n
Response: <raw binary piece data>
```

**Tracker to Tracker (Sync):**
```
SYNC\n
USER alice pass123\n
GROUP research alice bob\n
OWNER research alice\n
FILE research dataset.csv 5242880 10 hash1 hash2 ...\n
SEEDERS research_dataset.csv 192.168.1.5:6000\n
```

## Troubleshooting

### Common Issues

**"Address already in use"**
```bash
# Wait 30-60 seconds for TIME_WAIT state to clear, or:
sudo lsof -ti:5000 | xargs kill -9  # Replace 5000 with your port
```

**"Could not connect to any tracker"**
- Verify trackers are running: `ps aux | grep tracker`
- Check trackerinfo.txt has correct IP addresses and ports
- Ensure firewall allows connections: `sudo ufw allow 5000:5001/tcp`

**"Permission denied" on commands**
- Ensure you're logged in first with `login` command
- For owner commands, verify you created or own the group

**"File not found" during upload**
- Use absolute paths: `/home/user/file.txt`
- Check file permissions: `ls -la /path/to/file`
- Verify file exists and is readable

**Downloads stuck or slow**
- Verify seeders are online and reachable
- Check network connectivity: `ping <peer_ip>`
- Ensure P2P port is not blocked by firewall
- Check `show_downloads` to monitor progress

**Tracker sync not working**
- Both trackers must be running
- Verify sync port (main_port + 100) is accessible
- Look for "Received from tracker:" messages in tracker console output

## Performance Testing

**Benchmark download speed:**
```bash
# Create test files of various sizes
dd if=/dev/urandom of=/tmp/testfile100 bs=1M count=100
dd if=/dev/urandom of=/tmp/testfile1gb bs=1M count=1024

# Upload and measure
time upload_file testgroup /tmp/testfile100
time upload_file testgroup /tmp/testfile1gb

# Download from another client and measure
time download_file testgroup testfile100 /tmp/downloaded100
time download_file testgroup testfile1gb /tmp/downloaded1gb
```

**Expected results** (100Mbps network):
- 100MB file: 8-12 seconds
- 1GB file: 85-95 seconds
- Throughput: 10-12 MB/s
- CPU usage: 15-25%
- Memory usage: 20-25MB (constant across file sizes)

## System Behavior

### Session Management
- Only one session per user allowed system-wide
- Attempting login from second client returns "User already logged in"
- Logout required before logging in from different client
- Session maintained via persistent TCP connection to tracker

### State Synchronization
- All state changes propagate to peer trackers asynchronously
- Full state snapshot sent on each change (create user, join group, etc.)
- Eventual consistency model: changes may not be immediate on peer tracker
- No data loss if both trackers remain running

### File Lifecycle

**Upload:**
1. Client reads file and calculates SHA-1 hash for each piece
2. Metadata sent to tracker (filename, size, piece count, hashes, seeder address)
3. Tracker stores metadata and syncs with peer tracker
4. Client begins listening for incoming P2P requests

**Download:**
1. Client queries tracker for file metadata
2. Receives: size, piece count, hashes, list of seeders
3. Creates empty destination file with correct size
4. All pieces queued for download
5. Worker threads request pieces from seeders in parallel
6. Each piece verified before writing to disk
7. On completion: client automatically registers as seeder

**Stop Sharing:**
1. Client sends stop_share command to tracker
2. Tracker removes client from seeder list
3. If last seeder: file metadata deleted from tracker
4. Client stops responding to P2P requests for that file

## Security Considerations

**Current Implementation:**
- Passwords stored in plaintext (educational purposes)
- No encryption on network transfers
- No authentication between peers
- Trust-based system

**For Production Deployment:**
- Implement password hashing (bcrypt/argon2)
- Add TLS/SSL encryption for all connections
- Implement peer authentication tokens
- Add rate limiting and DoS protection
- Input validation and sanitization

## Advanced Usage

### Running on Different Machines

**Setup tracker on server (192.168.1.10):**
```bash
# Edit trackerinfo.txt
192.168.1.10 5000
192.168.1.11 5001

# Start tracker
./tracker trackerinfo.txt 1
```

**Connect client from remote machine:**
```bash
# Use same trackerinfo.txt
# Start client with machine's actual IP
./client 192.168.1.20:6000 trackerinfo.txt
```

### Port Forwarding for NAT

If clients are behind NAT/firewall:
```bash
# Forward external port to internal client port
# Example: Router external 9000 → internal 192.168.1.100:6000
```

### Multi-Group File Organization

```bash
# Organize files across different groups
create_group work
create_group personal
create_group research

upload_file work project_code.zip
upload_file personal vacation_photos.tar.gz
upload_file research dataset.csv

# Users see only files from their groups
```

## Monitoring

### Tracker Console
Monitor real-time activity:
```
User created: alice
Group created: research by alice
File uploaded: dataset.csv to group research by alice (Seeder: 127.0.0.1:6000)
Received from tracker: User bob created
```

### Network Analysis
```bash
# Monitor connections
netstat -tn | grep -E '5000|5001|6000'

# Capture traffic
sudo tcpdump -i lo -n port 6000 -A

# Monitor bandwidth
iftop -i lo
```

## Assumptions

- All tracker and client machines are reachable over IPv4; no IPv6 support is assumed or implemented.
- Clients and trackers run on a trusted local network; no malicious peers or man-in-the-middle actors are assumed (see Security Considerations).
- Each client is publicly reachable at the IP:PORT it registers with the tracker (no NAT traversal is performed).
- A user is logged into at most one client session at a time system-wide.
- Group names and usernames are unique across the system.
- The two trackers are configured with consistent, matching `trackerinfo.txt` files and are reachable from each other for synchronization.
- Files shared within a group remain accessible at their original path on the seeder's filesystem until `stop_share` is called or the seeder disconnects.
- Disk space on both client and tracker machines is sufficient for the files being shared/downloaded (up to 1GB per file, as tested).

## Limitations

**Current Limitations:**
- No NAT traversal (requires public IPs or port forwarding)
- No resume support (must restart failed downloads)
- No bandwidth throttling
- No piece selection optimization (random selection)
- Text protocol has higher overhead than binary
- Single tracker connection (no client-side load balancing)

**Potential Enhancements:**
- Implement download resume capability
- Add web-based user interface
- Support NAT traversal (STUN/TURN)
- Add TLS encryption
- Implement distributed hash table (DHT) for trackerless operation
- Optimize piece selection (rarest-first algorithm)

## Additional Resources

**Related Technologies:**
- BitTorrent Protocol: Industry-standard P2P file sharing
- IPFS: Content-addressed distributed file system
- WebTorrent: Browser-based P2P using WebRTC

**Technical References:**
- OpenSSL SHA-1: `man SHA1`
- POSIX Sockets: `man 7 socket`
- C++ Threading: `man std::thread`

## Acknowledgments

- BitTorrent protocol design principles
- POSIX socket programming documentation
- OpenSSL cryptographic library

---

**For detailed performance analysis:** See accompanying Performance Analysis Report

**Project Status:** Fully functional and tested

**Author:** Ameya Purohit
