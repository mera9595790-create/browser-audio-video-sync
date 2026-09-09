# AV Sync Delay — компенсация задержки аудио

Универсальный инструмент для Windows: исправляет рассинхронизацию видео и звука при
просмотре в браузерах через наушники с задержкой звука — **Bluetooth (A2DP)** или
**проводные** устройства с буферизацией. Задержка звука (~150–250 мс и больше, в
зависимости от устройства) не учитывается системой, поэтому видео опережает звук.
Программа внедряет DLL в аудио-процессы браузеров и подменяет показания
`IAudioClock::GetPosition`, заставляя штатный A/V-sync браузера задерживать видео ровно
на измеренную задержку.

Подход повторяет идею
[gurux13/chrome-audio-delay](https://github.com/gurux13/chrome-audio-delay)
и статьи https://habr.com/ru/articles/664966/, но работает со всеми браузерами:
**Chrome, Edge, Яндекс.Браузер** (общий механизм `audio.mojom.AudioService`) и
**Firefox** (AudioIPC/главный процесс). Идея теста задержки звука/видео взята из
[Trogen898/Frame-Sync](https://github.com/Trogen898/Frame-Sync).

## Возможности

- **Единый exe** `avsync_measure.exe`: DLL-хук встроен внутрь, отдельные файлы не нужны.
- **Автоматическое внедрение** (watcher): при запуске сам находит аудио-процессы
  браузеров и инжектит встроенную DLL.
- **Замер задержки** на заданной частоте (WASAPI render + захват микрофона + детектор
  Гёртцеля) в отдельном системном окне — вне браузера.
- **Калибровка микрофона**: поднесите микрофон к клавиатуре и нажмите ПРОБЕЛ — программа
  измерит собственную задержку входа микрофона и вычтет её из результата.
- **Per-browser задержки**: для каждого браузера своя задержка видеотракта
  (по умолчанию Chromium/Яндекс 277 мс, Firefox 447 мс — это собственное время вывода
  видео браузером). Итоговая задержка считается как
  `измеренная_задержка − задержка_микрофона − задержка_видеотракта`.
- **Применение на лету** через shared memory — браузеры подхватывают сразу, без
  перезапуска.
- **Трей**: иконка в трее всегда; сворачивание прячет окно в трей, при закрытии
  спрашивает «отключить задержку и выйти / свернуть в трей».
- **Автозапуск** при входе в Windows (свёрнуто в трей).
- **Два языка интерфейса**: русский (по умолчанию) и английский.

## Как пользоваться

1. Запустите `avsync_measure.exe`.
2. Подключите Bluetooth-наушники, выберите устройства воспроизведения/записи.
3. (Рекомендуется) откалибруйте микрофон кнопкой «Калибровка».
4. Запустите «Тест задержки» → нажмите «Остановить тест», чтобы заполнить медиану.
5. Нажмите «Применить» — задержка запишется в shared memory и применится к браузерам.
6. Кнопка «Отключить» ставит задержку 0 (сквозной режим).

## Сборка (из WSL/Linux, без sudo)

Тулчейн: портативный llvm-mingw в `~/.local/llvm-mingw`.

```
./build.sh          # -> build/avsync_hook.dll, build/avsync.exe, build/avsync_measure.exe
```

`build.sh` сначала собирает `avsync_hook.dll`, затем встраивает её байты в
`avsync_measure.exe` (через `embed.py`), так что для штатной работы нужен только один exe.

## Структура

| Путь | Назначение |
|---|---|
| `dll/avsync_hook.dll` | Хук vtable `IAudioClock::GetPosition` (общая таблица на процесс). Задержка из shared memory, меняется на лету. |
| `injector/avsync.exe` | Консольная диагностика: `list / inject / watch / setdelay / sessions / devices / selftest / getdelay`. |
| `measure/avsync_measure.exe` | GUI: замер + калибровка + конфигурация + watcher + трей + автозапуск. |
| `common/manualmap.h` | Manual-map загрузчик и энумерация процессов (общий для injector и GUI). |
| `common/config.h` | Структура shared-memory конфига (`Local\AVSyncDelayConfig`). |

## Как это работает

1. Watcher каждые ~800 мс сканирует процессы: Chromium-аудио
   (`--utility-sub-type=audio.mojom.AudioService`) и главный процесс Firefox.
2. В новый аудио-процесс выполняется **manual mapping**: образ DLL копируется в процесс,
   релокации и импорты настраиваются PIC-загрузчиком, вызывается точка входа.
   `LoadLibrary` для самой DLL не используется — подпись Microsoft не требуется
   (нужна только для импортов: kernel32/ole32).
3. DLL строит цепочку COM (`IMMDeviceEnumerator → IMMDevice → IAudioClient → IAudioClock`)
   и патчит общую vtable: `GetPosition` начинает возвращать позицию, отстающую на задержку.
4. A/V-sync браузера видит «отстающий» звук и задерживает видео.

## Предупреждения

- **Антивирус**: инжектор использует классическое внедрение (CreateRemoteThread + ручная
  загрузка образа) — Windows Defender может пометить его как HackTool. Собирайте из
  исходников локально.
- Внедрение в процессы браузера формально может нарушать их EULA; инструмент — для
  личного использования на своей машине.
- Задержка применяется к аудиочасам процесса: проводные устройства получат ту же
  задержку.

---

# AV Sync Delay — audio delay compensation

A universal Windows tool that fixes video/audio desync when watching videos in a
browser over headphones with audio latency — **Bluetooth (A2DP)** or **wired** devices
with buffering. The audio delay (~150–250 ms and up, depending on the device) is not
accounted for by the OS, so video leads audio. The program injects a DLL into the
browsers' audio processes and patches `IAudioClock::GetPosition`, making the browser's
native A/V-sync delay the video by exactly the measured delay.

The approach follows
[gurux13/chrome-audio-delay](https://github.com/gurux13/chrome-audio-delay)
and https://habr.com/ru/articles/664966/, but works with all browsers:
**Chrome, Edge, Yandex.Browser** (shared `audio.mojom.AudioService` mechanism) and
**Firefox** (AudioIPC / main process). The audio/video delay test idea is taken from
[Trogen898/Frame-Sync](https://github.com/Trogen898/Frame-Sync).

## Features

- **Single exe** `avsync_measure.exe`: the hook DLL is embedded, no separate files needed.
- **Automatic injection** (watcher): finds browser audio processes at startup and injects
  the embedded DLL.
- **Delay measurement** at a chosen frequency (WASAPI render + mic capture + Goertzel
  detector) in a standalone system window — outside the browser.
- **Microphone calibration**: hold the mic to the keyboard and press SPACE — the program
  measures the mic input latency and subtracts it from the result.
- **Per-browser delays**: each browser has its own video-path latency (defaults:
  Chromium/Yandex 277 ms, Firefox 447 ms — the browser's own video output time).
  Final delay = `measured − mic_latency − video_path_latency`.
- **Hot apply** via shared memory — browsers pick it up immediately, no restart.
- **Tray**: icon always present; minimize hides to tray; on close it asks
  "disable delay and exit / minimize to tray".
- **Autostart** at Windows logon (minimized to tray).
- **Two UI languages**: Russian (default) and English.

## Usage

1. Run `avsync_measure.exe`.
2. Connect Bluetooth headphones, pick playback/recording devices.
3. (Recommended) calibrate the mic with the "Calibrate" button.
4. Run "Delay test" → press "Stop Test" to fill the median.
5. Press "Apply" — the delay is written to shared memory and applied to browsers.
6. "Disable" sets the delay to 0 (pass-through).

## Building (from WSL/Linux, no sudo)

Toolchain: portable llvm-mingw at `~/.local/llvm-mingw`.

```
./build.sh          # -> build/avsync_hook.dll, build/avsync.exe, build/avsync_measure.exe
```

`build.sh` first builds `avsync_hook.dll`, then embeds its bytes into
`avsync_measure.exe` (via `embed.py`), so only one exe is needed in normal use.

## Layout

| Path | Purpose |
|---|---|
| `dll/avsync_hook.dll` | Patches the `IAudioClock::GetPosition` vtable (shared per process). Delay comes from shared memory, hot-swappable. |
| `injector/avsync.exe` | Console diagnostics: `list / inject / watch / setdelay / sessions / devices / selftest / getdelay`. |
| `measure/avsync_measure.exe` | GUI: measurement + calibration + configuration + watcher + tray + autostart. |
| `common/manualmap.h` | Manual-map loader and process enumeration (shared by injector and GUI). |
| `common/config.h` | Shared-memory config struct (`Local\AVSyncDelayConfig`). |

## How it works

1. The watcher scans processes every ~800 ms: Chromium audio
   (`--utility-sub-type=audio.mojom.AudioService`) and the Firefox main process.
2. A new audio process gets **manual mapping**: the DLL image is copied in, relocations
   and imports are fixed by a PIC loader, then the entry point runs. `LoadLibrary` is not
   used for the DLL itself — no Microsoft signature needed (only for its imports:
   kernel32/ole32).
3. The DLL builds a COM chain (`IMMDeviceEnumerator → IMMDevice → IAudioClient →
   IAudioClock`) and patches the shared vtable so `GetPosition` lags by the delay.
4. The browser's A/V-sync sees "late" audio and delays the video.

## Warnings

- **Antivirus**: the injector uses classic injection (CreateRemoteThread + manual image
  load) — Windows Defender may flag it as HackTool. Build locally from source.
- Injecting into browser processes may formally violate their EULA; the tool is intended
  for personal use on your own machine.
- The delay is applied to the process audio clock: wired devices get the same delay.
