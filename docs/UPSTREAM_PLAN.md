# План заимствований из upstream Vita3K

Сравнение: наша база (Vita3K Plus 1.4) и снимок upstream master (v0.2.1-dev, 14.11.2026). Истории git у снимка нет, поэтому «чья сторона новее» определялось по содержимому. Главный вывод: **Plus по большинству модулей впереди upstream** (память, NGS, аудио, кодеки, Vulkan-поверхности, vargs, Io). Крупные переписанные слои upstream (kernel/sync_primitives, Ref/waitable, call_result) брать не нужно. Ниже только точечные находки. Всё не проверено сборкой и на устройстве; помечено, где конфликты с нашими правками.

## Статус
Сделано (в `main`, сборкой не проверено): пункты 1–10. Пункт 7: аллокатор перенесён целиком, 14 тестов upstream и случайная проверка на пересечения и пропуски проходят на хосте. Пункт 9: `sysinfo` подключён в лог запуска Android (`CPU: ...`). Пункт 10: upstream-версию SFO целиком не брали (она строже нашей и отвергает файлы, которые мы читаем), перенесена только терпимость к заниженной длине UTF8-строки.

## Очередь коммитов (от простого и безопасного)

| # | Что | Файлы | Размер | Риск | Польза |
|---|---|---|---|---|---|
| 1 | `sceClibSnprintf`: ошибка при `result < 0`, а не `!result` | `modules/SceLibKernel/SceLibKernel.cpp` | 1 строка | низкий | пустая строка не считается ошибкой |
| 2 | Права сегмента `perms = p_flags & 7` | `kernel/src/load_self.cpp` | 1 строка | низкий | корректные права страниц |
| 3 | Адрес сегмента SELF из `seg_infos[i].offset`, а не `header_len + p_offset` | `kernel/src/load_self.cpp` | 1–2 строки | низкий–средний | корректная загрузка настоящих SELF; проверить на PKGj и 2–3 играх |
| 4 | TPIDRURW в CP15 + сохранение при переключении fiber (заодно исправить `SceThreadmgr.cpp:420`, где подставляется RO-значение) | `cpu/src/dynarmic_cpu.cpp`, `cpu/include/cpu/common.h`, `modules/SceFiber/SceFiber.cpp` | 30–40 строк | низкий | игры на Mono/PSM и fiber-движки не теряют TLS-регистр |
| 5 | Корректный unload экспортов модулей (`export_nid_owners`) | `kernel/src/load_self.cpp`, `kernel/include/kernel/state.h` | ~50 строк | средний (hunks пересекаются с нашим LIBNID-логом) | выгрузка/перезагрузка модулей не стирает чужие экспорты |
| 6 | NetCtl `INFO_GET_ETHER_ADDR` через нашу версию `sceNetGetMacAddress` (с запасным MAC) | `modules/SceNetCtl/SceNetCtl.cpp` | ~12 строк | низкий | приложения, запрашивающие MAC. Версию upstream `sceNetGetMacAddress` **не брать**: на Android 11+ `SIOCGIFHWADDR` запрещён |
| 7 | BitmapAllocator: потокобезопасный `free_slot_count`, быстрый поиск | `mem/src/allocator.cpp`, `mem/include/mem/allocator.h` | ~150 строк | средний (прогнать `mem/tests/allocator_tests.cpp`) | профилактика гонки; реальных падений от неё мы не видели |
| 8 | cubeb: громкость выше 100% программным gain | `audio/src/impl/cubeb_audio.cpp`, `audio/include/audio/impl/cubeb_audio.h` | ~25 строк | низкий | громкость на телефоне; ложится в хвост callback и `set_volume` |
| 9 | `sysinfo` (модель CPU, память, процесс) для логов | `util/src/sysinfo.cpp` + заголовок | ~100 строк | низкий | богаче `device-info.txt` и `[TICK]`; на Android проверить `/proc` |
| 10 | SFO: терпимость к заниженному `data_len` у UTF8-строк | `packages/src/sfo.cpp` | ~15 строк | низкий–средний | редкие кривые `param.sfo`; сомнительно, что встречается, делать последним |

## Крупные идеи
Сделано (16.11.2026, в `main`, на устройстве не проверено):
- **Патчи игр из XML** — перенесены `patch_xml.cpp` и расширение `patch.h`, `interface.cpp` подмешивает XML-патчи по NID главного модуля; экран «Патчи игр» в меню (скачивание patchlist.xml, переключатели). Сборка CI зелёная.
- **Импорт/экспорт сохранений** — psvpfstools поднят до e9c5c7c (там `decrypt_savedata`/`encrypt_savedata`), `savedata.cpp` перенесён (boost fs, без списка слотов), экран «Сохранения» (выбор папки, экспорт в формат PS Vita). Нужны права на все файлы (уже есть для установки).
- **Vulkan:** обзор по диффам ещё раз: в большинстве файлов наша сторона новее. Взята единственная находка — U5U5U5U1 (загрузка текстур как A1R5G5B5 и верные swizzle). Не взято: нативный RGBA4/E5B9G9R9 как формат поверхности (нашу эмуляцию через RGBA16F не трогал, она осознанная).

Остаётся решением:

- **SMAA (3 прохода, таблицы `AreaTex.h`/`SearchTex.h`, `SMAA.hlsl`, ~500 строк фильтра):** у нас уже есть Bilinear/Bicubic/FXAA/FSR. SMAA даёт лучшее сглаживание, но тяжелее для GPU телефона. Низкий приоритет.
- **VCI (образы картриджей):** для Android не нужно.

## Не брать

- Переписанные `kernel/sync_primitives`, `thread.cpp`, `SceThreadmgr.cpp` (Ref/objects/waitable/intrusive_list) и `call_result.h`: огромный слой, у нас свой, проверенный.
- `mem.cpp` upstream: нет page table, внешних маппингов и pin в арену, которые есть у нас.
- `vargs.h` (upstream убрал double/align8), `sceClibMemcpy/Memmove/Memset`, `sceIoPread/Pwrite` (не атомарные): у нас лучше.
- NGS, audio-pacing, aac (SBR), mjpeg, atrac9, pkg/sfo в целом: у нас новее.
- Updater (`Vita3K/Vita3K continuous`): сломает наш апдейтер.
- Всё про Windows/Linux/macOS, Qt, OpenGL, CI других платформ.
- Остальное в Vulkan: по найденным диффам fix-правок нет, у нас больше своих (depth alias, stencil, surface cast).

## Порядок работы
Один коммит на пункт, после каждого пуш в `main`. Сначала 1–6 одной серией (малый риск), затем 8, 9, 7, 4–5 по результатам CI и логов с телефона. Крупные идеи беру только после вашего выбора.

## Что осталось непроверенным
Всё перенесённое не проверено на устройстве. Крупные Vulkan-файлы (surface_cache, context, pipeline_cache, spirv_recompiler; тысячи строк расхождений) построчно не сверялись: без истории git нельзя отличить новое upstream от нашего.
