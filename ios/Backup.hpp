// Backing up and restoring Android: its data directory (the database that holds /data and the large
// files kept beside it) in one Apple Archive, with the app's own settings that belong to Android.
// The shared folder (Documents/Android) is not in it: it is in the Files app already.
// Android must not be running while either is done.
#pragma once

#import <Foundation/Foundation.h>

#include <cstdint>

namespace nestvane::backup {

// Application Support/Nestvane: what Android has of its own.
NSURL* dataDirectory();

// How many bytes Android's data takes now, which a backup takes at most.
uint64_t dataBytes();

// Writes Android's data and `settings` into an archive at `destination`. Returns nil when it is
// written, and otherwise says what went wrong; then there is no file.
NSString* write(NSURL* destination, NSDictionary* settings);

// Puts the backup's data in place of Android's. What was there is kept until Android has started
// from the backup (restoreProven) or failed to (rollBack), with `currentSettings`. Returns nil and
// the backup's settings when it is in place; otherwise what went wrong, and nothing has changed.
NSString* restore(NSURL* source, NSDictionary* currentSettings, NSDictionary* __autoreleasing* settings);

// Whether a restored backup has not yet brought Android to its home screen.
bool restorePending();

// Android reached its home screen from the restored backup: what it had before is let go.
void restoreProven();

// Android did not start from the restored backup: what it had before is put back. Returns the
// settings kept with it, or nil when there was nothing to put back.
NSDictionary* rollBack();

} // namespace nestvane::backup
