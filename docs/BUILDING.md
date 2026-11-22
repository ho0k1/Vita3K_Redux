# Сборка из исходников (Android, arm64-v8a)

Проще всего собирать в GitHub Actions: форкните репозиторий, откройте Actions → «Android arm64 (fast)» → Run workflow. Для подписанного релиза нужны секреты, см. [RELEASE.md](RELEASE.md).

Локально (Linux/macOS, Windows через WSL):

1. Установите JDK 17, Android SDK и NDK (версия NDK указана в `android/app/build.gradle` и в `.github/workflows/android-arm64.yml`), CMake и vcpkg (`VCPKG_ROOT` должен указывать на него).
2. Клонируйте вместе с сабмодулями:
   ```
   git clone --recursive https://github.com/ho0k1/Vita3K_Redux
   cd Vita3K_Redux
   ```
3. Соберите (скрипт копирует ресурсы и вызывает Gradle):
   ```
   bash .ci/build-android.sh out
   ```
   APK окажется в `out/app.apk`. Без ключа подписи собирается `Reldebug` (отладочная подпись `android/app/fixed-debug.keystore`, для тестов), с ключом из `SIGNING_*` собирается `Release`.

Первая сборка идёт долго (vcpkg и нативный код), повторные быстрее благодаря ccache.
