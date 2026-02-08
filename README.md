# 🧩 Multi-Threaded Web Proxy Server (C++20 /Windows)

A **high-performance, multi-threaded web proxy server** written in **Modern C++(C++20)** for the **Windows** platform.  
This project focuses on systems programming, network socket management, and concurrency patterns. It implements a custom LRU cache, robust thread-safe logging, and supports both **HTTP (GET)** requests and **HTTPS (CONNECT)** tunneling.

### ⚠️ Note on Ownership

- "The core proxy server, cache, and logging infrastructure are fully implemented by me."
- The log analyzer dashboard (`log_analyzer.html`) and log format design (`proxy.log`) are included for demonstration only — I have not written the visualization code.

## 🚀 Key Features

### 🧵 C++20 Multi-Threaded Architecture

- Uses the **Thread-per-client** model implemented via the std::thread.
- Decouples connection logic from the main acceptor loop, ensuring high responsiveness.

### 🔒 Semaphore-Based Connection Limiting
- Utilizes C++20 **std::counting_semaphore** to cap the maximum number of active clients (default: 2000).
- Prevents resource exhaustion (DoS) under heavy load without relying on OS-specific API calls for synchronization.

### 🌐 HTTP/1.1 GET Handling
- Parses incoming HTTP GET requests to extract host, port, and path.
- Forwards requests to origin servers and streams responses back to clients.
- **Automatic Caching:** Responses are intercepted and stored in memory to speed up subsequent requests.

### 🔐 HTTPS CONNECT Tunneling
- Implements the CONNECT method to handle SSL/TLS traffic.
- Establishes a bi-directional TCP tunnel between the client and the remote server.
- Uses `select()` I/O multiplexing to relay encrypted data efficiently between sockets.

### ⚡ Thread-Safe LRU Cache
- Custom **Least Recently Used (LRU)** cache implementation.
- Internals:
  - `std::unordered_map` for O(1) lookups.
  - Doubly‑linked list using `std::shared_ptr` and `std::weak_ptr` for memory-safe recency tracking.
  - Protected by `std::mutex` to ensure thread safety during concurrent access.

### 🧾 Modern Thread-Safe Logging

- Centralized ProxyLogger singleton.
- Uses C++20 `std::format` for type-safe, high-performance string formatting.
- Ensures atomic writes to `proxy.log` using mutex locking, preventing interleaved output from different threads.

---

## ⚙️ Architecture Overview

### 1️⃣ Server Initialization (`proxy_main.cpp`)
- Initializes **Winsock** (`WSAStartup`).
- Sets up the `std::counting_semaphore` for connection throttling.
- Binds the listening socket and enters the main accept loop.

### 2️⃣ Client Accept Loop (`proxy_main.cpp`)
- Waits for a semaphore slot (`sem.acquire()`).
- Blocks on accept() to handle the incoming connection.
- Spawns a detached `std::thread` to handle the specific client.

### 3️⃣ Client Handling (`proxy_handler.cpp`)
The handler reads the client request and determines the mode:

#### 🔹 HTTPS CONNECT
- Connects to the target server (default port 443).
- Returns `200 Connection Established`.
- Enters a `select()` loop to pipe raw bytes between client and server until timeout or closure.

#### 🔹 HTTP GET
- Checks the LRU Cache for the requested URL.
- **Cache Hit:** Serves data immediately from memory.
- **Cache Miss:** Connects to the origin server, downloads the content, serves it to the client, and inserts it into the cache.

### 4️⃣ Resource Management
- **RAII Principles:** Uses smart pointers (`std::unique_ptr`, `std::shared_ptr`) for memory management.
- **Socket Safety:** Implements SocketGuard wrappers to ensure sockets are closed (closesocket) even if exceptions occur.

---

## 📁 Project Structure

```
.
├── CMakeLists.txt         # CMake build configuration
├── proxy_main.cpp         # Entry point, socket setup, semaphore, thread spawning
├── proxy_handler.cpp      # Logic for HTTP parsing, CONNECT tunneling, and relaying
├── proxy_handler.hpp      
├── proxy_cache.cpp        # Custom LRU Cache implementation (Map + Linked List)
├── proxy_cache.hpp
├── proxy_logger.cpp       # Singleton logger using C++20 std::format
├── proxy_logger.hpp
├── log_analyzer.html      # Standalone HTML/JS dashboard for log visualization
├── proxy_cache_test.cpp         # Google Test unit tests for the cache
└── README.md
```

---

## 🧰 Dependencies

- **Platform:** Windows 10/11 (Uses `winsock2.h`)
- **Language:** C++20
- **Libraries:** `Ws2_32.lib` (Windows Socket Library)

---

## 🛠️ Building Instructions

### ✅ Prerequisites
- **CMake** (Version 3.10 or newer)
- A C++20 compatible compiler (e.g. **Visual Studio 2019+**, **MinGW-w64**)
- **Windows SDK** (Required for Winsock2)

---

### 🚀 Building with CMake

1. **Open a terminal** in the project root directory.
2. **Create a build directory:**
    ```bash
    mkdir build
    cd build
    cmake ..
    cmake --build . --config Release
    ```

## ▶️ Running the Proxy

### 1️⃣ Start the Proxy

```bash
./proxy.exe 8080
```

### 2️⃣ Configure Your Browser

- Proxy IP: `127.0.0.1`

- Port: `8080`

- Enable proxy for both HTTP and HTTPS.

### 3️⃣ Browse the Web

- Visit HTTP sites (cached).

- Visit HTTPS sites (tunneled).

Observe real-time logging in the console and proxy.log.

### 4️⃣ Analyze Logs

1. Open log_analyzer.html in a browser.

2. Drag and drop the proxy.log file.

3. View cache hit rate, misses, and live parsed logs.

### ⚠️ Limitations

- 🪟 Windows-only (depends on WinSock and WinAPI)

- ❌ No Connection: keep-alive

- 💾 GET-only caching (no POST/PUT/DELETE)

- 🔍 Basic HTTP parsing — may fail on complex headers

- 🧠 In-memory cache — cleared on restart

## 🧭 Future Work / Roadmap

- 🧩 Cross-platform support (POSIX sockets + pthreads)

- 🔁 Thread pool (replace thread-per-client model)

- 🔄 Keep-alive connections

- 📨 POST request tunneling

- 💽 Persistent on-disk cache

- ⚙️ Config file support (config.ini)

- 🤝 Contributing

Contributions are welcome!

Fork the repo
Create a branch:

```bash
git checkout -b feature/my-new-feature
```

Commit your changes:

```bash
git commit -am "Add some feature"
```

Push to your branch:

```bash
git push origin feature/my-new-feature
```

Open a Pull Request

💬 For major changes, please open an issue first to discuss what you’d like to modify.

# 👤 Author

Jasbeer Singh Chauhan
📧 jasbeersinghchauhan377@gmail.com

# 📈 Log Visualization Example

You can use the included log_analyzer.html — a simple HTML/JS dashboard that reads proxy.log and displays metrics using Chart.js:

```bash
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
```

That script loads the Chart.js library used for the pie chart visualization.