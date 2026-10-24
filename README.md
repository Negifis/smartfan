# SmartFan

Умное управление вентиляторами для **Mac на Intel**: обороты задаёт не один датчик процессора, а самый «опасный» из десятков датчиков — процессор, видеокарта, SSD, чипсет, Thunderbolt, батарея, корпус. Плюс значок в строке меню с температурой и переключением режимов.

> **Только для Intel.** На Mac с Apple Silicon (M1–M4) SmartFan не работает и пока не поддерживается: устройства для разработки и проверки у меня нет. Установщик на таких Mac просто откажется ставиться. Если у тебя есть Mac на M-процессоре и желание помочь — пиши в Issues.

*English summary is [below](#english).*

## Зачем

Обычные программы для вентиляторов (Macs Fan Control и подобные) дают привязать вентилятор только к одному датчику. Но у ноутбука горячих мест много: процессор может быть холодным, а SSD, батарея или Thunderbolt-контроллер (особенно при зарядке слева) — уже перегреваться. А если привязать всё к процессору, его датчик скачет от каждой вкладки браузера, и вентиляторы то воют, то затихают.

SmartFan решает это так:

- **У каждого датчика свои пороги** «старт → максимум». Процессору можно 68→92°C, SSD — 50→68°C, батарее — 38→47°C, корпусу под ладонями — 40→50°C.
- **Вентиляторы слушаются самого близкого к своему пределу датчика.** SSD на 66°C раскрутит их сильнее, чем процессор на 75°C.
- **Плавно, без ступенек**: скачущие датчики ядер сглаживаются, разгон быстрый (до +600 об/мин в секунду), замедление медленное (−120 об/мин в секунду) — вентиляторы не «дышат».
- **Защита**:
  - процессор или видеокарта на 96°C — сразу полные обороты;
  - датчики перестали читаться, служба остановилась или упала — вентиляторы возвращаются под автоматику macOS;
  - упавшую службу macOS перезапускает через несколько секунд.

## Значок в строке меню

На значке — температура самой горячей точки и средние обороты, например `62° 2160`. Значок оранжевый, когда вентиляторы на 70% и выше, и красный на пределе или если служба не отвечает.

В меню:

- самая горячая точка и «ведущий» датчик, который сейчас задаёт обороты;
- обороты каждого вентилятора;
- 8 датчиков, ближайших к своему порогу, и подменю со всеми датчиками;
- **режимы**:
  - *Умный* — по самому горячему датчику (по умолчанию);
  - *Тихий* — все пороги на 6°C выше;
  - *Максимум* — вентиляторы на полную;
  - *Apple (авто)* — вентиляторами управляет macOS;
- «Пороги датчиков…» — открыть файл настроек.

Интерфейс пока только на русском.

## Поддерживаемые Mac

| Модель | Статус |
|---|---|
| MacBookPro15,1 — MacBook Pro 15" 2018–2019 | готовый набор датчиков, проверено на 2019 (i7-9750H, macOS Sequoia 15.8) |
| другие Mac на Intel | должно работать, но нужен свой набор датчиков — см. ниже |
| Apple Silicon (M1–M4) | не поддерживается |

Требуется macOS 13 Ventura или новее.

## Установка из релиза

1. Скачай `SmartFan-<версия>-intel.zip` со страницы [Releases](../../releases) и распакуй.
2. В Терминале:
   ```bash
   cd ~/Downloads/SmartFan-1.0.0-intel
   sudo ./install.sh
   ```

Установщик:
- ставит службу `/usr/local/libexec/smartfan`, которая запускается при старте Mac;
- кладёт значок в `~/Applications/SmartFan.app` и добавляет его в автозапуск при входе;
- копирует пороги датчиков для твоей модели в `~/Library/Application Support/SmartFan/sensors.conf`.

Файлы подписаны ad-hoc, без сертификата разработчика. Поэтому установщик сам снимает с них карантин macOS.

Если стоит **Macs Fan Control** или похожая программа — выбери в ней режим Auto или закрой её, иначе они будут перехватывать вентиляторы друг у друга.

## Сборка из исходников

Нужны Command Line Tools (`xcode-select --install`).

```bash
git clone https://github.com/Negifis/smartfan.git
cd smartfan
make
sudo ./install.sh
```

`make release` собирает zip для релиза в `dist/`.

## Настройка порогов

Файл: `~/Library/Application Support/SmartFan/sensors.conf`. Формат строки:

```
КЛЮЧ  СТАРТ  МАКСИМУМ  описание
TH0a  50     68        SSD NAND
```

После правки перезапусти службу:

```bash
sudo launchctl kickstart -k system/local.smartfan
```

### Другая модель Mac на Intel

Датчики у разных моделей разные. Посмотреть свои:

```bash
smartfan-dump      # после установки; или ./build/smartfan-dump
```

Покажет все ключи температур (`T…`) и вентиляторов (`F…`) с текущими значениями. Значения `-127` и `0` означают, что датчика нет.

Скопируй `config/MacBookPro15,1.conf` в `config/<твоя модель>.conf`, оставь только существующие у тебя ключи и поправь пороги. Модель можно узнать командой `sysctl -n hw.model`. Буду рад pull request'у с конфигом для твоего Mac.

## Что где лежит

| Что | Где |
|---|---|
| Служба (root) | `/usr/local/libexec/smartfan`, `/Library/LaunchDaemons/local.smartfan.plist` |
| Значок | `~/Applications/SmartFan.app`, `~/Library/LaunchAgents/local.smartfan.menu.plist` |
| Пороги и режим | `~/Library/Application Support/SmartFan/sensors.conf`, `mode` |
| Состояние и лог | `/Library/Logs/SmartFan/status.txt`, `status.json`, `smartfan.log` |

Что служба делает прямо сейчас:

```bash
cat /Library/Logs/SmartFan/status.txt
```

## Удаление

```bash
sudo ./install.sh --uninstall
```

Вентиляторы сразу вернутся под управление macOS. Файл с порогами останется.

## Как это устроено

- `src/smc.c` — чтение и запись ключей контроллера SMC через IOKit.
- `src/smartfan.c` — служба. Раз в секунду читает датчики, считает для каждого долю пути от «старта» до «максимума», берёт максимум и переводит его в обороты от минимальных до максимальных для каждого вентилятора. Пишет в SMC `F<n>Md = 1` (ручной режим) и `F<n>Tg` (целевые обороты), при выходе — `F<n>Md = 0`.
- `src/menubar.swift` — значок в строке меню. Читает `status.json`, режим передаёт службе через файл `mode`.
- `src/dump.c` — список всех датчиков и ключей вентиляторов.

## Отказ от ответственности

SmartFan напрямую управляет вентиляторами через SMC. Защита от перегрева у macOS и самого контроллера остаётся, а служба при любой ошибке возвращает управление macOS. Но всё равно используй на свой риск.

## Лицензия

MIT

---

## English

**SmartFan** is a fan controller for **Intel Macs**. Fan speed follows whichever of dozens of SMC temperature sensors is closest to its own limit — CPU, GPU, SSD, chipset, Thunderbolt controllers, battery, palm rest — not just the CPU. Each sensor has its own start/max thresholds. Changes are smoothed: core sensors are filtered, ramp-up is fast and ramp-down slow.

Safety:
- CPU or GPU at 96°C forces full speed;
- on exit, lost sensors or SMC errors the fans are handed back to macOS.

A menu bar app shows the hottest temperature and fan speed and switches modes (smart / quiet / max / Apple auto). UI is Russian only for now.

**Intel only.** Apple Silicon is not supported yet — I don't have an M-series Mac to develop and test on. The installer refuses to run on Apple Silicon.

Tested on MacBookPro15,1 (MacBook Pro 15" 2019, i7-9750H, macOS Sequoia 15.8). Other Intel Macs need their own sensor list: run `smartfan-dump`, then copy `config/MacBookPro15,1.conf` to `config/<hw.model>.conf` and edit it. PRs welcome.

Install from a [release](../../releases):

```bash
cd ~/Downloads/SmartFan-1.0.0-intel
sudo ./install.sh              # install / update
sudo ./install.sh --uninstall  # remove
```

Build from source with `make`; requires macOS 13+ and Command Line Tools.

Use at your own risk. MIT license.
