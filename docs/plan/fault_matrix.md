# Матрица отказов P0: T-036

Имена ниже — GTest или CTest. Фактические прогоны, ограничения ОС и результаты
R указаны в отчёте T-036. Ссылка на общий механизм R означает представительную
проверку механизма на настоящем томе; отдельные команды проверяются K/S1–S3.

K — реальное завершение дочернего процесса; S1/S2 — отказ файлового sink до
начала/во время записи; S3 — SQLITE_FULL в настоящем движке SQLite; R — реальный
заполненный том. Подстановка S3 не заменяет R. Отсутствие файлового sink для
операции, которая пишет только SQLite, означает S1/S2 «не применимо»: её записи
проверяются в S3. Закрытие питания и внешние редакторы не входят в P0.

| Операция | K | S1 | S2 | S3 | R |
|---|---|---|---|---|---|
| library init | FaultProcess.init | Не применимо: нет оценки свободного места; создание БД — S3 | SmallVolume.InitConfigurationFull (настоящий отказ записи конфигурации) | FaultSqlite.InitFullNeverPublishesConfiguration | SmallVolume.InitConfigurationFull |
| create/change репозитория | CommitProtocol.KillEveryCheckpointThreeTimesAndRecover; AssetRepository.KillEveryRepositoryCheckpointThreeTimesWithOpenSnapshot | AssetRepository.ReadOnlyCapabilitiesSizeAndSpaceAreCheckedBeforeCommit | CommitProtocol.SpaceBeforeAndDuringWriteLeavesNoOperation | FaultSqlitePipeline.JournalFullBeforeAndAfterReplacement | SmallVolume.ChangePreflight; SmallVolume.ChangeDuringWrite |
| library add-ready | ImportReady.KillVerifiedAndReplacedForNewAndReplacementRecoverWithIndex; FaultProcess.ready; FaultProcess.ready_replace | FaultSpace.ReadyAndCopyPreflight | FaultSpace.ReadyAndCopyDuringWrite | FaultSqlitePipeline.JournalFullBeforeAndAfterReplacement | Общий потоковый протокол: SmallVolume.ChangeDuringWrite |
| library copy | FaultProcess.copy | FaultSpace.ReadyAndCopyPreflight | FaultSpace.ReadyAndCopyDuringWrite | FaultSqlitePipeline.JournalFullBeforeAndAfterReplacement | Общий потоковый протокол: SmallVolume.ChangeDuringWrite |
| library trash | FaultProcess.lifecycle | Не применимо: перемещение без копии ZIP | Не применимо: ZIP не записывается | FaultSqlitePipeline.JournalFullBeforeAndAfterReplacement | Представитель SQLite: SmallVolume.DatabaseFull |
| library restore | FaultProcess.lifecycle | Не применимо: перемещение без копии ZIP | Не применимо: ZIP не записывается | FaultSqlitePipeline.JournalFullBeforeAndAfterReplacement | Представитель SQLite: SmallVolume.DatabaseFull |
| library purge | FaultProcess.lifecycle | Не применимо: удаление, место на ZIP не расходуется | Не применимо: ZIP не записывается | FaultSqlitePipeline.JournalFullBeforeAndAfterReplacement | Представитель SQLite: SmallVolume.DatabaseFull |
| import run: папка | FaultProcess.import_folder | FaultSpace.ImportInputs | FaultSpace.ImportInputs | FaultSqlitePipeline.ImportJournalFull | SmallVolume.ImportInput |
| import run: ZIP | FaultProcess.import_zip | FaultSpace.ImportInputs | FaultSpace.ImportInputs | FaultSqlitePipeline.ImportJournalFull | Общая запись ассета: SmallVolume.ImportInput |
| import run: вложенный ZIP | FaultProcess.import_nested | FaultSpace.ImportInputs | FaultSpace.ImportInputs | FaultSqlitePipeline.ImportJournalFull | Общая запись ассета: SmallVolume.ImportInput |
| import run: между кандидатами | FaultProcess.import_between | FaultSpace.ImportInputs | FaultSpace.ImportInputs | FaultSqlitePipeline.ImportJournalFull | Общая запись ассета: SmallVolume.ImportInput |
| export полный | FaultProcess.export_full | Export.CancelSpaceAndCorruptTemporaryLeaveNoOutput | FaultSpace.ExportDuringWrite | FaultSqlitePipeline.ExportCheckpointFull | SmallVolume.FullExport |
| export выборочный | FaultProcess.export_files | FaultSpace.SelectiveExportPreflight | FaultSpace.ExportDuringWrite | FaultSqlitePipeline.ExportCheckpointFull | Общий файловый sink: SmallVolume.FullExport |
| library rebuild | FaultProcess.rebuild | Не применимо: только SQLite | Не применимо: только SQLite | FaultSqlitePipeline.IndexTransactionFull | SmallVolume.DatabaseFull |
| library sync | FaultProcess.sync | Не применимо: только SQLite | Не применимо: только SQLite | FaultSqlitePipeline.IndexTransactionFull | Представитель SQLite: SmallVolume.DatabaseFull |
| восстановление при открытии | OpenRecovery.CrashedChangeAtEveryFaultPointOpensWithoutExplicitRecovery; FaultProcess.recovery | Не применимо: готовый ZIP перемещается без упаковки | Не применимо: ZIP не переписывается | FaultSqlitePipeline.RecoveryFull | Представитель SQLite: SmallVolume.DatabaseFull |
| журнал операций | FaultProcess.operation_log | Не применимо: только SQLite | Не применимо: только SQLite | FaultSqlitePipeline.OperationLogFull | Представитель SQLite: SmallVolume.DatabaseFull |
| журнал приложения | FileLog.ErrorSurvivesKilledProcess | Не применимо: нет предварительной оценки места | FileLog.WriteFailureCountsDroppedAndRecoveryReportsItWithoutThrowing | Не применимо: JSON Lines, не SQLite | SmallVolume.ApplicationLogFull |
| экспорт диагностики | FaultProcess.diagnostic | Не применимо: нет предварительной оценки места | FaultSpace.DiagnosticExportDuringWrite | Не применимо: базы читаются, не записываются | SmallVolume.DiagnosticExportFull |

## Оракул

О-1…О-6 заданы T-036. Проверяются SHA всех подтверждённых ZIP и пользовательских
данных, отсутствие неподтверждённой публикации, журнал, временные файлы и
равенство снимков индекса после sync/rebuild через index_snapshot из T-035.
Для разрешённого purge после необратимого удаления проверяется отсутствие
ассета; до удаления — прежний ZIP. Для trash/restore меняется только положение,
байты сохраняются. Это прежняя семантика T-019, не изменение репозитория.

О-2 дополнительно закрепляют прежние Trash.ConflictsNeverReplaceEitherFile,
CommitProtocol.RecoveryUsesWholeFileIdentityNotOnlyManifest,
ImportRun.ForeignTrashAndDualLocationsAreConflictsWithoutWrites и
ImportRun.ForeignAssetAtAllocatedIdIsNeverChanged. Их ожидания не изменены.
О-6 и отображение SQLITE_FULL для **всех трёх** БД проверяет
FaultSqlite.AllThreeDatabasesRollbackRealSqliteFull, включая откат незавершённой
транзакции userdata. S3 — настоящая SQLite поверх делегирующего тестового VFS,
а не возврат заранее готового am::Error.

По В-6 О-3 «нет публикации» действует до replace. После replace завершённый ZIP
сохраняется, предупреждение несёт insufficient_space и free_space;
FaultSqlitePipeline.IndexFullAfterConfirmedCommitWarnsAndRemainsRecoverable
проверяет именно SQLITE_FULL, затем отдельный процесс выполняет sync и сравнение
с rebuild. Флаг, не записавшийся из-за места, не заменяет проверку stat в sync.
У незавершённого задания экспорта реальная нехватка места в checkpoint даёт
resource_error по прежнему контракту исполнителя; публикации нет.

Журнал приложения имеет прежний noexcept/best-effort контракт: вместо Result
потерянные записи считаются и после освобождения места сообщаются log.dropped.
О-3 в форме возвращаемого Result к нему неприменимо; подтверждённый префикс
журнала и возобновление проверяются на реальном томе. Убийство диагностики может
оставить неподтверждённый временный файл рядом с внешней целью; он не считается
подтверждёнными данными и не относится к библиотечной .am/.am-tmp (О-5).

Принудительное завершение отличается от возврата Error: RAII-откат в дочернем
процессе не выполняется. Там, где уже принят протокол из трёх повторов на точку,
новые проверки используют три повтора. Результат R без тома — SKIP с причиной,
а не PASS подставленного места. Вторая половина K-117 (незавершённые внешние
сессии и очистка кэша) неприменима в P0.

## State и границы обнаружения

В-3 сохраняет правило T-022: после штатно закрытой сессии нет полного обхода
страниц при открытии. Заголовок и обязательная схема state проверяются всегда,
до writer-изменений. Порча данных при нечистой сессии проверяется только для
чтения до изменения файлов. При чистой сессии повреждение обнаруживается
операцией, читающей эту страницу: FaultState.ClosedCorruptPageIsExplicitAtUseRollsBackAndVerifyFindsIt
подтвердил corrupt_data, откат транзакции, неизменные ZIP и обнаружение обоими
уровнями library verify (quick_check / integrity_check state). Это уточнение
формулировки задачи, не новая полная проверка каждого clean-open.

В-4: незакрытая сессия и пустой/отсутствующий WAL дают подозрение
journal_may_be_lost. Orphan-файлы сохраняются на одно успешное открытие;
обработка строк журнала прежняя. Возможны ложные срабатывания после штатного
checkpoint WAL: они допустимы в пользу сохранения данных. После закрытой
сессии действует обычный orphan_age. Три случая проверяет
FaultState.ClosedUnclosedWalAndNoWalUseDistinctOrphanPolicies.
