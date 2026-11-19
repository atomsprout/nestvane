#include "ProbeLinux.hpp"

#include <sqlite3.h>

#include <cstring>
#include <limits>
#include <stdexcept>

namespace nestvane {
namespace {

constexpr std::string_view root = "/data";
constexpr uint64_t pageMask = pageSize - 1;

bool childPath(std::string_view path) {
    if (!path.starts_with("/data/") || path.size() >= 4096 || path.find('\0') != std::string_view::npos)
        return false;
    path.remove_prefix(root.size() + 1);
    for (;;) {
        const auto slash = path.find('/');
        const auto component = path.substr(0, slash);
        if (component.empty() || component.size() > 255 || component == "." || component == "..") return false;
        if (slash == std::string_view::npos) return true;
        path.remove_prefix(slash + 1);
    }
}

bool execute(sqlite3* database, const char* statement) {
    return sqlite3_exec(database, statement, nullptr, nullptr, nullptr) == SQLITE_OK;
}

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    return Statement(statement, sqlite3_finalize);
}

std::string_view columnBytes(sqlite3_stmt* cursor, int index) {
    const auto* text = static_cast<const char*>(sqlite3_column_blob(cursor, index));
    return text ? std::string_view(text, size_t(sqlite3_column_bytes(cursor, index))) : std::string_view{};
}

bool bindBytes(sqlite3_stmt* cursor, int index, std::span<const uint8_t> bytes) {
    return (bytes.empty() ? sqlite3_bind_zeroblob(cursor, index, 0) :
            sqlite3_bind_blob(cursor, index, bytes.data(), int(bytes.size()), SQLITE_TRANSIENT)) == SQLITE_OK;
}

} // namespace

ProbeLinux::~ProbeLinux() {
    if (dataDatabase) sqlite3_close(dataDatabase);
}

bool ProbeLinux::dataPath(std::string_view path) const {
    return dataDatabase && (path == root || path.starts_with("/data/"));
}

void ProbeLinux::mountData(const std::string& databasePath) {
    if (dataDatabase || databasePath.empty()) throw std::runtime_error("invalid guest data mount");
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(databasePath.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (opened != SQLITE_OK || !execute(raw,
        "PRAGMA fullfsync=ON; PRAGMA synchronous=FULL; PRAGMA busy_timeout=2000;"
        "CREATE TABLE IF NOT EXISTS state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), generation INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO state VALUES(1,0);"
        "CREATE TABLE IF NOT EXISTS entries (path TEXT PRIMARY KEY, kind INTEGER NOT NULL,"
        " mode INTEGER NOT NULL, uid INTEGER NOT NULL, gid INTEGER NOT NULL, inode INTEGER NOT NULL,"
        " bytes BLOB NOT NULL, label BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS restorecon_progress (path TEXT PRIMARY KEY, value INTEGER NOT NULL); BEGIN;"))
        throw std::runtime_error("cannot open guest data database");

    auto version = prepare(raw, "SELECT generation FROM state WHERE singleton=1");
    auto records = prepare(raw, "SELECT path,kind,mode,uid,gid,inode,bytes,label FROM entries ORDER BY path");
    if (!version || !records || sqlite3_step(version.get()) != SQLITE_ROW)
        throw std::runtime_error("cannot read guest data database");
    const auto generation = sqlite3_column_int64(version.get(), 0);
    if (generation < 0 || generation == INT64_MAX) throw std::runtime_error("invalid guest data generation");
    version.reset();

    decltype(directories) loadedDirectories{{"/data", {0771, 1000, 1000, "u:object_r:system_data_root_file:s0"}}};
    decltype(writableFiles) loadedFiles;
    decltype(links) loadedLinks;
    std::set<uint64_t> inodes;
    uint64_t bytesTotal = 0, nextPersistentInode = nextInode;
    unsigned count = 0;
    int result = 0;
    while ((result = sqlite3_step(records.get())) == SQLITE_ROW) {
        auto* row = records.get();
        const auto path = columnBytes(row, 0);
        const auto kind = sqlite3_column_int(row, 1);
        const auto mode = sqlite3_column_int64(row, 2);
        const auto uid = sqlite3_column_int64(row, 3), gid = sqlite3_column_int64(row, 4);
        const auto inode = sqlite3_column_int64(row, 5);
        const auto bytes = columnBytes(row, 6), label = columnBytes(row, 7);
        if (++count > 4096 || (path != root && !childPath(path)) || (path == root && kind != 0) ||
            mode < 0 || mode > 07777 || uid < 0 || uid > UINT32_MAX || gid < 0 || gid > UINT32_MAX ||
            label.size() > 4096 ||
            (path != root && !loadedDirectories.contains(path.substr(0, path.rfind('/')))))
            throw std::runtime_error("invalid guest data entry");
        if (kind == 0 && bytes.empty() && label.find('\0') == std::string_view::npos) {
            auto context = label;
            if (context.empty() && path == root) context = loadedDirectories.at("/data").securityContext;
            loadedDirectories.insert_or_assign(std::string(path),
                Directory{uint32_t(mode), uint32_t(uid), uint32_t(gid), std::string(context)});
        } else if (kind == 1 && inode >= 65536 && inode < INT64_MAX - 4096 && bytes.size() <= maxDataFileBytes &&
                   inodes.insert(inode).second) {
            const auto allocation = (bytes.size() + pageMask) & ~pageMask;
            bytesTotal += allocation;
            if (bytesTotal > maxWritableBytes) throw std::runtime_error("guest data capacity exceeded");
            auto file = std::make_shared<WritableFile>();
            file->inode = inode;
            file->mode = mode;
            file->uid = uid;
            file->gid = gid;
            file->persistent = true;
            file->storage->size = bytes.size();
            file->storage->pages.resize(allocation);
            if (!bytes.empty()) std::memcpy(file->storage->pages.data(), bytes.data(), bytes.size());
            if (!label.empty()) file->attributes.emplace("security.selinux",
                                                        std::vector<uint8_t>(label.begin(), label.end()));
            loadedFiles.emplace(path, std::move(file));
            nextPersistentInode = std::max(nextPersistentInode, uint64_t(inode) + 1);
        } else if (kind == 2 && !bytes.empty() && label.empty() && bytes.size() < 4096 &&
                   bytes.find('\0') == std::string_view::npos) {
            loadedLinks.emplace(path, bytes);
        } else {
            throw std::runtime_error("invalid guest data record");
        }
    }
    records.reset();
    if (result != SQLITE_DONE) throw std::runtime_error("cannot read guest data entries");
    auto markers = prepare(raw, "SELECT path,value FROM restorecon_progress");
    if (!markers) throw std::runtime_error("cannot read guest restorecon progress");
    while ((result = sqlite3_step(markers.get())) == SQLITE_ROW) {
        const auto path = columnBytes(markers.get(), 0);
        const auto node = loadedDirectories.find(path);
        const auto value = sqlite3_column_int64(markers.get(), 1);
        if (node == loadedDirectories.end() || sqlite3_column_type(markers.get(), 1) != SQLITE_INTEGER ||
            value < 0 || value > UINT8_MAX)
            throw std::runtime_error("invalid guest restorecon progress");
        node->second.restoreconInProgress = uint8_t(value);
    }
    markers.reset();
    if (result != SQLITE_DONE || !execute(raw, "COMMIT") || directories.contains("/data") ||
        current->mountNamespace->mounts.contains("/data"))
        throw std::runtime_error("cannot mount guest data database");
    directories.merge(loadedDirectories);
    writableFiles.merge(loadedFiles);
    links.merge(loadedLinks);
    nextInode = nextPersistentInode;
    dataGeneration = generation;
    auto& group = *current->mountNamespace;
    const auto& directory = directories.at("/data");
    auto& view = group.mounts.emplace("/data", MountView{6, directory.mode, directory.gid,
                                                         group.rootPropagation, "/data"}).first->second;
    view.hostData = true;
    dataDatabase = database.release();
    ++group.generation;
}

bool ProbeLinux::syncData() {
    if (!dataDatabase) return true;
    auto* database = dataDatabase;
    if (!execute(database, "BEGIN IMMEDIATE")) return false;
    const auto write = [&] {
        auto check = prepare(database, "SELECT generation FROM state WHERE singleton=1");
        if (!check || sqlite3_step(check.get()) != SQLITE_ROW ||
            sqlite3_column_int64(check.get(), 0) != dataGeneration || dataGeneration == INT64_MAX - 1)
            return false;
        check.reset();
        if (!execute(database, "DELETE FROM entries; DELETE FROM restorecon_progress;")) return false;
        auto insert = prepare(database, "INSERT INTO entries VALUES(?,?,?,?,?,?,?,?)");
        auto marker = prepare(database, "INSERT INTO restorecon_progress VALUES(?,?)");
        if (!insert || !marker) return false;
        const auto row = [&](const std::string& path, int kind, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t inode,
                             std::span<const uint8_t> bytes = {}, std::span<const uint8_t> label = {}) {
            auto* cursor = insert.get();
            return sqlite3_reset(cursor) == SQLITE_OK &&
                sqlite3_bind_text(cursor, 1, path.data(), int(path.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_int(cursor, 2, kind) == SQLITE_OK &&
                sqlite3_bind_int64(cursor, 3, mode) == SQLITE_OK &&
                sqlite3_bind_int64(cursor, 4, uid) == SQLITE_OK &&
                sqlite3_bind_int64(cursor, 5, gid) == SQLITE_OK &&
                sqlite3_bind_int64(cursor, 6, inode) == SQLITE_OK &&
                bindBytes(cursor, 7, bytes) && bindBytes(cursor, 8, label) &&
                sqlite3_step(cursor) == SQLITE_DONE;
        };
        for (const auto& [path, directory] : directories) {
            if (!dataPath(path)) continue;
            const auto& context = directory.securityContext;
            if (!row(path, 0, directory.mode, directory.uid, directory.gid, 0, {},
                     {reinterpret_cast<const uint8_t*>(context.data()), context.size()})) return false;
            if (directory.restoreconInProgress &&
                (sqlite3_reset(marker.get()) != SQLITE_OK ||
                 sqlite3_bind_text(marker.get(), 1, path.data(), int(path.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
                 sqlite3_bind_int(marker.get(), 2, *directory.restoreconInProgress) != SQLITE_OK ||
                 sqlite3_step(marker.get()) != SQLITE_DONE)) return false;
        }
        for (const auto& [path, file] : writableFiles) {
            if (!dataPath(path)) continue;
            const auto attribute = file->attributes.find("security.selinux");
            const auto label = attribute == file->attributes.end() ? std::span<const uint8_t>{} :
                               std::span<const uint8_t>(attribute->second);
            if (!row(path, 1, file->mode, file->uid, file->gid, file->inode,
                     std::span(file->storage->pages).first(file->storage->size), label)) return false;
        }
        for (const auto& [path, target] : links) {
            if (dataPath(path) && !row(path, 2, 0, 0, 0, 0,
                                     {reinterpret_cast<const uint8_t*>(target.data()), target.size()})) return false;
        }
        return execute(database, "UPDATE state SET generation=generation+1 WHERE singleton=1");
    };
    if (!write() || !execute(database, "COMMIT")) {
        execute(database, "ROLLBACK");
        return false;
    }
    ++dataGeneration;
    return true;
}

} // namespace nestvane
