# Release-APK с подписью из GitHub Secrets

Release-сборка отличается от обычной (`Reldebug`): включена минификация, package id без суффикса `.debug`, подпись вашим ключом. Ключ не хранится в репозитории, он лежит только в GitHub Secrets.

## 1. Создать ключ (один раз)
```
keytool -genkeypair -v -keystore release.keystore -alias vita3k -keyalg RSA -keysize 2048 -validity 36500
```
Запомните оба пароля (хранилища и ключа). **Сохраните `release.keystore` в надёжном месте.** Потеряете ключ, и новые версии не поставятся поверх старых. Никогда не коммитьте его.

## 2. Добавить секреты
Repo → Settings → Secrets and variables → Actions → New repository secret:

| Секрет | Значение |
|---|---|
| `SIGNING_KEYSTORE_BASE64` | вывод `base64 -w0 release.keystore` |
| `SIGNING_STORE_PASSWORD` | пароль хранилища |
| `SIGNING_KEY_ALIAS` | `vita3k` (alias из шага 1) |
| `SIGNING_KEY_PASSWORD` | пароль ключа |

## 3. Запустить сборку
- **Вручную:** Actions → «Android arm64 (fast)» → Run workflow → галочка `release`. APK появится в артефактах (`…-release`).
- **Тегом:** `git tag v1.0 && git push origin v1.0`. Сборка создаст GitHub Release с файлом `Vita3K-Redux-v1.0-arm64-v8a.apk`. Описание берётся из `docs/releases/<тег>.md` (если файла нет, генерируется автоматически); теги с `beta`, `rc` или `alpha` публикуются как pre-release.
- **Полный релиз без тега в репозитории (через ручной запуск):** Run workflow → `release` + `publish_tag` = `v1.2`. Релиз и тег создаст GitHub на собранном коммите (нужно, если тег нельзя запушить).
- **Проверка перед публикацией:** Actions → Run workflow → галочки `release` и `dev_release`: подписанная Release-сборка попадёт в pre-release `dev-<sha>` для проверки на телефоне, тег ставить не нужно. Release собирается с R8-минификацией, которую надо проверить на устройстве перед тегом.

Если секрет не задан, шаг подписи остановит сборку с понятной ошибкой.

## Предупреждения
- Другой package id и другой ключ: release ставится **рядом** с debug-сборкой, данные и настройки у неё отдельные.
- R8-минификация не проверялась на устройстве. Если приложение падает на старте, смотрите лог и добавьте правила в `proguard-rules.pro` (JNI-классы и методы `NativeLib`).

## Тестовая сборка без артефактов Actions (квота заполнена)
Actions → «Android arm64 (fast)» → Run workflow → включить `dev_release`. APK появится в Releases как pre-release `dev-<sha>` (файлы релизов не входят в квоту Actions storage). Подпись у такой сборки отладочная, для проверки на телефоне; публичный релиз по-прежнему делается тегом `v*`.
