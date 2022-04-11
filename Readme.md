# Docker WSL Bridge

This project creates a unix domain socket inside WSL2 that can be used to control the Windows host's docker daemon. This
can be used to launch Windows containers from WSL2 without needing to expose any network sockets or SSH.

The code in this repository is a simple Windows service written in C that manages a single child process. The service is
written generically and could be used for other purposes, however that is out of the scope of this readme.

## Prerequisites

1. A C compiler.

2. A WSL2 instance with `socat`. This is installable on alpine via `apk add socat`.

3. A copy of the `docker.exe` CLI for Windows. The current link to these binaries is
   https://download.docker.com/win/static/stable/x86_64. If that link is broken, try searching for "docker engine static
   binaries windows".

4. `dockerd.exe` running on the Windows host, with a known `npipe` address such as
   `npipe\\:////./pipe/docker_engine_windows`.

## How to install

1. Build the service binary. It's a single C file with no dependencies (`service.c`). Recommended build command:

   ```
   cl service.c /Fedocker-wsl-bridge.exe /DWIN32 /D_WINDOWS /GL /Zi /Os /DNDEBUG /GL /GF /Gy /GA /link /OPT:REF /OPT:ICF /entry:mainCRTStartup /subsystem:console /merge:.pdata=.text /merge:.rdata=.text kernel32.lib shell32.lib advapi32.lib
   ```

2. (Optional) Move the service binary somewhere safe and change its permissions.

3. `sc.exe create docker.wsl.bridge binPath= "C:\path\to\docker-wsl-bridge.exe" type= userown`

4. Open `regedit.exe` to `Computer\HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\docker.wsl.bridge` and edit
   `ImagePath` to pass the wsl.exe command line as the first argument. In my home configuration, this works to bind
   `/run/host-services/docker.win.proxy.sock` in WSL2 to the Windows daemon at
   `npipe\\:////./pipe/docker_engine_windows`:

    ```
    C:\docker-wsl-bridge.exe "C:\Windows\System32\wsl.exe -u root --cd / -d docker-desktop -e /usr/bin/socat UNIX-LISTEN:/run/host-services/docker.win.proxy.sock,fork \"EXEC:\\\"/mnt/host/c/docker.exe --host=npipe\\:////./pipe/docker_engine_windows system dial-stdio\\\"\""
    ```

5. Sign out and sign back in. You should find a service named `docker.wsl.bridge_??????` in the services mmc snap-in
   (`services.msc`) which can be started and stopped to manage the bridge.

## How it works

The core operation is handled by `socat` which can fork a child process off in response to a unix domain socket
connection. For the child process, we can use the _Windows_ docker cli via WSL2's ability to launch Windows executables.
In support of SSH, the docker cli has a "dumb pipe" mode invoked through the undocumented `docker system dial-stdio`
command. Bringing all this together, we have the core daemon implementation:

```
/usr/bin/socat UNIX-LISTEN:/run/host-services/docker.win.proxy.sock,fork EXEC:"/mnt/host/c/docker.exe --host=npipe\\:////./pipe/docker_engine_windows system dial-stdio"
```

This can be tested from WSL2 by simply running docker against the new socket location:

```
docker --host=unix:///run/host-services/docker.win.proxy.sock run -it --rm hello-world:nanoserver
```

This is sufficient if you're willing to manually start the daemon whenever your machine restarts, by launching WSL2 and
manually running the command above. However, we can do better and manage this via a wrapping Windows service.

Unfortunately, WSL2 is per-user so it can't be productively invoked from a normal service running as LocalSystem.
Fortunately, Windows 10 introduced the concept of "Per-User Services"[1]. These are templates that are instantiated into
individual services for each user on login. Those instances then run in the user context just like a normal process,
making it possible to invoke `wsl.exe` and therefore invoke our `socat` daemon. Additional useful information about
creating per-user services is available in a blog post[2] by Helge Klein.

[1]: https://docs.microsoft.com/windows/application-management/per-user-services-in-windows
[2]: https://helgeklein.com/blog/per-user-services-in-windows-info-and-configuration/
