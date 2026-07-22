#include <esp_attr.h>

#include <freertos/FreeRTOS.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace {

constexpr size_t kRxBytes = 1440;
constexpr size_t kRxSlots = 2;

DRAM_ATTR uint8_t s_rx_buffers[kRxSlots][kRxBytes];
bool s_rx_in_use[kRxSlots] = {};
portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;

static int claimSlot()
{
    int slot = -1;
    portENTER_CRITICAL(&s_rx_lock);
    for (size_t i = 0; i < kRxSlots; ++i) {
        if (!s_rx_in_use[i]) {
            s_rx_in_use[i] = true;
            slot = static_cast<int>(i);
            break;
        }
    }
    portEXIT_CRITICAL(&s_rx_lock);
    return slot;
}

static void releaseSlot(const int slot)
{
    portENTER_CRITICAL(&s_rx_lock);
    s_rx_in_use[slot] = false;
    portEXIT_CRITICAL(&s_rx_lock);
}

} // namespace

// ESP-lwIP's readv()/recvmsg() scatter path can stall while libsmb2 receives a
// long non-blocking READ reply into a final iovec backed by PSRAM. CMake maps
// only libsmb2's readv calls to this function. TCP still owns segmentation,
// flow control and retransmission; this shim only scatters one received MSS.
extern "C" ssize_t NRL_SMB_Readv(const int fd, const struct iovec *iov,
                                 const int iovcnt)
{
    if (iov == nullptr || iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }

    const int slot = claimSlot();
    if (slot < 0) {
        errno = EAGAIN;
        return -1;
    }

    size_t wanted = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len > kRxBytes - wanted) {
            wanted = kRxBytes;
            break;
        }
        wanted += iov[i].iov_len;
    }

    const ssize_t received = recv(fd, s_rx_buffers[slot], wanted, 0);
    if (received > 0) {
        size_t copied = 0;
        for (int i = 0; i < iovcnt && copied < static_cast<size_t>(received);
             ++i) {
            size_t chunk = iov[i].iov_len;
            const size_t remaining = static_cast<size_t>(received) - copied;
            if (chunk > remaining) chunk = remaining;
            memcpy(iov[i].iov_base, s_rx_buffers[slot] + copied, chunk);
            copied += chunk;
        }
    }

    releaseSlot(slot);
    return received;
}
