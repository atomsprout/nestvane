#include "ProbeLinux.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <limits>
#include <stdexcept>

namespace nestvane {
namespace {

constexpr std::string_view root = "/data";

bool inside(std::string_view path, std::string_view parent) {
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
}
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
    if (graphicsDma) graphicsDma->stop();
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
        "PRAGMA fullfsync=ON; PRAGMA synchronous=FULL; PRAGMA busy_timeout=2000; PRAGMA temp_store=MEMORY;"
        "CREATE TEMP TABLE live_entries (path TEXT PRIMARY KEY) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), generation INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO state VALUES(1,0);"
        "CREATE TABLE IF NOT EXISTS entries (path TEXT PRIMARY KEY, kind INTEGER NOT NULL,"
        " mode INTEGER NOT NULL, uid INTEGER NOT NULL, gid INTEGER NOT NULL, inode INTEGER NOT NULL,"
        " bytes BLOB NOT NULL, label BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS restorecon_progress (path TEXT PRIMARY KEY, value INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS directory_user_attributes (path TEXT NOT NULL, name TEXT NOT NULL,"
        " value BLOB NOT NULL, PRIMARY KEY(path,name));"
        "CREATE TABLE IF NOT EXISTS file_user_attributes (path TEXT NOT NULL, name TEXT NOT NULL,"
        " value BLOB NOT NULL, PRIMARY KEY(path,name));"
        "CREATE TABLE IF NOT EXISTS inode_acls (path TEXT NOT NULL, defaults INTEGER NOT NULL,"
        " value BLOB NOT NULL, PRIMARY KEY(path,defaults));"
        "CREATE TABLE IF NOT EXISTS inode_times (path TEXT PRIMARY KEY, a_sec INTEGER NOT NULL, a_ns INTEGER NOT NULL,"
        " m_sec INTEGER NOT NULL, m_ns INTEGER NOT NULL, c_sec INTEGER NOT NULL, c_ns INTEGER NOT NULL); BEGIN;"))
        throw std::runtime_error("cannot open guest data database");

    std::string filesDirectory;
    if (databasePath != ":memory:" && !databasePath.starts_with("file:")) {
        filesDirectory = databasePath + ".files";
        if (mkdir(filesDirectory.c_str(), 0700) && errno != EEXIST) filesDirectory.clear();
    }
    // What an earlier guest in this process left to remove is settled below, against the records.
    if (!filesDirectory.empty()) SharedBuffer::takeRetired(filesDirectory + "/");
    std::set<std::string> externalNames;
    // The shared storage is what its host directory has. Without the directory this is the first
    // time, and what the database has of the shared storage moves there.
    struct stat hostStatus{};
    const bool sharedFresh = !sharedHostRoot.empty() && ::stat(sharedHostRoot.c_str(), &hostStatus) != 0;
    if (sharedFresh) {
        std::error_code ignored;
        std::filesystem::create_directories(sharedHostRoot, ignored);
    }
    // A record of the shared storage whose file or directory the host no longer has is passed over.
    const auto shared = [&](std::string_view path) { return !sharedHostRoot.empty() && inside(path, sharedGuestRoot); };
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
    // What is already stored is loaded whatever is free now: only more than the whole volume is wrong.
    const uint64_t maxWritableBytes = std::max(maxTransientBytes, hostVolume().total);
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
        if (shared(path) && !loadedDirectories.contains(path.substr(0, path.rfind('/')))) continue;
        if (shared(path) && kind == 0 && !sharedFresh &&
            (::stat(sharedHost(path).c_str(), &hostStatus) || !S_ISDIR(hostStatus.st_mode))) continue;
        if (++count > maxDataRecords || (path != root && !childPath(path)) || (path == root && kind != 0) ||
            mode < 0 || mode > 07777 || uid < 0 || uid > UINT32_MAX || gid < 0 || gid > UINT32_MAX ||
            label.size() > 4096 ||
            (path != root && !loadedDirectories.contains(path.substr(0, path.rfind('/')))))
            throw std::runtime_error("invalid guest data entry");
        if (kind == 0 && bytes.empty() && label.find('\0') == std::string_view::npos) {
            auto context = label;
            if (context.empty() && path == root) context = loadedDirectories.at("/data").securityContext;
            loadedDirectories.insert_or_assign(std::string(path),
                Directory{uint32_t(mode), uint32_t(uid), uint32_t(gid), std::string(context)});
            if (const auto host = sharedHost(path); !host.empty()) ::mkdir(host.c_str(), 0755);
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
            if (file->storage->placedMoves != file->storage->pages.moves) file->storage->replace();
            if (!bytes.empty()) std::memcpy(file->storage->pages.data(), bytes.data(), bytes.size());
            if (!label.empty()) file->attributes.emplace("security.selinux",
                                                        std::vector<uint8_t>(label.begin(), label.end()));
            // A large file that an earlier version kept in the database moves out of it.
            dataFilesDirectory = filesDirectory;
            if (shared(path)) {
                file->storage->pages.makeExact(sharedHost(path), bytes.size());
            } else if (!filesDirectory.empty() && bytes.size() >= externalFileBytes &&
                file->storage->pages.backWith(externalFile(uint64_t(inode)))) {
                externalNames.insert(std::to_string(inode));
                compactData = true;
            }
            dataFilesDirectory.clear();
            loadedFiles.emplace(path, std::move(file));
            nextPersistentInode = std::max(nextPersistentInode, uint64_t(inode) + 1);
        } else if (kind == 3 && inode >= 65536 && inode < INT64_MAX - 4096 && bytes.size() == sizeof(uint64_t) &&
                   !filesDirectory.empty() && inodes.insert(inode).second) {
            // A regular file whose content is in a host file: the record has its size.
            uint64_t size;
            std::memcpy(&size, bytes.data(), sizeof(size));
            if (size > maxDataFileBytes) throw std::runtime_error("invalid guest data record");
            const auto allocation = (size + pageMask) & ~pageMask;
            bytesTotal += allocation;
            if (bytesTotal > maxWritableBytes) throw std::runtime_error("guest data capacity exceeded");
            auto file = std::make_shared<WritableFile>();
            file->inode = inode;
            file->mode = mode;
            file->uid = uid;
            file->gid = gid;
            file->persistent = true;
            file->storage->size = size;
            file->storage->pages.attach(filesDirectory + "/" + std::to_string(inode), allocation);
            if (!shared(path) || !file->storage->pages.makeExact(sharedHost(path), size))
                externalNames.insert(std::to_string(inode));
            if (!label.empty()) file->attributes.emplace("security.selinux",
                                                        std::vector<uint8_t>(label.begin(), label.end()));
            loadedFiles.emplace(path, std::move(file));
            nextPersistentInode = std::max(nextPersistentInode, uint64_t(inode) + 1);
        } else if (kind == 4 && shared(path) && bytes.empty() && inode >= 65536 && inode < INT64_MAX - 4096 &&
                   !inodes.contains(inode)) {
            // A file of the shared storage: the host file has the content and says how long it is.
            const auto host = sharedHost(path);
            if (::stat(host.c_str(), &hostStatus) || !S_ISREG(hostStatus.st_mode) ||
                uint64_t(hostStatus.st_size) > maxDataFileBytes) continue;
            const uint64_t size = uint64_t(hostStatus.st_size);
            const auto allocation = (size + pageMask) & ~pageMask;
            if (bytesTotal + allocation > maxWritableBytes) continue;
            bytesTotal += allocation;
            inodes.insert(inode);
            auto file = std::make_shared<WritableFile>();
            file->inode = inode;
            file->mode = mode;
            file->uid = uid;
            file->gid = gid;
            file->persistent = true;
            file->storage->size = size;
            file->storage->pages.attachExact(host, allocation);
            if (!label.empty()) file->attributes.emplace("security.selinux",
                                                        std::vector<uint8_t>(label.begin(), label.end()));
            loadedFiles.emplace(path, std::move(file));
            nextPersistentInode = std::max(nextPersistentInode, uint64_t(inode) + 1);
        } else if (kind == 2 && !bytes.empty() && bytes.size() < 4096 &&
                   bytes.find('\0') == std::string_view::npos && label.find('\0') == std::string_view::npos) {
            loadedLinks.emplace(path, Link{std::string(bytes), std::string(label)});
        } else {
            throw std::runtime_error("invalid guest data record: " + std::string(path) + ", kind " + std::to_string(kind) +
                                     ", " + std::to_string(bytes.size()) + " bytes");
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
        if (node == loadedDirectories.end() && shared(path)) continue;
        if (node == loadedDirectories.end() || sqlite3_column_type(markers.get(), 1) != SQLITE_INTEGER ||
            value < 0 || value > UINT8_MAX)
            throw std::runtime_error("invalid guest restorecon progress");
        node->second.restoreconInProgress = uint8_t(value);
    }
    markers.reset();
    if (result != SQLITE_DONE) throw std::runtime_error("cannot read guest restorecon entries");
    auto attributes = prepare(raw, "SELECT path,name,value,0 FROM directory_user_attributes"
                                   " UNION ALL SELECT path,name,value,1 FROM file_user_attributes");
    if (!attributes) throw std::runtime_error("cannot read guest user attributes");
    while ((result = sqlite3_step(attributes.get())) == SQLITE_ROW) {
        const auto path = columnBytes(attributes.get(), 0);
        const auto name = columnBytes(attributes.get(), 1), value = columnBytes(attributes.get(), 2);
        decltype(Directory::userAttributes)* inodeAttributes = nullptr;
        if (sqlite3_column_int(attributes.get(), 3)) {
            if (const auto file = loadedFiles.find(path); file != loadedFiles.end())
                inodeAttributes = &file->second->userAttributes;
        } else if (const auto directory = loadedDirectories.find(path); directory != loadedDirectories.end()) {
            inodeAttributes = &directory->second.userAttributes;
        }
        if (!inodeAttributes && shared(path)) continue;
        if (!inodeAttributes || !name.starts_with("user.") || name.size() <= 5 ||
            name.size() > 255 || name.find('\0') != std::string_view::npos ||
            name == "user.restorecon_in_progress" || value.size() > Directory::maxUserAttributeValue ||
            inodeAttributes->size() >= Directory::maxUserAttributes)
            throw std::runtime_error("invalid guest user attribute");
        std::vector<uint8_t> bytes;
        if (!value.empty()) bytes.assign(value.begin(), value.end());
        inodeAttributes->emplace(std::string(name), std::move(bytes));
    }
    attributes.reset();
    if (result != SQLITE_DONE) throw std::runtime_error("cannot read guest user attribute entries");
    auto acls = prepare(raw, "SELECT path,defaults,value FROM inode_acls");
    if (!acls) throw std::runtime_error("cannot read guest inode ACLs");
    while ((result = sqlite3_step(acls.get())) == SQLITE_ROW) {
        const auto path = columnBytes(acls.get(), 0), value = columnBytes(acls.get(), 2);
        const auto defaults = sqlite3_column_int64(acls.get(), 1);
        auto decoded = PosixAcl::decode({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
        if (!decoded || decoded->empty() || (defaults != 0 && defaults != 1) ||
            sqlite3_column_type(acls.get(), 1) != SQLITE_INTEGER)
            throw std::runtime_error("invalid guest inode ACL");
        if (const auto dir = loadedDirectories.find(path); dir != loadedDirectories.end()) {
            (defaults ? dir->second.defaultAcl : dir->second.accessAcl) = std::move(*decoded);
        } else if (const auto file = loadedFiles.find(path); !defaults && file != loadedFiles.end()) {
            file->second->accessAcl = std::move(*decoded);
        } else if (!shared(path)) {
            throw std::runtime_error("ACL for missing or incompatible guest inode");
        }
    }
    acls.reset();
    if (result != SQLITE_DONE) throw std::runtime_error("cannot read guest inode ACL entries");
    auto inodeTimes = prepare(raw, "SELECT path,a_sec,a_ns,m_sec,m_ns,c_sec,c_ns FROM inode_times");
    if (!inodeTimes) throw std::runtime_error("cannot read guest inode times");
    while ((result = sqlite3_step(inodeTimes.get())) == SQLITE_ROW) {
        const auto path = columnBytes(inodeTimes.get(), 0);
        GuestInodeTimes times{};
        for (unsigned i = 0; i < times.size(); ++i) {
            if (sqlite3_column_type(inodeTimes.get(), i + 1) != SQLITE_INTEGER)
                throw std::runtime_error("invalid guest inode timestamp");
            times[i] = sqlite3_column_int64(inodeTimes.get(), i + 1);
            if ((i & 1) && (times[i] < 0 || times[i] >= 1'000'000'000))
                throw std::runtime_error("invalid guest inode timestamp");
        }
        if (const auto dir = loadedDirectories.find(path); dir != loadedDirectories.end())
            dir->second.times = times;
        else if (const auto file = loadedFiles.find(path); file != loadedFiles.end())
            file->second->times = times;
        else if (!shared(path))
            throw std::runtime_error("timestamp for missing guest inode");
    }
    inodeTimes.reset();
    if (result != SQLITE_DONE || !execute(raw, "COMMIT") || directories.contains("/data") ||
        current->mountNamespace->mounts.contains("/data"))
        throw std::runtime_error("cannot mount guest data database");
    // What the host directory has that no record names was put there from outside: it belongs to
    // whoever owns the directory it is in. Names that begin with a dot are the host's own.
    if (const auto top = loadedDirectories.find(sharedGuestRoot); !sharedHostRoot.empty() && top != loadedDirectories.end()) {
        std::error_code failed;
        for (auto entry = std::filesystem::recursive_directory_iterator(
                 sharedHostRoot, std::filesystem::directory_options::skip_permission_denied, failed);
             !failed && entry != std::filesystem::recursive_directory_iterator(); entry.increment(failed)) {
            const auto guest = std::string(sharedGuestRoot) + entry->path().string().substr(sharedHostRoot.size());
            const bool directory = entry->is_directory(failed), regular = !directory && entry->is_regular_file(failed);
            const bool known = loadedDirectories.contains(guest) || loadedFiles.contains(guest) || loadedLinks.contains(guest);
            const auto owner = loadedDirectories.find(guest.substr(0, guest.rfind('/')));
            if (known) continue;
            if (entry->path().filename().string().starts_with(".") || !childPath(guest) || owner == loadedDirectories.end() ||
                ++count > maxDataRecords || (!directory && !regular)) {
                if (directory) entry.disable_recursion_pending();
                continue;
            }
            const auto& parent = owner->second;
            if (directory) {
                loadedDirectories.emplace(guest, Directory{parent.mode, parent.uid, parent.gid, parent.securityContext});
                continue;
            }
            const uint64_t size = entry->file_size(failed);
            const auto allocation = (size + pageMask) & ~pageMask;
            if (failed || size > maxDataFileBytes || bytesTotal + allocation > maxWritableBytes) {
                failed.clear();
                continue;
            }
            bytesTotal += allocation;
            auto file = std::make_shared<WritableFile>();
            file->inode = nextPersistentInode++;
            file->mode = 0660;
            file->uid = parent.uid;
            file->gid = parent.gid;
            file->persistent = true;
            file->storage->size = size;
            file->storage->pages.attachExact(entry->path().string(), allocation);
            if (::stat(entry->path().c_str(), &hostStatus) == 0)
                file->times = {hostStatus.st_atimespec.tv_sec, hostStatus.st_atimespec.tv_nsec, hostStatus.st_mtimespec.tv_sec,
                               hostStatus.st_mtimespec.tv_nsec, hostStatus.st_ctimespec.tv_sec, hostStatus.st_ctimespec.tv_nsec};
            const auto& context = parent.securityContext;
            if (!context.empty()) file->attributes["security.selinux"].assign(context.c_str(), context.c_str() + context.size() + 1);
            loadedFiles.emplace(guest, std::move(file));
        }
    }
    // Host files that no record names were written after the last sync, or belong to files deleted since.
    if (!filesDirectory.empty()) {
        if (DIR* listing = opendir(filesDirectory.c_str())) {
            while (const dirent* entry = readdir(listing)) {
                const std::string name = entry->d_name;
                if (name != "." && name != ".." && !externalNames.contains(name)) unlink((filesDirectory + "/" + name).c_str());
            }
            closedir(listing);
        }
    }
    dataFilesDirectory = filesDirectory;
    directories.merge(loadedDirectories);
    writableFiles.merge(loadedFiles);
    links.merge(loadedLinks);
    nextInode = nextPersistentInode;
    dataGeneration = generation;
    auto& group = *current->mountNamespace;
    const auto& directory = directories.at("/data");
    auto& view = group.mounts.emplace("/data", MountView{6, directory.mode, directory.gid,
                                                         0, "/data"}).first->second;
    setMountPropagation(view, group.rootPropagation);
    view.hostData = true;
    dataDatabase = database.release();
    ++group.generation;
}

bool ProbeLinux::syncData() {
    if (!dataDatabase) return true;
    auto* database = dataDatabase;
    if (!execute(database, "BEGIN IMMEDIATE")) return false;
    std::set<std::string_view> externalFiles;
    const auto write = [&] {
        auto check = prepare(database, "SELECT generation FROM state WHERE singleton=1");
        if (!check || sqlite3_step(check.get()) != SQLITE_ROW ||
            sqlite3_column_int64(check.get(), 0) != dataGeneration || dataGeneration == INT64_MAX - 1)
            return false;
        check.reset();
        if (!execute(database, "DELETE FROM live_entries; DELETE FROM restorecon_progress; DELETE FROM inode_times;"
                               "DELETE FROM directory_user_attributes; DELETE FROM file_user_attributes;"
                               "DELETE FROM inode_acls;"))
            return false;
        // Avoid rewriting unchanged file payloads on every guest fsync. Compare
        // the bytes as well as metadata because a shared mapping can write them.
        auto insert = prepare(database,
            "INSERT INTO entries VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET "
            "kind=excluded.kind, mode=excluded.mode, uid=excluded.uid, gid=excluded.gid, "
            "inode=excluded.inode, bytes=excluded.bytes, label=excluded.label "
            "WHERE (entries.kind, entries.mode, entries.uid, entries.gid, entries.inode, entries.bytes, entries.label) "
            "IS NOT (excluded.kind, excluded.mode, excluded.uid, excluded.gid, excluded.inode, excluded.bytes, excluded.label)");
        auto seen = prepare(database, "INSERT INTO live_entries VALUES(?)");
        auto marker = prepare(database, "INSERT INTO restorecon_progress VALUES(?,?)");
        auto inodeTimes = prepare(database, "INSERT INTO inode_times VALUES(?,?,?,?,?,?,?)");
        auto directoryAttributes = prepare(database, "INSERT INTO directory_user_attributes VALUES(?,?,?)");
        auto fileAttributes = prepare(database, "INSERT INTO file_user_attributes VALUES(?,?,?)");
        auto acls = prepare(database, "INSERT INTO inode_acls VALUES(?,?,?)");
        if (!insert || !seen || !marker || !inodeTimes || !directoryAttributes || !fileAttributes || !acls)
            return false;
        const auto writeAttributes = [&](sqlite3_stmt* cursor, const std::string& path, const auto& attributes) {
            for (const auto& [name, value] : attributes) {
                if (sqlite3_reset(cursor) != SQLITE_OK ||
                    sqlite3_bind_text(cursor, 1, path.data(), int(path.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(cursor, 2, name.data(), int(name.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
                    !bindBytes(cursor, 3, value) || sqlite3_step(cursor) != SQLITE_DONE) return false;
            }
            return true;
        };
        const auto writeAcl = [&](const std::string& path, bool defaults, const PosixAcl& acl) {
            if (acl.empty()) return true;
            const auto value = acl.encode();
            auto* cursor = acls.get();
            return sqlite3_reset(cursor) == SQLITE_OK &&
                sqlite3_bind_text(cursor, 1, path.data(), int(path.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_int(cursor, 2, defaults) == SQLITE_OK &&
                bindBytes(cursor, 3, value) && sqlite3_step(cursor) == SQLITE_DONE;
        };
        const auto writeTimes = [&](const std::string& path, const GuestInodeTimes& times) {
            if (times == GuestInodeTimes{}) return true;
            auto* cursor = inodeTimes.get();
            if (sqlite3_reset(cursor) != SQLITE_OK ||
                sqlite3_bind_text(cursor, 1, path.data(), int(path.size()), SQLITE_TRANSIENT) != SQLITE_OK)
                return false;
            for (unsigned i = 0; i < times.size(); ++i)
                if (sqlite3_bind_int64(cursor, i + 2, times[i]) != SQLITE_OK) return false;
            return sqlite3_step(cursor) == SQLITE_DONE;
        };
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
                sqlite3_step(cursor) == SQLITE_DONE &&
                sqlite3_reset(seen.get()) == SQLITE_OK &&
                sqlite3_bind_text(seen.get(), 1, path.data(), int(path.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_step(seen.get()) == SQLITE_DONE;
        };
        for (const auto& [path, directory] : directories) {
            if (!dataPath(path)) continue;
            const auto& context = directory.securityContext;
            if (!row(path, 0, directory.mode, directory.uid, directory.gid, 0, {},
                     {reinterpret_cast<const uint8_t*>(context.data()), context.size()}) ||
                !writeTimes(path, directory.times) || !writeAcl(path, false, directory.accessAcl) ||
                !writeAcl(path, true, directory.defaultAcl)) return false;
            if (directory.restoreconInProgress &&
                (sqlite3_reset(marker.get()) != SQLITE_OK ||
                 sqlite3_bind_text(marker.get(), 1, path.data(), int(path.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
                 sqlite3_bind_int(marker.get(), 2, *directory.restoreconInProgress) != SQLITE_OK ||
                 sqlite3_step(marker.get()) != SQLITE_DONE)) return false;
            if (!writeAttributes(directoryAttributes.get(), path, directory.userAttributes)) return false;
        }
        for (const auto& [path, file] : writableFiles) {
            if (!dataPath(path)) continue;
            const auto attribute = file->attributes.find("security.selinux");
            const auto label = attribute == file->attributes.end() ? std::span<const uint8_t>{} :
                               std::span<const uint8_t>(attribute->second);
            const uint64_t size = file->storage->size;
            const bool exact = file->storage->pages.exact();
            const bool external = file->storage->pages.backed() && !exact;
            if (external) externalFiles.insert(file->storage->pages.backingFile());
            const auto content = exact ? std::span<const uint8_t>{} :
                                 external ? std::span(reinterpret_cast<const uint8_t*>(&size), sizeof(size)) :
                                 std::span<const uint8_t>(file->storage->pages).first(size);
            if (!row(path, exact ? 4 : external ? 3 : 1, file->mode, file->uid, file->gid, file->inode, content, label) ||
                !writeTimes(path, file->times) || !writeAcl(path, false, file->accessAcl) ||
                !writeAttributes(fileAttributes.get(), path, file->userAttributes)) return false;
        }
        for (const auto& [path, link] : links) {
            if (dataPath(path) && !row(path, 2, 0, 0, 0, 0,
                                     {reinterpret_cast<const uint8_t*>(link.target.data()), link.target.size()},
                                     {reinterpret_cast<const uint8_t*>(link.securityContext.data()),
                                      link.securityContext.size()})) return false;
        }
        return execute(database, "DELETE FROM entries WHERE path NOT IN (SELECT path FROM live_entries);"
                                 "UPDATE state SET generation=generation+1 WHERE singleton=1");
    };
    if (!write() || !execute(database, "COMMIT")) {
        execute(database, "ROLLBACK");
        return false;
    }
    ++dataGeneration;
    // The files of large files that are gone: no record names them any more.
    if (!dataFilesDirectory.empty())
        for (const auto& path : SharedBuffer::takeRetired(dataFilesDirectory + "/"))
            if (!externalFiles.contains(path)) unlink(path.c_str());
    if (std::exchange(compactData, false)) execute(database, "VACUUM");
    return true;
}

void ProbeLinux::rescanShared() {
    // inotify's masks, as in ProbeLinux.cpp.
    constexpr uint32_t fileClosedWrite = 8, fileCreated = 256, fileDeleted = 512;
    if (sharedHostRoot.empty() || !directories.contains(sharedGuestRoot)) return;
    std::vector<std::string> changedDirectories;
    // A directory by its host and guest paths, then its subdirectories.
    std::function<void(const std::string&, const std::string&)> visit = [&](const std::string& host, const std::string& guest) {
        struct stat status{};
        if (::stat(host.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || !directories.contains(guest)) return;
        const std::pair<int64_t, int64_t> time{status.st_mtimespec.tv_sec, status.st_mtimespec.tv_nsec};
        auto& seen = sharedListedTimes[host];
        if (seen != time) {
            // The first look only notes the time: the tables were made from the host when the data was mounted.
            const bool first = seen == std::pair<int64_t, int64_t>{};
            seen = time;
            std::set<std::string> names;
            bool changed = false;
            if (DIR* listing = opendir(host.c_str())) {
                while (const dirent* entry = readdir(listing)) {
                    const std::string name = entry->d_name;
                    if (name.starts_with(".")) continue;  // ".", "..", and the host's own files
                    names.insert(name);
                    if (first) continue;
                    const auto childHost = host + "/" + name, childGuest = guest + "/" + name;
                    if (directories.contains(childGuest) || writableFiles.contains(childGuest) || links.contains(childGuest))
                        continue;
                    struct stat child{};
                    if (::lstat(childHost.c_str(), &child) != 0 || !childPath(childGuest)) continue;
                    const auto parent = directories.at(guest);
                    if (S_ISDIR(child.st_mode)) {
                        if (directories.size() >= maxGuestDirectories) continue;
                        directories.emplace(childGuest, Directory{parent.mode, parent.uid, parent.gid, parent.securityContext});
                        sharedListedTimes[childHost] = {-1, -1};  // new: what is in it is listed in this same look
                        fileEvent(childGuest, fileCreated, true);
                        changed = true;
                        continue;
                    }
                    if (!S_ISREG(child.st_mode) || uint64_t(child.st_size) > maxDataFileBytes ||
                        writableFiles.size() >= maxPersistentFiles) continue;
                    auto file = std::make_shared<WritableFile>();
                    file->inode = nextInode++;
                    file->mode = 0660;
                    file->uid = parent.uid;
                    file->gid = parent.gid;
                    file->persistent = true;
                    file->storage->size = uint64_t(child.st_size);
                    file->storage->pages.attachExact(childHost, (uint64_t(child.st_size) + pageMask) & ~pageMask);
                    file->times = {child.st_atimespec.tv_sec, child.st_atimespec.tv_nsec, child.st_mtimespec.tv_sec,
                                   child.st_mtimespec.tv_nsec, child.st_ctimespec.tv_sec, child.st_ctimespec.tv_nsec};
                    const auto& context = parent.securityContext;
                    if (!context.empty()) file->attributes["security.selinux"].assign(context.c_str(), context.c_str() + context.size() + 1);
                    writableFiles.emplace(childGuest, std::move(file));
                    fileEvent(childGuest, fileCreated);
                    fileEvent(childGuest, fileClosedWrite);
                    changed = true;
                }
                closedir(listing);
            }
            // What the guest has here that the host no longer has was removed from outside. Only files
            // the host holds (written through to it) go: a file the host could not take stays in the
            // database and was never on the host.
            if (!first) {
                const auto prefix = guest + "/";
                const auto here = [&](const std::string& path) {
                    const auto name = std::string_view(path).substr(prefix.size());
                    return name.find('/') == std::string_view::npos && !name.starts_with(".") && !names.contains(std::string(name));
                };
                std::vector<std::string> gone;
                for (auto it = writableFiles.lower_bound(prefix); it != writableFiles.end() && it->first.starts_with(prefix); ++it)
                    if (here(it->first) && it->second->persistent && it->second->storage->pages.exact()) gone.push_back(it->first);
                for (const auto& path : gone) {
                    writableFiles.erase(path);
                    fileEvent(path, fileDeleted);
                    changed = true;
                }
                // A directory gone from the host goes with everything in it the host held; one that
                // still has a file the host never had stays, with that file.
                gone.clear();
                for (auto it = directories.lower_bound(prefix); it != directories.end() && it->first.starts_with(prefix); ++it)
                    if (here(it->first)) gone.push_back(it->first);
                for (const auto& top : gone) {
                    const auto inner = top + "/";
                    std::vector<std::string> files, folders;
                    for (auto it = writableFiles.lower_bound(inner); it != writableFiles.end() && it->first.starts_with(inner); ++it)
                        if (it->second->persistent && it->second->storage->pages.exact()) files.push_back(it->first);
                    for (const auto& path : files) {
                        writableFiles.erase(path);
                        fileEvent(path, fileDeleted);
                    }
                    for (auto it = directories.lower_bound(inner); it != directories.end() && it->first.starts_with(inner); ++it)
                        folders.push_back(it->first);
                    folders.push_back(top);
                    // Deepest first, each only when nothing is left in it.
                    std::sort(folders.begin(), folders.end(), std::greater<>());
                    for (const auto& folder : folders) {
                        const auto within = folder + "/";
                        const auto file = writableFiles.lower_bound(within);
                        const auto directory = directories.lower_bound(within);
                        const auto link = links.lower_bound(within);
                        if ((file != writableFiles.end() && file->first.starts_with(within)) ||
                            (directory != directories.end() && directory->first.starts_with(within)) ||
                            (link != links.end() && link->first.starts_with(within))) continue;
                        directories.erase(folder);
                        sharedListedTimes.erase(sharedHost(folder));
                        fileEvent(folder, fileDeleted, true);
                    }
                    changed = true;
                }
            }
            if (changed) changedDirectories.push_back(guest);
        }
        std::vector<std::string> subdirectories;
        const auto prefix = guest + "/";
        for (auto it = directories.lower_bound(prefix); it != directories.end() && it->first.starts_with(prefix); ++it)
            if (it->first.find('/', prefix.size()) == std::string::npos) subdirectories.push_back(it->first);
        for (const auto& child : subdirectories) {
            // Android/ is the apps' own, their data and obb directories: nobody puts files there.
            if (guest == sharedGuestRoot && child == guest + "/Android") continue;
            visit(sharedHost(child), child);
        }
    };
    visit(sharedHostRoot, std::string(sharedGuestRoot));
    if (changedDirectories.empty()) return;
    wakePollers();
    syncDataSoon();
    if (apkInbox)
        for (const auto& directory : changedDirectories)
            apkInbox->submitLine("scan", "/storage/emulated/0" + directory.substr(sharedGuestRoot.size()));
}

} // namespace nestvane
