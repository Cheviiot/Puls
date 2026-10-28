## Что изменено

Кратко опишите изменение и его назначение.

## Совместимость и приватность

- [ ] Сохранены публичные команды, flags, exit codes и JSON schema 1 либо описаны необходимые изменения.
- [ ] Help, README, CHANGELOG и golden checks обновлены при изменении поведения.
- [ ] Diff не содержит credentials, keystore, IP/ISP, production responses или случайных generated binaries.
- [ ] Описано влияние сетевого трафика и поведение live-тестов.

## Проверки C++/Qt

- [ ] Запущены targeted CTest для изменённого поведения.
- [ ] Полный CTest прошёл с `--repeat until-fail:10`.
- [ ] ASan/UBSan выполнены для изменений engine или сетевого кода.
- [ ] TSan выполнен для engine, если изменена конкурентность и среда его поддерживает.
- [ ] `cppcheck` прошёл.
- [ ] Python release packager tests прошли: `python3 -m unittest discover -s scripts/tests -v`.
- [ ] Android verifier/smoke contract tests прошли: `python3 -m unittest discover -s cpp/android/ci -p 'test_*.py' -v`.
- [ ] `actionlint .github/workflows/*.yml` прошёл при изменении workflow.
- [ ] `shellcheck scripts/install.sh` и PowerShell `-Help` прошли при изменении установщиков.
- [ ] Проверки CI для затронутых desktop targets и Windows installer завершились успешно.
- [ ] Android CI с проверкой и запуском подписанного APK прошёл, если изменение затрагивает Android.

## Go reference-проверки

- [ ] Если изменён код `cmd/` или `internal/`, выполнены соответствующие Go reference checks (`go test`, `go test -race`, `go vet`, `staticcheck`, `govulncheck`).
- [ ] Go reference-код не изменялся.

## Риски и откат

Укажите возможные риски, совместимость CLI/JSON и способ отката.
