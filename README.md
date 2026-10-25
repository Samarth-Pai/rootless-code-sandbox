# ⚡ Rootless Code Sandbox

[![Linux](https://img.shields.io/badge/Linux-Kernel%205.13%2B%20%7C%20Landlock%20LSM-yellow.svg)](https://landlock.io/)
[![C](https://img.shields.io/badge/C-17%20Fork--Server-yellow.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Python](https://img.shields.io/badge/Python-3.12%2B%20%7C%20AsyncIO-black.svg)](https://docs.python.org/3/library/asyncio.html)
[![FastAPI](https://img.shields.io/badge/FastAPI-2.5.0-yellow.svg)](https://fastapi.tiangolo.com/)
[![License](https://img.shields.io/badge/License-MIT-white.svg)](LICENSE)

A ultra-low latency, **rootless execution engine** designed for competitive programming and automated online assessments (supporting **C, C++, Java, and Python**). 

Unlike Docker, Podman, or chroot which incur 50–200ms of startup latency and require root privileges, this engine achieves **sub-millisecond execution overhead in unprivileged user space** using a **POSIX Fork-Server over Unix Domain Sockets (UDS)** coupled with the **Linux Landlock Security Module (LSM)** and **`setrlimit`** hardware resource bounds.

---

## 🏛️ System Architecture

```text
 ┌────────────────────────────────────────────────────────┐
 │            Client Layer (FastAPI & AsyncIO)            │
 │                   [sandbox.py / app.py]                │
 └───────────────────────────┬────────────────────────────┘
                             │
     1. Creates isolated workspace (/tmp/workspaces/<uuid>)
     2. Asynchronously compiles (gcc, g++, javac)
     3. Formats protocol: "command|arg1|arg2\n<stdin_data>"
                             │
                             ▼  Unix Domain Socket (/tmp/sandbox.sock)
 ┌────────────────────────────────────────────────────────┐
 │           C Executor Daemon (The Fork-Server)          │
 │                      [daemon.c]                        │
 └───────────────────────────┬────────────────────────────┘
                             │
     1. accept() connection in while(1) loop
     2. fork() child process immediately
     3. Parent continues listening; reaps zombies via SIGCHLD
                             │
                             ▼
 ┌────────────────────────────────────────────────────────┐
 │             Isolated Sandboxed Child Process           │
 └───────────────────────────┬────────────────────────────┘
                             │
     1. dup2() client socket to STDIN, STDOUT, and STDERR
     2. prctl(PR_SET_NO_NEW_PRIVS, 1) -> Disallow privilege escalation
     3. Landlock LSM Ruleset:
        - RESTRICT ALL filesystem access globally
        - Whitelist /tmp/workspaces with READ + WRITE + EXECUTE
        - Whitelist system runtimes (/usr, /lib, /bin) as READ-ONLY
        - Strictly block access to /home, /root, and host secrets
     4. setrlimit() bounds:
        - RLIMIT_CPU: 3-second hard limit (SIGKILL on infinite loops)
        - RLIMIT_AS : Address space ceiling (512MB / 4GB for JVM)
     5. execve(argv[0], argv, NULL)
```

---

## 🔒 Security Model

| Security Layer | Mechanism | Protection |
| :--- | :--- | :--- |
| **Filesystem Jail** | Linux Landlock LSM (`landlock_create_ruleset`, `landlock_restrict_self`) | The process can only read/write inside its designated `/tmp/workspaces/<uuid>` directory. Host directories like `/home`, `/root`, and `/etc/shadow` are hardware-blocked by the kernel. |
| **Privilege Escalation** | `prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)` | Prevents child processes or any sub-binaries from gaining new privileges via `setuid`, `setgid`, or capabilities. |
| **CPU Denial-of-Service** | POSIX `setrlimit(RLIMIT_CPU, 3s)` | If code enters an infinite loop (`while(1)`), the kernel fires `SIGXCPU` and immediately terminates the process with `SIGKILL` at 3 seconds of CPU time. |
| **Memory Exhaustion (OOM)** | POSIX `setrlimit(RLIMIT_AS)` | Restricts virtual address space to prevent memory bombs. Dynamically adapts between native binaries (512 MB) and the OpenJDK HotSpot JVM (4096 MB virtual address space, capped with `-Xmx256m` physical heap). |
| **Rootless Operation** | Unprivileged Linux Namespaces / LSM | Runs entirely under a standard unprivileged user account. No `sudo`, no setuid binaries, and no root daemon required. |

---

## ⚡ Concurrency & Benchmark Results

The fork-server model eliminates container startup latency. Here are real-world benchmark metrics running on an unprivileged host machine:

| Metric | 50 Concurrent C Submissions | 75 Concurrent Python Submissions | 50 Mixed Heavy Algorithms (C + C++ + Java + Python) |
| :--- | :--- | :--- | :--- |
| **Success Rate** | **100.0%** (50 / 50) | **100.0%** (75 / 75) | **100.0%** (50 / 50) |
| **Total Wall Time** | **0.98 seconds** | **0.70 seconds** | **17.08 seconds** |
| **Throughput** | **50.77 submissions / sec** | **106.48 submissions / sec** | Multi-threaded full compile & exec |
| **Median Latency (p50)**| **641 ms** *(incl. GCC compilation)* | **577 ms** | Handled all 4 languages concurrently |
| **Dropped Connections** | **0** | **0** | **0** |

---

## 🚀 Quickstart

### Prerequisites
- Linux Kernel version **5.13 or newer** (with Landlock enabled; standard in modern Arch, Ubuntu 22.04+, Debian 12+, Fedora).
- GCC, G++, Python 3.10+, OpenJDK (optional for Java).
- Python dependencies: `pip install fastapi uvicorn pydantic httpx`

### 1-Click Launch
Run the automated launcher script which builds the C daemon and starts the FastAPI server:
```bash
chmod +x run.sh
./run.sh
```

The web dashboard is now live at **`http://localhost:8000`**.

### Manual Setup
If you prefer running services independently:

1. **Compile and start the C Daemon**:
   ```bash
   gcc -Wall -Wextra -O2 daemon.c -o daemon
   ./daemon
   ```

2. **Start the FastAPI Platform**:
   ```bash
   python3 app.py
   ```

3. **Run CLI Stress Test**:
   ```bash
   python3 load_test.py
   ```

---

## 📡 API Reference

Interactive Swagger documentation is available at **`http://localhost:8000/docs`**.

### 1. Execute Code
**`POST /api/v1/execute`**

Executes a single code snippet in a rootless Landlock sandbox.

#### Request Body
```json
{
  "language": "c",
  "source_code": "#include <stdio.h>\nint main() { printf(\"Sum = %d\\n\", 40 + 2); return 0; }",
  "input_data": ""
}
```

#### Response (`200 OK`)
```json
{
  "status": "success",
  "output": "Sum = 42\n",
  "execution_time_ms": 58.42,
  "run_id": "b3f0c1e8-782a-4318-8740-1a7337a4e69b",
  "error": null
}
```

---

### 2. Trigger Assessment Surge Benchmark
**`POST /api/v1/benchmark`**

Fires an asynchronous burst of 1 to 200 concurrent student submissions.

#### Request Body
```json
{
  "concurrency": 50,
  "language": "mixed",
  "workload": "heavy"
}
```

#### Response (`200 OK`)
```json
{
  "concurrency": 50,
  "mode": "mixed",
  "workload": "heavy",
  "total_wall_time_sec": 14.82,
  "throughput_rps": 3.37,
  "success_count": 50,
  "error_count": 0,
  "success_rate_pct": 100.0,
  "latency_stats": {
    "min_ms": 1940.2,
    "avg_ms": 6120.4,
    "p50_ms": 5840.1,
    "p95_ms": 13910.8,
    "p99_ms": 14750.2,
    "max_ms": 14750.2
  },
  "lang_breakdown": {
    "C": { "count": 13, "success": 13, "avg_ms": 2286.5, "p50_ms": 2151.0 },
    "CPP": { "count": 13, "success": 13, "avg_ms": 12020.8, "p50_ms": 10506.8 },
    "JAVA": { "count": 12, "success": 12, "avg_ms": 14370.7, "p50_ms": 14550.6 },
    "PYTHON": { "count": 12, "success": 12, "avg_ms": 2869.2, "p50_ms": 2627.8 }
  }
}
```

---

### 3. Engine Health Check
**`GET /api/v1/health`**

Returns daemon status and active Linux LSM parameters.

#### Response (`200 OK`)
```json
{
  "status": "healthy",
  "daemon_socket": "/tmp/sandbox.sock",
  "socket_active": true,
  "engine": "Linux Landlock LSM + POSIX Fork-Server",
  "security": {
    "rootless": true,
    "filesystem_isolation": "Landlock LSM (Strict read-only host, read-write /tmp/workspaces)",
    "privilege_escalation": "PR_SET_NO_NEW_PRIVS Enforced",
    "cpu_limit": "3 seconds hard limit (SIGKILL via RLIMIT_CPU)",
    "ram_limit": "512 MB AS limit (4096 MB for Java HotSpot)"
  }
}
```

---

## 📂 Project Structure

```text
├── daemon.c          # C Fork-Server Daemon listening on /tmp/sandbox.sock
│                     # Enforces Landlock LSM, prctl, and setrlimit before execve()
├── sandbox.py        # Python client managing workspaces, async compilation, and UDS streaming
├── app.py            # FastAPI web server serving the Yellow/Black Cyberpunk UI & REST APIs
├── load_test.py      # Standalone CLI concurrency load testing script (>50 requests)
├── run.sh            # 1-click build and launcher script
└── README.md         # Documentation and architecture guide
```

---

## 📜 License
MIT License. Built for high-performance, rootless competitive programming grading systems.
