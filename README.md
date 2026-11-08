# msshell
no root termux terminal with on demand opengl es2.0 window
# MyShell - Android Terminal Shell

A minimal POSIX-compatible shell packaged as an Android APK.

## Features

- **22 builtins**: cd, pwd, echo, export, unset, env, help, exit, clear,
  history, type, source, eval, true, false, test, alias, wait, set, read,
  trap, umask
- **Pipes**: `ls | grep .c | wc -l`
- **Redirections**: `>file`, `>>file`, `<file`, `2>&1`
- **Background jobs**: `sleep 10 &`
- **Variable expansion**: `$HOME`, `${PATH}`, `$?`
- **Aliases**: `alias ll='ls -la'`
- **Command history**
- **Signal handling** (Ctrl-C)

## Project Structure

```
android/MyShell/
├── app/
│   ├── src/main/
│   │   ├── java/com/myshell/app/
│   │   │   ├── MainActivity.java     # Main activity
│   │   │   ├── TerminalView.java     # Terminal UI component
│   │   │   └── TerminalShell.java    # Shell process manager
│   │   ├── jni/
│   │   │   ├── Android.mk            # NDK build config
│   │   │   ├── Application.mk        # NDK application config
│   │   │   └── shell.c               # C shell source (copied from src/)
│   │   ├── jniLibs/                  # Compiled binaries (generated)
│   │   ├── res/
│   │   │   ├── layout/activity_main.xml
│   │   │   ├── values/strings.xml
│   │   │   └── mipmap-*/ic_launcher*.png
│   │   └── AndroidManifest.xml
│   └── build.gradle
├── build.gradle
└── settings.gradle
```

## Build Options

### Option 1: Build APK (requires Android Studio/NDK)

1. Install Android Studio with NDK
2. Set `ANDROID_HOME` environment variable
3. Run:
   ```bash
   build_apk.bat
   ```
   or
   ```bash
   ./gradlew assembleDebug
   ```

### Option 2: Build for Termux (no NDK needed)

1. Install Termux from F-Droid
2. Copy `shell.c` to your phone
3. In Termux:
   ```bash
   pkg install clang make
   clang -O2 -o myshell shell.c
   ./myshell
   ```

### Option 3: Cross-compile with Zig

```bash
python tools/build_native.py
```

This uses the Zig toolchain to compile the C source into ARM64/x86_64
binaries that can be placed in the APK's `jniLibs/` folder.

## Install APK

```bash
adb install app/build/outputs/apk/debug/app-debug.apk
```

## Usage

Once installed, the app opens with a terminal interface. Type commands:

```bash
$ help
$ ls -la
$ cd /sdcard
$ pwd
$ echo "Hello World"
$ export MYVAR="test"
$ echo $MYVAR
$ cat > file.txt << EOF
> Hello
> World
> EOF
```

## Architecture

The app uses a native C shell binary compiled for Android (ARM64/x86).
The Java layer handles:
1. Extracting the native binary from the APK
2. Running it as a subprocess
3. Connecting stdin/stdout to the terminal UI
4. Rendering text output

## License

MIT
