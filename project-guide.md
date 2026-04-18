#  Project Jackfruit — Multi-Container Runtime

A lightweight **educational container runtime built on Linux** to demonstrate core operating system concepts such as:

*  Multi-container supervision
*  Kernel memory monitoring
* Memory limit enforcement (OOM-like behaviour)
* ⚙️PU scheduling experiments
*  Bounded-buffer logging

The runtime uses **Alpine Linux root filesystems** and a **custom user-space container engine**.

---

# 👥 Team

| Member                 | SRN           |
| -------------------    | ------------- |
| vikhyath bharadwaj k s | PES2UG24CS586 |
| vishal                 | PES2UG24CS591 |

---

# 📂 Project Structure

```
project-jackfruit/
│
├── engine
├── monitor.ko
├── logs/
│
├── rootfs-base
├── rootfs-alpha
├── rootfs-beta
│
├── cpu_hog
├── memory_hog
│
└── boilerplate/
```

---

# ⚠️Troubleshooting — Clean Slate

If you encounter this error:

```
execvp failed to run /bin/sh (errno: No such file or directory)
```

It usually means the **Alpine filesystem download failed**, leaving empty folders.

Stop the supervisor (`Ctrl + C` in Terminal 1) and run the following in **Terminal 2**.

<details>
<summary>Reset environment and rebuild filesystem</summary>

### Clean existing files

```bash
sudo rm -rf logs/* rootfs-alpha rootfs-beta rootfs-base
make clean
```

### Download Alpine Mini RootFS

```bash
mkdir rootfs-base

wget -qO- \
"https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz" \
| tar -xz -C rootfs-base
```

### Verify download

```bash
ls rootfs-base
```

Expected output example:

```
bin dev etc home ...
```

If **nothing prints**, the download failed.

### Copy filesystem to containers

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Restart container

```bash
sudo ./engine start alpha ./rootfs-alpha "ls -l /"
```

### Check logs

```bash
cat logs/alpha.log
```

</details>

---

# ⚙️ Phase 1 — Environment Setup

<details>
<summary>Open setup steps</summary>

### Compile User-Space Engine

```bash
cd boilerplate
make
```

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Setup Alpine Root Filesystem

```bash
mkdir rootfs-base

wget -qO- \
"https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz" \
| tar -xz -C rootfs-base
```

Verify download:

```bash
ls rootfs-base
```

Copy filesystem to container directories:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

</details>

---

# 🧩 Phase 2 — Multi-Container Supervision

<details>
<summary>Launch and manage containers</summary>

### Start Supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs-base
```

### Launch Containers (Terminal 2)

```bash
sudo ./engine start alpha ./rootfs-alpha "ls -l /"
sudo ./engine start beta ./rootfs-beta "ls -l /"
```

📸 **Screenshot #1 — Multi-Container Supervision**

Capture Terminal 2 showing both containers being started successfully.

---

### Test CLI and IPC

```bash
sudo ./engine stop alpha
```

📸 **Screenshot #4 — CLI and IPC**

Take a **split-view screenshot** showing:

* Terminal 2 sending the command
* Terminal 1 supervisor acknowledgement

---

### Check Metadata Tracking

```bash
sudo ./engine ps
```

📸 **Screenshot #2 — Metadata Tracking**

Capture the output showing containers and their states.

</details>

---

# 📜 Phase 3 — Bounded-Buffer Logging

Verify log capture:

```bash
cat logs/alpha.log
```

📸 **Screenshot #3 — Logging**

Capture the terminal showing **Alpine directory listing inside the log file**.

---

# 🧠 Phase 4 — Kernel Memory Monitor (OOM Killer)

<details>
<summary>Memory limit testing</summary>

### Inject Memory Hog

```bash
sudo cp memory_hog ./rootfs-alpha/
```

Run the memory hog container:

```bash
sudo ./engine start alpha-hog ./rootfs-alpha "/memory_hog 10 500"
```

### Observe Memory Enforcement

Wait **5–10 seconds**, then run:

```bash
sudo dmesg | tail -n 15
```

📸 **Screenshot #5 — Soft Limit Warning**

Capture the `dmesg` output highlighting the **SOFT LIMIT warning line**.

📸 **Screenshot #6 — Hard Limit Enforcement**

Capture the `dmesg` output highlighting the **HARD LIMIT kill line**.

</details>

---

# ⚙️ Phase 5 — CPU Scheduler Experiment

<details>
<summary>CPU scheduling comparison</summary>

### Copy workload generators

```bash
sudo cp cpu_hog ./rootfs-alpha/
sudo cp cpu_hog ./rootfs-beta/
```

### Run containers with different priorities

```bash
sudo ./engine start cpu-alpha ./rootfs-alpha "/cpu_hog 10" --nice 0
sudo ./engine start cpu-beta ./rootfs-beta "/cpu_hog 10" --nice 19
```

### Analyze scheduling logs

Wait **15 seconds**, then run:

```bash
cat logs/cpu-alpha.log
cat logs/cpu-beta.log
```

📸 **Screenshot #7 — Scheduling Experiment**

Capture the logs **side-by-side** showing the difference in progress.

</details>

---

# 🧹 Phase 6 — Clean Teardown

Stop the supervisor in **Terminal 1**

```
Ctrl + C
```

Then run the following commands in **Terminal 2**.

⚠️ Since the containers and logs were created using **sudo**, the log files are owned by **root**.
Running `make clean` without sudo may cause a **Permission denied** error.

Use **`sudo make clean`** to ensure proper cleanup.

```bash
sudo killall engine
sudo rm -f /tmp/mini_runtime.sock
sudo rmmod monitor
sudo make clean
```

📸 **Screenshot #8 — Clean Teardown**

Capture the terminal showing the cleanup commands finishing **without errors or permission issues**.

---

# 📚 Learning Outcomes

This project demonstrates:

* Linux container fundamentals
* User-space runtime architecture
* Kernel module interaction
* Memory monitoring and enforcement
* CPU scheduling behaviour
* Log buffering mechanisms

---

# 📸 Screenshots

Store screenshots in a folder:

```
/screenshots
```

Example usage:

```
![Multi Container](screenshots/multi_container.png)
```
