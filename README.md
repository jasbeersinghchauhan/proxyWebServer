# 🧩 C-Based Multi-Threaded Web Proxy for Windows

A **high-performance, multi-threaded web proxy server** written entirely in **C** for the **Windows** platform.  
Built for robustness and efficiency, it handles both **HTTP (GET)** requests and **HTTPS (CONNECT)** tunneling.

Includes a **thread-safe LRU cache** and a **log visualization dashboard** (`log_analyzer.html`).

## 🚀 Features

### 🧵 Multi-Threaded Architecture
- Uses a **thread-per-client** model to handle many simultaneous connections.

### 🔒 Semaphore-Based Connection Limiting
- Employs a **Windows semaphore** to cap concurrent clients, preventing overloads.

### 🌐 HTTP/1.1 GET Handling
- Parses GET requests, forwards them to the origin server, and streams responses back to the client.

### 🔐 HTTPS CONNECT Tunneling
- Correctly handles CONNECT requests by establishing a **bi-directional TCP tunnel** for encrypted HTTPS traffic.

### ⚡ Thread-Safe LRU Cache
- Custom-built cache (`proxy_cache.c`) for GET requests.
- Backed by:
  - **Hash map** (`hashmap.h`) for O(1) lookups.
  - **Doubly-linked list** for recency tracking.
  - **CRITICAL_SECTION** for thread-safe access

### 🧾 Thread-Safe Logging
- A dedicated logger (`proxy_logger.c`) that writes logs atomically (no interleaving between threads).

### 📊 Log Analyzer Dashboard
- A **zero-dependency**, single-file HTML/JS dashboard (`log_analyzer.html`).
- Parses `proxy.log` to visualize cache performance, request stats, and live logs.

---

## ⚙️ Architecture Overview

### 1️⃣ Server Start (`proxy_main.c`)
- Initializes **Winsock** (`WSAStartup`).
- Initializes the **logger** and **cache**.
- Creates a **semaphore** (`CreateSemaphoreW`) to limit concurrent clients.
- Binds and listens on the specified port.

### 2️⃣ Client Connection (`proxy_main.c`)
- Blocks on `accept()`.
- On connection:
  - Waits on the semaphore (`WaitForSingleObject()`).
  - Spawns a new thread (`CreateThread`) for each client.

### 3️⃣ Client Handling (`proxy_handler.c`)
Each thread:
- Reads the initial browser request.

#### 🔹 HTTPS CONNECT
- Establishes a raw TCP tunnel to the target server (e.g., `google.com:443`).
- Sends `HTTP/1.1 200 OK` to the client.
- Uses `select()` to forward encrypted traffic both ways.

#### 🔹 HTTP GET
- Parses the URL and checks the cache.
- **Cache Hit:** Responds directly from cache.
- **Cache Miss:**
  - Connects to the origin server.
  - Streams the response to the client **and** stores it for future use.

### 4️⃣ Cleanup (`proxy_handler.c`)
- Closes client and remote sockets.
- Calls `ReleaseSemaphore()` to free a client slot.
- Thread exits cleanly.

---

## 📁 Project Structure
```
.
├── proxy_main.c         # Main server: socket setup, bind, listen, accept loop
├── proxy_main.h         #
├── proxy_handler.c      # Core client logic: handles HTTP/HTTPS, request parsing
├── proxy_handler.h      #
├── proxy_cache.c        # Thread-safe LRU cache implementation
├── proxy_cache.h        #
├── proxy_logger.c       # Thread-safe file logger
├── proxy_logger.h       #
├── hashmap.c            # Hashmap 
├── hashmap.h            #
├── log_analyzer.html    # Standalone HTML/JS dashboard for log visualization
└── README.md            # This file
```

---

## 🧰 Dependencies

- **Platform:** Windows  
  (Uses `WinSock2`, `Windows.h`, and `CRITICAL_SECTION` for synchronization)
- **Libraries:** `Ws2_32.lib`
- **External Code:** `hashmap.h` (single-header hash map)

---

## 🛠️ Building the Project

### ✅ Prerequisites
- A C compiler (e.g. **Visual Studio**, **MSVC**, or **MinGW GCC**)
- **Windows SDK**
- `hashmap.h` in the project directory

---

### 🧩 Build with Visual Studio
1. Create a new **Windows Console Application**.
2. Add all `.c` and `.h` files to the project.
3. Go to **Project Properties → Linker → Input**.
4. Add `Ws2_32.lib` to **Additional Dependencies**.
5. Build the solution (`F7`).

---

### 🧩 Build with GCC (MinGW)
Open a terminal and run:

```bash
gcc proxy_main.c proxy_handler.c proxy_cache.c hashmap.c proxy_logger.c -o proxy.exe -lws2_32
```

This will create proxy.exe.

## ▶️ Running the Proxy
### 1️⃣ Start the Proxy

```bash
.\proxy.exe 8080
```


### 2️⃣ Configure Your Browser

1. Go to your browser’s proxy settings.

2. Choose Manual proxy configuration.

3. Set:

  - HTTP Proxy: 127.0.0.1

  - Port: 8080

  - Check “Also use this proxy for HTTPS”.

### 3️⃣ Browse the Web

- Visit HTTP sites (cached).

- Visit HTTPS sites (tunneled).

Observe real-time logging in the console and proxy.log.

### 4️⃣ Analyze Logs

1. Open log_analyzer.html in a browser.

2. Drag and drop the proxy.log file.

3. View cache hit rate, misses, and live parsed logs.

### ⚠️ Known Issues & Limitations

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
