//
// Created by jinhai on 23-5-16.
//

module;

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>

import stl;
import logger;
import third_party;
import txn_manager;
import txn;
import storage;
import wal_entry;
import infinity_exception;
import infinity_assert;
import new_catalog;
import db_entry;
import table_collection_type;
import parser;
import txn_store;
import segment_entry;
import block_entry;
import table_collection_entry;
import data_access_state;

module wal_manager;

namespace infinity {

// WalCommandType -> String
String WalManager::WalCommandTypeToString(WalCommandType type) {
    switch (type) {
        case WalCommandType::INVALID:
            return "INVALID";
        case WalCommandType::CREATE_DATABASE:
            return "CREATE_DATABASE";
        case WalCommandType::DROP_DATABASE:
            return "DROP_DATABASE";
        case WalCommandType::CREATE_TABLE:
            return "CREATE_TABLE";
        case WalCommandType::DROP_TABLE:
            return "DROP_TABLE";
        case WalCommandType::ALTER_INFO:
            return "ALTER_INFO";
        case WalCommandType::IMPORT:
            return "IMPORT";
        case WalCommandType::APPEND:
            return "APPEND";
        case WalCommandType::DELETE:
            return "DELETE";
        case WalCommandType::CHECKPOINT:
            return "CHECKPOINT";
        case WalCommandType::CREATE_INDEX:
            return "CREATE_INDEX";
        case WalCommandType::DROP_INDEX:
            return "DROP_INDEX";
        default: {
            Error<StorageException>("Not supported wal command type", __FILE_NAME__, __LINE__);
        }
    }
}

// using namespace std;
// namespace std::filesystem = std::filesystem;

WalManager::WalManager(Storage *storage,
                       String wal_path,
                       u64 wal_size_threshold,
                       u64 full_checkpoint_interval_sec,
                       u64 delta_checkpoint_interval_sec,
                       u64 delta_checkpoint_interval_wal_bytes)
    : storage_(storage), wal_path_(std::move(wal_path)), wal_size_threshold_(wal_size_threshold),
      full_checkpoint_interval_sec_(full_checkpoint_interval_sec), delta_checkpoint_interval_sec_(delta_checkpoint_interval_sec),
      delta_checkpoint_interval_wal_bytes_(delta_checkpoint_interval_wal_bytes), running_(false) {}

WalManager::~WalManager() {
    Stop();
    que_.clear();
    que2_.clear();
}

void WalManager::Start() {
    bool expected = false;
    bool changed = running_.compare_exchange_strong(expected, true);
    if (!changed)
        return;
    Path wal_dir = Path(wal_path_).parent_path();
    if (!std::filesystem::exists(wal_dir)) {
        std::filesystem::create_directory(wal_dir);
    }
    // TODO: recovery from wal checkpoint
    ofs_ = std::ofstream(wal_path_, std::ios::app | std::ios::binary);
    if (!ofs_.is_open()) {
        Error<StorageException>(Format("Failed to open wal file: {}", wal_path_), __FILE_NAME__, __LINE__);
    }
    auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch());
    int64_t now = seconds_since_epoch.count();
    full_ckp_when_ = now;
    delta_ckp_when_ = now;
    max_commit_ts_ = 0;
    wal_size_ = 0;
    flush_thread_ = std::thread([this] { Flush(); });
    checkpoint_thread_ = std::thread([this] { Checkpoint(); });
}

void WalManager::Stop() {
    bool expected = true;
    bool changed = running_.compare_exchange_strong(expected, false);
    if (!changed) {
        LOG_INFO("WalManager::Stop already stopped");
        return;
    }

    LOG_INFO("WalManager::Stop begin to stop txn manager");
    // Notify txn manager to stop.
    TxnManager *txn_mgr = storage_->txn_manager();
    txn_mgr->Stop();

    // pop all the entries in the queue. and notify the condition variable.
    std::lock_guard guard(mutex_);
    for (const auto &entry : que_) {
        auto wal_entry = que_.front();
        Txn *txn = txn_mgr->GetTxn(wal_entry->txn_id);
        if (txn != nullptr) {
            txn->CancelCommitTxnBottom();
        }
    }
    que_.clear();

    // Wait for checkpoint thread to stop.
    LOG_INFO("WalManager::Stop checkpoint thread join");
    checkpoint_thread_.join();

    // Wait for flush thread to stop
    LOG_INFO("WalManager::Stop flush thread join");
    flush_thread_.join();

    ofs_.close();
}

// Session request to persist an entry. Assuming txn_id of the entry has
// been initialized.
int WalManager::PutEntry(SharedPtr<WalEntry> entry) {
    if (!running_.load()) {
        return -1;
    }
    int rc = 0;
    mutex_.lock();
    if (running_.load()) {
        que_.push_back(entry);
        rc = -1;
    }
    mutex_.unlock();
    return rc;
}

void WalManager::SetWalState(TxnTimeStamp max_commit_ts, int64_t wal_size) {
    mutex2_.lock();
    this->max_commit_ts_ = max_commit_ts;
    this->wal_size_ = wal_size;
    mutex2_.unlock();
}
void WalManager::GetWalState(TxnTimeStamp &max_commit_ts, int64_t &wal_size) {
    mutex2_.lock();
    max_commit_ts = this->max_commit_ts_;
    wal_size = this->wal_size_;
    mutex2_.unlock();
}

// Flush is scheduled regularly. It collects a batch of transactions, sync
// wal and do parallel committing. Each sync cost ~1s. Each checkpoint cost
// ~10s. So it's necessary to sync for a batch of transactions, and to
// checkpoint for a batch of sync.
void WalManager::Flush() {
    LOG_TRACE("WalManager::Flush mainloop begin");
    while (running_.load()) {
        TxnTimeStamp max_commit_ts = 0;
        int64_t wal_size = 0;
        mutex_.lock();
        que_.swap(que2_);
        mutex_.unlock();
        if (que2_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        GetWalState(max_commit_ts, wal_size);
        for (const auto &entry : que2_) {
            // Empty WalEntry (read-only transactions) shouldn't go into WalManager.
            Assert<StorageException>(!entry->cmds.empty(), Format("WalEntry of txn_id {} commands is empty", entry->txn_id), __FILE_NAME__, __LINE__);
            int32_t exp_size = entry->GetSizeInBytes();
            Vector<char> buf(exp_size);
            char *ptr = buf.data();
            entry->WriteAdv(ptr);
            int32_t act_size = ptr - buf.data();
            if (exp_size != act_size)
                LOG_ERROR(Format("WalManager::Flush WalEntry estimated size {} differ "
                                 "with the actual one {}",
                                 exp_size,
                                 act_size));
            ofs_.write(buf.data(), ptr - buf.data());
            LOG_TRACE(Format("WalManager::Flush done writing wal for txn_id {}, commit_ts {}", entry->txn_id, entry->commit_ts));
            if (entry->cmds[0]->GetType() != WalCommandType::CHECKPOINT) {
                max_commit_ts = entry->commit_ts;
                wal_size += act_size;
            }
        }
        ofs_.flush();
        TxnManager *txn_mgr = storage_->txn_manager();
        for (const auto &entry : que2_) {
            // Commit sequentially so they get visible in the same order with wal.
            Txn *txn = txn_mgr->GetTxn(entry->txn_id);
            if (txn != nullptr) {
                txn->CommitTxnBottom();
            }
        }
        que2_.clear();

        // Check if the wal file is too large.
        try {
            auto file_size = std::filesystem::file_size(wal_path_);
            if (file_size > wal_size_threshold_) {
                this->SwapWalFile(max_commit_ts);
            }
        } catch (std::exception &e) {
            LOG_WARN(e.what());
        } catch (...) {
            LOG_WARN("WalManager::Flush threads get exception");
            return;
        }
        SetWalState(max_commit_ts, wal_size);
    }
    LOG_TRACE("WalManager::Flush mainloop end");
}

/*****************************************************************************
 * CHECKPOINT WAL FILE
 *****************************************************************************/

// Do checkpoint for transactions which's lsn no larger than the given one.
void WalManager::Checkpoint() {
    LOG_TRACE("WalManager::Checkpoint mainloop begin");
    while (running_.load()) {
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch());
        int64_t now = seconds_since_epoch.count();
        TxnTimeStamp max_commit_ts;
        int64_t wal_size;
        GetWalState(max_commit_ts, wal_size);
        bool is_full_checkpoint = false;
        bool is_delta_checkpoint = false;
        if (now - full_ckp_when_ > full_checkpoint_interval_sec_ && wal_size != full_ckp_wal_size_) {
            is_full_checkpoint = true;
        } else if ((now - delta_ckp_when_ > delta_checkpoint_interval_sec_ && wal_size != delta_ckp_wal_size_) ||
                   wal_size - delta_ckp_wal_size_ > delta_checkpoint_interval_wal_bytes_) {
            is_delta_checkpoint = true;
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        TxnManager *txn_mgr = nullptr;
        Txn *txn = nullptr;
        try {
            txn_mgr = storage_->txn_manager();
            txn = txn_mgr->CreateTxn();
            txn->BeginTxn();
            LOG_INFO(Format("created transaction for checkpoint, txn_id: {}, begin_ts: {}, max_commit_ts {}",
                            txn->TxnID(),
                            txn->BeginTS(),
                            max_commit_ts));
            txn->Checkpoint(max_commit_ts, is_full_checkpoint);
            txn->CommitTxn();

            ckp_commit_ts_ = max_commit_ts;
            delta_ckp_when_ = now;
            delta_ckp_wal_size_ = wal_size;
            if (is_full_checkpoint) {
                full_ckp_when_ = now;
                full_ckp_wal_size_ = wal_size;
            }
            LOG_INFO(Format("WalManager::Checkpoint {} done for commit_ts <= {}", is_full_checkpoint ? "full" : "delta", max_commit_ts));
            RecycleWalFile();
        } catch (std::exception &e) {
            LOG_ERROR(Format("WalManager::Checkpoint failed: {}", e.what()));
        }
    }
    LOG_TRACE("WalManager::Checkpoint mainloop end");
}

/**
 * @brief Swap the wal file to a new one.
 * We will swap a new wal file when the current wal file is too large.
 * Just rename the current wal file to a new one, and create a new wal file with
 * the original name. So we only focus on the current wal file: wal.log When
 * replaying the wal file, we will just start with the wal.log file.
 * @param max_commit_ts The max commit timestamp of the transactions in the
 * current wal file.
 */
void WalManager::SwapWalFile(const TxnTimeStamp max_commit_ts) {
    if (ofs_.is_open()) {
        ofs_.close();
    }

    Path old_file_path = Path(wal_path_);

    String new_file_path = old_file_path.string() + '.' + std::to_string(max_commit_ts);
    LOG_INFO(Format("Wal Swap to new path: {}", new_file_path.c_str()));

    // Rename the current wal file to a new one.
    std::filesystem::rename(wal_path_, new_file_path);

    // Create a new wal file with the original name.
    ofs_ = std::ofstream(wal_path_, std::ios::app | std::ios::binary);
    if (!ofs_.is_open()) {
        Error<StorageException>(Format("Failed to open wal file: {}", wal_path_), __FILE_NAME__, __LINE__);
    }
}

/*****************************************************************************
 * REPLAY WAL FILE
 *****************************************************************************/
/**
 * @brief Replay the wal file.
 *  wal format: wal.log.<max_commit_ts>
 *  the wal.log file is the current wal file.
 *  the most recent wal.log file is the current wal file.
 *
 *  time min -------------------------------------------------------------------------------------------------------------------------------> time max
 *  ================ =========================  =================================================================================================
 *  |   wal.log.1  | |       wal.log.3       |  |                                wal.log                                                        |
 *  ---------------- -------------------------  --------------------------------------------------------------------------------------------------
 *  |[entry｜entry]| |  [entry｜entry｜entry] ｜ | [entry｜entry｜entry| <checkpoint{ max_commit_ts ; catalog_path }>｜entry?(bad checksum)｜entry]|
 *  ================ =========================  ==================================================================================================
 *                                                  ⬆️     ｜                                          ⬆️-------- ⬅️ ---------------------- ⬅️ phase 1:
 find the checkpoint entry
 *                                                  ｜      ｜                      ⬇️
 *                                                  ｜------｜------- ⬅️ -----------｜     phase 2: find the first entry which commit_ts low equal (<=)
 the max_commit_ts
 *                                                          ｜
 *                                                          ➡️ ------ |       ...  (jump checkpoint entry) ...       | continue => end
 *
 *                                                                    phase 3: replay the entries by the order of the wal.log
 *                                                                    - case 1: the entry is a checkpoint entry, jump it.
 *                                                                    - case 2: the entry is a normal entry, replay it.
 *                                                                    - case 3: the entry is a bad entry, stop replaying and end.



 *  phase 1(⬅️): - find the checkpoint entry in the wal.log file. ( Attention: From back to front find the first checkpoint entry)
 *           - get the max commit timestamp of the checkpoint entry.
 *           - get the catalog path of the checkpoint entry.
 *
 *  phase 2(⬅️): - find the first entry which commit_ts low equal (<=) the max_commit_ts
 *  phase 3(➡️): - replay the entries by the order of the wal.log
 * @return int64_t The max commit timestamp of the transactions in the wal file.
 *
 */
int64_t WalManager::ReplayWalFile() {
    // if the wal directory does not exist, just return 0.
    if (!std::filesystem::exists(wal_path_)) {
        storage_->InitNewCatalog();
        return 0;
    }
    // if the wal file is empty, just return 0.
    if (std::filesystem::file_size(wal_path_) == 0) {
        storage_->InitNewCatalog();
        return 0;
    }

    for (const auto &entry : std::filesystem::directory_iterator(Path(wal_path_).parent_path())) {
        if (entry.is_regular_file()) {
            wal_list_.push_back(entry.path().string());
        }
    }

    // e.g. wal_list_ = {wal.log , wal.log.100 , wal.log.50}
    std::sort(wal_list_.begin(), wal_list_.end(), [](const std::string &a, const std::string &b) {
        auto get_lastNumber = [](const std::string &s) {
            auto pos = s.find_last_of('.');
            if (pos != std::string::npos) {
                return std::stol(s.substr(pos + 1));
            } else {
                throw std::invalid_argument("No '.' found");
            }
        };
        bool is_a_wal_log = (a.length() >= 7 && a.substr(a.length() - 7) == "wal.log");
        bool is_b_wal_log = (b.length() >= 7 && b.substr(b.length() - 7) == "wal.log");

        if (is_a_wal_log) {
            return true;
        } else if (is_b_wal_log) {
            return false;
        }

        SizeT num_a = get_lastNumber(a);
        SizeT num_b = get_lastNumber(b);

        return num_a > num_b;
    });

    LOG_INFO("Start Wal Replay");
    // log the wal files.
    for (const auto &wal_file : wal_list_) {
        LOG_TRACE(Format("List wal file: {}", wal_file.c_str()));
    }
    LOG_INFO(Format("List wal file size: {}", wal_list_.size()));

    i64 max_commit_ts = 0;
    String catalog_path;
    Vector<SharedPtr<WalEntry>> replay_entries;
    Vector<String> checkpoint_catalog_paths;

    WalListIterator iterator(wal_list_);
    iterator.Init();
    // phase 1: find the max commit ts and catalog path
    LOG_INFO("Replay phase 1: find the max commit ts and catalog path");
    while (iterator.Next()) {
        auto wal_entry = iterator.GetEntry();

        replay_entries.push_back(wal_entry);

        if (wal_entry->IsCheckPoint()) {

            checkpoint_catalog_paths.push_back(wal_entry->GetCheckpointInfo().second);

            if (wal_entry->IsFullCheckPoint()) {
                auto [current_max_commit_ts, current_catalog_path] = wal_entry->GetCheckpointInfo();
                if (current_max_commit_ts > max_commit_ts) {
                    max_commit_ts = current_max_commit_ts;
                    catalog_path = current_catalog_path;
                }
                LOG_TRACE(Format("Find checkpoint max commit ts: {}", max_commit_ts));
                LOG_TRACE(Format("Find catalog path: {}", catalog_path));
                break;
            } else {
                // delta checkpoint
                auto [current_max_commit_ts, current_catalog_path] = wal_entry->GetCheckpointInfo();
                // if the current max commit ts is greater than the max commit ts, update the max commit ts and catalog path
                if (current_max_commit_ts > max_commit_ts) {
                    max_commit_ts = current_max_commit_ts;
                    catalog_path = current_catalog_path;
                }
            }
        }
    }

    // phase 2: by the max commit ts, find the entries to replay
    LOG_INFO("Replay phase 2: by the max commit ts, find the entries to replay");
    while (iterator.Next()) {
        auto wal_entry = iterator.GetEntry();
        if (wal_entry->commit_ts > max_commit_ts) {
            replay_entries.push_back(wal_entry);
        }
    }

    // Note: Init a new catalog when not found any checkpoint wal entry
    // Indicates that the system has not done checkpoint in the previous.
    if (checkpoint_catalog_paths.empty()) {
        storage_->InitNewCatalog();
    } else {
        std::reverse(checkpoint_catalog_paths.begin(), checkpoint_catalog_paths.end());
        storage_->AttachCatalog(checkpoint_catalog_paths);
    }

    // phase 3: replay the entries
    LOG_INFO("Replay phase 3: replay the entries");
    std::reverse(replay_entries.begin(), replay_entries.end());
    i64 last_commit_ts = 0;
    for (const auto &entry : replay_entries) {
        if (entry->commit_ts > last_commit_ts) {
            last_commit_ts = entry->commit_ts;
        }
        if (entry->commit_ts < max_commit_ts) {
            continue;
        }
        if (entry->IsCheckPoint()) {
            continue;
        }
        ReplayWalEntry(*entry);
    }
    LOG_TRACE(Format("Last commit ts: {}", last_commit_ts));
    return last_commit_ts;
}
/*****************************************************************************
 * GC WAL FILE
 *****************************************************************************/

/**
 * @brief Gc the old wal files.
 * Only delete the wal.log.* files. the wal.log file is current wal file.
 * Check if the wal.log.* files are too old.
 * if * is little than the max_commit_ts, we will delete it.
 */
void WalManager::RecycleWalFile() {
    // Gc old wal files.
    LOG_INFO("WalManager::Checkpoint begin to gc wal files");
    if (std::filesystem::exists(wal_path_)) {
        for (const auto &entry : std::filesystem::directory_iterator(Path(wal_path_).parent_path())) {
            if (entry.is_regular_file() && entry.path().string().find("wal.log.") != std::string::npos) {
                auto suffix = entry.path().string().substr(entry.path().string().find_last_of('.') + 1);
                if (std::stoll(suffix) < ckp_commit_ts_) {
                    std::filesystem::remove(entry.path());
                    LOG_TRACE(Format("WalManager::Checkpoint delete wal file: {}", entry.path().string().c_str()));
                }
            }
        }
    }
    LOG_INFO("WalManager::Checkpoint end to gc wal files");
}
void WalManager::ReplayWalEntry(const WalEntry &entry) {
    for (const auto &cmd : entry.cmds) {
        LOG_TRACE(Format("Replay wal cmd: {}, commit ts: {}", WalManager::WalCommandTypeToString(cmd->GetType()).c_str(), entry.commit_ts));
        switch (cmd->GetType()) {
            case WalCommandType::CREATE_DATABASE:
                WalCmdCreateDatabaseReplay(*dynamic_cast<const WalCmdCreateDatabase *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::DROP_DATABASE:
                WalCmdDropDatabaseReplay(*dynamic_cast<const WalCmdDropDatabase *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::CREATE_TABLE:
                WalCmdCreateTableReplay(*dynamic_cast<const WalCmdCreateTable *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::DROP_TABLE:
                WalCmdDropTableReplay(*dynamic_cast<const WalCmdDropTable *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::ALTER_INFO:
                Error<NotImplementException>("WalCmdAlterInfo Replay Not implemented", __FILE_NAME__, __LINE__);
                break;
            case WalCommandType::CREATE_INDEX:
                WalCmdCreateIndexReplay(*dynamic_cast<const WalCmdCreateIndex *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::DROP_INDEX:
                WalCmdDropIndexReplay(*dynamic_cast<const WalCmdDropIndex *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::IMPORT:
                WalCmdImportReplay(*dynamic_cast<const WalCmdImport *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::APPEND:
                WalCmdAppendReplay(*dynamic_cast<const WalCmdAppend *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::DELETE:
                WalCmdDeleteReplay(*dynamic_cast<const WalCmdDelete *>(cmd.get()), entry.txn_id, entry.commit_ts);
                break;
            case WalCommandType::CHECKPOINT:
                break;
            default:
                Error<StorageException>("WalManager::ReplayWalEntry unknown wal command type", __FILE_NAME__, __LINE__);
        }
    }
}
void WalManager::WalCmdCreateDatabaseReplay(const WalCmdCreateDatabase &cmd, u64 txn_id, i64 commit_ts) {
    auto result = NewCatalog::CreateDatabase(storage_->catalog(), cmd.db_name, txn_id, commit_ts, storage_->txn_manager());
    if (!result.Success()) {
        Error<StorageException>("Wal Replay: Create database failed", __FILE_NAME__, __LINE__);
    }
    result.entry_->Commit(commit_ts);
}
void WalManager::WalCmdCreateTableReplay(const WalCmdCreateTable &cmd, u64 txn_id, i64 commit_ts) {
    auto db_entry_result = NewCatalog::GetDatabase(storage_->catalog(), cmd.db_name, txn_id, commit_ts);
    if (!db_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get database failed", __FILE_NAME__, __LINE__);
    }
    auto db_entry = dynamic_cast<DBEntry *>(db_entry_result.entry_);
    auto result = DBEntry::CreateTableCollection(db_entry,
                                                 TableCollectionType::kTableEntry,
                                                 cmd.table_def->table_name(),
                                                 cmd.table_def->columns(),
                                                 txn_id,
                                                 commit_ts,
                                                 nullptr);
    if (!result.Success()) {
        Error<StorageException>("Wal Replay: Create table failed", __FILE_NAME__, __LINE__);
    }
    result.entry_->Commit(commit_ts);
}

void WalManager::WalCmdDropDatabaseReplay(const WalCmdDropDatabase &cmd, u64 txn_id, i64 commit_ts) {
    auto result = NewCatalog::DropDatabase(storage_->catalog(), cmd.db_name, txn_id, commit_ts, nullptr);
    if (!result.Success()) {
        Error<StorageException>("Wal Replay: Drop database failed", __FILE_NAME__, __LINE__);
    }
    result.entry_->Commit(commit_ts);
}

void WalManager::WalCmdDropTableReplay(const WalCmdDropTable &cmd, u64 txn_id, i64 commit_ts) {
    auto db_entry_result = NewCatalog::GetDatabase(storage_->catalog(), cmd.db_name, txn_id, commit_ts);
    if (!db_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get database failed", __FILE_NAME__, __LINE__);
    }
    auto db_entry = dynamic_cast<DBEntry *>(db_entry_result.entry_);
    auto result = DBEntry::DropTableCollection(db_entry, cmd.table_name, ConflictType::kReplace, txn_id, commit_ts, nullptr);
    if (!result.Success()) {
        Error<StorageException>("Wal Replay: Drop table failed", __FILE_NAME__, __LINE__);
    }
    result.entry_->Commit(commit_ts);
}

void WalManager::WalCmdCreateIndexReplay(const WalCmdCreateIndex &cmd, u64 txn_id, i64 commit_ts) {
    auto db_entry_result = NewCatalog::GetDatabase(storage_->catalog(), cmd.db_name_, txn_id, commit_ts);
    if (!db_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get database failed", __FILE_NAME__, __LINE__);
    }
    auto db_entry = dynamic_cast<DBEntry *>(db_entry_result.entry_);
    auto table_entry_result = DBEntry::GetTableCollection(db_entry, cmd.table_name_, txn_id, commit_ts);
    if (!table_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get table failed", __FILE_NAME__, __LINE__);
    }
    auto table_entry = dynamic_cast<TableCollectionEntry *>(table_entry_result.entry_);
    auto index_def_entry_result = TableCollectionEntry::CreateIndex(table_entry, cmd.index_def_, ConflictType::kReplace, txn_id, commit_ts, nullptr);
    if (!index_def_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Create index failed", __FILE_NAME__, __LINE__);
    }
    auto fake_txn = MakeUnique<Txn>(storage_->txn_manager(), storage_->catalog(), txn_id);
    auto table_store = MakeShared<TxnTableStore>(cmd.table_name_, table_entry, fake_txn.get());

    TableCollectionEntry::CreateIndexFile(table_entry, table_store.get(), *cmd.index_def_, commit_ts, storage_->buffer_manager());
    TableCollectionEntry::CommitCreateIndex(table_entry, table_store->uncommitted_indexes_);
}

void WalManager::WalCmdDropIndexReplay(const WalCmdDropIndex &cmd, u64 txn_id, i64 commit_ts) {
    Error<NotImplementException>("WalCmdDropIndex Replay Not implemented", __FILE_NAME__, __LINE__);
}

void WalManager::WalCmdImportReplay(const WalCmdImport &cmd, u64 txn_id, i64 commit_ts) {
    // Segment entry
    auto db_entry_result = NewCatalog::GetDatabase(storage_->catalog(), cmd.db_name, txn_id, commit_ts);
    if (!db_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get database failed", __FILE_NAME__, __LINE__);
    }
    auto db_entry = dynamic_cast<DBEntry *>(db_entry_result.entry_);
    auto table_entry_result = DBEntry::GetTableCollection(db_entry, cmd.table_name, txn_id, commit_ts);
    if (!table_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get table failed", __FILE_NAME__, __LINE__);
    }
    auto table_entry = dynamic_cast<TableCollectionEntry *>(table_entry_result.entry_);

    // get the segment entries id
    auto segment_entry = SegmentEntry::MakeNewSegmentEntry(table_entry, cmd.segment_id, storage_->buffer_manager(), false);
    segment_entry->min_row_ts_ = commit_ts;
    segment_entry->max_row_ts_ = commit_ts;
    for (int id = 0; id < cmd.block_entries_size; ++id) {
        auto block_entry = MakeUnique<BlockEntry>(segment_entry.get(), id, 0, segment_entry->column_count_, storage_->buffer_manager());
        block_entry->max_row_ts_ = commit_ts;
        block_entry->min_row_ts_ = commit_ts;
        block_entry->block_version_->created_.emplace_back(commit_ts, block_entry->row_count_);
        segment_entry->block_entries_.emplace_back(std::move(block_entry));
    }
}
void WalManager::WalCmdDeleteReplay(const WalCmdDelete &cmd, u64 txn_id, i64 commit_ts) {
    Error<NotImplementException>("WalCmdDelete Replay Not implemented", __FILE_NAME__, __LINE__);
}
void WalManager::WalCmdAppendReplay(const WalCmdAppend &cmd, u64 txn_id, i64 commit_ts) {
    auto db_entry_result = NewCatalog::GetDatabase(storage_->catalog(), cmd.db_name, txn_id, commit_ts);
    if (!db_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get database failed", __FILE_NAME__, __LINE__);
    }
    auto db_entry = dynamic_cast<DBEntry *>(db_entry_result.entry_);
    auto table_entry_result = DBEntry::GetTableCollection(db_entry, cmd.table_name, txn_id, commit_ts);
    if (!table_entry_result.Success()) {
        Error<StorageException>("Wal Replay: Get table failed", __FILE_NAME__, __LINE__);
    }
    auto table_entry = dynamic_cast<TableCollectionEntry *>(table_entry_result.entry_);

    auto fake_txn = MakeUnique<Txn>(storage_->txn_manager(), storage_->catalog(), txn_id);

    auto table_store = MakeShared<TxnTableStore>(cmd.table_name, table_entry, fake_txn.get());
    table_store->Append(cmd.block);

    auto append_state = MakeUnique<AppendState>(table_store->blocks_);
    table_store->append_state_ = std::move(append_state);

    TableCollectionEntry::Append(table_store->table_entry_, table_store->txn_, table_store.get(), storage_->buffer_manager());
    TableCollectionEntry::CommitAppend(table_store->table_entry_, table_store->txn_, table_store->append_state_.get());
}

} // namespace infinity