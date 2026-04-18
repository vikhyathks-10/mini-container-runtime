# Mini Container Runtime in C

## Description
This project implements a lightweight container runtime similar to Docker using C. It demonstrates process isolation using Linux namespaces and chroot, along with memory monitoring using a kernel module.

## Features
- Container creation using clone()
- Filesystem isolation using chroot()
- Supervisor process for managing containers
- Logging system using producer-consumer model
- Kernel module for memory monitoring (RSS tracking)

## Concepts Used
- Namespaces (PID, UTS, Mount)
- chroot()
- clone()
- IPC (pipe, socket)
- Synchronization (mutex, condition variables)
- Memory management (RSS)
- CPU scheduling (nice values)

##Commands

Start supervisor:
./engine supervisor ./rootfs-base

Start container:
./engine start alpha ./rootfs-alpha /bin/sh

List containers:
./engine ps

View logs:
./engine logs alpha

Load kernel module:
sudo insmod monitor.ko

Check logs:
dmesg | tail

##  Experiments
- Memory Enforcement using memory_hog- CPU Scheduling using cpu_hog

## 💻 Author
Vikhyath bharadwaj k s and vishal 

## 📸 Screenshots

### Screenshot 1
![Screenshot](Screenshots/1.jpeg)

### Screenshot 2
![Screenshot](Screenshots/2.jpeg)

### Screenshot 3
![Screenshot](Screenshots/3.jpeg)

### Screenshot 4
![Screenshot](Screenshots/4.jpeg)

### Screenshot 5
![Screenshot](Screenshots/5.jpeg)

### Screenshot 6
![Screenshot](Screenshots/6.jpeg)

### Screenshot 7
![Screenshot](Screenshots/7.jpeg)

### Screenshot 8
![Screenshot](Screenshots/8.jpeg)

### Screenshot 9
![Screenshot](Screenshots/9.jpeg)

### Screenshot 10
![Screenshot](Screenshots/10.jpeg)

### Screenshot 11
![Screenshot](Screenshots/11.jpeg)

### Screenshot 12
![Screenshot](Screenshots/12.jpeg)

