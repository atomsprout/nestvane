#include "ProbeLinux.hpp"

#include <algorithm>
#include <cstring>

namespace nestvane {
namespace {

constexpr uint64_t invalid = uint64_t(-22), fault = uint64_t(-14);
constexpr uint32_t bcTransaction = 0x40406300, bcReply = 0x40406301, bcFreeBuffer = 0x40086303;
constexpr uint32_t bcTransactionSg = 0x40486311, bcReplySg = 0x40486312;
constexpr uint32_t brTransaction = 0x80407202, brTransactionSecctx = 0x80487202, brReply = 0x80407203;
constexpr uint32_t brDeadReply = 0x7205, brComplete = 0x7206, brSpawnLooper = 0x720d, brFailedReply = 0x7211;
constexpr uint32_t oneWay = 1, acceptReplyFiles = 0x10;
constexpr uint32_t acceptNodeFiles = 0x100, sendSecurityContext = 0x1000;
constexpr uint32_t binderFile = 0x66642a85, closeOnExec = 0x80000;
constexpr uint32_t binderObject = 0x73622a85, binderHandle = 0x73682a85;
constexpr uint32_t binderBuffer = 0x70742a85, hasParent = 1;
constexpr uint32_t binderFileArray = 0x66646185;
constexpr uint32_t brIncrefs = 0x80107207, brAcquire = 0x80107208;
constexpr uint32_t brRelease = 0x80107209, brDecrefs = 0x8010720a;
constexpr uint32_t brDeadBinder = 0x8008720f, brClearDeath = 0x80087210;
// Android 17 service managers keep references to hundreds of registered objects.
constexpr size_t binderObjectLimit = 4096;
// How many packets may wait for one thread. A transaction that hands over many objects at once puts
// two there for each: an app with dozens of content providers publishes them all in one call, and at
// 64 that call failed (Douyin stopped as it started).
constexpr size_t directedLimit = 4096;

template <class T>
bool readGuest(Machine& machine, uint64_t address, T& value) {
    return machine.readInto(address, {reinterpret_cast<uint8_t*>(&value), sizeof value});
}

const uint8_t* readBinderBytes(Machine& machine, uint64_t address, size_t size,
                               std::vector<uint8_t>& storage) {
    if (!size) return nullptr;
    if (const auto* bytes = machine.read(address, size, 4)) return bytes;
    storage.resize(size);
    return machine.readInto(address, storage) ? storage.data() : nullptr;
}

std::string binderInterface(std::span<const uint8_t> parcel) {
    constexpr size_t headerSize = 4 * sizeof(uint32_t);
    constexpr uint32_t system = 0x53595354, vendor = 0x564e4452, recovery = 0x5245434f;
    if (parcel.size() < headerSize) return {};
    uint32_t header, length;
    std::memcpy(&header, parcel.data() + 2 * sizeof(uint32_t), sizeof header);
    std::memcpy(&length, parcel.data() + 3 * sizeof(uint32_t), sizeof length);
    if ((header != system && header != vendor && header != recovery) || !length || length > 192)
        return {};
    const auto nameBytes = parcel.subspan(headerSize);
    if (nameBytes.size() < (length + 1) * 2 || nameBytes[length * 2] || nameBytes[length * 2 + 1])
        return {};
    std::string name;
    name.reserve(length);
    for (size_t index = 0; index < length; ++index) {
        const auto ch = nameBytes[index * 2];
        if (nameBytes[index * 2 + 1] ||
            !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '$')) return {};
        name += char(ch);
    }
    return name;
}

template <class T>
bool writeGuest(Machine& machine, uint64_t address, const T& value) {
    return machine.write(address, {reinterpret_cast<const uint8_t*>(&value), sizeof value});
}

} // namespace

void ProbeLinux::BinderConnection::changeReference(const std::shared_ptr<BinderNode>& node,
                                                       int64_t strong, int64_t weak) {
    const bool hadStrong = node->strong != 0;
    const bool hadWeak = node->weak != 0;
    node->strong = uint64_t(int64_t(node->strong) + strong);
    node->weak = uint64_t(int64_t(node->weak) + weak);
    if (hadStrong == bool(node->strong) && hadWeak == bool(node->weak)) return;
    if (const auto changes = referenceChanges.lock()) changes->insert(node);
}

ProbeLinux::BinderConnection::~BinderConnection() {
    if (const auto dirty = deathsDirty.lock()) *dirty = true;
    for (const auto& [handle, ref] : handles)
        changeReference(ref.node, -int64_t(ref.strong) - ref.inBuffer, -int64_t(ref.weak));
    for (const auto& [offset, buffer] : allocations)
        for (const auto& node : buffer.localReferences) changeReference(node, -1, 0);
}

void ProbeLinux::refreshBinderReferences() {
    BinderReferenceChanges changes;
    changes.swap(*binderReferenceChanges);
    bool queued = false;
    for (const auto& item : changes) {
        const auto node = item.lock();
        if (!node) continue;
        const auto owner = node->owner.lock();
        if (!owner || !processAlive(owner->owner)) continue;
        const auto send = [&](uint32_t command) {
            BinderPacket packet{command};
            packet.reference = {node->object.value, node->object.cookie};
            owner->incoming.push_back(packet);
            queued = true;
        };
        const bool strong = node->strong != 0;
        const bool weak = strong || node->weak != 0;
        if (weak && !node->ownerWeak) {
            send(brIncrefs);
            node->ownerWeak = node->pendingWeak = true;
        }
        if (strong && !node->ownerStrong) {
            send(brAcquire);
            node->ownerStrong = node->pendingStrong = true;
        }
        if (!strong && node->ownerStrong && !node->pendingStrong) {
            send(brRelease);
            node->ownerStrong = false;
        }
        if (!weak && !node->ownerStrong && !node->pendingWeak) {
            if (node->ownerWeak) send(brDecrefs);
            owner->localNodes.erase(node->object.value);
        }
    }
    if (queued) wakeBinderPollers();
}

bool ProbeLinux::binderAvailableForProcessWork(const BinderConnection& connection, uint64_t tid) {
    if (!connection.loopers.contains(tid) || connection.pendingCalls.contains(tid)) return false;
    const auto replies = connection.replies.find(tid);
    return replies == connection.replies.end() || replies->second.empty();
}

bool ProbeLinux::binderReadable(const BinderConnection& connection, uint64_t pid, uint64_t tid) const {
    if (connection.owner != pid) return false;
    const auto directed = connection.directed.find(tid);
    return (directed != connection.directed.end() && !directed->second.empty()) ||
           (!connection.incoming.empty() && binderAvailableForProcessWork(connection, tid)) ||
           binderCallLostPeer(connection, tid);
}

bool ProbeLinux::binderCallLostPeer(const BinderConnection& connection, uint64_t tid) const {
    const auto pending = connection.pendingCalls.find(tid);
    if (pending == connection.pendingCalls.end()) return false;
    const auto& call = pending->second.back();
    return call.target.expired() || !processAlive(call.pid);
}

uint64_t ProbeLinux::storeBinderBuffer(BinderConnection& target, std::span<const uint8_t> data,
                                      std::span<const uint8_t> offsets, std::span<const uint8_t> extra,
                                      std::string_view context, bool asynchronous) {
    if (!target.mapped) return 0;
    const auto endPayload = (data.size() + size_t{7}) & ~size_t{7};
    const auto endOffsets = endPayload + offsets.size();
    const auto endBuffers = endOffsets + extra.size();
    const auto size = (endBuffers + (context.empty() ? 0 : context.size() + 1) + size_t{7}) & ~size_t{7};
    const auto needed = std::max(size_t{8}, size);
    // The Android Binder allocator charges the aligned mapped bytes to the
    // asynchronous budget. Host packet metadata is outside the guest mapping.
    const auto asyncCharge = asynchronous ? needed : 0;
    if (asyncCharge && target.asyncBytes + asyncCharge > target.mapped->size / 2) return 0;
    uint64_t offset = 0;
    for (const auto& [start, buffer] : target.allocations) {
        if (start - offset >= needed) break;
        offset = start + buffer.size;
    }
    if (needed > target.mapped->size || offset > target.mapped->size - needed) return 0;
    target.allocations.emplace(offset, BinderAllocation{needed, {}, {}, false, asyncCharge});
    target.asyncBytes += asyncCharge;
    auto begin = target.mapped->pages.begin() + offset;
    std::fill_n(begin, needed, 0);
    std::copy(data.begin(), data.end(), begin);
    std::copy(offsets.begin(), offsets.end(), begin + endPayload);
    std::copy(extra.begin(), extra.end(), begin + endOffsets);
    std::copy(context.begin(), context.end(), begin + endBuffers);
    return target.address + offset;
}

bool ProbeLinux::freeBinderBuffer(BinderConnection& connection, uint64_t address, bool delivered) {
    if (address < connection.address) return false;
    const auto buffer = connection.allocations.find(address - connection.address);
    if (buffer == connection.allocations.end() || buffer->second.delivered != delivered) return false;
    for (const auto handle : buffer->second.handles) {
        const auto it = connection.handles.find(handle);
        auto& ref = it->second;
        connection.changeReference(ref.node, -1, 0);
        if (!--ref.inBuffer && !ref.strong && !ref.weak) connection.handles.erase(it);
    }
    for (const auto& node : buffer->second.localReferences) connection.changeReference(node, -1, 0);
    if (buffer->second.asyncCharge) {
        connection.asyncBytes -= buffer->second.asyncCharge;
        const auto pending = connection.pendingAsync.find(buffer->second.asyncTarget);
        if (pending->second.empty()) {
            connection.pendingAsync.erase(pending);
        } else {
            connection.incoming.push_back(std::move(pending->second.front()));
            pending->second.pop_front();
            wakeBinderPollers();
        }
    }
    connection.allocations.erase(buffer);
    return true;
}

void ProbeLinux::refreshBinderDeaths() {
    *binderDeathsDirty = false;
    bool queued = false;
    std::erase_if(binderDeathSubscribers, [&](const auto& subscriber) {
        const auto connection = subscriber.lock();
        if (!connection) return true;
        for (auto& [handle, death] : connection->deaths) {
            if (death.sent) continue;
            const auto target = death.target.lock();
            if (target && processAlive(target->owner)) continue;
            // Process work, as Linux queues it, which only a thread that waits for nothing takes. Given
            // to the thread that asked for it while that thread waited in a call to the process that
            // died, it ran the death handler inside the call, and the call's own answer then came to
            // the handler's next call: system_server aborted ("Unexpected BR_REPLY"). A process with no
            // looper threads still gets it on the thread that asked.
            const auto receiver = threads.find(death.thread);
            auto& queue = connection->loopers.empty() && receiver != threads.end() && receiver->second.wait != Wait::exited
                ? connection->directed[death.thread] : connection->incoming;
            if (queue.size() >= 32) {
                *binderDeathsDirty = true;
                continue;
            }
            BinderPacket packet{brDeadBinder};
            packet.reference[0] = death.cookie;
            queue.push_back(packet);
            death.sent = true;
            queued = true;
        }
        return connection->deaths.empty();
    });
    if (queued) wakeBinderPollers();
}

uint64_t ProbeLinux::binderCommand(Machine& machine, const Descriptor& fd, uint32_t command,
                                  std::span<const uint8_t> data) {
    static_assert(sizeof(BinderTransaction) == 64);
    auto& connection = *fd.binder;
    switch (command) {
        case 0x630b: // BC_REGISTER_LOOPER
            if (connection.looperRequested && connection.loopers.try_emplace(currentTid, true).second)
                connection.looperRequested = false;
            return 0;
        case 0x630c: // BC_ENTER_LOOPER
            connection.loopers.try_emplace(currentTid, false);
            return 0;
        case 0x630d: // BC_EXIT_LOOPER
            connection.loopers.erase(currentTid);
            return 0;
        case bcFreeBuffer: {
            uint64_t address = 0;
            std::memcpy(&address, data.data(), sizeof address);
            return freeBinderBuffer(connection, address, true) ? 0 : invalid;
        }
        case 0x40046304: case 0x40046305: case 0x40046306: case 0x40046307: {
            uint32_t handle = 0;
            std::memcpy(&handle, data.data(), sizeof handle);
            if (!handle) return 0; // Handle zero is tied to the context manager.
            const auto it = connection.handles.find(handle);
            if (it == connection.handles.end()) return invalid;
            auto& ref = it->second;
            const bool weak = command == 0x40046304 || command == 0x40046307;
            auto& count = weak ? ref.weak : ref.strong;
            const bool increment = command == 0x40046304 || command == 0x40046305;
            if (increment ? count == UINT32_MAX : !count) return invalid;
            const auto change = increment ? 1 : -1;
            connection.changeReference(ref.node, weak ? 0 : change, weak ? change : 0);
            if (increment) ++count;
            else if (!--count && !ref.inBuffer && !ref.strong && !ref.weak) connection.handles.erase(it);
            return 0;
        }
        case 0x40106308: case 0x40106309: { // BC_INCREFS_DONE / BC_ACQUIRE_DONE
            std::array<uint64_t, 2> pointerCookie{};
            std::memcpy(pointerCookie.data(), data.data(), sizeof pointerCookie);
            const auto node = connection.localNodes.find(pointerCookie[0]);
            if (node == connection.localNodes.end() || node->second->object.cookie != pointerCookie[1])
                return invalid;
            auto& pending = command == 0x40106308 ? node->second->pendingWeak : node->second->pendingStrong;
            if (pending) {
                pending = false;
                binderReferenceChanges->insert(node->second);
            }
            return 0;
        }
        case 0x400c630e: case 0x400c630f: { // BC_REQUEST/CLEAR_DEATH_NOTIFICATION
            uint32_t handle;
            uint64_t cookie;
            std::memcpy(&handle, data.data(), sizeof handle);
            std::memcpy(&cookie, data.data() + sizeof handle, sizeof cookie);
            const auto death = connection.deaths.find(handle);
            if (command == 0x400c630e) {
                const auto ref = connection.handles.find(handle);
                if (ref == connection.handles.end() || death != connection.deaths.end()) return invalid;
                connection.deaths.emplace(handle, BinderDeath{ref->second.node->owner, cookie, currentTid});
                if (std::none_of(binderDeathSubscribers.begin(), binderDeathSubscribers.end(),
                                 [&](const auto& subscriber) { return subscriber.lock() == fd.binder; }))
                    binderDeathSubscribers.emplace_back(fd.binder);
                refreshBinderDeaths();
                return 0;
            }
            if (death == connection.deaths.end() || death->second.cookie != cookie) return invalid;
            if (death->second.sent && !death->second.acknowledged) {
                death->second.cleared = true;
                return 0;
            }
            BinderPacket packet{brClearDeath};
            packet.reference[0] = cookie;
            connection.directed[currentTid].push_back(packet);
            connection.deaths.erase(death);
            if (connection.deaths.empty()) *binderDeathsDirty = true;
            return 0;
        }
        case 0x40086310: { // BC_DEAD_BINDER_DONE
            uint64_t cookie;
            std::memcpy(&cookie, data.data(), sizeof cookie);
            const auto death = std::find_if(connection.deaths.begin(), connection.deaths.end(), [&](const auto& item) {
                return item.second.cookie == cookie && item.second.sent && !item.second.acknowledged;
            });
            if (death == connection.deaths.end()) return invalid;
            death->second.acknowledged = true;
            if (death->second.cleared) {
                BinderPacket packet{brClearDeath};
                packet.reference[0] = cookie;
                connection.directed[currentTid].push_back(packet);
                connection.deaths.erase(death);
                if (connection.deaths.empty()) *binderDeathsDirty = true;
            }
            return 0;
        }
        case bcTransaction: case bcReply: case bcTransactionSg: case bcReplySg:
            break;
        default:
            return invalid;
    }
    auto& notices = connection.directed[currentTid];
    if (notices.size() >= directedLimit) return uint64_t(-28);
    const bool replying = command == bcReply || command == bcReplySg;
    auto& outstanding = connection.replies[currentTid];
    const auto fail = [&](uint32_t status) {
        if (replying && !outstanding.empty()) {
            const auto route = outstanding.back();
            outstanding.pop_back();
            if (const auto caller = route.connection.lock()) {
                BinderPacket failed{status};
                failed.completesCall = true;
                caller->directed[route.thread].push_back(std::move(failed));
                notices.push_back({brComplete});
                wakeBinderPollers();
                return uint64_t{0};
            }
        }
        notices.push_back({status});
        return uint64_t{0};
    };
    BinderTransaction transaction;
    std::memcpy(&transaction, data.data(), sizeof transaction);
    const bool scatterGather = command == bcTransactionSg || command == bcReplySg;
    uint64_t extraSize = 0;
    if (scatterGather) std::memcpy(&extraSize, data.data() + sizeof transaction, sizeof extraSize);
    if (transaction.dataSize > binderMappingLimit || extraSize > binderMappingLimit ||
        transaction.offsetsSize % sizeof(uint64_t) || extraSize % 8 ||
        transaction.offsetsSize / sizeof(uint64_t) > transaction.dataSize / sizeof(BinderObject))
        return fail(brFailedReply);
    std::shared_ptr<BinderConnection> target;
    uint64_t replyThread = 0;
    uint64_t targetPointer = 0, targetCookie = 0;
    bool sendLabel = false, acceptsFiles = false;
    if (replying) {
        if (outstanding.empty()) return fail(brFailedReply);
        auto route = outstanding.back();
        // A caller told that its callee died (BR_DEAD_REPLY, as soon as the process counts as gone)
        // has gone on. A thread of the dying process can still reply: that reply answers nothing the
        // caller waits for now, and given to it, system_server aborted ("Unexpected BR_REPLY").
        if (const auto caller = route.connection.lock(); caller && route.call) {
            const auto calls = caller->pendingCalls.find(route.thread);
            if (calls == caller->pendingCalls.end() || calls->second.empty() || calls->second.back().call != route.call) {
                outstanding.pop_back();
                notices.push_back({brComplete});
                return uint64_t{0};
            }
        }
        target = route.connection.lock();
        replyThread = route.thread;
        acceptsFiles = route.acceptsFiles;
    } else if (uint32_t(transaction.target) == 0) {
        target = binderDevices.at(fd.path).manager.lock();
        if (target) {
            targetPointer = target->target;
            targetCookie = target->cookie;
            sendLabel = target->sendSecurityContext;
            acceptsFiles = target->managerAcceptsFiles;
        }
    } else {
        const auto ref = connection.handles.find(uint32_t(transaction.target));
        if (ref == connection.handles.end()) return fail(brFailedReply);
        const auto& node = *ref->second.node;
        target = node.owner.lock();
        targetPointer = node.object.value;
        targetCookie = node.object.cookie;
        sendLabel = node.object.flags & sendSecurityContext;
        acceptsFiles = node.object.flags & acceptNodeFiles;
    }
    if (!target || !processAlive(target->owner)) return fail(brDeadReply);
    const bool asynchronous = !replying && (transaction.flags & oneWay);
    const bool synchronousCall = !replying && !asynchronous;
    if (synchronousCall && !outstanding.empty()) {
        // Send a synchronous callback to the waiting caller, including callers
        // further up the chain. That thread need not be a registered looper.
        for (const BinderRoute* route = &outstanding.back(); route; route = route->parent.get()) {
            if (route->connection.lock() != target) continue;
            const auto thread = threads.find(route->thread);
            if (thread == threads.end() || thread->second.wait == Wait::exited) continue;
            replyThread = route->thread;
            break;
        }
    }
    const auto capacityReached = [&](std::string_view resource, size_t count) {
        if (androidLogObserver && binderCapacityReports < 16) {
            ++binderCapacityReports;
            androidLogObserver("Binder capacity: " + std::string(resource) + " count=" + std::to_string(count) +
                               ", sender=" + std::to_string(processId()) + ", target=" +
                               std::to_string(target->owner) + ", code=" + std::to_string(transaction.code));
        }
        return fail(brFailedReply);
    };
    auto& destination = replyThread ? target->directed[replyThread] : target->incoming;
    const auto label = sendLabel ? std::string_view(current->process->context) : std::string_view{};
    const auto endPayload = (transaction.dataSize + 7) & ~uint64_t{7};
    const auto extraOffset = endPayload + transaction.offsetsSize;
    const auto extent = extraOffset + extraSize + (label.empty() ? 0 : label.size() + 1);
    if (!target->mapped || std::max<uint64_t>(8, (extent + 7) & ~uint64_t{7}) > target->mapped->size)
        return capacityReached("transaction bytes", extent);

    std::vector<uint8_t> splitPayload, splitOffsets;
    const auto* bytes = readBinderBytes(machine, transaction.buffer, transaction.dataSize, splitPayload);
    if (transaction.dataSize && !bytes) return fail(brFailedReply);
    const auto* offsets = readBinderBytes(machine, transaction.offsets, transaction.offsetsSize, splitOffsets);
    if (transaction.offsetsSize && !offsets) return fail(brFailedReply);

    struct Translation {
        size_t offset;
        BinderObject object;
        uint32_t handle;
    };
    struct BufferObject {
        uint32_t type, flags;
        uint64_t address, length, parent, parentOffset;
    };
    static_assert(sizeof(BufferObject) == 40);
    struct BufferTranslation {
        size_t objectIndex, objectOffset, extraOffset, parentIndex;
        BufferObject object;
    };
    std::vector<Translation> translations;
    std::vector<BufferTranslation> buffers;
    std::vector<BinderFileTransfer> files;
    std::vector<uint8_t> extra(extraSize);
    std::map<uint64_t, std::shared_ptr<BinderNode>> newNodes;
    std::map<const BinderNode*, uint32_t> knownHandles;
    std::map<uint32_t, BinderReference> newHandles;
    if (transaction.offsetsSize) {
        for (const auto& [handle, ref] : target->handles) knownHandles.emplace(ref.node.get(), handle);
    }
    size_t previousEnd = 0;
    size_t nextExtraOffset = 0;
    auto nextHandle = target->nextHandle;
    for (size_t index = 0; index < transaction.offsetsSize; index += sizeof(uint64_t)) {
        uint64_t offset = 0;
        std::memcpy(&offset, offsets + index, sizeof offset);
        if (offset % 4 || offset < previousEnd || offset > transaction.dataSize ||
            sizeof(BinderObject) > transaction.dataSize - offset) return fail(brFailedReply);
        previousEnd = offset + sizeof(BinderObject);
        BinderObject object{};
        std::memcpy(&object, bytes + offset, sizeof object);
        if (object.type == binderBuffer) {
            if (!scatterGather || sizeof(BufferObject) > transaction.dataSize - offset) return fail(brFailedReply);
            BufferObject buffer;
            std::memcpy(&buffer, bytes + offset, sizeof buffer);
            if (buffer.flags & ~hasParent || buffer.length > extra.size() - nextExtraOffset)
                return fail(brFailedReply);
            size_t parentIndex = 0;
            if (buffer.flags & hasParent) {
                if (buffers.empty()) return fail(brFailedReply);
                parentIndex = buffers.size() - 1;
                uint64_t minimum = 0;
                while (buffers[parentIndex].objectIndex != buffer.parent) {
                    const auto& prior = buffers[parentIndex];
                    if (!(prior.object.flags & hasParent)) return fail(brFailedReply);
                    minimum = prior.object.parentOffset + sizeof(uint64_t);
                    parentIndex = prior.parentIndex;
                }
                const auto length = buffers[parentIndex].object.length;
                if (buffer.parentOffset < minimum || buffer.parentOffset > length ||
                    sizeof(uint64_t) > length - buffer.parentOffset) return fail(brFailedReply);
            }
            if (buffer.length && !machine.readInto(buffer.address, std::span(extra).subspan(nextExtraOffset, buffer.length)))
                return fail(brFailedReply);
            buffers.push_back({index / sizeof(uint64_t), size_t(offset), nextExtraOffset, parentIndex, buffer});
            nextExtraOffset += (buffer.length + 7) & ~uint64_t{7};
            previousEnd = offset + sizeof buffer;
            continue;
        }
        if (object.type == binderFileArray) {
            // An array of descriptors inside one of the buffers sent before it: how HIDL passes the
            // descriptors of a native handle, such as the shared memory of an audio stream's queues.
            struct FileArray {
                uint32_t type, padding;
                uint64_t count, parent, parentOffset;
            } array;
            if (!scatterGather || !acceptsFiles || sizeof(array) > transaction.dataSize - offset) return fail(brFailedReply);
            std::memcpy(&array, bytes + offset, sizeof array);
            const auto parent = std::ranges::find_if(buffers, [&](const BufferTranslation& buffer) { return buffer.objectIndex == array.parent; });
            if (parent == buffers.end() || array.count > 64 || array.parentOffset > parent->object.length ||
                array.count * sizeof(uint32_t) > parent->object.length - array.parentOffset) return fail(brFailedReply);
            for (uint64_t item = 0; item < array.count; ++item) {
                const size_t place = parent->extraOffset + array.parentOffset + item * sizeof(uint32_t);
                uint32_t number;
                std::memcpy(&number, extra.data() + place, sizeof number);
                const auto file = descriptors().find(number);
                if (file == descriptors().end() || file->second.binder) return fail(brFailedReply);
                files.push_back({extraOffset + place, std::make_shared<Descriptor>(file->second), true});
            }
            previousEnd = offset + sizeof array;
            continue;
        }
        if (object.type == binderFile) {
            const auto file = descriptors().find(uint32_t(object.value));
            // A Binder connection belongs to its opener and can own pending packets itself.
            if (!acceptsFiles || file == descriptors().end() || file->second.binder)
                return fail(brFailedReply);
            files.push_back({size_t(offset) + offsetof(BinderObject, value),
                             std::make_shared<Descriptor>(file->second)});
            continue;
        }
        if (object.type == binderObject && object.value == 0 && object.cookie == 0) continue;
        std::shared_ptr<BinderNode> node;
        if (object.type == binderObject && object.value) {
            if (const auto found = connection.localNodes.find(object.value); found != connection.localNodes.end())
                node = found->second;
            else if (const auto pending = newNodes.find(object.value); pending != newNodes.end())
                node = pending->second;
            else {
                node = std::make_shared<BinderNode>(BinderNode{fd.binder, object});
                newNodes.emplace(object.value, node);
            }
            if (node->object.cookie != object.cookie) return fail(brFailedReply);
            const auto localObjectCount = connection.localNodes.size() + newNodes.size();
            if (localObjectCount > binderObjectLimit) return capacityReached("local objects", localObjectCount);
        } else if (object.type == binderHandle && object.value <= UINT32_MAX) {
            const auto ref = connection.handles.find(uint32_t(object.value));
            if (ref == connection.handles.end() || ref->second.node->owner.expired()) return fail(brFailedReply);
            node = ref->second.node;
        } else {
            return fail(brFailedReply);
        }
        if (node->owner.lock() == target) {
            translations.push_back({size_t(offset), node->object, 0});
            continue;
        }
        if (!knownHandles.contains(node.get())) {
            const auto remoteObjectCount = target->handles.size() + newHandles.size();
            if (!nextHandle || remoteObjectCount >= binderObjectLimit)
                return capacityReached("remote handles", remoteObjectCount);
            knownHandles.emplace(node.get(), nextHandle);
            newHandles.emplace(nextHandle++, BinderReference{node});
        }
        const auto handle = knownHandles.at(node.get());
        translations.push_back({size_t(offset), {binderHandle, object.flags, handle, 0}, handle});
    }
    if (notices.size() + 2 * newNodes.size() + 1 > directedLimit) return fail(brFailedReply);
    const auto address = storeBinderBuffer(*target, {bytes, size_t(transaction.dataSize)},
                                          {offsets, size_t(transaction.offsetsSize)}, extra, label,
                                          asynchronous);
    if (!address) {
        std::string resource = "receive buffers";
        if (androidLogObserver && binderCapacityReports < 16) {
            const auto needed = std::max<uint64_t>(8, (extent + 7) & ~uint64_t{7});
            const auto asyncLimit = target->mapped->size / 2;
            const auto allocated = std::ranges::count_if(target->allocations, [](const auto& entry) {
                return entry.second.delivered;
            });
            const auto interface = binderInterface({bytes, size_t(transaction.dataSize)});
            resource += " needed=" + std::to_string(needed) +
                " mapping=" + std::to_string(target->mapped->size) +
                " async-bytes=" + std::to_string(target->asyncBytes) + '/' + std::to_string(asyncLimit) +
                " delivered=" + std::to_string(allocated) +
                " allocation-failure=" + (asynchronous && target->asyncBytes + needed > asyncLimit
                    ? "async-quota" : "no-contiguous-space");
            if (!interface.empty()) resource += " interface=" + interface;

            const auto backlog = std::ranges::max_element(target->pendingAsync, {},
                [](const auto& entry) { return entry.second.size(); });
            if (backlog != target->pendingAsync.end() && !backlog->second.empty()) {
                const auto& pending = backlog->second.front().transaction;
                const auto pendingOffset = pending.buffer - target->address;
                const auto pendingBytes = std::span(target->mapped->pages).subspan(pendingOffset, pending.dataSize);
                resource += " largest-oneway-queue=" + std::to_string(backlog->second.size()) +
                    " queued-code=" + std::to_string(pending.code);
                if (const auto queuedInterface = binderInterface(pendingBytes); !queuedInterface.empty())
                    resource += " queued-interface=" + queuedInterface;
            }
        }
        return capacityReached(resource, target->allocations.size());
    }
    target->nextHandle = nextHandle;
    target->handles.merge(newHandles);
    auto& allocation = target->allocations.at(address - target->address);
    for (const auto& item : translations) {
        auto* destination = target->mapped->pages.data() + (address - target->address) + item.offset;
        std::memcpy(destination, &item.object, sizeof item.object);
        if (item.handle) {
            auto& ref = target->handles.at(item.handle);
            ++ref.inBuffer;
            target->changeReference(ref.node, 1, 0);
            allocation.handles.push_back(item.handle);
        } else if (item.object.value && item.object.type == binderObject) {
            // A proxy flattened back to its owner keeps the local object alive through delivery.
            const auto found = target->localNodes.find(item.object.value);
            if (found != target->localNodes.end()) {
                target->changeReference(found->second, 1, 0);
                allocation.localReferences.push_back(found->second);
            }
        }
    }
    auto* targetBytes = target->mapped->pages.data() + (address - target->address);
    for (const auto& item : buffers) {
        auto buffer = item.object;
        buffer.address = address + extraOffset + item.extraOffset;
        std::memcpy(targetBytes + item.objectOffset, &buffer, sizeof buffer);
        if (buffer.flags & hasParent) {
            const auto offset = buffers[item.parentIndex].extraOffset + buffer.parentOffset;
            std::memcpy(targetBytes + extraOffset + offset, &buffer.address, sizeof buffer.address);
        }
    }
    for (const auto& [pointer, node] : newNodes) {
        if (target == fd.binder) continue;
        BinderPacket weak{brIncrefs}, strong{brAcquire};
        weak.reference = strong.reference = {pointer, node->object.cookie};
        node->ownerWeak = node->pendingWeak = true;
        node->ownerStrong = node->pendingStrong = true;
        notices.push_back(weak);
        notices.push_back(strong);
    }
    connection.localNodes.merge(newNodes);
    transaction.target = targetPointer;
    transaction.cookie = targetCookie;
    transaction.senderPid = replying || (transaction.flags & oneWay) ? 0 : processId();
    transaction.senderUid = userId();
    transaction.buffer = address;
    transaction.offsets = address + endPayload;
    const auto opcode = replying ? brReply : label.empty() ? brTransaction : brTransactionSecctx;
    BinderPacket packet{opcode, transaction, label.empty() ? 0 : address + extraOffset + extra.size()};
    if (synchronousCall) {
        auto parent = outstanding.empty() ? std::shared_ptr<const BinderRoute>{}
                                          : std::make_shared<const BinderRoute>(outstanding.back());
        packet.reply = BinderRoute{fd.binder, currentTid, bool(transaction.flags & acceptReplyFiles), std::move(parent),
                                   ++nextBinderCall};
    }
    packet.files = std::move(files);
    packet.completesCall = replying;
    if (replying) {
        packet.answers = outstanding.back().call;
        packet.sender = processId();
    }
    const auto fileCount = packet.files.size();
    if (asynchronous) {
        // Other worker threads may handle synchronous work or a different node.
        // The next oneway for this node becomes readable when the current buffer is freed.
        allocation.asyncTarget = targetPointer;
        const auto [pending, first] = target->pendingAsync.try_emplace(targetPointer);
        auto& queue = first ? destination : pending->second;
        queue.push_back(std::move(packet));
    } else {
        destination.push_back(std::move(packet));
    }
    if (synchronousCall)
        connection.pendingCalls[currentTid].push_back({target, target->owner, transaction.code,
            binderInterface({bytes, size_t(transaction.dataSize)}), nextBinderCall});
    if (binderTrace.size() < 8192) {
        binderTrace += "  guest binder " + std::string(replying ? "reply" : "transaction") +
            " sender=" + std::to_string(processId()) + " target=" + std::to_string(target->owner) +
            " code=" + std::to_string(transaction.code) + " objects=" +
            std::to_string(transaction.offsetsSize / sizeof(uint64_t)) + " files=" +
            std::to_string(fileCount) + "\n";
    }
    if (replying) outstanding.pop_back();
    notices.push_back({brComplete});
    wakeBinderPollers();
    return 0;
}

uint64_t ProbeLinux::binderWriteRead(Machine& machine, const Descriptor& fd) {
    struct Exchange {
        uint64_t writeSize, writeConsumed, writeBuffer, readSize, readConsumed, readBuffer;
    } request;
    static_assert(sizeof(Exchange) == 48);
    const auto argument = machine.cpu.x[2];
    if (!readGuest(machine, argument, request)) return fault;
    if (request.writeSize > 128 * 1024 || request.readSize > 128 * 1024 ||
        request.writeConsumed > request.writeSize || request.readConsumed > request.readSize ||
        request.writeBuffer > UINT64_MAX - request.writeSize || request.readBuffer > UINT64_MAX - request.readSize)
        return invalid;
    std::vector<uint8_t> splitCommand;
    while (request.writeConsumed < request.writeSize) {
        const auto position = request.writeBuffer + request.writeConsumed;
        const auto remaining = request.writeSize - request.writeConsumed;
        uint32_t command = 0;
        if (remaining < sizeof command) return invalid;
        if (!readGuest(machine, position, command)) return fault;
        const auto size = (command >> 16) & 0x3fff;
        if (remaining < sizeof command + size) return invalid;
        const auto* data = readBinderBytes(machine, position + sizeof command, size, splitCommand);
        if (size && !data) return fault;
        if (const auto error = binderCommand(machine, fd, command, {data, size}); error)
            return writeGuest(machine, argument, request) ? error : fault;
        request.writeConsumed += sizeof command + size;
    }

    if (!binderReferenceChanges->empty()) refreshBinderReferences();
    auto& connection = *fd.binder;
    if (!request.readSize) return writeGuest(machine, argument, request) ? 0 : fault;
    if (!binderReadable(connection, processId(), currentTid)) {
        return writeGuest(machine, argument, request) ? uint64_t(-11) : fault;
    }
    auto& directed = connection.directed[currentTid];
    // A reply is only for a thread that waits for that very call. One that has no call or another
    // one, say because it was told its callee died before the reply came, would take it for the
    // answer to something else, or abort ("Unexpected BR_REPLY", in system_server). It is dropped.
    while (!directed.empty() && directed.front().command == brReply) {
        const auto calls = connection.pendingCalls.find(currentTid);
        const auto& reply = directed.front();
        const bool waiting = calls != connection.pendingCalls.end() && !calls->second.empty() &&
                             (!reply.answers || calls->second.back().call == reply.answers);
        if (waiting) break;
        if (androidLogObserver && binderStaleReplies++ < 16)
            androidLogObserver("Binder: dropped a reply from PID " + std::to_string(reply.sender) + " to PID " +
                               std::to_string(processId()) + " thread " + std::to_string(currentTid) +
                               " for call " + std::to_string(reply.answers) + ", which it does not wait for (" +
                               (calls == connection.pendingCalls.end() || calls->second.empty()
                                    ? std::string("no call") : "call " + std::to_string(calls->second.back().call)) + ")");
        freeBinderBuffer(connection, reply.transaction.buffer, false);
        directed.pop_front();
    }
    if (!binderReadable(connection, processId(), currentTid))
        return writeGuest(machine, argument, request) ? uint64_t(-11) : fault;
    if (directed.empty() && binderCallLostPeer(connection, currentTid)) {
        BinderPacket failed{brDeadReply};
        failed.completesCall = true;
        directed.push_back(std::move(failed));
    }
    auto& queue = directed.empty() ? connection.incoming : directed;
    auto& next = queue.front();
    std::vector<uint32_t> fileNumbers;
    fileNumbers.reserve(next.files.size());
    for (size_t i = 0; i < next.files.size(); ++i) {
        const auto number = freeDescriptor(fileNumbers.empty() ? 0 : fileNumbers.back() + 1);
        if (number == descriptorCapacity) break;
        fileNumbers.push_back(number);
    }
    if (fileNumbers.size() != next.files.size()) {
        notePipeOddity("Binder: no descriptor numbers for " + std::to_string(next.files.size()) + " passed files");
        if (next.reply) {
            if (const auto caller = next.reply->connection.lock()) {
                BinderPacket failed{brFailedReply};
                failed.completesCall = true;
                caller->directed[next.reply->thread].push_back(std::move(failed));
            }
        }
        freeBinderBuffer(connection, next.transaction.buffer, false);
        if (next.command == brReply) {
            next = BinderPacket{brFailedReply};
            next.completesCall = true;
        } else {
            queue.pop_front();
            wakeBinderPollers();
            return writeGuest(machine, argument, request) ? uint64_t(-11) : fault;
        }
    }
    const auto size = (next.command >> 16) & 0x3fffU;
    constexpr size_t maxPayload = sizeof(BinderTransaction) + sizeof(uint64_t);
    std::array<uint8_t, 2 * sizeof(uint32_t) + maxPayload> response{};
    const auto room = request.readSize - request.readConsumed;
    const auto packetSize = sizeof(uint32_t) + size;
    if (size > maxPayload || room < packetSize)
        return writeGuest(machine, argument, request) ? uint64_t(-28) : fault;

    bool requestLooper = false;
    if (connection.maxThreads && !connection.looperRequested && !request.readConsumed &&
        room >= packetSize + sizeof(uint32_t) && connection.loopers.contains(currentTid)) {
        std::erase_if(connection.loopers, [&](const auto& looper) {
            const auto task = threads.find(looper.first);
            return task == threads.end() || task->second.wait == Wait::exited;
        });
        const auto registered = std::ranges::count_if(connection.loopers, [](const auto& looper) { return looper.second; });
        const auto idle = [&](const auto& looper) {
            if (looper.first == currentTid) return false;
            // A poll wake makes a thread runnable before it has taken any Binder work.
            return threads.at(looper.first).idleBinder.lock().get() == &connection;
        };
        requestLooper = registered < connection.maxThreads && std::ranges::none_of(connection.loopers, idle);
    }
    size_t prefix = 0;
    if (requestLooper) {
        std::memcpy(response.data(), &brSpawnLooper, sizeof(brSpawnLooper));
        prefix = sizeof(brSpawnLooper);
    }
    auto* packet = response.data() + prefix;
    std::memcpy(packet, &next.command, sizeof(uint32_t));
    if (next.command == brIncrefs || next.command == brAcquire || next.command == brRelease ||
        next.command == brDecrefs || next.command == brDeadBinder || next.command == brClearDeath)
        std::memcpy(packet + sizeof(uint32_t), next.reference.data(), size);
    else if (size >= sizeof(BinderTransaction))
        std::memcpy(packet + sizeof(uint32_t), &next.transaction, sizeof(BinderTransaction));
    if (size > sizeof(BinderTransaction))
        std::memcpy(packet + sizeof(uint32_t) + sizeof(BinderTransaction), &next.securityContext, sizeof(uint64_t));
    const auto count = prefix + packetSize;
    if (!machine.write(request.readBuffer + request.readConsumed, std::span(response).first(count))) return fault;
    request.readConsumed += count;
    if (!writeGuest(machine, argument, request)) return fault;
    if (requestLooper) connection.looperRequested = true;
    if (size >= sizeof(BinderTransaction)) {
        const auto offset = next.transaction.buffer - connection.address;
        for (size_t i = 0; i < next.files.size(); ++i) {
            auto& file = *next.files[i].file;
            file.flags |= closeOnExec;
            descriptors().emplace(fileNumbers[i], std::move(file));
            auto* field = connection.mapped->pages.data() + offset + next.files[i].offset;
            const uint64_t value = fileNumbers[i];
            std::memcpy(field, &value, next.files[i].narrow ? sizeof(uint32_t) : sizeof(value));
        }
        connection.allocations.at(offset).delivered = true;
    }
    if (next.reply) connection.replies[currentTid].push_back(*next.reply);
    if (next.completesCall) {
        const auto calls = connection.pendingCalls.find(currentTid);
        if (calls != connection.pendingCalls.end()) {
            calls->second.pop_back();
            if (calls->second.empty()) connection.pendingCalls.erase(calls);
        }
    }
    queue.pop_front();
    return 0;
}

uint64_t ProbeLinux::binderIoctl(Machine& machine, const Descriptor& fd) {
    if (fd.pathOnly()) return uint64_t(-9);
    auto& connection = *fd.binder;
    if (connection.owner != processId()) return uint64_t(-1);
    const auto& x = machine.cpu.x;
    switch (x[1]) {
        case 0xc0046209: // BINDER_VERSION
            return writeGuest(machine, x[2], uint32_t{8}) ? 0 : fault;
        case 0x40046205: { // BINDER_SET_MAX_THREADS
            uint32_t limit = 0;
            if (!readGuest(machine, x[2], limit)) return fault;
            if (limit > 256) return invalid;
            connection.maxThreads = limit;
            return 0;
        }
        case 0x4018620d: case 0x40046207: { // BINDER_SET_CONTEXT_MGR_EXT / BINDER_SET_CONTEXT_MGR
            auto& node = binderDevices.at(fd.path);
            const auto& context = current->process->context;
            const bool allowed = (fd.path.ends_with("/binder") && context == "u:r:servicemanager:s0") ||
                                 (fd.path.ends_with("/hwbinder") && context == "u:r:hwservicemanager:s0");
            if (!allowed) return uint64_t(-1);
            if (!node.manager.expired()) return uint64_t(-16);
            if (x[1] == 0x4018620d) {
                struct ManagerObject { uint32_t type, flags; uint64_t ptr, cookie; } value{};
                if (!readGuest(machine, x[2], value)) return fault;
                connection.target = value.ptr;
                connection.cookie = value.cookie;
                connection.sendSecurityContext = value.flags & sendSecurityContext;
                connection.managerAcceptsFiles = value.flags & acceptNodeFiles;
            }
            node.manager = fd.binder;
            wakeBinderPollers();
            return 0;
        }
        case 0x40046208: // BINDER_THREAD_EXIT
            connection.loopers.erase(currentTid);
            connection.directed.erase(currentTid);
            connection.replies.erase(currentTid);
            connection.pendingCalls.erase(currentTid);
            return 0;
        case 0xc0306201: // BINDER_WRITE_READ
            return binderWriteRead(machine, fd);
        default:
            return uint64_t(-25);
    }
}

} // namespace nestvane
