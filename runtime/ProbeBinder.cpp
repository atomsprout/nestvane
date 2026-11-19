#include "ProbeLinux.hpp"

#include <algorithm>
#include <cstring>

namespace nestvane {
namespace {

constexpr uint64_t invalid = uint64_t(-22), fault = uint64_t(-14);
constexpr uint32_t bcTransaction = 0x40406300, bcReply = 0x40406301, bcFreeBuffer = 0x40086303;
constexpr uint32_t brTransaction = 0x80407202, brTransactionSecctx = 0x80487202, brReply = 0x80407203;
constexpr uint32_t brDeadReply = 0x7205, brComplete = 0x7206, brFailedReply = 0x7211;
constexpr uint32_t oneWay = 1, sendSecurityContext = 0x1000;
constexpr uint32_t binderObject = 0x73622a85, binderHandle = 0x73682a85;
constexpr uint32_t brIncrefs = 0x80107207, brAcquire = 0x80107208;
constexpr uint32_t brDeadBinder = 0x8008720f, brClearDeath = 0x80087210;

template <class T>
bool readGuest(Machine& machine, uint64_t address, T& value) {
    const auto* data = machine.read(address, sizeof value, 4);
    if (!data) return false;
    std::memcpy(&value, data, sizeof value);
    return true;
}

template <class T>
bool writeGuest(Machine& machine, uint64_t address, const T& value) {
    return machine.write(address, {reinterpret_cast<const uint8_t*>(&value), sizeof value});
}

} // namespace

bool ProbeLinux::binderReadable(const BinderConnection& connection) const {
    if (connection.owner != processId()) return false;
    const auto directed = connection.directed.find(currentTid);
    return (directed != connection.directed.end() && !directed->second.empty()) ||
           (connection.loopers.contains(currentTid) && !connection.incoming.empty());
}

uint64_t ProbeLinux::storeBinderBuffer(BinderConnection& target, std::span<const uint8_t> data,
                                      std::span<const uint8_t> offsets, std::string_view context) {
    if (!target.mapped) return 0;
    const auto endPayload = (data.size() + size_t{7}) & ~size_t{7};
    const auto endOffsets = endPayload + offsets.size();
    const auto size = (endOffsets + (context.empty() ? 0 : context.size() + 1) + size_t{7}) & ~size_t{7};
    const auto needed = std::max(size_t{8}, size);
    uint64_t offset = 0;
    for (const auto& [start, buffer] : target.allocations) {
        if (start - offset >= needed) break;
        offset = start + buffer.size;
    }
    if (needed > target.mapped->size || offset > target.mapped->size - needed) return 0;
    target.allocations.emplace(offset, BinderAllocation{needed, {}});
    auto begin = target.mapped->pages.begin() + offset;
    std::fill_n(begin, needed, 0);
    std::copy(data.begin(), data.end(), begin);
    std::copy(offsets.begin(), offsets.end(), begin + endPayload);
    std::copy(context.begin(), context.end(), begin + endOffsets);
    return target.address + offset;
}

bool ProbeLinux::refreshBinderDeaths() {
    bool queued = false;
    std::erase_if(binderDeathSubscribers, [&](const auto& subscriber) {
        const auto connection = subscriber.lock();
        if (!connection) return true;
        for (auto& [handle, death] : connection->deaths) {
            if (death.sent) continue;
            const auto target = death.target.lock();
            if (target && std::any_of(threads.begin(), threads.end(), [&](const auto& task) {
                return task.second.process->id == target->owner && task.second.wait != Wait::exited;
            })) continue;
            const auto receiver = threads.find(death.thread);
            auto& queue = receiver != threads.end() && receiver->second.wait != Wait::exited
                ? connection->directed[death.thread] : connection->incoming;
            if (queue.size() >= 32) continue;
            BinderPacket packet{brDeadBinder};
            packet.reference[0] = death.cookie;
            queue.push_back(packet);
            death.sent = true;
            queued = true;
        }
        return connection->deaths.empty();
    });
    if (queued) wakePollers();
    return queued;
}

uint64_t ProbeLinux::binderCommand(Machine& machine, const Descriptor& fd, uint32_t command,
                                  std::span<const uint8_t> data) {
    static_assert(sizeof(BinderTransaction) == 64);
    auto& connection = *fd.binder;
    switch (command) {
        case 0x630b: case 0x630c: // BC_REGISTER_LOOPER / BC_ENTER_LOOPER
            connection.loopers.insert(currentTid);
            return 0;
        case 0x630d: // BC_EXIT_LOOPER
            connection.loopers.erase(currentTid);
            return 0;
        case bcFreeBuffer: {
            uint64_t address = 0;
            std::memcpy(&address, data.data(), sizeof address);
            if (address < connection.address) return invalid;
            const auto buffer = connection.allocations.find(address - connection.address);
            if (buffer == connection.allocations.end()) return invalid;
            for (const auto handle : buffer->second.handles) {
                const auto it = connection.handles.find(handle);
                auto& ref = it->second;
                if (!--ref.inBuffer && !ref.strong && !ref.weak) connection.handles.erase(it);
            }
            connection.allocations.erase(buffer);
            return 0;
        }
        case 0x40046304: case 0x40046305: case 0x40046306: case 0x40046307: {
            uint32_t handle = 0;
            std::memcpy(&handle, data.data(), sizeof handle);
            if (!handle) return 0; // Handle zero is tied to the context manager.
            const auto it = connection.handles.find(handle);
            if (it == connection.handles.end()) return invalid;
            auto& ref = it->second;
            auto& count = command == 0x40046304 || command == 0x40046307 ? ref.weak : ref.strong;
            const bool increment = command == 0x40046304 || command == 0x40046305;
            if (increment ? count == UINT32_MAX : !count) return invalid;
            if (increment) ++count;
            else if (!--count && !ref.inBuffer && !ref.strong && !ref.weak) connection.handles.erase(it);
            return 0;
        }
        case 0x40106308: case 0x40106309: { // BC_INCREFS_DONE / BC_ACQUIRE_DONE
            std::array<uint64_t, 2> pointerCookie{};
            std::memcpy(pointerCookie.data(), data.data(), sizeof pointerCookie);
            const auto node = connection.localNodes.find(pointerCookie[0]);
            return node != connection.localNodes.end() && node->second->object.cookie == pointerCookie[1]
                ? 0 : invalid;
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
            }
            return 0;
        }
        case bcTransaction: case bcReply:
            break;
        default:
            return invalid;
    }
    auto& notices = connection.directed[currentTid];
    if (notices.size() >= 64) return uint64_t(-28);
    const auto fail = [&](uint32_t status) {
        notices.push_back({status});
        return uint64_t{0};
    };
    BinderTransaction transaction;
    std::memcpy(&transaction, data.data(), sizeof transaction);
    if (transaction.dataSize > 64 * 1024 || transaction.offsetsSize % sizeof(uint64_t) ||
        transaction.offsetsSize > 64 * sizeof(uint64_t)) return fail(brFailedReply);
    const auto* bytes = transaction.dataSize ? machine.read(transaction.buffer, transaction.dataSize, 4) : nullptr;
    if (transaction.dataSize && !bytes) return fail(brFailedReply);
    const auto* offsets = transaction.offsetsSize ? machine.read(transaction.offsets, transaction.offsetsSize, 4) : nullptr;
    if (transaction.offsetsSize && !offsets) return fail(brFailedReply);
    const bool replying = command == bcReply;
    auto& outstanding = connection.replies[currentTid];
    std::shared_ptr<BinderConnection> target;
    uint64_t replyThread = 0;
    uint64_t targetPointer = 0, targetCookie = 0;
    bool sendLabel = false;
    if (replying) {
        if (outstanding.empty()) return fail(brFailedReply);
        auto route = outstanding.back();
        target = route.connection.lock();
        replyThread = route.thread;
    } else if (uint32_t(transaction.target) == 0) {
        target = binderDevices.at(fd.path).manager.lock();
        if (target) {
            targetPointer = target->target;
            targetCookie = target->cookie;
            sendLabel = target->sendSecurityContext;
        }
    } else {
        const auto ref = connection.handles.find(uint32_t(transaction.target));
        if (ref == connection.handles.end()) return fail(brFailedReply);
        const auto& node = *ref->second.node;
        target = node.owner.lock();
        targetPointer = node.object.value;
        targetCookie = node.object.cookie;
        sendLabel = node.object.flags & sendSecurityContext;
    }
    if (!target) {
        if (replying) outstanding.pop_back();
        return fail(brDeadReply);
    }
    auto& destination = replying ? target->directed[replyThread] : target->incoming;
    if (destination.size() >= 32) return fail(brFailedReply);
    const auto label = sendLabel ? std::string_view(current->process->context) : std::string_view{};

    struct Translation {
        size_t offset;
        BinderObject object;
        uint32_t handle;
    };
    std::vector<Translation> translations;
    std::map<uint64_t, std::shared_ptr<BinderNode>> newNodes;
    std::map<const BinderNode*, uint32_t> knownHandles;
    std::map<uint32_t, BinderReference> newHandles;
    if (transaction.offsetsSize) {
        for (const auto& [handle, ref] : target->handles) knownHandles.emplace(ref.node.get(), handle);
    }
    size_t previousEnd = 0;
    auto nextHandle = target->nextHandle;
    for (size_t index = 0; index < transaction.offsetsSize; index += sizeof(uint64_t)) {
        uint64_t offset = 0;
        std::memcpy(&offset, offsets + index, sizeof offset);
        if (offset % 4 || offset < previousEnd || offset > transaction.dataSize ||
            sizeof(BinderObject) > transaction.dataSize - offset) return fail(brFailedReply);
        previousEnd = offset + sizeof(BinderObject);
        BinderObject object{};
        std::memcpy(&object, bytes + offset, sizeof object);
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
            if (node->object.cookie != object.cookie || connection.localNodes.size() + newNodes.size() > 256)
                return fail(brFailedReply);
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
            if (!nextHandle || target->handles.size() + newHandles.size() >= 256) return fail(brFailedReply);
            knownHandles.emplace(node.get(), nextHandle);
            newHandles.emplace(nextHandle++, BinderReference{node});
        }
        const auto handle = knownHandles.at(node.get());
        translations.push_back({size_t(offset), {binderHandle, object.flags, handle, 0}, handle});
    }
    if (notices.size() + 2 * newNodes.size() + 1 > 64) return fail(brFailedReply);
    const auto address = storeBinderBuffer(*target, {bytes, size_t(transaction.dataSize)},
                                          {offsets, size_t(transaction.offsetsSize)}, label);
    if (!address) return fail(brFailedReply);
    target->nextHandle = nextHandle;
    target->handles.merge(newHandles);
    auto& allocation = target->allocations.at(address - target->address);
    for (const auto& item : translations) {
        auto* destination = target->mapped->pages.data() + (address - target->address) + item.offset;
        std::memcpy(destination, &item.object, sizeof item.object);
        if (item.handle) {
            ++target->handles.at(item.handle).inBuffer;
            allocation.handles.push_back(item.handle);
        }
    }
    for (const auto& [pointer, node] : newNodes) {
        if (target == fd.binder) continue;
        BinderPacket weak{brIncrefs}, strong{brAcquire};
        weak.reference = strong.reference = {pointer, node->object.cookie};
        notices.push_back(weak);
        notices.push_back(strong);
    }
    connection.localNodes.merge(newNodes);
    transaction.target = targetPointer;
    transaction.cookie = targetCookie;
    transaction.senderPid = replying || (transaction.flags & oneWay) ? 0 : processId();
    transaction.senderUid = userId();
    transaction.buffer = address;
    transaction.offsets = address + ((transaction.dataSize + 7) & ~uint64_t{7});
    const auto opcode = replying ? brReply : label.empty() ? brTransaction : brTransactionSecctx;
    BinderPacket packet{opcode, transaction, label.empty() ? 0 : transaction.offsets + transaction.offsetsSize};
    if (!replying && !(transaction.flags & oneWay)) packet.reply = BinderRoute{fd.binder, currentTid};
    destination.push_back(packet);
    if (binderTrace.size() < 8192) {
        binderTrace += "  guest binder " + std::string(replying ? "reply" : "transaction") +
            " sender=" + std::to_string(processId()) + " target=" + std::to_string(target->owner) +
            " code=" + std::to_string(transaction.code) + " objects=" +
            std::to_string(transaction.offsetsSize / sizeof(uint64_t)) + "\n";
    }
    if (replying) outstanding.pop_back();
    notices.push_back({brComplete});
    wakePollers();
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
    while (request.writeConsumed < request.writeSize) {
        const auto position = request.writeBuffer + request.writeConsumed;
        const auto remaining = request.writeSize - request.writeConsumed;
        uint32_t command = 0;
        if (remaining < sizeof command) return invalid;
        if (!readGuest(machine, position, command)) return fault;
        const auto size = (command >> 16) & 0x3fff;
        if (remaining < sizeof command + size) return invalid;
        const auto* data = size ? machine.read(position + sizeof command, size, 4) : nullptr;
        if (size && !data) return fault;
        if (const auto error = binderCommand(machine, fd, command, {data, size}); error)
            return writeGuest(machine, argument, request) ? error : fault;
        request.writeConsumed += sizeof command + size;
    }

    auto& connection = *fd.binder;
    if (!request.readSize) return writeGuest(machine, argument, request) ? 0 : fault;
    if (!binderReadable(connection)) {
        return writeGuest(machine, argument, request) ? uint64_t(-11) : fault;
    }
    auto& directed = connection.directed[currentTid];
    auto& queue = directed.empty() ? connection.incoming : directed;
    const auto& next = queue.front();
    const auto size = (next.command >> 16) & 0x3fffU;
    std::array<uint8_t, sizeof(uint32_t) + sizeof(BinderTransaction) + sizeof(uint64_t)> response{};
    if (size > response.size() - sizeof(uint32_t) ||
        request.readSize - request.readConsumed < sizeof(uint32_t) + size)
        return writeGuest(machine, argument, request) ? uint64_t(-28) : fault;
    std::memcpy(response.data(), &next.command, sizeof(uint32_t));
    if (next.command == brIncrefs || next.command == brAcquire ||
        next.command == brDeadBinder || next.command == brClearDeath)
        std::memcpy(response.data() + sizeof(uint32_t), next.reference.data(), size);
    else if (size >= sizeof(BinderTransaction))
        std::memcpy(response.data() + sizeof(uint32_t), &next.transaction, sizeof(BinderTransaction));
    if (size > sizeof(BinderTransaction))
        std::memcpy(response.data() + sizeof(uint32_t) + sizeof(BinderTransaction),
                    &next.securityContext, sizeof(uint64_t));
    const auto count = sizeof(uint32_t) + size;
    if (!machine.write(request.readBuffer + request.readConsumed, std::span(response).first(count))) return fault;
    request.readConsumed += count;
    if (!writeGuest(machine, argument, request)) return fault;
    if (next.reply) connection.replies[currentTid].push_back(*next.reply);
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
                connection.sendSecurityContext = value.flags & 0x1000;
            }
            node.manager = fd.binder;
            wakePollers();
            return 0;
        }
        case 0x40046208: // BINDER_THREAD_EXIT
            connection.loopers.erase(currentTid);
            connection.directed.erase(currentTid);
            connection.replies.erase(currentTid);
            return 0;
        case 0xc0306201: // BINDER_WRITE_READ
            return binderWriteRead(machine, fd);
        default:
            return uint64_t(-25);
    }
}

} // namespace nestvane
