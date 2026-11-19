# mshell — no-root Termux terminal with on-demand OpenGL window and Nadrino OS

## English

A Termux-native terminal that works without root and without any display server.
Type **`!experiment`** at the shell prompt to open an OpenGL ES 2.0 terminal
window that shares the same shell session, or **`!adaptivenadrino`** to launch
the Nadrino OS desktop — a Windows 95/98-style virtual operating system
running inside the terminal grid.

Type **`$simpleinterface`** (or run **`--simple`**) to open the dock-based
configuration UI: add named containers, pick a theme (Classic / Win7 Aero),
set a box64 stability preset, and hit **LAUNCH** to boot the chosen OS.

Run **`!setconf`** in the launcher to auto-detect your device class and save
recommended settings (frame budget, scrollback, read buffer) to
`~/.mshell.conf` — every future window inherits them automatically.

## Русский

Терминал для Termux, работающий без root-прав и без сервера отображения.
Напишите **`!experiment`** в приглашении оболочки, чтобы открыть окно
терминала OpenGL ES 2.0 с той же сессией, или **`!adaptivenadrino`** для
запуска рабочего стола Nadrino OS — виртуальной ОС в стиле Windows 95/98.

Напишите **`$simpleinterface`** (или запустите **`--simple`**) для открытия
конфигурационного интерфейса с док-панелью: добавляйте именованные контейнеры,
выбирайте тему (Classic / Win7 Aero), задавайте уровень стабильности box64
и нажимайте **LAUNCH** для запуска выбранной системы.

Выполните **`!setconf`** в лаунчере, чтобы автоматически определить класс
устройства и сохранить рекомендуемые настройки (кадровая частота, история,
буфер чтения) в `~/.mshell.conf` — каждое следующее окно наследует их
автоматически.

## How it works

```
mshell (launcher)
  │
  ├── Runs a shell behind a pty, mirrors I/O to your real terminal
  ├── Sees every byte typed; detects `!experiment` and `!adaptivenadrino`
  ├── On `!experiment` → fork/execs itself as `mshell --gl`
  │   └── OpenGL window: SDL2 + GLES2 + same pty, same session
  │   └── When you close the window, the shell session continues
  │
  ├── On `!adaptivenadrino` → fork/execs itself as `mshell --os`
  │   └── Nadrino OS desktop: SDL2 + GLES2, classic Windows 95 UI
  │   └── Floating windows, Start menu, taskbar, desktop icons
  │
  ├── On `$simpleinterface` → fork/execs itself as `mshell --simple`
  │   └── Dock-based configuration UI: containers, themes, stability
  │
  └── If built without SDL2 → `make launcher` → just a wrapper shell
```

No root needed. No display server needed for the launcher. The `--gl`,
`--os`, and `--simple` windows need SDL's video driver (Termux:X11, KMS
with root, or a Wayland compositor).

## Building

### Prerequisites (Termux)

```bash
pkg update
pkg install clang make libutil-dev
```

### Two build modes

```bash
# Full build: launcher + GL window (needs SDL2 + EGL/GLESv2)
make                       # or: make all

# Launcher only (no SDL, no display server needed at all)
make launcher
```

### SDL2 packages (for the GL window)

```bash
pkg install libsdl2
# libutil-dev is already installed — provides forkpty()
# libGLESv2 and libEGL are provided by Android's bionic at runtime
```

## Usage

### Launcher mode (default)

```bash
./mshell                           # starts a shell
!experiment                        # opens the OpenGL window
!adaptivenadrino                   # opens the Nadrino OS desktop
$simpleinterface                   # opens the dock-based configuration UI
# close the window → shell continues
exit                               # done
```

### GL-only mode

```bash
./mshell --gl                      # OpenGL window straight away
```

### Nadrino OS mode

```bash
./mshell --os                      # Nadrino OS desktop straight away
```

### Simple Interface (dock + containers + box64 presets)

```bash
./mshell --simple                  # dock UI straight away
```

### Pick a shell

```bash
MSHELL_SHELL=/usr/bin/bash ./mshell
```

### No-mirror mode

```bash
./mshell --no-mirror               # shell output goes to GL window only
```

## Controls (GL window)

| Action              | Key(s)            |
|---------------------|-------------------|
| Scroll back         | PgUp              |
| Scroll forward      | PgDn              |
| Interrupt (Ctrl-C)  | Ctrl+C            |
| Exit shell          | Ctrl-D or exit    |
| Function keys       | F1–F12            |
| Mouse click/drag    | Mouse (in apps that ask for it) |

## Terminal emulation

The GL window implements the pieces of VT/xterm that full-screen programs
actually need:

- **Alternate screen** — `?47`, `?1047` and `?1049`. `vim`, `less`, `htop` and
  `nano` draw on a second screen buffer and restore your session exactly as it
  was when they exit, instead of leaving their output behind. The two buffers
  are a pointer swap, so switching costs nothing.
- **Mouse reporting** — `?1000` (clicks), `?1002` (drag) and `?1006` (SGR
  encoding). When a program requests them, clicks, drags and wheel turns are
  forwarded to it as escape sequences; when it doesn't, the mouse stays with
  the window manager.
- **SGR styling** — bold, dim, italic, underline, blink, inverse and hidden.
  Bold brightens the colour by adding 32 per channel with a clamp, so already
  bright colours stay correct instead of clipping.
- **Replies to queries** — Device Attributes (`DA`) and Cursor Position
  Report (`DSR 6`), so programs that probe the terminal get an answer.

## Nadrino OS

Type **`!adaptivenadrino`** or run **`./mshell --os`** to launch the Nadrino OS
desktop — a Windows 95/98-style virtual operating system rendered in the terminal
grid. Features:

- **Classic Windows 95/98 palette**: teal desktop, navy title bars, gray taskbar
- **Floating windows**: drag by the title bar, close with `x`, minimize
- **Start menu**: click Start to toggle the menu
- **Desktop icons**: double-click (or tap) to launch apps
- **Built-in apps**:
  - **Terminal** — echo shell output
  - **Calculator** — basic arithmetic
  - **Text Editor** — simple text input
  - **File Manager** — browses files
  - **About** — system information
- **Taskbar**: Start button + window buttons + clock

### Controls (Nadrino OS)

| Action              | Key(s)            |
|---------------------|-------------------|
| Click / drag        | Mouse             |
| Function keys       | F1–F12            |
| Close window        | Click `x` button  |
| Toggle Start menu   | Click Start       |

## Simple Interface

Type **`$simpleinterface`** or run **`./mshell --simple`** for a full-screen
dock UI used to configure and boot an environment.

Layout: a **circular dock** at the bottom center holds the launchable apps,
a **LAUNCH** bar sits along the bottom edge, named **containers** fill the
left pane and **settings** the right pane.

| Action                       | Key(s)                    |
|------------------------------|---------------------------|
| Move between zones           | Tab                       |
| Move selection               | Up / Down                 |
| Add container                | `N`                       |
| Rename selected container    | `R`                       |
| Delete selected container    | `D`                       |
| Open / toggle                | Enter / Space, or click   |
| Change setting               | Left / Right              |
| Launch chosen OS             | Click LAUNCH, or `L`      |
| Launch dock app              | Click a dock icon         |
| Back out / quit              | Esc                       |

Settings:

- **OS** — Nadrino OS or the GL terminal
- **Theme** — Classic (Win95) or Aero (Windows 7)
- **box64 preset** — `Basic`, `Enhanced`, or `Stability x3`

A **container** is just a named group: give it a name, put app icons in it,
and it shows up in the dock when that container is selected. Names are up to
31 characters and are edited in place.

Everything is drawn with the built-in 5×7 bitmap font (ASCII 0x20–0x7E), so
no assets or locale data are required.

## Windows 7 / Aero theme

Nadrino OS ships two looks, selectable from the Start menu or the simple
interface:

- **Classic** — the original Win95/98 palette: teal desktop, navy title bars.
- **Aero** — a Windows 7-style translucency: blue-glass title bars, lighter
  window chrome, soft highlights. Applied as a post-render recolour pass, so
  every app and window picks it up automatically.

## Display servers

The `--gl` window needs a display:

- **Termux:X11** (no root): install from the Tur repo, start the X server,
  `SDL_VIDEODRIVER=x11 ./mshell --gl`
- **KMS/DRM** (root): `SDL_VIDEODRIVER=kmsdrm ./mshell --gl`
- **The launcher itself** needs no display — it runs entirely in the terminal.

## Project structure

```
termux-gl/
├── main.c      # Two-mode entry point: launcher + GL window + Nadrino OS
├── term.c      # Grid model, ANSI/VT parser, 500-line scrollback
├── term.h      # Public API for the terminal model
├── gl.c        # GLES2 renderer (shaders, atlas, quad batching)
├── gl.h        # Renderer API
├── font.c      # 5×7 bitmap font, 2× scaled into atlas
├── font.h      # Font constants and atlas API
├── os_nadrino.c# Nadrino OS desktop (Win95/Aero UI, apps, windows)
├── os_nadrino.h# Nadrino OS data structures and API
├── simple_ui.c # $simpleinterface: dock, containers, box64 presets
├── simple_ui.h # Simple interface API
├── Makefile    # Two targets: mshell (full), mshell-launcher (no SDL)
└── README.md   # This file
```

## Architecture

### Launcher mode

1. `forkpty()` creates a pty master fd running a shell.
2. `select()` polls both stdin (your keyboard) and the pty master.
3. Bytes from the shell are mirrored to stdout; your keystrokes are forwarded
   to the pty.
4. `!experiment` is detected by scanning mirrored output — it must start a
   command (after prompt, newline, `;`, `|`, `&`), with no quotes.
5. On trigger: `fork()` a child that re-exec's `mshell --gl`; the launcher
   waits on it, then continues.

### GL window mode

1. SDL2 creates an OpenGL ES 2.0 context on the display surface.
2. Same `forkpty()` + `select()` + `term_feed()` pipeline as the launcher.
3. Grid is rendered each frame: background pass + glyph pass = one `glDrawArrays`.
4. SDL keyboard events map to terminal key sequences via `term_send_key()`.
5. When the window closes, the shell session (on the pty) survives.

## Technical notes

- **Colors are packed `0xAABBGGRR`** — uploads to GLES2 as `UNSIGNED_BYTE`
  with `GL_RGBA` without swizzling.
- **The atlas uses `GL_LUMINANCE`** — the fragment shader reads `.r` as coverage
  and multiplies the vertex colour by it.
- **`forkpty()`** creates a pty master set to non-blocking mode.
- **Scrollback** is a ring buffer of rows. `term_cell()` with `off > 0` returns
  cells from the ring; `off == 0` returns the live grid.
- **OSC 0/2** window titles update the SDL window title.
- **Alternate screen** is a second full-size buffer; DECSET 47/1047/1049 swaps
  a pointer, so `vim` and `less` leave your session untouched on exit.
- **Mouse reporting** uses DECSET 1000/1002 for basic events and 1006 for
  SGR-style coordinates; `term_mouse()` encodes and writes to the pty.

## License

MIT — use it however you like.

---

# Русский

## mshell — терминал Termux без root с OpenGL окном и Nadrino OS

Нативный терминал для Termux, работающий без root-прав и без сервера отображения.
Напишите **`!experiment`** в приглашении оболочки, чтобы открыть окно терминала
OpenGL ES 2.0 с той же сессией, или **`!adaptivenadrino`** для запуска рабочего
стола Nadrino OS — виртуальной ОС в стиле Windows 95/98 внутри терминальной сетки.

Напишите **`$simpleinterface`** (или запустите **`--simple`**) для открытия
конфигурационного интерфейса с док-панелью: добавляйте именованные контейнеры,
выбирайте тему (Classic / Win7 Aero), задавайте уровень стабильности box64
и нажимайте **LAUNCH** для запуска выбранной системы.

Выполните **`!setconf`** в лаунчере, чтобы автоматически определить класс
устройства и сохранить рекомендуемые настройки (кадровая частота, история,
буфер чтения) в `~/.mshell.conf` — каждое следующее окно наследует их
автоматически.

## Как это работает

```
mshell (лаунчер)
  │
  ├── Запускает оболочку за pty, дублирует I/O в ваш терминал
  ├── Видит каждый напечатанный байт; обнаруживает `!experiment` и `!adaptivenadrino`
  ├── На `!experiment` → fork/exec: `mshell --gl`
  │   └── OpenGL окно: SDL2 + GLES2 + тот же pty, та же сессия
  │   └── При закрытии окна сессия оболочки продолжается
  │
  ├── На `!adaptivenadrino` → fork/exec: `mshell --os`
  │   └── Рабочий стол Nadrino OS: SDL2 + GLES2, интерфейс Windows 95
  │   └── Плавающие окна, меню Пуск, панель задач, иконки на рабочем столе
  │
  ├── На `$simpleinterface` → fork/exec: `mshell --simple`
  │   └── Конфигурационный интерфейс с док-панелью: контейнеры, темы, стабильность
  │
  └── Без SDL2 → `make launcher` → просто оболочка-обёртка
```

Не нужен root. Не нужен сервер отображения для лаунчера. Окна `--gl`,
`--os` и `--simple` требуют видеодрайвер SDL (Termux:X11, KMS с root или
Wayland композитор).

## Сборка

### Зависимости (Termux)

```bash
pkg update
pkg install clang make libutil-dev
```

### Два режима сборки

```bash
# Полная сборка: лаунчер + GL окно (нужны SDL2 + EGL/GLESv2)
make                       # или: make all

# Только лаунчер (без SDL, без сервера отображения)
make launcher
```

### Пакеты SDL2 (для GL окна)

```bash
pkg install libsdl2
# libutil-dev уже установлен — предоставляет forkpty()
# libGLESv2 и libEGL предоставляются Android bionic во время выполнения
```

## Использование

### Режим лаунчера (по умолчанию)

```bash
./mshell                           # запускает оболочку
!experiment                        # открывает OpenGL окно
!adaptivenadrino                   # открывает рабочий стол Nadrino OS
$simpleinterface                   # открывает конфигурационный интерфейс
# закройте окно → оболочка продолжается
exit                               # готово
```

### Только GL режим

```bash
./mshell --gl                      # сразу OpenGL окно
```

### Режим Nadrino OS

```bash
./mshell --os                      # сразу рабочий стол Nadrino OS
```

### Интерфейс с док-панелью (контейнеры + box64 настройки)

```bash
./mshell --simple                  # сразу интерфейс с док-панелью
```

### Выбрать оболочку

```bash
MSHELL_SHELL=/usr/bin/bash ./mshell
```

### Настройки производительности

```bash
# Автоопределение устройства и сохранение настроек
!setconf

# Явный выбор профиля
!setconf low    # бюджетные устройства (≤4 ядра, <3 ГБ): 30 fps, 200 строк истории
!setconf mid    # средний класс (по умолчанию): 60 fps, 500 строк
!setconf high   # флагманы (≥8 ядер, ≥6 ГБ): 60 fps, 1000 строк

# Переменная окружения для однократного переопределения
MSHELL_PERF=low ./mshell
```

Файл настроек: `~/.mshell.conf`

## Управление (GL окно)

| Действие              | Клавиша(и)        |
|---------------------|-------------------|
| Прокрутка назад         | PgUp              |
| Прокрутка вперёд      | PgDn              |
| Прервать (Ctrl-C)  | Ctrl+C            |
| Выйти из оболочки          | Ctrl-D или exit    |
| Функциональные клавиши       | F1–F12            |
| Нажатие / перетаскивание мыши    | Мышь (в программах, которые просят) |

## Эмуляция терминала

GL-окно реализует те части VT/xterm, которые нужны полноэкранным программам:

- **Альтернативный экран** — `?47`, `?1047` и `?1049`. `vim`, `less`, `htop`
  и `nano` рисуют на втором буфере и восстанавливают ваш сеанс ровно таким,
  каким он был при выходе, вместо того чтобы оставлять за собой мусор.
  Переключение — простая замена указателя, поэтому стоимость нулевая.
- **Отчёты мыши** — `?1000` (клики), `?1002` (перетаскивание) и `?1006` (SGR
  кодировка). Когда программа запрашивает их, клики, перетаскивания и колёсико
  передаются ей как escape-последовательности; когда не запрашивает — мышь
  остаётся у оконного менеджера.
- **SGR-стили** — bold, dim, italic, underline, blink, inverse и hidden.
  Bold осветляет цвет, добавляя 32 к каждому каналу с ограничением до 255,
  поэтому уже яркие цвета остаются корректными.
- **Ответы на запросы** — Device Attributes (`DA`) и Cursor Position
  Report (`DSR 6`), чтобы программы, проверяющие терминал, получали ответ.

## Nadrino OS

Напишите **`!adaptivenadrino`** или запустите **`./mshell --os`** для запуска
рабочего стола Nadrino OS — виртуальной ОС в стиле Windows 95/98 в терминальной
сетке. Возможности:

- **Палитра Windows 95/98**: бирюзовый рабочий стол, тёмно-синие заголовки окон, серая панель задач
- **Плавающие окна**: перетаскивание за заголовок, закрытие `x`, сворачивание
- **Меню Пуск**: нажмите Пуск для переключения
- **Иконки на рабочем столе**: двойной клик (или тап) для запуска приложений
- **Встроенные приложения**:
  - **Терминал** — вывод оболочки
  - **Калькулятор** — базовая арифметика
  - **Редактор текста** — простой ввод текста
  - **Файловый менеджер** — просмотр файлов
  - **О системе** — информация о системе
- **Панель задач**: кнопка Пуск + кнопки окон + часы

### Управление (Nadrino OS)

| Действие              | Клавиша(и)        |
|---------------------|-------------------|
| Клик / перетаскивание        | Мышь             |
| Функциональные клавиши       | F1–F12            |
| Закрыть окно        | Клик `x` кнопка  |
| Переключить меню Пуск   | Клик Пуск       |

## Интерфейс с док-панелью

Напишите **`$simpleinterface`** или запустите **`./mshell --simple`** для
полноэкранного интерфейса док-панели для конфигурации и запуска систем.

Макет: **круглая док-панель** внизу по центру содержит запускаемые приложения,
панель **LAUNCH** внизу экрана, именованные **контейнеры** заполняют левую панель,
**настройки** — правую.

| Действие                       | Клавиша(и)                    |
|------------------------------|---------------------------|
| Перемещение между зонами           | Tab                       |
| Перемещение выделения               | Вверх / Вниз                 |
| Добавить контейнер                | `N`                       |
| Переименовать выбранный контейнер    | `R`                       |
| Удалить выбранный контейнер    | `D`                       |
| Открыть / переключить                | Enter / Space, или клик   |
| Изменить настройке               | Влево / Вправо              |
| Запустить выбранную ОС             | Клик LAUNCH, или `L`      |
| Запустить приложение док-панели              | Клик иконка док-панели         |
| Выйти              | Esc                       |

Настройки:

- **ОС** — Nadrino OS или GL терминал
- **Тема** — Classic (Win95) или Aero (Windows 7)
- **Набор box64** — `Basic`, `Enhanced` или `Stability x3`

**Контейнер** — это просто именованная группа: задайте имя, добавьте иконки приложений,
и он появится в док-панели при выборе контейнера. Имена до 31 символа,
редактируются на месте.

Всё рисуется встроенным 5×7 шрифтом (ASCII 0x20–0x7E), поэтому
дополнительные файлы или локали не требуются.

## Тема Windows 7 / Aero

Nadrino OS имеет два стиля, переключаются из меню Пуск или интерфейса с док-панелью:

- **Classic** — оригинальная палитра Win95/98: бирюзовый рабочий стол, тёмно-синие заголовки.
- **Aero** — стиль Windows 7 с прозрачностью: голубые стеклянные заголовки, светлая рамка окон, мягкие выделения. Применяется как постобработка перекрашивания, поэтому каждое приложение и окно получает её автоматически.

## Серверы отображения

Окно `--gl` требует дисплея:

- **Termux:X11** (без root): установите из репозитория Tur, запустите X сервер,
  `SDL_VIDEODRIVER=x11 ./mshell --gl`
- **KMS/DRM** (root): `SDL_VIDEODRIVER=kmsdrm ./mshell --gl`
- **Лаунчер** не требует дисплея — работает полностью в терминале.

## Структура проекта

```
termux-gl/
├── main.c      # Точка входа: лаунчер + GL окно + Nadrino OS
├── term.c      # Модель сетки, ANSI/VT парсер, буфер прокрутки
├── term.h      # Публичный API терминальной модели
├── gl.c        # GLES2 рендерер (шейдеры, атлас, очередь квадов)
├── gl.h        # API рендерера
├── font.c      # 5×7 растровый шрифт, масштаб 2× в атлас
├── font.h      # Константы шрифта и API атласа
├── os_nadrino.c# Рабочий стол Nadrino OS (интерфейс Win95/Aero, приложения, окна)
├── os_nadrino.h# Структуры данных и API Nadrino OS
├── simple_ui.c # Интерфейс с док-панелью: контейнеры, box64 настройки
├── simple_ui.h # API интерфейса с док-панелью
├── perf.c      # Автоопределение устройства и настройки производительности
├── perf.h      # API определения производительности
├── Makefile    # Два таргета: mshell (полная), mshell-launcher (без SDL)
└── README.md   # Этот файл
```

## Архитектура

### Режим лаунчера

1. `forkpty()` создаёт master fd pty с запущенной оболочкой.
2. `select()` опрашивает stdin (ваша клавиатура) и master pty.
3. Байты от оболочки дублируются в stdout; нажатия клавиш пересылаются в pty.
4. `!experiment` обнаруживается сканированием дублированного вывода — должен начинаться
   команду (после приглашения, новой строки, `;`, `|`, `&`), без кавычек.
5. При срабатывании: `fork()` дочерний процесс, который перезапускает `mshell --gl`; лаунчер
   ожидает его, затем продолжает.

### Режим GL окна

1. SDL2 создаёт контекст OpenGL ES 2.0 на поверхности дисплея.
2. Тот же конвейер `forkpty()` + `select()` + `term_feed()` как в лаунчере.
3. Сетка рендерится каждый кадр: фон + глиф = один `glDrawArrays`.
4. SDL события клавиатуры отображаются в последовательности клавиш через `term_send_key()`.
5. При закрытии окна сессия оболочки (на pty) выживает.

## Технические заметки

- **Цвета упакованы `0xAABBGGRR`** — загружаются в GLES2 как `UNSIGNED_BYTE`
  с `GL_RGBA` без перестановки.
- **Атлас использует `GL_LUMINANCE`** — фрагментный шейдер читает `.r` как покрытие
  и умножает цвет вершины на него.
- **`forkpty()`** создаёт master pty в неблокирующем режиме.
- **Буфер прокрутки** — кольцевой буфер строк. `term_cell()` с `off > 0` возвращает
  ячейки из кольца; `off == 0` возвращает живую сетку.
- **OSC 0/2** заголовки окон обновляют заголовок SDL окна.

## Лицензия

MIT — используйте как угодно.
