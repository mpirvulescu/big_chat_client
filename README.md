# B.I.G Chat - Terminal Client

A robust, cross-platform terminal chat client written in C. This client was co-developed as part of a specialized 2-person team within a 16-developer engineering cohort to interface with a custom Server-Authoritative TCP backend.

It features a non-blocking `ncurses` UI, real-time message history, and a resilient networking layer capable of handling malformed packets and complex server-side state desyncs.

## Key Features

* **Terminal User Interface (TUI):** A responsive, multi-window interface built with `ncurses`, featuring dynamic viewport resizing, scrollable history, and a live user sidebar.
* **Hybrid State Architecture:** Implements a strict Server-Authoritative model for message fetching, combined with an **Optimistic UI** for editing and deleting. This ensures local client speed while maintaining perfect sync with delayed server broadcasts.
* **Asynchronous Networking:** Utilizes non-blocking socket polling (`poll()`) to seamlessly handle simultaneous incoming broadcasts and outgoing user inputs without freezing the UI thread.
* **Defensive Parsing:** Engineered to safely handle unexpected network behaviors, including structural reflection packets, missing foreign-key constraints (Ghost Accounts), and variable struct alignments.
* **Cross-Platform & Memory Safe:** Architected for strict compliance with the Clang Static Analyzer (`scan-build`). Compiles natively and runs leak-free on **Linux, macOS, and FreeBSD**.

## Technical Stack

* **Language:** C (C99/C11)
* **UI Library:** `ncurses`
* **Networking:** Raw TCP Sockets (`<sys/socket.h>`), POSIX `poll`
* **Build System:** Make / CMake
* **Analysis:** Clang Static Analyzer (`scan-build`), `cppcheck`

## Architecture Highlights

### The "Optimistic UI" Implementation
Different servers in the cohort implemented broadcast ACKs differently (some echoed `0x33` broadcasts to the sender, others only sent a `0x35` ACK). To ensure instant user feedback across all server implementations, this client uses an Optimistic UI. When a user edits a message, the UI updates instantly using the immutable server timestamp, fires the TCP request asynchronously, and safely intercepts/drops the subsequent server broadcast to prevent duplicate entries.

### Strict Memory Safety & Portability
Handling binary payloads over a network requires strict memory management. The client dynamically casts endianness (`be64toh`, `htobe64`) using OS-agnostic macros to support Apple Darwin, Linux, and BSD architectures. Furthermore, dynamic array limits are parsed and shielded against integer-promotion overflows to satisfy aggressive static analyzer bounds-checking.

## Getting Started

### Prerequisites
To build and run this client, you will need a C compiler (`gcc` or `clang`), `make`, and the `ncurses` development library.

**Ubuntu / Debian:**
```bash
sudo apt-get update
sudo apt-get install build-essential libncurses5-dev libncursesw5-dev
```

**macOS (via Homebrew):**
```bash
brew install ncurses
```

### Building the Client

Clone the repository and run `make`:

```bash
git clone [https://github.com/yourusername/big-chat-client.git](https://github.com/yourusername/big-chat-client.git)
cd big-chat-client
make clean
make
```

### Running the Client
Execute the compiled binary. Follow the on-screen prompts to connect to a server, register, and join a channel.

```bash
./big_chat_client
```

## Controls

* `Up/Down` or `Ctrl+U/Ctrl+D`: Scroll message history.
* `Up Arrow` (at bottom of chat): Enter Select Mode.
* `E`: Edit selected message (Optimistic UI).
* `D`: Delete selected message (Optimistic UI).
* `Esc`: Exit Select Mode / Return to Channel List.
* `/quit`: Gracefully close the socket and exit the program.

## Acknowledgments
Built as part of the British Columbia Institute of Technology Computer Systems Technology curriculum.
