# План заимствований из upstream Vita3K

Сравнение: наша база (Vita3K Plus 1.4) и снимок upstream master (v0.2.1-dev, 14.11.2026). Истории git у снимка нет, поэтому «чья сторона новее» определялось по содержимому. Главный вывод: **Plus по большинству модулей впереди upstream** (память, NGS, аудио, кодеки, Vulkan-поверхности, vargs, Io). Крупные переписанные слои upstream (kernel/sync_primitives, Ref/waitable, call_result) брать не нужно. Ниже только точечные находки. Всё не проверено сборкой и на устройстве; помечено, где конфликты с нашими правками.

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

## Крупные идеи (отдельные этапы, нужно решение)

- **Патчи игр из XML (patchlist.xml от vita3k-patches):** `patch_xml.cpp` + расширение `patch.h` (~250 строк) и список/включение патчей. На Android нужен свой экран на Compose. Польза: исправления совместимости для отдельных игр. Объём средний, UI обязателен.
- **Импорт/экспорт зашифрованных сохранений (`savedata.cpp`, ~300 строк):** удобно для переноса сохранений между телефоном и Vita/ПК, нужен экран выбора. Объём средний.
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
Графику (Vulkan) просмотрел по диффам выборочно, агенты глубокой проверки прервались из-за лимита. Если хотите, пройду её отдельно.
