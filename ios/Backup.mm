#include "Backup.hpp"

#include <AppleArchive/AppleArchive.h>
#include <sqlite3.h>

#include <fcntl.h>
#include <string_view>

namespace nestvane::backup {
namespace {

// Inside the archive, beside the data: what made it, when, and the settings that go with it.
NSString* const InfoName = @"nestvane-backup.plist";
constexpr NSInteger Format = 1;

NSURL* supportDirectory() {
    return [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
}

// What Android had before a restore, until Android has started from the backup.
NSURL* previousDirectory() {
    return [supportDirectory() URLByAppendingPathComponent:@"Nestvane-previous" isDirectory:YES];
}

NSString* size(uint64_t bytes) {
    return [NSByteCountFormatter stringFromByteCount:(long long)bytes countStyle:NSByteCountFormatterCountStyleFile];
}

uint64_t available(NSURL* directory) {
    NSNumber* free = [directory resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey] error:nil]
        [NSURLVolumeAvailableCapacityForImportantUsageKey];
    return free.longLongValue > 0 ? free.unsignedLongLongValue : UINT64_MAX;
}

// Only files and directories, by names inside the destination: an archive that says otherwise is
// not a backup this app made, and nothing of it is written.
int extractedSafely(void*, AAEntryMessage message, const char* path, void* data) {
    if (message != AA_ENTRY_MESSAGE_EXTRACT_BEGIN) return 0;
    const std::string_view name(path ? path : "");
    if (name.starts_with('/') || name == ".." || name.starts_with("../") || name.ends_with("/..") ||
        name.find("/../") != std::string_view::npos)
        return -1;
    const auto header = static_cast<AAHeader>(data);
    uint64_t type = 0;
    const int index = header ? AAHeaderGetKeyIndex(header, AA_FIELD_TYP) : -1;
    if (index < 0 || AAHeaderGetFieldUInt(header, uint32_t(index), &type) < 0) return -1;
    return type == AA_ENTRY_TYPE_REG || type == AA_ENTRY_TYPE_DIR ? 0 : -1;
}

// The database opens and says which generation of Android's data it is.
bool readable(NSURL* database) {
    sqlite3* raw = nullptr;
    bool good = sqlite3_open_v2(database.fileSystemRepresentation, &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK;
    sqlite3_stmt* statement = nullptr;
    good = good && sqlite3_prepare_v2(raw, "SELECT generation FROM state WHERE singleton=1", -1, &statement, nullptr) == SQLITE_OK &&
           sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    sqlite3_close(raw);
    return good;
}

} // namespace

NSURL* dataDirectory() {
    return [supportDirectory() URLByAppendingPathComponent:@"Nestvane" isDirectory:YES];
}

uint64_t dataBytes() {
    uint64_t total = 0;
    NSDirectoryEnumerator* files = [NSFileManager.defaultManager enumeratorAtURL:dataDirectory()
        includingPropertiesForKeys:@[NSURLFileSizeKey] options:0 errorHandler:nil];
    for (NSURL* file in files) {
        NSNumber* bytes = nil;
        [file getResourceValue:&bytes forKey:NSURLFileSizeKey error:nil];
        total += bytes.unsignedLongLongValue;
    }
    return total;
}

NSString* write(NSURL* destination, NSDictionary* settings) {
    NSFileManager* manager = NSFileManager.defaultManager;
    NSURL* data = dataDirectory();
    if (![manager fileExistsAtPath:[data URLByAppendingPathComponent:@"guest-data.sqlite3"].path])
        return @"Android has nothing to back up yet. Start it once first.";
    const uint64_t bytes = dataBytes(), free = available(destination.URLByDeletingLastPathComponent);
    if (free < bytes + (256ull << 20))
        return [NSString stringWithFormat:@"The backup needs up to %@ of free space, and there is %@.", size(bytes + (256ull << 20)), size(free)];

    NSDictionary* app = NSBundle.mainBundle.infoDictionary;
    NSDictionary* info = @{@"format": @(Format), @"version": app[@"CFBundleShortVersionString"] ?: @"",
                           @"build": app[@"CFBundleVersion"] ?: @"", @"created": NSDate.date, @"settings": settings ?: @{}};
    NSURL* infoURL = [data URLByAppendingPathComponent:InfoName];
    if (![info writeToURL:infoURL error:nil]) return @"Nestvane could not write the backup.";

    [manager removeItemAtURL:destination error:nil];
    AAByteStream file = AAFileStreamOpenWithPath(destination.fileSystemRepresentation, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    // LZ4: fast, so a backup takes about as long as copying the data.
    AAByteStream compressed = file ? AACompressionOutputStreamOpen(file, AA_COMPRESSION_ALGORITHM_LZ4, 4 << 20, 0, 0) : nullptr;
    AAArchiveStream archive = compressed ? AAEncodeArchiveOutputStreamOpen(compressed, nullptr, nullptr, 0, 0) : nullptr;
    AAFieldKeySet keys = AAFieldKeySetCreateWithString("TYP,PAT,DAT,MOD,MTM");
    AAPathList paths = AAPathListCreateWithDirectoryContents(data.fileSystemRepresentation, nullptr, nullptr, nullptr, 0, 0);
    bool written = archive && keys && paths &&
                   AAArchiveStreamWritePathList(archive, paths, keys, data.fileSystemRepresentation, nullptr, nullptr, 0, 0) == 0;
    // Each stream writes what it still holds as it closes: a failure there is a failed backup too.
    written = AAArchiveStreamClose(archive) >= 0 && written;
    written = AAByteStreamClose(compressed) >= 0 && written;
    written = AAByteStreamClose(file) >= 0 && written;
    AAPathListDestroy(paths);
    AAFieldKeySetDestroy(keys);
    [manager removeItemAtURL:infoURL error:nil];
    if (written) return nil;
    [manager removeItemAtURL:destination error:nil];
    return available(destination.URLByDeletingLastPathComponent) < (64ull << 20)
        ? @"The backup did not fit: there is not enough free space."
        : @"Nestvane could not write the backup.";
}

NSString* restore(NSURL* source, NSDictionary* currentSettings, NSDictionary* __autoreleasing* settings) {
    NSFileManager* manager = NSFileManager.defaultManager;
    NSURL* staging = [supportDirectory() URLByAppendingPathComponent:@"Nestvane-restore" isDirectory:YES];
    [manager removeItemAtURL:staging error:nil];
    if (![manager createDirectoryAtURL:staging withIntermediateDirectories:YES attributes:nil error:nil])
        return @"Nestvane could not make room for the backup.";

    AAByteStream file = AAFileStreamOpenWithPath(source.fileSystemRepresentation, O_RDONLY, 0);
    AAByteStream plain = file ? AADecompressionInputStreamOpen(file, 0, 0) : nullptr;
    AAArchiveStream archive = plain ? AADecodeArchiveInputStreamOpen(plain, nullptr, nullptr, 0, 0) : nullptr;
    AAArchiveStream extract = archive ? AAExtractArchiveOutputStreamOpen(staging.fileSystemRepresentation, nullptr,
                                                                         extractedSafely, 0, 0) : nullptr;
    bool read = extract && AAArchiveStreamProcess(archive, extract, nullptr, extractedSafely, 0, 0) >= 0;
    read = AAArchiveStreamClose(extract) >= 0 && read;
    AAArchiveStreamClose(archive);
    AAByteStreamClose(plain);
    AAByteStreamClose(file);

    NSURL* infoURL = [staging URLByAppendingPathComponent:InfoName];
    NSDictionary* info = read ? [NSDictionary dictionaryWithContentsOfURL:infoURL error:nil] : nil;
    if (!read || ![info[@"format"] isEqual:@(Format)] ||
        !readable([staging URLByAppendingPathComponent:@"guest-data.sqlite3"])) {
        [manager removeItemAtURL:staging error:nil];
        return read ? @"This file is not a Nestvane backup." : @"This file is not a Nestvane backup, or it is damaged, or there is not enough free space.";
    }
    [manager removeItemAtURL:infoURL error:nil];

    // What Android has now is kept, with the settings that go with it, unless it is itself a restored
    // backup that has not started yet: then what was there before that is still kept.
    NSURL* data = dataDirectory(), *previous = previousDirectory();
    const bool keptAlready = [manager fileExistsAtPath:previous.path];
    bool moved = false;
    if (keptAlready) {
        [manager removeItemAtURL:data error:nil];
    } else if ([manager fileExistsAtPath:data.path]) {
        [@{@"format": @(Format), @"settings": currentSettings ?: @{}} writeToURL:[data URLByAppendingPathComponent:InfoName] error:nil];
        moved = [manager moveItemAtURL:data toURL:previous error:nil];
        if (!moved) {
            [manager removeItemAtURL:[data URLByAppendingPathComponent:InfoName] error:nil];
            [manager removeItemAtURL:staging error:nil];
            return @"Nestvane could not set Android's data aside.";
        }
    }
    if (![manager moveItemAtURL:staging toURL:data error:nil]) {
        if (moved && [manager moveItemAtURL:previous toURL:data error:nil])
            [manager removeItemAtURL:[data URLByAppendingPathComponent:InfoName] error:nil];
        [manager removeItemAtURL:staging error:nil];
        return @"Nestvane could not put the backup in place.";
    }
    NSDictionary* saved = info[@"settings"];
    if (settings) *settings = [saved isKindOfClass:NSDictionary.class] ? saved : @{};
    return nil;
}

bool restorePending() {
    return [NSFileManager.defaultManager fileExistsAtPath:previousDirectory().path];
}

void restoreProven() {
    [NSFileManager.defaultManager removeItemAtURL:previousDirectory() error:nil];
}

NSDictionary* rollBack() {
    NSFileManager* manager = NSFileManager.defaultManager;
    NSURL* data = dataDirectory(), *previous = previousDirectory();
    if (![manager fileExistsAtPath:previous.path]) return nil;
    NSURL* failed = [supportDirectory() URLByAppendingPathComponent:@"Nestvane-failed" isDirectory:YES];
    [manager removeItemAtURL:failed error:nil];
    [manager moveItemAtURL:data toURL:failed error:nil];
    if (![manager moveItemAtURL:previous toURL:data error:nil]) {
        [manager moveItemAtURL:failed toURL:data error:nil];
        return nil;
    }
    [manager removeItemAtURL:failed error:nil];
    NSURL* infoURL = [data URLByAppendingPathComponent:InfoName];
    NSDictionary* saved = [NSDictionary dictionaryWithContentsOfURL:infoURL error:nil][@"settings"];
    [manager removeItemAtURL:infoURL error:nil];
    return [saved isKindOfClass:NSDictionary.class] ? saved : @{};
}

} // namespace nestvane::backup
