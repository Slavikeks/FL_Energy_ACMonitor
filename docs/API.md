# Web API и интеграции

Веб-интерфейс открывается по HTTP на адресе устройства. При первом запуске
используются `admin/admin`; пароль после входа можно сменить в интерфейсе.
Пароль хранится в NVS как PBKDF2-SHA256 salted hash. Настройки сети, NTP и
timezone сохраняются в той же версии конфигурации. Журнал событий и HTTP-сессии
хранятся в RAM и очищаются после перезапуска.

## API

| Метод | Путь | Доступ |
|---|---|---|
| `POST` | `/api/v1/auth/login` | Публичный; form fields `username`, `password`; выдаёт HttpOnly cookie |
| `GET` | `/api/v1/auth/me` | Сессия; возвращает CSRF token текущей сессии |
| `POST` | `/api/v1/auth/logout` | Сессия + `X-CSRF-Token` |
| `POST` | `/api/v1/auth/password` | Сессия + CSRF; form fields `username`, `password` (минимум 6 символов) |
| `GET` | `/json` | Сессия; публичная typed runtime модель, schema version 1 |
| `GET` | `/api/v1/management/secrets` | Session; returns configured SNMP/MQTT secrets with `Cache-Control: no-store` |
| `GET` | `/api/v1/log` | Сессия; до 50 последних событий из RAM ring buffer |
| `POST` | `/api/v1/settings` | Сессия + CSRF; network/time, automation, SNMP и MQTT form fields |
| `POST` | `/api/v1/ota` | Сессия + CSRF; multipart field `firmware`, filename `firmware.bin` |
| `GET` | `/mib` | Сессия; загружает MIB как файл |

Cookie `fl_session` имеет атрибуты `HttpOnly`, `SameSite=Strict`, ограниченный
срок действия и путь `/`. API изменений проверяет случайный CSRF token через
`X-CSRF-Token`. Устройство на этом этапе обслуживает обычный HTTP в локальной
сети; транспортный TLS здесь не реализован.

Упрощённый пример ответа `/json`:

```json
{
  "schema_version": 1,
  "device": {
    "name": "FL_Energy_ACMonitor",
    "version": "0.7.4",
    "uptime_s": 42,
    "time_valid": false,
    "time_local": null
  },
  "network": {"link": true, "dhcp": true, "ip": "192.0.2.50"},
  "time": {"server": "pool.ntp.org", "timezone": "UTC-5"},
  "bus": {"baud": 4800, "poll_ok": 0, "timeouts": 0},
  "conditioners": [], "groups": [], "alarms": []
}
```

Credentials, password hashes, cookie/CSRF values and raw Config are excluded from
`/json` and event log messages. The dedicated management secrets endpoint requires a valid session. Current runtime capacity is 16 registered units
and 16 groups; bus address range remains 0–63.

Each conditioner in `/json` also reports `automation_health` and
`consecutive_poll_failures`. Reserve starts wait while any group member's power
state is uncertain; a member is treated as lost after the configured consecutive
C0 poll failure threshold (three by default). A known powered unit with a
reported operational fault is commanded OFF and must confirm shutdown before
its reserve is started.

## Этап 2: API XYE
Conditioner API objects expose fresh `fan_actual` measurements as 0=stopped, 1=low, 2=medium, 3=high; `fan_auto` separately reports automatic fan mode. In automatic mode, physical speed may be unavailable, so `fan_actual` can be `null` while `fan_auto` is `true`. `compressor_running` reports the protocol compressor flag. `error_flags` and `protection_flags` are raw 16-bit masks. All four temperatures (`t1_deci_c`, `t2_deci_c`, `t2b_deci_c`, `t3_deci_c`) use tenths of a degree Celsius and are now included in `/json`, MQTT state and the SNMP AC table. `/json` returns `null` for an unavailable or stale sensor; MQTT uses `null`; SNMP omits the unsupported or unconfirmed sensor OID. The Status card hides a temperature tile when its value is unavailable.

Fresh experimental values are visible in `/json` and the Status card for inspection. SNMP and MQTT publish these measurements only after the conditioner model is confirmed, following the existing telemetry safety gate. The SNMP AC table adds fan-auto mode at column 11, compressor state at column 12, protection flags at column 13, T2B at column 14 and T3 at column 15. The web card names the known protection mask `0x0002` as defrost and shows any remaining bits as an unknown hexadecimal mask.


- `POST /api/v1/ac/scan` — авторизованный запрос с CSRF; запускает read-only C0 scan адресов 0–63.
- `POST /api/v1/ac/register` — принимает поля `address` (0–63) и `name`, сохраняет реестр в NVS.
- `/json` показывает прогресс сканирования, найденные адреса, метрики ошибок и статусы зарегистрированных кондиционеров. Зависимые от модели значения помечаются как `experimental`.

## Этап 4: группы и автоматика

GET /api/v1/state возвращает компактную runtime-модель для интерфейса; GET /json открывает ту же модель в форматированном виде.

POST /api/v1/ac/remove принимает address и удаляет кондиционер из реестра. Удаление отклоняется, пока адрес входит в группу или занят командой.

POST /api/v1/groups/upsert сохраняет группу. Form fields: id (0 создаёт новый ID), name, members (адреса через запятую), mode (manual/auto), required_active, maximum_active, ac_mode, setpoint_c, fan, high_temp_c, high_delay_s, recovery_temp_c, recovery_delay_s, handover_s, verify_s, min_on_s, min_off_s, interval_days, rotation_hour, rotation_minute, rotation_enabled.

POST /api/v1/groups/delete принимает id. Удаление блокируется во время handover. POST /api/v1/settings дополнительно принимает automation_enabled (0/1). Включение запрещено, если участники AUTO-групп не подтверждены владельцем.

Automatic control is off by default. MANUAL groups are not automated. Web commands are rejected for AUTO-group members unless their per-device Manual control checkbox is enabled. Powered manual-control members still consume an M slot; while N is short, automation may start another member if M has room. The controller reapplies detectable group mode, setpoint and fan differences after C0 polling. A reserve is verified by C0 before the old unit is stopped; temperature emergency logic requires fresh confirmed T1.

AUTO groups may enable `ventilation_reserve` when the group's operating mode is Cool, Heat, or Dry. Inactive, eligible members are kept powered in Fan mode at the configured group fan setting; they are excluded from the `active` list and from N/M counts, and are promoted to the operating mode when demand requires them. `/json` exposes currently confirmed fan-only reserves in `groups[].ventilation_reserves`. Group `color` is a `#RRGGBB` value used to tint the web cards. The ventilation option is incompatible with MANUAL groups and a group operating mode of Fan.
В компактном /api/v1/state и /json для зарегистрированного кондиционера также возвращается power (true/false из последнего ответа C0 либо null). События /api/v1/log включают локальное время YYYY-MM-DD HH:MM:SS с точностью до секунд либо null до синхронизации времени. Карточка статуса считает работающими устройства с актуальной связью и power: true; выключенный резерв остаётся в общем количестве, но не входит в число работающих.

## Этап 5: SNMP и MQTT

The authenticated settings form loads saved community/credentials from `GET /api/v1/management/secrets`; blank editable values clear them. Raw secrets are excluded from `/json`.

| Поле | Назначение |
|---|---|
| `snmp_enabled`, `snmp_community` | Read-only SNMPv2c agent и RO community |
| `snmp_system_name`, `snmp_location`, `snmp_contact` | SNMP system group |
| `trap_receiver`, `trap_port` | Единственный IPv4 trap destination и UDP port |
| `mqtt_enabled`, `mqtt_broker`, `mqtt_port` | Исходящее MQTT-подключение |
| `mqtt_username`, `mqtt_password` | Необязательная MQTT-аутентификация и очистка credentials |
| `mqtt_topic_prefix`, `mqtt_publish_period_s` | Topic prefix и период retained state публикации (5–3600 сек) |

В `/json` возвращаются только настройки и признаки наличия секретов; сами SNMP
community, MQTT username и password не сериализуются. Обе функции по умолчанию
выключены. SNMP агент принимает GET/GETNEXT/GETBULK и не меняет настройки.

### SNMP: куда смотреть

Предприятие FLINK LLC зарегистрировано под номером **67213**. Все собственные OID начинаются с .1.3.6.1.4.1.67213. SNMPv2c read-only агент слушает UDP/161 и отвечает на GET, GETNEXT и GETBULK. Он не принимает SET и не меняет настройки.

| Имя | OID | Что возвращает |
|---|---|---|
| sysDescr.0 | .1.3.6.1.2.1.1.1.0 | FL Energy AC Monitor и версия прошивки |
| sysObjectID.0 | .1.3.6.1.2.1.1.2.0 | Тип устройства .1.3.6.1.4.1.67213.1 |
| sysUpTime.0 | .1.3.6.1.2.1.1.3.0 | Время работы SNMP-агента в сотых долях секунды |
| sysContact.0 | .1.3.6.1.2.1.1.4.0 | Контакт из Settings |
| sysName.0 | .1.3.6.1.2.1.1.5.0 | Имя устройства из Settings |
| sysLocation.0 | .1.3.6.1.2.1.1.6.0 | Расположение из Settings |

Кондиционеры находятся в таблице .1.3.6.1.4.1.67213.3.1.<столбец>.<XYE-адрес>. Столбцы 1–10: адрес, имя, доступность (0 неизвестно, 1 online, 2 offline), фактическое питание (1 включён, 2 выключен), режим (0 охлаждение, 1 обогрев, 2 вентиляция, 3 осушение), установка ×10 °C, скорость вентилятора (0 авто, 1 низкая, 2 средняя, 3 высокая), T1 ×10 °C, T2 ×10 °C и флаги ошибки. Например, имя адреса 1: .1.3.6.1.4.1.67213.3.1.2.1.

Группы находятся в .1.3.6.1.4.1.67213.4.1.<столбец>.<ID группы>. Столбцы 1–8: ID, имя, режим (0 ручной, 1 автоматический), состояние (0 ручной, 1 нормальная работа, 2 ротация, 3 снижена готовность, 4 аварийный режим), обязательное число N, максимум M, число работающих устройств и наибольшая свежая T1 ×10 °C. Например, состояние группы 1: .1.3.6.1.4.1.67213.4.1.4.1.

Показания AC и температура группы появляются только после свежего подтверждённого ответа; неизвестные значения не маскируются нулём — соответствующий объект отсутствует в WALK. Дерево намеренно не содержит статистику RS-485 и время последнего ответа. Полное описание типов и объектов есть в [MIB-файле](../web/assets/FL-ENERGY-ACMONITOR-MIB.mib), его также можно скачать из Help или по /mib.

### SNMP traps

Устройство отправляет SNMPv2c traps на единственный заданный IPv4-адрес и UDP-порт (обычно 162). В уведомлениях используются стандартные sysUpTime.0 и snmpTrapOID.0, затем восемь переменных .1.3.6.1.4.1.67213.2.10.<поле>.0.

| snmpTrapOID.0 | Событие |
|---|---|
| .1.3.6.1.4.1.67213.2.0.1 | Кондиционер стал недоступен или связь восстановилась |
| .1.3.6.1.4.1.67213.2.0.2 | Появилась подтверждённая ошибка кондиционера или она сброшена |
| .1.3.6.1.4.1.67213.2.0.3 | Группа не обеспечивает заданное число работающих устройств или восстановилась |
| .1.3.6.1.4.1.67213.2.0.4 | Перегрев группы или восстановление температуры |

Поля события: .2.10.1.0 — проблема (1) или восстановление (2); .2.10.2.0 — важность (0 информация, 1 предупреждение, 2 критическое); .2.10.3.0 — тип события (1–4 в порядке таблицы выше); .2.10.4.0 — имя AC; .2.10.5.0 — имя группы; .2.10.6.0 — короткий текст ASCII; .2.10.7.0 — UTC время RFC3339 или пусто до синхронизации SNTP; .2.10.8.0 — код ошибки или значение температуры, когда применимо. Имя не относящегося к событию объекта передаётся пустым. Русские имена транслитерируются в ASCII для совместимости с приёмниками.

AlarmManager отправляет trap после 10 секунд непрерывного состояния. В течение 60 секунд после запуска устройство только наблюдает: начальные проблемы видны в состоянии, но не создают серию уведомлений.
### AUTO-group manual control

`POST /api/v1/ac/manual-control` accepts `address` and `manual_control` (`0`/`1`). The Status card exposes this checkbox only for a conditioner in an AUTO group. When enabled, that member is excluded from automatic selection and setting correction, and Web commands are allowed; clearing the checkbox returns it to group control. The existing NVS registry flag is reused, so no registry migration is needed. A confirmed powered manual-control unit still consumes one M slot. It does not satisfy N; if N is short and M has room, automation can start another member.

While global automation is enabled, powered and healthy AUTO members are checked against group mode and setpoint after C0 polls. Detectable differences queue a C3 correction with the group's current mode, fan and setpoint. The protocol reports fan-auto state and actual fan speed, not a separate selected fixed-speed value; fixed-speed differences are checked only while the fan should be running continuously. The command cooldown prevents repeated writes while C0 verification is pending.

### MQTT topics

Device ID — 12 hex-символов из MAC WT32-ETH01. При стандартном prefix `fl_energy`
создаются:

- `fl_energy/<device-id>/state` — retained JSON с версией, uptime, временем,
  состояниями AC и групп; устаревшие/неподтверждённые значения передаются как
  `null`.
- `fl_energy/<device-id>/event` — JSON одного из четырёх alarm transitions,
  включая имя AC/группы и человекочитаемый текст.
- `fl_energy/<device-id>/status` — retained `online`; Last Will retained `offline`.

Устройство не подписывается на topics и не принимает MQTT-команды. MQTT
работает по обычному TCP без TLS; SNMPv2c community также не шифруется,
используйте только доверенную изолированную сеть. Reconnect выполняется раз в
10 секунд; TCP connect и ожидание MQTT CONNACK ограничены одной секундой каждый.
Подключение синхронное, поэтому проверьте XYE polling при недоступном broker;
для локального broker укажите IP, чтобы избежать дополнительного DNS ожидания.
