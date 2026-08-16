# SSH ControlMaster Configuration Guide

This guide explains how to configure SSH connection multiplexing (`ControlMaster`) so you only need to type your OTP / 2FA password **once**, and all subsequent commands / scripts / agents can connect to the HUJI cluster instantly with zero prompts.

---

## 1. How It Works
With `ControlMaster`, SSH keeps a persistent master socket open in `~/.ssh/` after your first interactive login. Subsequent SSH connections to `mlx-stud-04` (or through `bava.cs.huji.ac.il`) automatically reuse that existing authenticated channel without opening a new TCP handshake or asking for OTP / passwords again.

---

## 2. Configuration (`~/.ssh/config`)

Add the following block to your `~/.ssh/config` file (on your local machine, e.g. `C:\Users\marmo\.ssh\config`):

```ssh
# Ensure ~/.ssh/sockets exists (mkdir -p ~/.ssh/sockets)

# HUJI Bastion / Jump Host
Host bava
    HostName bava.cs.huji.ac.il
    User ateret.tabib
    ControlMaster auto
    ControlPath ~/.ssh/sockets/cm-%r@%h:%p
    ControlPersist 4h
    ServerAliveInterval 60
    ServerAliveCountMax 10

# Direct cluster nodes via Jump Host
Host mlx-stud-04 mlxstud04
    HostName mlx-stud-04.cs.huji.ac.il
    User ateret.tabib
    ProxyJump bava
    ControlMaster auto
    ControlPath ~/.ssh/sockets/cm-%r@%h:%p
    ControlPersist 4h
    ServerAliveInterval 60
    ServerAliveCountMax 10

Host mlx-stud-01 mlxstud01
    HostName mlx-stud-01.cs.huji.ac.il
    User ateret.tabib
    ProxyJump bava
    ControlMaster auto
    ControlPath ~/.ssh/sockets/cm-%r@%h:%p
    ControlPersist 4h
```

---

## 3. Usage

1. Open PowerShell or Terminal and create the socket folder:
   ```powershell
   mkdir -Force ~/.ssh/sockets
   ```
2. Start the master session (enter your OTP + IDng password once):
   ```powershell
   ssh mlx-stud-04
   ```
3. Leave that session open or in background. For the next 4 hours (`ControlPersist 4h`), any script or agent command:
   ```powershell
   ssh mlx-stud-04 "cd ~/ex3_network && make test"
   ```
   will execute **instantly** without asking for OTP or passwords!
