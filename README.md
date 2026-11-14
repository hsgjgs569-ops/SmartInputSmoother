# SmartInputSmoother (Geode mod, GD 2.2.0 / Geode 2.2.144)

«Умное сглаживание ввода» (не автокликер): игрок кликает физически (6–12 CPS), мод дробит удержание/нажатия на микро-клики (до ~30 CPS) для более ровной волны. При отпускании физической кнопки эмуляция прекращается мгновенно (контроль сохраняется).

## Структура
- `mod.json` — метаданные мода (Geode 2.x, GD 2.2.0)
- `CMakeLists.txt` — сборка через `setup_geode_mod()`
- `src/main.cpp` — хук `GJBaseGameLayer::handleButton`, пульсация через `CCScheduler`
- `.github/workflows/build.yml` — GitHub Actions сборка Android (arm64-v8a), артефакт `.geode`

## Настройки
- **Enable** — вкл/выкл
- **Target CPS** (10–60) — частота микро-кликов
- **Smoothing Mode** — `Linear` (равномерно) / `Burst` (короткие серии)

## Примечания по логике
- Хук: `GJBaseGameLayer::handleButton(bool down, int button, bool isPlayer1)` с ранним приоритетом.
- На `down=true` от игрока: запускается кадровый планировщик, генерирующий микро-импульсы (down/up) с интервалом `1/TargetCPS` пока кнопка удерживается.
- На `down=false`: цикл немедленно останавливается и отправляется финальный `down=false`.
- Рекурсия предотвращается флагом подавления для эмулированных событий.

## Сборка локально (если есть окружение)
```bash
export GEODE_SDK=/path/to/geode
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```
