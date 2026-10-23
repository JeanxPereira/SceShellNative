# App database: DbPreparation and the ShellCore AppInfo store

The home screen (NPXS40002) lists the titles of the ShellUI's `app.db`
(`/<sandbox word>/priv/mms/app.db`, tables `tbl_contentinfo`, `tbl_iconinfo_<user>`, …). The
eboot's AppDb client builds those tables from SceShellCore's AppInfo store,
`/system_data/priv/mms/appinfo.db`, which SceShellCore fills from the installed apps. The
work is started by **DbPreparation**, a library both modules link, which the managed
`Sce.Vsh.DbPreparationWrapper` waits on during boot.

This document covers DbPreparation (eboot and ShellCore) and the AppInfo store
(`psmhost/eboot/db_preparation*`, `psmhost/shellcore/db_preparation_server*`,
`psmhost/shellcore/app_db*`) and the eboot's AppBrowse mirror, `app.db`
(`psmhost/eboot/app_browse*`, see "app.db" below). Addresses: `eboot` = ShellUI
`NPXS40087/eboot.bin`, `SC` = `SceShellCore.elf` (FW 4.03).

## Boot sequence

```
eboot initializeSystem (0xC440)
  0x56C0  ── start SceDbpPreparationThread with Event{CEX, user -1, INITIALIZE}
               InitializeDb (0x36A00) → cexInitialize (0x3AF00)
                 IPC 0x160007 IsBadPowerDownDetected           ──► SC SceDbPreparationServer
                 prepare units [app.db, media_event_listener, async_storage]
                 IPC 0x160009/0x16000A BeginWaitForInitialization ──► SC waits for its own
                                                                        preparation thread:
                                                                        app.db unit = AppInfoDaemon::Init
eboot initializeFrameworkDependentModules (0xD050)
  0x5850  ── CEX user-event callback (user created/deleted → CREATE_USER/DESTROY_USER)

managed BootManager.BackgroundInitDbRecovery
  CexCheckRequest()  ── join SceDbpPreparationThread; DbRecovery request (dbr.db-err)?
                        none → start SceDbpNotificationThread:
                               DeleteInvalidUser, NotifyDbInitialized (IPC 0x160002 + listeners)
managed BootManager.EvalNotifyDBInitializeStarted
  WaitNotificationThread() ── join SceDbpNotificationThread, then GetDbInitializedEvent()
                              → PsmEvent OnNotifyDbInitialized {cexOrKratos 0, userId -1,
                                 isPrimaryUser false, eventType 0}
```

On the host, `psmhost/eboot/boot_manager_init.cpp` runs the two eboot steps at the points
the eboot does; SceShellCore's side starts on the first client connect (HOST POLICY below).

## eboot DbPreparation (`psmhost/eboot/db_preparation.cpp`)

| Piece | Firmware | Host |
|---|---|---|
| managed natives | icall table 0xCC5500..0xCC5630, thunks 0x3B550..0x3B5F0 | `SSN_ICALL("Sce.Vsh.DbPreparationWrapper::…")` |
| manager | ctor 0x35470: mutex EBF118, preparation thread object EBF120, preparer EBF128 (vtable 0xCC5470), notifier EBF130 (vtable 0xCC5440), notification thread object EBF138, pre-notify hook EBF140/EBF160 | `Manager`, `Preparer`, `Notifier` |
| threads | StartThread 0x34BD0, WaitThread 0x34D90; "SceDbpPreparationThread" (0x34F50 → InitializeDb), "SceDbpNotificationThread" (0x352B0) | `ssn::dbp::Thread` (shared with SC) |
| units | 0x38740, list by build/kind 0x38700; handle vtable 0xCC52F0 calls impl +16/+24, 0 without an impl | `UnitSlot`, `register_unit_impl` |
| notifier | NotifyDbInitialized 0x3AD20 (once: IPC 0x160002, listeners +16), NotifyDbTerminating 0x3ADB0; listener lists 0/1 (0x35870/0x35990) | `register_listener` |
| invalid users | EBF110 (vtable 0xCC4EE8): callbacks 0x33B00, DeleteInvalidUser 0x33E90 (IPC 0x160004/5/6) | `register_invalid_user_callback` |
| client | "SceDbPreparationServer" IPMI client 0x39D70; methods 0x160000..0x16000A | direct calls into `shellcore::dbp_server` |

Natives:

| Native | Firmware | Behaviour |
|---|---|---|
| `CexCheckRequestNative(out bool)` | 0x33460 | joins the preparation thread; DbRecovery CheckRequest (0x57DE10: `/<word>/priv/mms/dbr.db-err`, 8-byte header with 0xCAFE in the top 16 bits) → `true`; otherwise starts the notification thread (the out value is not written) |
| `WaitNotificationThreadNative()` | 0x35300 | WaitThread of the notification thread |
| `GetDbInitializedEventNative(out Event)` | 0x35310 | the notification thread object's 16-byte event (copied from the preparer's event when the thread started) |
| `NotifyDbInitializedNative(int, bool)` | 0x34FD0 | NotifyDbInitialized {build of the current event, uid, primary, 4·(Kratos)}; error dialogs when first |
| `NotifyDbTerminatingNative(int, bool)` | 0x35090 | NotifyDbTerminating {…, 4·(Kratos)+1} |
| `DeleteInvalidUserNative()` | 0x33D00 | 0x33E90 |
| `GetDbRecoveryStatusNative(out …)` | 0x33560 → 0x3AE50 → 0x64C980 | DbRecovery state 1 (idle): `{0, 0, 0.0, 1, 0}` (constant 0xB8B860) |
| `CexDbRecoveryStartNative`, `KratosDbRecoveryStartNative` | 0x33500 / 0x33A60 → DbRecovery 0x64C8A0 | not reproduced (0x809C103B) |
| `KratosLogInNative`, `KratosLogOutNative` | 0x33600 / 0x337F0 | reproduced as far as the manager; the Kratos units are not |

WaitThread returns, on its first call, the first dword of the thread's exit value — the thread
object itself, i.e. `event.cexOrKratos` (0 on CEX); afterwards the stored status.

Units of the eboot (CEX list `[app.db 0x809C1054, media_event_listener 0x809C107C,
async_storage 0x809C1058]`):

* `app.db` — impl EBA448 (vtable 0xCC42F0, prepare 0x520, terminate 0x5E0) is the eboot AppDb
  client, `psmhost/eboot/app_browse.cpp` (see "app.db" below): registered from the static
  initialiser 0xC80; prepare registers its listener EBA478 on list 0.
* `media_event_listener` — impl 0x21EF858 (MediaEventListener IPC client, 0x7238A0): not
  reproduced.
* `async_storage` — internal (0x38E40): creates `/<word>/priv/mms/nobackup/async_storage_inernal`
  and its `version.txt` (int 1); reproduced in `db_preparation_async_storage.cpp`.

## ShellCore DbPreparation server (`psmhost/shellcore/db_preparation_server.cpp`)

Server object 0x77CB90, dispatcher 0x77D0F0:

| Method | Handler | Host function |
|---|---|---|
| 0x160000 InitializeDbPreparation(Event) | 0x7785B0 → preparer +8 (0x779CD0) | `initialize_db_preparation` |
| 0x160001 TerminateDbPreparation(Event) | 0x778640 | `terminate_db_preparation` |
| 0x160002 NotifyDbInitialized(Event) | 0x778710 → notifier 0x778C80 | `notify_db_initialized` |
| 0x160003 NotifyDbTerminating(Event) | 0x7787C0 | `notify_db_terminating` |
| 0x160004 / 0x160005 invalid users | 0x77CA00 / 0x77CA10 | empty list |
| 0x160006 DeleteInvalidUser | 0x777340 | no-op (empty list) |
| 0x160007 IsBadPowerDownDetected(out u8) | 0x77CA30 (DbRecovery state 0x1259530) | 0 (HOST VALUE) |
| 0x160008 GetStatus(out 24 bytes) | 0x77CA60 | preparer state |
| 0x160009 + 0x16000A wait | 0x77CAA0 "SceDbPreparationWaitThread": WaitThread(1AB4858) | `wait_for_initialization` |

SceShellCore's own preparation (manager 0x778A90, vtable 0x19AA380) runs its CEX initialize
0x77BE90: user-id cache (not reproduced), bad power-down check, `existSystemDbDir` (0x77BDC0),
then the units of 0x77A420: **app.db** (impl 0x1A14FE8: prepare 0x1B78E0 = AppInfoDaemon::Init,
error 0x809C104F), av_content.db, savedata.db, share, add_content.db, add_content_backup.db
(not reproduced).

## appinfo.db (`psmhost/shellcore/app_db*.cpp`)

Path: `/system_data/priv/mms/appinfo.db` (0xB48DA0) → `<cache>/system_data/priv/mms/appinfo.db`.

Schema (0xBC2D50, statements at 0x19CA210):

```sql
CREATE TABLE IF NOT EXISTS tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));
CREATE TABLE IF NOT EXISTS tbl_appinfo(titleId TEXT NOT NULL,metaDataId TEXT,key INT NOT NULL,val,PRIMARY KEY(titleId, metaDataId, key));
CREATE TABLE IF NOT EXISTS tbl_conceptinfo(local_concept_id TEXT NOT NULL,metadata_id TEXT,key INT NOT NULL,val,PRIMARY KEY(local_concept_id, key));
```

`key` holds the key *name* (bound as TEXT); `val` is INTEGER or TEXT; empty strings are bound as
NULL (binder 0xBEEE50 types 1/3/5). Connection setup (0xBDF4D0): busy timeout 30000 ms,
`PRAGMA foreign_keys=ON`, `recursive_triggers=ON`, `case_sensitive_like=ON`.

`tbl_version` rows: `appver` = 0x9CCD01B6 and `appinfover` = 29 (0xBAB580), `build_target` = 0,
`access_index` (0xBC3480), `appinfo_sync_index` / `conceptinfo_sync_index` (0xB61960 / 0xB98700).

### Daemon start (AppInfoDaemon::Init 0xBAAEF0)

1. `mkdir /system_data/priv/mms` (0700).
2. Open read-write (0xBDE170, flags 2). Failure (missing file: CANTOPEN) → status 256.
3. Version check (0xBBD4D0): no `tbl_version`, `appinfover` ≤ 0, `build_target` ≠ 0,
   `appinfover` > 29 or ≤ 25 and ≠ 29 → 256 (rebuild); older `appinfover` or different
   `appver` → flags 0x20 | 0x40·(appver changed) (| 0x80 for 25 < appinfover < 29).
4. Sync counters (0xB61130 / 0xB97D80).
5. Status 256 → DbRecovery: Reset (0x12268B0: unlink, create — 0xBBCF20 opens with
   READWRITE|CREATE, creates the tables, `access_index`) and Promote (0x1226A50 →
   RecoveryPromoteAppInfo 0xBBD1A0: promote event 1 = AppInstallAll 0x225E40, then
   SetValueTblVersion 0x1D9CCD01B6).

**HOST POLICY** (declared, logged): the console reaches step 5 through DbRecovery (corruption
report 0x125B2A0 → `dbr.db-err` → ShellUI recovery screens → recovery service). The host runs the
Reset + Promote directly in the daemon, so a missing or invalid appinfo.db is rebuilt from the
installed apps at start; `dbr.db-err` is never written, so the ShellUI never sees a recovery
request. A database with the current versions is reused as is (the console promotes apps again
only on system software updates, `appver` change → event 9, not reproduced).

### Population (RNPS apps)

AppInstallAll (0x225E40) installs `/system_ex/app/`, `/system_ex/rnps/apps/` and `/user/app/`
titles; the host reproduces the RNPS root (`/system_ex/rnps/apps/` = `host::ctx().rnps_root`/apps):

* titles = the directories of the root (0x228BA0);
* per title, 0x2294F0: skip `NPXS29998`; for each directory `<root><tid>/appdb/<dir>`:
  param file = `param.json` (else `param.sfo`), must be a regular file; skip when
  `ignore_cex.dat` exists (CEX unit; `ignore_devkit.dat` / `ignore_testkit.dat` are not checked
  on CEX); the title id is `<tid>` for `default`, else `<dir>`; skip titles promoted from
  `/system_data/priv/appmeta[/external]/<t>/param.{sfo,json}`; AppPromote
  `(param, <dir>, <dir>, <root><tid>, 2, 0, 1, 1, -1)` (0x232950);
* param.json (0x1067790) → SFO-shaped param list: 16 integers always present (CATEGORY_TYPE =
  `applicationCategoryType`, ATTRIBUTE, ATTRIBUTE2, ATTRIBUTE3, ATTRIBUTE_EXE,
  DOWNLOAD_DATA_SIZE, USER_DEFINED_PARAM_1..4, DISPLAYLOCATION = `displayLocation`,
  PARENTAL_LEVEL, MASS_SIZE, SERVICE_LAUNCH_BUTTON_KEY_CODE, CONTENT_BADGE_TYPE,
  NOTICE_SCREEN_VERSION), CONTENT_ID, TITLE_ID (9 chars, 4 letters + 5 digits), WEBAPP_URI,
  DEEPLINK_URI, HUBAPP_URI, CONCEPT_ID, MUSIC_CORE_NAME, BACKGROUND_BASEMAT_TYPE,
  SDK_VERSION, CONTENT_VERSION, VERSION_FILE_URI, TITLE_00..29 (`localizedParameters[lang]`,
  table 0x19DDD40), TITLE (default language), AGE_LEVEL, INSTALL_DIR_SAVEDATA,
  SAVE_DATA_TRANSFER_TITLE_ID_LIST(_PS4), APP_TYPE (`applicationDrmType`), SYSTEM_VER_PPR,
  USB_DIR_LIST, SERVICE_ID_ADDCONT_ADD_n, BACKGROUND_IMAGE_ID, GAME_INTENT; plus
  `pubtools.toolVersion` and `musicCoreTitleId` on the side;
* AppPromote (0x243DD0) copies the 101 known keys (TITLE_ID also as `_title_id`), then:
  `_current_slot` 0; integer 0 for missing DISP_LOCATION_1/2, USER_DEFINED_PARAM_1..4,
  `#exit_type`, ATTRIBUTE_INTERNAL; TITLE ""; PUBTOOL_VERSION; `_install_version` (system
  software version); `_app_format_type` 1; `_ps_platform` (CATEGORY_TYPE 0x6000100/0x6000300 →
  1, 0x6000000 → 2, else 0); `_metadata_path` = appdb dir; `_org_path` = app dir;
  `_contents_location` 2; `_uninstallable` (0x24C300); `_view_category` (0x24CA80);
  `#_mtime` (UTC, `YYYY-MM-DD HH:MM:SS.mmm`); `#_contents_status` (pre_promote.dat);
  `_disable_live_detail`; `_contents_ext_type` (ATTRIBUTE_INTERNAL & 0x10 → 3);
  `_hdd_location` 0; `_m2_device_id` 0;
* metadata id: type 4 (`_metadata_path` under `/system_ex/`) → `:shellapp` (0xB29A00/0xB29EA0);
  a new title is stored as the prioritised content `prior:shellapp` (0xB6F980);
* InsertAppInfo (0xB6C700): copy without `#_access_index`/`#_last_access_time`, minus
  `#_promote_time #_install_time #_last_access_time _sort_priority #_access_index _path_info
  _path_info_2 _local_concept_id`; `_title_id`, `METADATA_ID`; `_primary_title_sort` (0xB2C9D0);
  `_local_concept_id` = `cid:scp:%016lx` of CONCEPT_ID, else `cid:local:<titleId>`; derived keys
  (0xB60770): `_sort_priority` (table of 14 system titles, default 100), DISPLAYLOCATION
  defaults (0xB5F320), DEEPLINK_URI / HUBAPP_URI defaults (0xB5F8F0), `_external_hdd_app_status`
  0, `#_install_time` (table of 18 system titles, `#_ctime`, `#_promote_time`), `_ps_platform`
  overrides, `sync_index`; rows via 0xBC6BD0 — the keys of set 0x1B0ED98 (`_sort_priority`,
  `#_last_access_time`, `#_access_index`, catalog image paths, DISC_*) go under metaDataId
  `prior`; the concept row set (0xB74630/0xB80870, key tables 0x1AFAA30/0x1AFB660/0x1AFB750);
* install status (0x237740): `_install_status`, `_install_sub_status`,
  `_not_install_sub_status`; then UpdateAppInfo stores the promote record again (REPLACE).

### Server API for the eboot AppDb client (`app_db.h`)

| Client (eboot) | Server (SC) | Host |
|---|---|---|
| appDbIpcClientInitialize 0x30EBB0 | SceAppDbIpc server | `ipc_client_initialize()` |
| AppInfoClient::Initialize 0x2740C0 ("SceAppInfoServer") | 0xBAE740 | `app_info_client_initialize()` |
| appDbIpcGetAppInfoValueList 0x274580 (GetAppInfoJson) | 0xBAF2A0 → 0xBB28C0 → 0xBC4030 | `get_app_info_value_list(titleId, metadataId, keys, appType, AppInfo*)` |
| AppDbIpcSearch 0x275460 (GetTitleListJson) | 0xBAFEE0 → 0xB3E1D0/0xB3E2D0 → 0xBCBC30 | `search(ConditionChain, sort, mode, resultType, …)` |
| AppDbIpcUpdateAppInfo 0x276A50 | 0xBAE8B0 → 0xB3DA70 → 0xBCA240 | `update_app_info(AppInfo)` |
| PurgeDeletedList 0x276D00 | — | logged, 0 |

GetAppInfo (0xBC4030): the title must have rows `metaDataId LIKE <metadataId>`; a metadata id
containing `prior` reads `metaDataId LIKE 'prior%'` (prioritised rows plus the `prior`
shared keys), any other reads it together with `prior`.

Search (0xBCBC30): a chain of conditions joined by AND (`INNER JOIN` / `,tbl_appinfo aN`) or
OR (all on `a0`), each `(aN.key=? AND (aN.val<op> OR …))` with the operators of 0x19CA248
(`=?`, ` LIKE ? ESCAPE '$'`, `!=?`, `&?=?`, `<?`, `>?`); sort fields add `LEFT JOIN`s on
`tN` and an `ORDER BY` (`ASC` for type 1, `DESC` otherwise, type 0 ignored); the pseudo key
`TITLE_SYSTEM_LANG` sorts by the system language's `TITLE_xx` falling back to `TITLE`. Result
type 0 returns the title ids of rows whose metadata id contains `prior`.

The IPMI/JSON transport (3584-byte chunks, `field_list` / `value_list` / `op` / `bit_mask` /
`next_cond_type` / `TitleID` / `MetadataID`) is replaced by the structures.

## Paths

| Console | Host |
|---|---|
| `/system_data/...` (ShellCore) and `/<word>/...` (ShellUI) | `<cache>/system_data/...` |
| `/system_ex/rnps/...` | `host::ctx().rnps_root/...` |
| `/user/...` | `<cache>/user/...` |
| other | the firmware dump |

Values stored in the database are console paths (as on the console); consumers map them.

## Not reproduced (each logged once)

DbRecovery (eboot 0x64xxxx, SC 0x125xxxx) and its recovery screens (replaced by the host policy
above); the media event listener unit; the Kratos (kiosk) paths' units; the ShellCore units
other than app.db; the user-id cache and invalid users; the pre-notification hook EBF160;
error dialog events to the ShellUI; `/system_ex/app/`, `/user/app/` and appmeta-based titles;
param.sfo apps; the merge path for a title already stored; `_usb_dir_list_xx` /
`_ps3_title_id_for_boot_xx` splitting; concept_title.db; delete marks; the concept merge and
the conceptinfo sync index increment; notifications to AppDb clients (appDbIpc events); search
modes 1..3 and concept value lists; the derived-key steps of UpdateAppInfo; PurgeDeletedList.

## app.db: the eboot AppBrowse mirror (`psmhost/eboot/app_browse*.cpp`)

The shell's managed accessors read `app.db` directly (`Sce.Vsh.Accessor.Db`:
`AppBrowseItemAccessor` reads `tbl_iconinfo_<user> as icon left outer join tbl_contentinfo`;
`ConceptBrowseItemAccessor` reads `tbl_concepticoninfo_<user> as cicon` joined with
`tbl_conceptmetadata`, the user's `tbl_iconinfo_` and `tbl_contentinfo`, with `cicon.validFlag = 1`).
The home screen NPXS40002 lists concepts per space, "game" and "media". The eboot keeps app.db
as a mirror of SceShellCore's appinfo.db.

### Unit, client start and recovery (`app_browse.cpp`)

| Piece | eboot | Host |
|---|---|---|
| unit impl / listener | static init 0xC80 (EBA448 vtable 0xCC42F0, EBA478 vtable 0xCC4320) | `AppDbUnit`, `AppDbListener` |
| prepare | 0x520: InitClient 0x30E230, 0x30E660, 0x30E5A0, invalid-user callback 0x33B00(0xB20), listener on list 0 | `AppDbUnit::prepare` |
| InitClient | 0x30E230: appDbIpcClientInitialize 0x30EBB0, AppInfoClient 0x2740C0, DbAccessor::Init 0x33E210(path, 2, {lang, 114}), CheckDbSyncIndex 0x30E4C0, table caches 0x277940/0x28DC70/0x261990, observers 0x30F080 | `init_client` |
| openDb | 0x33E4C0: Open(path, flags), busy 30000, `PRAGMA foreign_keys=ON`, `PRAGMA recursive_triggers=ON`, `PRAGMA cache_size = 0x40000/page_size` | `open_db` |
| version | 0x33F540 / 0x33EAA0: tbl_version `appbrowsever` (114) and `locale`. Result bits: 0x40 none, 2 newer, 0x10 older, 1 other locale, 0x100 open failed. DbRecovery when `flags & 0xFFFFF9CE` (0x33B060) | `check_version`, `needs_recovery` |
| recovery reset | 0x3463C0 RecoveryAppDbReset: unlink, then Init(path, 6, version, schema 0x347810) | `recovery_reset` |
| recovery promote | 0x346520: conceptinfo 0x331BD0(0, 0), then appinfo 0x312140(0, 0) | `recovery_promote` |
| DbInitialized | 0x640: InitializeAfterDbRecovery 0x299050, Enable_AppDb 0x30E800 (the event-flag thread 0x2DC7B0 sets flags 18, which runs the 0x30E700 sync) | `AppDbListener::on_initialized` |
| revision native | GetAppBrowseDbRevisionNative 0x299070: tbl_version `table_access_index` | `SSN_PINVOKE` |
| user events | 0x30E49B: on a retail unit InitClient registers 0x30E550 with sceUserServiceRegisterEventCallback. Type 256 (SceShellCore CreateUser 0x6FB800 posts it at 0x6FBDBB) -> 0x34A780 -> 0x34A7D0: 0x3064C0 tbl_addon_/tbl_info_, then 0x2AA740 (ReflectContentInfoToIconInfo command, execute 0x3005E0): 0x28DC90 tbl_iconinfo_, 0x2619B0 tbl_concepticoninfo_, every tbl_contentinfo title through 0x2C24A0 -> 0x2E56C0. Type 257 -> 0x34B300 | `on_user_event`, `add_user` |

**HOST POLICY** (logged): a missing or invalid app.db, or one the version/sync checks send to
DbRecovery, is rebuilt by running RecoveryAppDbReset and RecoveryAppDbPromote directly. The
console reports it to DbRecovery (0x57FEE0) and its recovery service runs the same two steps.
The schema covers the users registered at that moment; a user created later (the initial setup
creates the first one after app.db exists) gets its tables from the user-created event above.
Without that event, a database built before the user existed has no `tbl_iconinfo_<user>`, and
the home screen's queries fail with "no such table" (the home never mounts).

### Schema (`app_browse_db.cpp`, exact statements)

1. `CREATE TABLE IF NOT EXISTS tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));` (0x304CD0).
2. For each registered user (sceUserServiceGetRegisteredUserIdList, 0x304D40):
   * `CREATE TABLE IF NOT EXISTS tbl_addon_%010d (entitlementId …)` (0x3064F0)
   * `CREATE TABLE IF NOT EXISTS tbl_info_%010d (key TEXT NOT NULL,value TEXT,PRIMARY KEY(key));` (0x306A00)
   * `CREATE TABLE tbl_iconinfo_%010d (…)` (0x284790, column list 0xB05ECC)
   * `CREATE TABLE tbl_concepticoninfo_%010d (…)` (0x25A3E0, column list 0xB1FC52)
3. `CREATE TABLE IF NOT EXISTS tbl_contentinfo (…)` (0x277970, column list 0xB39BC9).
4. `CREATE TABLE IF NOT EXISTS tbl_conceptmetadata (…)` (0x2C7CD0, column list 0xB35460).

The full column lists are in `app_browse_db.cpp`. Rows are written as
`INSERT INTO t(key,cols…) VALUES(?1,…)` (0x264300) or `UPDATE t SET col=?2,… WHERE key=?1`
(0x2649B0). Empty strings bind as NULL (0x3AF3B0).

### Promote and sync

* **appinfo** (0x312140, execute 0x324BA0):
  * The SceShellCore titles with `sync_index > from AND < to+1` (0x2DD410: search mode 0, pairs, sorted by sync_index).
  * Per title, the metadata ids give the packageLocation flags (0x3260B0: "internal" 1, "external" 2).
  * InsertTransactionCommand 0x327800 writes tbl_contentinfo.
  * The `appinfo_sync_index` is stored (0x315A00).
* **conceptinfo** (0x331BD0, execute 0x33A400):
  * The concepts with the same condition (search mode 1; SC 0xB49870 → 0xBD5D40).
  * Per concept, 0x338FF0 turns the ConceptInfo into tbl_conceptmetadata (0x332C50 + 0x337090), written by 0x337C80.
  * The `conceptinfo_sync_index` is stored (0x338020).

### tbl_contentinfo (`app_browse_content.cpp`)

**0x318150** runs only for an AppInfo that has `_app_format_type`. Notable mappings:

| Column | AppInfo source |
|---|---|
| contentLocation | `_contents_location`, copied as is (2 for RNPS apps) |
| sortPriority | `_sort_priority`; 100 when absent (the SceShellCore map gives 1000 to NPXS40071/NPXS40139) |
| uninstallable | `_uninstallable`; 1 when absent |
| installTime | `#_install_time`, else `#_promote_time` |
| viewCategory | `_view_category`: 0 "game", 1 "app" |
| ps4parentalLevel, ps4contentType, ps4Attribute | PS4 apps only |
| pprAttribute3, pprAgeLevel | PS5 apps only |
| platform | `_ps_platform` |
| appinfoContentType | 0x26F880, from METADATA_ID (":shellapp" → 4, …) |
| AppInfoJson | AppInfo vtable +0 (0x268F70): `{"field_list":[{"data","key","size","type"}…],"mimeType":0,…}` |

The other columns are straight copies: appFormatType, contentId (CONTENT_ID), metaDataPath
(`_metadata_path`), contentStatus (`#_contents_status`), pathInfo, dispLocation
(DISPLAYLOCATION), pathInfo2, size, promoteTime, hddLocation, externalHddAppStatus, mTime,
sizeOtherHdd, categoryType, attributeInternal, conceptId, localConceptId, pprDeeplinkUri,
pprHubAppUri, installStatus, discStatus, appType, primaryTitleSort, bgMatteType, bgImageId,
contentBadgeType, is3rdPartyVideoApp, upgradePs5TitleIdList.

**0x3224F0** adds the title and paths:
* titleName: TITLE of the system language, else TITLE.
* snd0Info: `<md>/snd0.at9` when the file exists.
* icon0Info, pic0Info, pic1Info: `<md>/icon0[_xx].png?ts=<time>`, built from the `_path_*_info`
  masks that SceShellCore's content-path check (0xB71B60) stores.

**0x321D10**, for a new row only: lastAccessTime (install time, else promote time) and
lastAccessIndex (`#_access_index`).

**0x3234E0** sets packageLocation.

### tbl_iconinfo_<user> (`app_browse_icon.cpp`)

The ContentInfo observer 0x2E5600 runs modifyAppearanceControl (0x2E56C0) for every user that has
an iconinfo table:

1. Record type (0x2D39A0): ":shellapp" content → 7.
2. Title status (0x2E0A10): type 7 → 10 when dispLocation bit 0 is clear.
3. Visibility class (table 0xBF4380): status 10 → visible.
4. updateLastAccess (0x2E1250): `access_index` + 1, and the current time.
5. updateRepresentativeInfo (0x2E1850):
   * titleName, localConceptId, conceptId, contentId, hubAppUri, platform, …
   * dispLocation: the content's, with bit 0 cleared.
   * visible.
   * installStatus (0x2B87E0): content value 0 → 2.
   * primaryTitleSort (0x2B7120): `installed << 32 | sort`.
6. Insert (0x2AF300) or update (0x2AEC60). The user info `go_west_by_user` is set to "1" when the row has a lastAccessIndex.

### tbl_concepticoninfo_<user> (`app_browse_concept.cpp`)

The IconInfo observer 0x2EAEA0 derives the concept operation from the rows' `visible` (0x2EBC50).
An insert goes through 0x2EBF50 to 0x2EC300, which sets:

* **validFlag**: 1 when tbl_conceptmetadata has the concept (0x2B8930 then copies its fields).
* **lastAccess\* / lastPlayed\***: from the changed title.
* **The primary title** (0x2F16B0 → 0x2BD290):
  * primaryTitleId, primaryTitleName, primaryTitlePlatform, primaryTitleBgMatteType
  * psNowFlag, deeplinkUri, hubAppUri, snd0Info
  * conceptName: the title name, unless the concept has SCP data
  * metaDataPath, icon0Info, pic0Info: from the content
  * lastInteractedTime
* **The aggregate over the user's titles of the concept**: dispLocation (OR of the titles' dispLocation, plus 0x400 or 0x800), installedStatus, platform bits, filterTypeHidden, ….

The row is written by 0x2EA0D0 (insert) or 0x2EA1B0 (update).

### Not reproduced (each logged once)

* DbRecovery itself (replaced by the host policy) and the corruption-report check 0x57F3D0.
* The SyncManagement event-flag thread and SceShellCore's sync events (one sync runs at DbInitialized).
* GoWestByInitOrder (0x327E80).
* The deleted-title and deleted-concept lists (search modes 2 and 3).
* The last-access write-back to SceShellCore (0x2DE450).
* The UTM, entitlement, GLT and UCM (catalog) data paths; share-play links; disc records.
* The language-change refresh (0x30E5C0).
* Dropping a deleted user's tables (0x34B300).
* For a created user: the transaction around 0x3064C0 (0x33ED80 / 0x33EEF0) and the
  SetInitOrderTransactionCommand at the end of 0x3005E0 (0x300AB0, 0x2AC900, 0x2FB1E0).
* The concept move and removal paths (0x2F0800).
* Notifications to the shell (0x30F850).
* The catalog downloader and the periodic jobs.
* The collator behind `library_canonicalize` (0x4A3260). The function is registered and answers with the collator error.
