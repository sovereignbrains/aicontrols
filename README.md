# AIControls

Нативный клиент для [Claude Code](https://docs.claude.com/en/docs/claude-code) под Windows: чат с Claude,
инструменты с подтверждением каждого действия, секреты, терминал и работа в репозиториях GitHub.
C++23, Win32, Direct2D и DirectWrite — без Electron, без браузера внутри, один exe на несколько мегабайт.

> Не официальный продукт Anthropic. «Claude» — товарный знак Anthropic; клиент лишь запускает
> установленный у вас CLI `claude` и разговаривает с ним.

## Что умеет

**Чат**
- Ответы потоком, markdown: заголовки, списки, цитаты, ссылки, код в строке и блоки кода.
- Вложения: файлы и снимки экрана (`+`, `Ctrl+U`, `Ctrl+V`, перетаскивание); картинки ужимаются до 1568 px.
- Сообщение можно отправить, пока Claude отвечает: оно уходит в текущий ход. `Esc` — остановить.
- Выделение текста мышью через несколько сообщений, `Ctrl+C`.
- Выбор модели и effort, кольцо лимитов тарифа (сессия 5 ч и неделя, время сброса).

**Инструменты Claude** — свой MCP-сервер внутри клиента: `run_command` (PowerShell), `read_file`,
`write_file`, `edit_file`, `list_directory`. Чтение выполняется сразу, всё, что меняет систему, ждёт
«Разрешить» на карточке в ленте; на карточке потом виден вывод команды.

**Секреты** — «имя → значение» в файле, зашифрованном DPAPI (привязан к учётной записи Windows).
Claude видит только имена: значения подставляются в команду (`env_secrets` или `{{secret:ИМЯ}}`)
и вырезаются из вывода. Добавление, правка, переименование и удаление — в сайдбаре.

**Остальное**
- Сайдбар: поиск по чатам, история с продолжением разговора, удаление чата (в корзину), аккаунт.
- Терминал справа: PowerShell через ConPTY и свой VT-эмулятор, вкладки, прокрутка, выделение, копирование.
- Папка чата: папка по умолчанию, недавние, любая папка или репозиторий GitHub — клонируется
  в `%USERPROFILE%\projects` по токену из секрета `GITHUB`.
- Вход в аккаунт Claude и выход (`claude auth`), панели растягиваются, своя шапка вместо заголовка окна.

## Что нужно

- Windows 10 1809+ или Windows 11 (x64).
- Установленный [Claude Code](https://docs.claude.com/en/docs/claude-code/setup) — нативный `claude.exe`
  в `%USERPROFILE%\.local\bin` или в `PATH` — и подписка Claude (Pro или Max).
- Для репозиториев GitHub — `git` в `PATH` и токен в секрете `GITHUB`.

## Сборка

Visual Studio 2022 Build Tools (MSVC, C++23), CMake 3.28+, Ninja и [vcpkg](https://vcpkg.io) в `C:\vcpkg`
(путь задан в `CMakePresets.json`). Зависимости — `wil` и `nlohmann-json` — vcpkg ставит сам.

```bat
tools\build.cmd dev        :: build\dev\aicontrols.exe (Debug)
tools\build.cmd dist       :: build\dist\aicontrols.exe (Release)
tools\dist.cmd 0.1.0       :: + установщик build\installer\AIControls-Setup-0.1.0.exe (Inno Setup 6)
tools\build.cmd analyze    :: то же с MSVC /analyze — перед каждым пушем
ctest --test-dir build\dev :: тесты ядра
```

Перенести секреты из другой программы, не показывая значений: передайте их ей переменными окружения
и запустите `aicontrols.exe --import-secrets ИМЯ1,ИМЯ2`. Список имён — `aicontrols.exe --list-secrets`.

## Устройство

```
src/app    окно: лента, поле ввода, сайдбар, терминал, панели (Direct2D/DirectWrite, RichEdit)
src/core   всё без интерфейса:
           claude_process  claude.exe в stream-json, очередь до initialize, job object
           tools           инструменты и MCP-хост      secrets   DPAPI-хранилище
           markdown        разбор в текст и стили      store     история чатов, настройки
           vt_screen, pty  эмулятор терминала, ConPTY  auth      claude auth, запуск CLI
           github          список репозиториев, клон   attachment  вложения
tests      тесты ядра; tests/probes — пробники, запускаются вручную
docs       протокол claude.exe: docs/protocol.md
```

Настройки — `%APPDATA%\AIControls\settings.json`, секреты — `%APPDATA%\AIControls\secrets.bin`.

## Лицензия

Код — [MIT](LICENSE). Шрифты в `assets/fonts` (Unbounded, Golos Text, JetBrains Mono) — SIL Open Font
License, тексты лицензий лежат рядом с ними.
