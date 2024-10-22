## Assignment3
### G V Ganesh Maurya - 2024201030

## 1. Introduction
This Peer-to-Peer (P2P) file-sharing system enables users to share and download files across a distributed network. It consists of two main components: trackers and clients. The system allows users to create groups, share files, and download files from multiple peers simultaneously.

## 2. Overview
### 2.1 Tracker
- Maintain information of clients and shared files.
- Support client communication for peer-to-peer file transfer.

### 2.2 Clients
- Register with the tracker by creating an account.
- Login using user credentials.
- Create or join groups, share files within groups, and download files from peers.

## 3. Functionalities
### 3.1 Trackers
- Maintain synchronized information about all clients and shared files.
- Ensure that all connected trackers are in sync.

### 3.2 Clients
- **User Account Management**: Create an account and log in.
- **Group Management**: Create groups, join groups, accept/reject join requests, and manage group memberships.
- **File Sharing**: Upload files, download files, and manage shared files.
- **Concurrent Downloads**: Download files from multiple peers, ensuring efficient distribution and file integrity.

## 4. Implementation Details
- Run Tracker
```./tracker tracker_info.txt tracker_no```
- Run Client
```./client <IP>:<PORT> tracker_info.txt```