# План портирования: Android Matrix-клиент → gomuks (Go)

**Исходный проект:** `progressive-android/` — C++/Kotlin Matrix-клиент (~110 C++ файлов, Gradle+CMake)
**Целевой проект:** `gomuks/` — CLI Matrix-клиент на Go (TUI, mautrix-go, SQLite)
**Дата:** Май 2026

---

## Раздел 1: Gap-анализ (сравнение функциональности)

### 1.1 Аутентификация

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| Логин по паролю | ✅ Gomuks | `hicli/login.go`, `login_enhanced.go` | C++/Kotlin SDK |
| SSO/OIDC логин | 🔶 Частично | `gomuks/sso.go`: веб-сервер для обработки редиректа, `login_enhanced.go`: `GetSSOLoginURL()` | Полный OIDC-менеджер (`oidc_manager.cpp`, `oidc_auth.cpp`), Custom Tab, refresh token |
| Token login | ✅ Gomuks | `login_enhanced.go`: `LoginToken()` | Есть |
| Refresh token management | ❌ Отсутствует | Нет | `oidc_manager.cpp` — автоматический refresh токенов |
| QR-код логин | ❌ Отсутствует | Нет | `qrcode/` — сканер QR, `LoginMode.kt` |
| Well-known (.well-known) | 🔶 Частично | `login_enhanced.go`: `DiscoverLoginFlows()` — разрешение `.well-known` перед логином | `well_known.cpp`, HomeServerConnectionConfig |
| SSO Providers (список IdP) | 🔶 Частично | `login_enhanced.go`: структуры `LoginFlowResult`, `SSOProvider`, но без UI выбора провайдера | Полный список провайдеров в UI |
| CAPTCHA | ❌ Отсутствует | Нет | `LoginCaptchaFragment.kt` |
| Terms of Service (соглашения) | ❌ Отсутствует | Нет | `terms/`, `LoginWebFragment.kt` |
| Сброс пароля | ❌ Отсутствует | Нет | `LoginResetPasswordFragment.kt` |
| Регистрация | ❌ Отсутствует | Нет | `LoginSignUpSignInSelectionFragment.kt` |

### 1.2 Сетевой транспорт

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| SOCKS5 прокси | ✅ Gomuks | `hicli/proxy.go`: SOCKS5 через `golang.org/x/net/proxy` | `proxy.cpp` |
| HTTP прокси | ✅ Gomuks | `hicli/proxy.go`: HTTP прокси через `http.ProxyURL` | `proxy.cpp` |
| Выбор типа подключения (Tor/I2P/Yggdrasil) | ✅ Gomuks | `tui/view-login.go`: селектор, `tui/yggdrasil.go`: определение адресов Yggdrasil | Нет (Android не поддерживает) |
| Кастомный DNS | ❌ Отсутствует | Нет | Android SDK позволяет |
| TLS certificate pinning | ❌ Отсутствует | Нет | `http_client.cpp`, HomeServerConnectionConfig |
| Network monitor (онлайн/офлайн) | ❌ Отсутствует | Нет | `network_monitor.cpp`, `connection-status.go` — только индикатор |
| Авто-реконнект | ✅ Gomuks | `hicli/sync.go`: автоматический перезапуск синхронизации | `sync_handler.cpp` |

### 1.3 Шифрование (E2EE)

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| Olm (1-1 шифрование) | ✅ Gomuks | mautrix-go `crypto` пакет | `olm.cpp`, `olm_session.cpp` |
| Megolm (групповое шифрование) | ✅ Gomuks | mautrix-go, `sync.go`: обработка событий, `decryptionqueue.go` | `megolm_decryptor.cpp` |
| Кросс-подпись (Cross-Signing) | 🔶 Частично | `hicli/verify.go`: загрузка/генерация ключей, подпись своих устройств, загрузка из SSSS; **нет UI** | Полный `cross_signing.cpp`, `cross_signing_manager.cpp`, +UI |
| SSSS (Secure Secret Storage) | 🔶 Частично | `hicli/verify.go`: `SSSS.GetDefaultKeyData()`, `Verify()`, `ResetEncryption()`; **нет UI** | `securestorage/` |
| Key Backup (резервное копирование ключей) | 🔶 Частично | `hicli/backupupload.go`: загрузка ключей, `RestoreKeyBackup()`; **нет UI** | `key_backup.cpp`, `key_backup_manager.cpp`, `keysbackup/` UI |
| SAS верификация (Emoji сравнение) | 🔶 Частично | Бэкенд в mautrix-go `crypto`; **нет TUI-интерфейса** | `sas_verification.cpp`, `verification/` UI |
| QR-код верификация | ❌ Отсутствует | Нет | `verification/` (QR-режим SAS) |
| Запрос ключей (Key re-request) | ✅ Gomuks | `sync.go`: `HandleRoomKeyRequest`, `HandleSecretRequest` | `keyshare.cpp`, `keysrequest/` |

### 1.4 Сессия

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| Multi-account (несколько аккаунтов) | ❌ Отсутствует | Одна сессия в `HiClient` | `session/`, `session_manager.cpp`, переключение аккаунтов |
| Токен-стор (хранение токенов) | ✅ Gomuks | SQLite в `database` пакете | `session/`, SecureStorage |
| Logout (выход) | ✅ Gomuks | `gomuks/logout.go` | `signout/` |
| Device management (управление устройствами) | ❌ Отсутствует | Нет | `device_manager.cpp`, `device_manager_full.cpp` |

### 1.5 TUI-интерфейс

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| Список комнат | ✅ Gomuks | `tui/room-list.go` | RoomListFragment + C++ бэкенд |
| Таймлайн сообщений | ✅ Gomuks | `tui/room-view.go`, `tui/message-view.go` | TimelineFragment |
| Композер (ввод сообщений) | ✅ Gomuks | `tui/room-view.go` (встроен) | Composer |
| Markdown/HTML рендеринг | ✅ Gomuks | `tui/messages/htmlmessage.go`, `goldmark` | `markdown.cpp`, `message_content.cpp` |
| Emoji | ✅ Gomuks | `emojirunes`, Unicode-рендеринг | EmojiCompat |
| Отправка файлов/изображений | 🔶 Частично | `tui/messages/filemessage.go` — отображение; отправка через команды | Полная поддержка + превью |
| Медиа-просмотрщик (полноэкранный) | ❌ Отсутствует | Нет | `media_viewer.cpp`, `media/`, `attachment-viewer/` |
| Редактирование сообщений | 🔶 Частично | В `sync.go`: обработка `RelReplace`, но **нет UI редактирования** | Полная поддержка редактирования |
| Удаление сообщений (redaction) | ✅ Gomuks | `sync.go`: `processRedaction` | Есть |
| Реакции (Reactions) | 🔶 Частично | В `sync.go`: обработка `RelAnnotation`; **нет UI** | `reactions/` |
| Упоминания (Mentions) | 🔶 Частично | Push-правила учитывают; **нет UI-подсветки** | `mention_parser.cpp` |
| Цитирование (Reply) | 🔶 Частично | Загрузка replied-to события в `sync.go`; **нет UI кнопки Reply** | Полноценный reply UI |
| Треды (Threads) | ❌ Отсутствует | Нет | `thread_manager.cpp`, `thread_aggregator.cpp` |
| Spaces (пространства) | 🔶 Частично | `sync.go`: `spaceDataCollector`, `SpaceEdge`; **нет UI навигации по Space** | `spaces/`, `space_utils.cpp` |
| Голосования (Polls) | ❌ Отсутствует | Нет | `poll/`, `poll_utils.cpp` |
| Локация (Location sharing) | ❌ Отсутствует | Нет | `location/`, `message_location.cpp` |
| Стикеры | 🔶 Частично | Обработка `EventSticker`; **нет отправки** | Есть |
| Slash-команды | ✅ Gomuks | `tui/localcommands.go`, `tui/commands.go` | `slash_command.cpp` |
| Поиск сообщений (Full-text search) | ❌ Отсутствует | Нет | `search_index.cpp` |
| Fuzzy-поиск по комнатам | ✅ Gomuks | `tui/fuzzy-search-modal.go` | Нет (другая реализация) |
| Прямые чаты (DM) | ✅ Gomuks | `hicli/direct.go`, `sync.go`: обработка `DirectChats` | Есть |
| Список участников комнаты | 🔶 Частично | `tui/member-list.go` — базовый; **нет поиска/сортировки/фильтра** | Полный список с поиском |
| Профиль пользователя | 🔶 Частично | `hicli/profile.go` — загрузка; **нет TUI-просмотра профиля** | `roommemberprofile/` |
| Автодополнение (Autocomplete) | ❌ Отсутствует | Нет | `autocomplete/` |
| Форматирование текста | ✅ Gomuks | Markdown через `/me`, `/notice`, HTML-парсинг | `text_formats.cpp` |
| Draft (черновики сообщений) | ❌ Отсутствует | Нет | `draft_manager.cpp` |
| История аватаров | ❌ Отсутствует | Нет | `avatar_history.cpp` |
| Быстрый просмотр URL (URL preview) | ❌ Отсутствует | Нет | `url_preview.cpp` |

### 1.6 Данные (офлайн, кеш, поиск)

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| Офлайн-кеш событий (SQLite) | ✅ Gomuks | `hicli/database/` — полный кеш в SQLite | `eventcache.cpp`, `eventdb.cpp` |
| Медиа-кеш (файлы, изображения) | 🔶 Частично | `sync.go`: `addMediaCache()` — референции; **нет автоматической загрузки/кеширования медиа** | Полный медиа-кеш, `offline_cache.cpp` |
| Полнотекстовый поисковый индекс | ❌ Отсутствует | Нет | `search_index.cpp` (FTS) |

### 1.7 Звонки (VoIP)

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| WebRTC аудио/видео звонки | ❌ Отсутствует | Нет | `call/`, `call_manager.cpp`, `webrtc/` |
| Конференции | ❌ Отсутствует | Нет | `conference/` |
| Переключение аудиоустройств | ❌ Отсутствует | Нет | `CallSoundDeviceChooserBottomSheet.kt` |

### 1.8 Платформенная интеграция

| Возможность | Статус | Реализация в gomuks | Реализация в Android |
|---|---|---|---|
| Desktop-уведомления | 🔶 Частично | Web Push (`gomuks/push.go`, `push_disabled.go`); **нет системных уведомлений в CLI-режиме** | `notifications/`, `notif_*.cpp` |
| Звуковые уведомления | 🔴 Частично | Конфигурация есть (`NotifySound`), реализация отсутствует | Есть |
| Deeplinks (matrix.to, matrix:) | ❌ Отсутствует | Нет | `matrixto/`, `permalink/` |
| Шеринг (поделиться файлом в Matrix) | ❌ Отсутствует | Нет | `share/` |
| Импорт/Экспорт данных | ❌ Отсутствует | Нет | `exporter.cpp`, `importer/` |
| Интеграция виджетов | ❌ Отсутствует | Нет | `widgets/`, `widget_manager.cpp` |
| Экран блокировки (PIN) | ❌ Отсутствует | Нет | `pin/` |

---

## Раздел 2: Стратегия портирования

Для каждой отсутствующей или частичной функции указан подход, целевые пакеты, зависимости, трудозатраты и риски.

### 2.1 Аутентификация

| Функция | Подход | Целевые пакеты | Go-зависимости | Трудоёмкость | Риск |
|---|---|---|---|---|---|
| **OIDC SSO логин** (полный цикл) | Порт логики из `oidc_manager.cpp` / `oidc_auth.cpp` в Go | `pkg/hicli/login_oidc.go`, `tui/view-login.go` | `golang.org/x/oauth2`, `github.com/coreos/go-oidc/v3` | Medium (1-3d) | Low |
| **Refresh Token Management** | Нативная реализация в Go | `pkg/hicli/token_refresh.go` | `golang.org/x/oauth2` | Medium (1-3d) | Low |
| **QR-код логин** | Порт из `features/qrcode/` | `pkg/hicli/login_qr.go`, `tui/widget/qr-login.go` | `github.com/makiuchi-d/gozxing` (QR-декодинг), `github.com/yeqown/go-qrcode/v2` (генерация) | Large (3-7d) | Med |
| **Список SSO провайдеров (UI)** | Доработка UI на основе уже имеющихся структур `SSOProvider` | `tui/view-login.go` | Нет | Small (<1d) | Low |
| **CAPTCHA** | Нативная реализация | `tui/widget/captcha.go` | Нет (внешний браузер/URL) | Small (<1d) | Low |
| **Terms of Service** | Нативная реализация | `tui/widget/terms.go` | Нет | Small (<1d) | Low |
| **Сброс пароля** | Нативная реализация | `tui/view-reset-password.go` | Нет | Small (<1d) | Low |
| **Регистрация** | Нативная реализация | `tui/view-register.go`, `pkg/hicli/register.go` | `mautrix-go` (уже есть `Register`) | Medium (1-3d) | Low |

### 2.2 Сетевой транспорт

| Функция | Подход | Целевые пакеты | Go-зависимости | Трудоёмкость | Риск |
|---|---|---|---|---|---|
| **TLS Certificate Pinning** | Нативная реализация | `pkg/hicli/tls_pinning.go` | Стандартный `crypto/tls` + `crypto/x509` | Medium (1-3d) | Med |
| **Кастомный DNS** | Нативная реализация через `net.Resolver` | `pkg/hicli/dns.go` | Стандартный `net` | Small (<1d) | Low |
| **Network monitor** | Порт из `network_monitor.cpp` | `pkg/hicli/network_monitor.go` | Нет | Small (<1d) | Low |

### 2.3 Шифрование

| Функция | Подход | Целевые пакеты | Go-зависимости | Трудоёмкость | Риск |
|---|---|---|---|---|---|
| **UI кросс-подписи** | Нативный TUI-интерфейс поверх существующего `verify.go` | `tui/widget/cross-signing.go` | Нет (mautrix-go уже всё умеет) | Large (3-7d) | Med |
| **UI Key Backup** | Нативный TUI-интерфейс поверх `backupupload.go` | `tui/widget/key-backup.go` | Нет | Medium (1-3d) | Low |
| **SAS Emoji верификация (TUI)** | Нативный TUI для процесса SAS уже в mautrix-go | `tui/widget/sas-verify.go` | Нет (mautrix-go `crypto`) | Large (3-7d) | Med |
| **QR верификация (TUI)** | Генерация QR в терминале + сканирование через внешнее приложение | `tui/widget/qr-verify.go` | `github.com/mdp/qrterminal` (ASCII QR) | Medium (1-3d) | Med |
| **Device management UI** | Нативный TUI | `tui/device-list.go`, `pkg/hicli/device_manager.go` | Нет | Medium (1-3d) | Low |

### 2.4 Сессия

| Функция | Подход | Целевые пакеты | Go-зависимости | Трудоёмкость | Риск |
|---|---|---|---|---|---|
| **Multi-account** | Порт из `session_manager.cpp` — абстракция `SessionManager`, переключение активной сессии | `pkg/hicli/session_manager.go`, `tui/account-switcher.go` | Нет | X-Large (1-3w) | High |
| **Token store для multi-account** | Расширение существующей `database.Account` | `pkg/hicli/database/account.go` | Нет | включено в Multi-account | - |

### 2.5 TUI-интерфейс

| Функция | Подход | Целевые пакеты | Go-зависимости | Трудоёмкость | Риск |
|---|---|---|---|---|---|
| **Медиа-просмотрщик (ASCII/Kitty/iTerm2)** | Порт из `media_viewer.cpp` — отрисовка изображений в терминале | `tui/widget/media-viewer.go` | `github.com/qeesung/image2ascii` или Kitty/iTerm протокол | Large (3-7d) | Med |
| **UI редактирования сообщений** | Нативный TUI — режим редактирования в композере | `tui/room-view.go`, `tui/messages/edit.go` | Нет | Medium (1-3d) | Low |
| **UI реакций** | Нативный TUI — панель выбора реакции | `tui/widget/reactions.go` | Нет | Medium (1-3d) | Low |
| **Mentions-подсветка** | Доработка `htmlmessage.go` | `tui/messages/htmlmessage.go` | Нет | Small (<1d) | Low |
| **Reply UI** | Нативный TUI | `tui/room-view.go` | Нет | Small (<1d) | Low |
| **Threads** | Порт из `thread_manager.cpp`, `thread_aggregator.cpp` | `pkg/hicli/threads.go`, `tui/thread-view.go` | `mautrix-go` поддержка `m.thread` | X-Large (1-3w) | High |
| **Spaces UI** | Нативный TUI поверх существующего `SpaceEdge` | `tui/space-nav.go`, `tui/widget/space-panel.go` | Нет | Large (3-7d) | Med |
| **Polls (голосования)** | Порт из `poll_utils.cpp` | `pkg/hicli/polls.go`, `tui/widget/poll-view.go`, `tui/widget/poll-create.go` | Нет | Medium (1-3d) | Low |
| **Location sharing** | Порт из `location/` — отображение и отправка координат | `pkg/hicli/location.go`, `tui/widget/location-view.go` | `github.com/paulmach/orb` (GeoJSON) | Medium (1-3d) | Med |
| **Full-text search** | Порт из `search_index.cpp` — FTS на SQLite | `pkg/hicli/search.go`, `tui/search-view.go` | SQLite FTS5 (уже в дереве зависимостей) | Large (3-7d) | Med |
| **Autocomplete** | Нативный TUI — выпадающее меню с подсказками | `tui/widget/autocomplete.go` | Нет | Medium (1-3d) | Low |
| **Черновики (Drafts)** | Нативная реализация | `pkg/hicli/drafts.go` | SQLite | Small (<1d) | Low |
| **История аватаров** | Нативная реализация | `pkg/hicli/avatar_history.go` | Нет | Small (<1d) | Low |
| **URL Preview** | Порт из `url_preview.cpp` | `pkg/hicli/url_preview.go` | Нет (использует Matrix API `GET /_matrix/media/v3/preview_url`) | Small (<1d) | Low |
| **Профиль пользователя (TUI)** | Нативный TUI | `tui/user-profile.go` | Нет | Small (<1d) | Low |
| **Список участников (расширенный)** | Доработка `member-list.go` | `tui/member-list.go` | `github.com/lithammer/fuzzysearch` (уже есть) | Small (<1d) | Low |

### 2.6 Звонки

| Функция | Подход | Целевые пакеты | Go-зависимости | Трудоёмкость | Риск |
|---|---|---|---|---|---|
| **WebRTC звонки** | Порт из `call_manager.cpp`, `webrtc/` | `pkg/hicli/call.go`, `pkg/hicli/webrtc/` | `github.com/pion/webrtc/v4` | X-Large (1-3w) | High |
| **Конференции** | Поверх базовых звонков | `pkg/hicli/conference.go` | `github.com/pion/webrtc` | Large (3-7d) | High |

### 2.7 Платформенная интеграция

| Функция | Подход | Целевые пакеты | Go-зависимости | Трудоёмкость | Риск |
|---|---|---|---|---|---|
| **Desktop-уведомления (CLI)** | Нативная реализация через D-Bus (Linux), terminal bell | `pkg/gomuks/desktop_notify.go` | `github.com/godbus/dbus/v5` (Linux) | Medium (1-3d) | Med |
| **Звуковые уведомления** | Воспроизведение через терминал (bell) или внешнюю команду | `pkg/gomuks/sound.go` | Нет (`os/exec` для `paplay`/`aplay`) | Small (<1d) | Low |
| **Deeplinks (matrix.to)** | Нативная обработка CLI-аргументов | `cmd/gomuks/`, `pkg/gomuks/deeplink.go` | Нет | Small (<1d) | Low |
| **File associations** | Десктоп-файл + IPC для открытия файлов | `desktop/`, `pkg/gomuks/ipc.go` | Нет | Small (<1d) | Low |

---

## Раздел 3: Фазы реализации

### P0: Foundation (Фундамент) — 3-5 дней

**Уже выполнено:** Прокси-транспорт, enhanced login, connection type selector.

| Задача | Трудоёмкость | Зависимости |
|---|---|---|
| TLS Certificate Pinning | Medium (1-3d) | `hicli/proxy.go` |
| Кастомный DNS | Small (<1d) | `hicli/proxy.go` |
| Network monitor | Small (<1d) | `hicli/sync.go` |
| **Итого P0:** | **3-5d** | |

### P1: Enhanced Auth (Расширенная аутентификация) — 6-10 дней

| Задача | Трудоёмкость | Зависимости |
|---|---|---|
| OIDC SSO полный цикл (Custom Tab эмуляция через `xdg-open`) | Medium (1-3d) | `login_enhanced.go` |
| Refresh Token Management | Medium (1-3d) | OIDC SSO |
| SSO Providers UI (список IdP) | Small (<1d) | `view-login.go` |
| QR-код логин | Large (3-7d) | Нет |
| Регистрация (Registration) | Medium (1-3d) | Нет |
| Сброс пароля | Small (<1d) | Нет |
| Terms of Service | Small (<1d) | Регистрация |
| CAPTCHA | Small (<1d) | Регистрация |
| **Итого P1:** | **6-10d** | |

### P2: Session & Sync (Сессия и синхронизация) — 6-14 дней

| Задача | Трудоёмкость | Зависимости |
|---|---|---|
| Multi-account (Session Manager) | X-Large (1-3w) | P1 (нужна поддержка нескольких логинов) |
| Device Management UI | Medium (1-3d) | P2 (multi-account) |
| **Итого P2:** | **6-14d** (зависит от Multi-account, который можно отложить) | |

### P3: E2EE Hardening (Усиление шифрования) — 8-17 дней

| Задача | Трудоёмкость | Зависимости |
|---|---|---|
| UI кросс-подписи (ввод recovery key, просмотр статуса) | Large (3-7d) | `hicli/verify.go` |
| UI Key Backup (просмотр статуса, восстановление) | Medium (1-3d) | `hicli/backupupload.go` |
| SAS Emoji верификация (TUI) | Large (3-7d) | mautrix-go `crypto` |
| QR верификация (ASCII QR в терминале) | Medium (1-3d) | SAS |
| **Итого P3:** | **8-17d** | |

### P4: TUI Enhancement (Улучшение интерфейса) — 15-28 дней

| Задача | Трудоёмкость | Зависимости |
|---|---|---|
| Медиа-просмотрщик (ASCII/Kitty) | Large (3-7d) | `tui/messages/filemessage.go` |
| UI редактирования сообщений | Medium (1-3d) | `tui/room-view.go` |
| UI реакций | Medium (1-3d) | `tui/room-view.go` |
| Reply UI | Small (<1d) | `tui/room-view.go` |
| Mentions-подсветка | Small (<1d) | `tui/messages/htmlmessage.go` |
| Autocomplete (упоминания, emoji, команды) | Medium (1-3d) | `tui/widget/autocomplete.go` |
| Full-text search (FTS на SQLite) | Large (3-7d) | `hicli/database/` |
| Профиль пользователя (TUI) | Small (<1d) | `hicli/profile.go` |
| Список участников (расширенный) | Small (<1d) | `tui/member-list.go` |
| URL Preview (предпросмотр ссылок) | Small (<1d) | `hicli/sync.go` |
| Черновики (Drafts) | Small (<1d) | `tui/room-view.go` |
| **Итого P4:** | **15-28d** | |

### P5: Advanced Features (Продвинутые функции) — 14-28 дней

| Задача | Трудоёмкость | Зависимости |
|---|---|---|
| Threads (Треды) | X-Large (1-3w) | P4 (таймлайн) |
| Spaces UI (Навигация по пространствам) | Large (3-7d) | `sync.go` (SpaceEdge уже есть) |
| Polls (Голосования) | Medium (1-3d) | P4 (таймлайн) |
| Location sharing | Medium (1-3d) | P4 (медиа) |
| **Итого P5:** | **14-28d** | |

### P6: Platform Integration (Платформенная интеграция) — 5-9 дней

| Задача | Трудоёмкость | Зависимости |
|---|---|---|
| Desktop-уведомления (D-Bus, terminal bell) | Medium (1-3d) | `gomuks/push.go` |
| Звуковые уведомления | Small (<1d) | Desktop-уведомления |
| Deeplinks (matrix.to) | Small (<1d) | Нет |
| File associations (десктоп-файл, IPC) | Small (<1d) | Deeplinks |
| Режим headless/daemon для уведомлений | Medium (1-3d) | Desktop-уведомления |
| **Итого P6:** | **5-9d** | |

### Отложено / Низкий приоритет

| Функция | Причина |
|---|---|
| WebRTC звонки (+конференции) | Чрезвычайно высокие трудозатраты для CLI-клиента, малая практическая ценность в терминале |
| Импорт/Экспорт данных | Нишевая функция |
| Интеграция виджетов | Нет смысла в CLI без браузерного движка |
| PIN/блокировка экрана | Не применимо к CLI (есть блокировка ОС) |
| История аватаров | Низкий приоритет |
| Шеринг (поделиться в Matrix из других приложений) | Требует платформенной IPC-интеграции, сложно для CLI |

---

## Раздел 4: Оценка рисков

### 4.1 Технические риски

| Риск | Уровень | Описание | Митигация |
|---|---|---|---|
| **Сложность Multi-account** | High | `HiClient` — синглтон, глубоко завязан на одно подключение. Потребуется рефакторинг всей архитектуры. | Начать с изоляции состояния в `Session` структуру, обёртку `SessionManager`. Может потребоваться 2-3 недели. |
| **Threads** | High | Треды завязаны на `m.thread` relation, требуют отдельных таймлайнов, индикаторов непрочитанных, спецификации MSC3440. | Использовать `mautrix-go` (уже частично поддерживает). Поэтапная реализация: чтение → отправка → полный UI. |
| **WebRTC звонки** | High | `pion/webrtc` — зрелая библиотека, но интеграция аудио в терминале сложна (захват/воспроизведение аудио, эхоподавление). | Отложить до лучших времён. Рассмотреть интеграцию с Element Call через `tui/widget/element-call.go`. |
| **Full-text search** | Medium | SQLite FTS5 требует поддержки Unicode tokenizer (ICU). При компиляции с CGo проблем нет, но WASM-сборка может быть проблематична. | Использовать CGo-сборку для десктопа, для WASM — простой LIKE-поиск или внешний FTS. |
| **Media viewer (ASCII)** | Medium | Качество ASCII-арта сильно зависит от терминала, размера шрифта, цветовой палитры. | Поддержка нескольких бэкендов: ASCII, Kitty terminal protocol, iTerm2 inline images. |
| **OIDC/OAuth2** | Low | Стандартный протокол, `go-oidc` — зрелая библиотека. Проблема — открытие браузера из CLI для аутентификации. | Использовать локальный HTTP-сервер (уже есть `gomuks/sso.go`) + `xdg-open`/`open`. |

### 4.2 Интеграционные риски

| Риск | Уровень | Описание | Митигация |
|---|---|---|---|
| **mautrix-go версионирование** | Low | Проект активно развивается Tulir Asokan, может менять API. | Фиксировать версию в `go.mod`, следить за changelog. |
| **Совместимость с серверами Matrix** | Low | Спецификация Matrix стабильна, но некоторые серверы (Synapse, Dendrite, Conduit) имеют особенности. | Тестировать на Synapse (основной) + Dendrite. |
| **WASM-сборка** | Low | Некоторые Go-библиотеки несовместимы с WASM (CGo, системные вызовы). | Разделять код через build tags (`//go:build !js`). |

### 4.3 Ресурсные риски

| Риск | Уровень | Описание | Митигация |
|---|---|---|---|
| **Ограниченность ресурсов разработки** | High | Проект gomuks — по сути one-maintainer (Tulir Asokan). Портируемые функции требуют значительного времени. | Приоритезировать Quick Wins (Раздел 6). Фокусироваться на функциях, максимально полезных для CLI-клиента. |
| **Документация Android-кода** | Medium | C++ код Android может быть слабо документирован. | Читать Kotlin API layer и тесты для понимания логики. |

---

## Раздел 5: Сводка трудозатрат

### 5.1 По фазам (человеко-дни)

| Фаза | Best Case | Realistic | Worst Case |
|---|---|---|---|
| P0: Foundation | 3 | 4 | 5 |
| P1: Enhanced Auth | 6 | 8 | 10 |
| P2: Session & Sync | 6 | 10 | 14 |
| P3: E2EE Hardening | 8 | 12 | 17 |
| P4: TUI Enhancement | 15 | 21 | 28 |
| P5: Advanced Features | 14 | 20 | 28 |
| P6: Platform Integration | 5 | 7 | 9 |
| **ИТОГО** | **57** | **82** | **111** |

### 5.2 Критический путь

```
P0 (Foundation) → P1 (Auth) → P3 (E2EE) → P4 (TUI) → P5 (Advanced) → P6 (Platform)
                    ↘ P2 (Session) ↗
```

- **P0 → P1** обязательная зависимость (нужен транспорт для OIDC)
- **P2** можно делать параллельно с P1/P3 (разная кодовая база)
- **P3** желателен до P4 (верификация влияет на UX)
- **P4** обязателен до P5 (Threads/Spaces требуют улучшенного таймлайна)
- **P6** можно делать параллельно с P4/P5

### 5.3 Оптимальное расписание (Realistic, 1 разработчик)

- **Неделя 1-2:** P0 (Foundation) + начало P1 (Auth)
- **Неделя 3-4:** P1 (Auth) завершение + P3 (E2EE) начало
- **Неделя 5-7:** P3 (E2EE) завершение + P4 (TUI) начало
- **Неделя 8-10:** P4 (TUI) + P2 (Session)
- **Неделя 11-13:** P4 (TUI) завершение + P5 (Advanced)
- **Неделя 14-15:** P6 (Platform) + полировка

**Итого: 12-16 недель (3-4 месяца) при реалистичной оценке.**

---

## Раздел 6: Quick Wins (Быстрые победы)

Нижеперечисленные функции дают максимальный эффект при минимальных затратах. Рекомендуется реализовать в первую очередь.

| # | Функция | Трудоёмкость | Влияние |
|---|---|---|---|
| 1 | **SSO Providers UI** — список IdP в окне логина | Small (<1d) | Критично для пользователей SSO-серверов |
| 2 | **Reply UI** — кнопка/клавиша «Ответить» в таймлайне | Small (<1d) | Значительно улучшает UX общения |
| 3 | **Mentions-подсветка** — подсветка @username в сообщениях | Small (<1d) | Привлекает внимание к важным сообщениям |
| 4 | **Drafts** — сохранение неотправленных сообщений | Small (<1d) | Защита от потери текста при случайном выходе |
| 5 | **Desktop-уведомления (D-Bus)** | Medium (1-3d) | Ключевая функция для мессенджера |
| 6 | **Звуковые уведомления (terminal bell)** | Small (<1d) | Простое и эффективное дополнение к уведомлениям |
| 7 | **Профиль пользователя (TUI)** | Small (<1d) | Базовая функция любого мессенджера |
| 8 | **Расширенный список участников** (поиск, фильтр) | Small (<1d) | Значительно улучшает навигацию в больших комнатах |
| 9 | **URL Preview** | Small (<1d) | Делает ссылки информативными |
| 10 | **Deeplinks (matrix.to)** | Small (<1d) | Позволяет открывать ссылки извне |
| 11 | **OIDC SSO** (завершение) | Medium (1-3d) | Разблокирует вход для пользователей matrix.org и других OIDC-серверов |
| 12 | **Refresh Token** | Medium (1-3d) | Устраняет необходимость перелогина |
| 13 | **Full-text search (FTS5)** | Large (3-7d) | Критично для поиска в истории |
| 14 | **Key Backup UI** | Medium (1-3d) | Критично для безопасности — пользователи должны видеть статус бэкапа |
| 15 | **Autocomplete** | Medium (1-3d) | Значительно ускоряет набор сообщений |
| 16 | **Network monitor** | Small (<1d) | Информирует о проблемах подключения |
| 17 | **Редактирование сообщений (UI)** | Medium (1-3d) | Базовая функция Matrix |
| 18 | **Реакции (UI)** | Medium (1-3d) | Современная функция, ожидаемая пользователями |

**Быстрый старт: первые 16 Quick Wins = ~13-26 дней, покрывают 80% пользовательских потребностей.**

---

## Приложение A: Ключевые файлы gomuks для портирования

| Файл | Назначение | Что нужно добавить |
|---|---|---|
| `pkg/hicli/hicli.go` | Основной клиент | Multi-account, session manager |
| `pkg/hicli/sync.go` | Синхронизация | Threads, polls, location |
| `pkg/hicli/verify.go` | Верификация, SSSS | UI-методы для TUI |
| `pkg/hicli/backupupload.go` | Резервное копирование ключей | UI-методы, прогресс-коллбэки |
| `pkg/hicli/login_enhanced.go` | Расширенный логин | OIDC, QR login |
| `pkg/hicli/proxy.go` | Прокси-транспорт | TLS pinning, custom DNS |
| `tui/view-login.go` | Экран логина | SSO providers, OIDC flow, QR, регистрация |
| `tui/room-view.go` | Комната (таймлайн+композер) | Reply UI, edit UI, reactions |
| `tui/room-list.go` | Список комнат | Индикаторы тредов, polls, непрочитанных |
| `tui/messages/htmlmessage.go` | Рендеринг HTML | Mentions-подсветка, threads, polls |
| `tui/messages/filemessage.go` | Отображение файлов | Медиа-просмотрщик |
| `tui/config/config.go` | Конфигурация | Multi-account поля, TLS pinning |
| `pkg/gomuks/push.go` | Push-уведомления | Desktop-уведомления, звук |

## Приложение B: Ключевые файлы Android для изучения

| Файл | Функция | Что портировать |
|---|---|---|
| `oidc_manager.cpp` / `oidc_auth.cpp` | OIDC-менеджер | Логика OIDC-флоу, refresh token |
| `cross_signing.cpp` / `cross_signing_manager.cpp` | Кросс-подпись | UI-состояния, flow |
| `key_backup.cpp` / `key_backup_manager.cpp` | Резервное копирование ключей | UI-состояния, прогресс |
| `sas_verification.cpp` | SAS верификация | Процесс сравнения emoji/decimal |
| `thread_manager.cpp` / `thread_aggregator.cpp` | Треды | Модель данных, агрегация |
| `poll_utils.cpp` | Голосования | Парсинг, подсчёт голосов |
| `search_index.cpp` | Полнотекстовый поиск | Индексация FTS |
| `session_manager.cpp` | Управление сессиями | Multi-account архитектура |
| `call_manager.cpp` / `webrtc/` | Звонки | WebRTC-интеграция |
| `notif_analyzer.cpp` / `notif_formatter.cpp` | Уведомления | Форматирование, приоритезация |
| `event_relations.cpp` | Редактирование/реакции | Модель редактирования |
| `space_utils.cpp` | Пространства | Навигация по Space |
| `location/` | Локация | Отправка/отображение координат |
| `proxy.cpp` | Прокси | TLS pinning, DNS |
