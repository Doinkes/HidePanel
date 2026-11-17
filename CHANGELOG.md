# Changelog

Все заметные изменения проекта. Формат — [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версии — [SemVer](https://semver.org/lang/ru/).

## [1.0] — 2026-11-17

Первая публичная версия. / First public release.

### Возможности

* Мгновенное скрытие панели задач: пустой регион окна (`SetWindowRgn`) вместо слайд-анимации
  оболочки; появление за 2–3 мс по касанию нижней кромки экрана, скрытие — сразу после ухода
  курсора.
* Рабочая область равна полному монитору (`SPI_SETWORKAREA` без broadcast): развёрнутые окна
  занимают весь экран, панель рисуется поверх них.
* Штатное авто-скрытие Windows отключается на время работы и восстанавливается при выходе.
* Значок в области уведомлений с меню: настройки, автозапуск, выход. При первом запуске
  автоматически выносится из «скрытых».
* Окно настроек с пояснениями: толщина горячей полосы, период опроса курсора, режим полного
  экрана, fullscreen-guard, восстановление авто-скрытия. Интерфейс на русском и английском.
* Панель не появляется поверх полноэкранных игр и видео (отключаемо).
* Поддержка нескольких мониторов, восстановление после перезапуска `explorer.exe`.
* Защита от падения: файл состояния и ключ `--restore`.
* Без прав администратора, один exe (~230 КБ), без внешних зависимостей.

### Features (EN)

* Instant taskbar hide via an empty window region instead of the shell's slide animation;
  the panel appears in ~2–3 ms at the bottom edge and hides the moment the cursor leaves.
* Work area = full monitor (`SPI_SETWORKAREA`, no broadcast): maximized windows cover the whole
  screen and the taskbar is drawn on top.
* Native Windows auto-hide is disabled while running and restored on exit.
* Tray icon with a menu: settings, start with Windows, exit.
* Settings window with explanations; UI in Russian and English.
* Never shows over fullscreen games and video (optional).
* Multi-monitor aware, survives `explorer.exe` restarts.
* No admin rights, single ~230 KB exe, no external dependencies.
