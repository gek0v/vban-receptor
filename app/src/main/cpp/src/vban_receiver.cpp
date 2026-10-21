#include "vban_receiver.hpp"

#include <cstring>
#include <chrono>
#include <cmath>
#include <array>
#include <iostream>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #if defined(_MSC_VER)
    #pragma comment(lib, "ws2_32.lib")
    #endif

    using socket_t = SOCKET;
    inline constexpr socket_t INVALID_SOCKET_VAL = INVALID_SOCKET;
    using sockopt_len_t = int;
    using sockopt_val_t = const char*;
    using sockopt_mut_val_t = char*;

    #define CLOSESOCKET(s) ::closesocket(s)
    #define SHUTDOWNSOCKET(s) ::shutdown(s, SD_BOTH)
    #define GET_SOCKET_ERR() WSAGetLastError()
    #define IS_TIMEOUT_ERR(e) ((e) == WSAETIMEDOUT || (e) == WSAEWOULDBLOCK)

    inline int vban_inet_pton(int af, const char* src, void* dst) {
        return InetPtonA(af, src, dst);
    }

    #define LOGI(...) do {} while(0)
    #define LOGE(...) do {} while(0)
    #define LOGW(...) do {} while(0)
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>

    #if defined(__ANDROID__)
    #include <android/log.h>
    #include <sys/resource.h>
    #include <pthread.h>
    #define LOG_TAG "VbanReceiver"
    #define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
    #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
    #define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
    #else
    #define LOGI(...) do {} while(0)
    #define LOGE(...) do {} while(0)
    #define LOGW(...) do {} while(0)
    #endif

    using socket_t = int;
    inline constexpr socket_t INVALID_SOCKET_VAL = -1;
    using sockopt_len_t = socklen_t;
    using sockopt_val_t = const void*;
    using sockopt_mut_val_t = void*;

    #define CLOSESOCKET(s) ::close(s)
    #define SHUTDOWNSOCKET(s) ::shutdown(s, SHUT_RDWR)
    #define GET_SOCKET_ERR() (errno)
    #define IS_TIMEOUT_ERR(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)

    inline int vban_inet_pton(int af, const char* src, void* dst) {
        return ::inet_pton(af, src, dst);
    }
#endif

namespace vban {

namespace {

void ensureSocketSubsystemInit() {
#if defined(_WIN32)
    static struct WinsockInit {
        WinsockInit() {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
        }
        ~WinsockInit() {
            WSACleanup();
        }
    } s_init;
#endif
}

} // anonymous namespace

VbanReceiver::VbanReceiver(
    SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& ringBuffer,
    ReconfigureCallback reconfigCb
)
    : m_ringBuffer(ringBuffer),
      m_reconfigCb(std::move(reconfigCb)) {
    ensureSocketSubsystemInit();
}

VbanReceiver::~VbanReceiver() {
    stop();
}

bool VbanReceiver::isMulticastAddress(std::string_view ip) noexcept {
    if (ip.empty()) {
        return false;
    }

    in_addr addr{};
    std::string ipStr(ip);
    if (vban_inet_pton(AF_INET, ipStr.c_str(), &addr) <= 0) {
        return false;
    }

    // Class D: 224.0.0.0/4 -> 0xE0000000 to 0xEFFFFFFF
    const uint32_t hostAddr = ntohl(addr.s_addr);
    return (hostAddr >= 0xE0000000U) && (hostAddr <= 0xEFFFFFFFU);
}

bool VbanReceiver::openSocket(const ReceiverSocketConfig& config) {
    closeSocket();

    // 1. Create UDP datagram socket
    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET_VAL) {
        LOGE("Failed to create UDP socket: error %d", GET_SOCKET_ERR());
        return false;
    }

    // 2. Set SO_REUSEADDR
    int reuseAddr = 1;
    if (::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<sockopt_val_t>(&reuseAddr),
                     sizeof(reuseAddr)) != 0) {
        LOGW("Warning: setsockopt SO_REUSEADDR failed: %d", GET_SOCKET_ERR());
    }

    // 3. Set SO_REUSEPORT where supported (Linux/Android)
#if defined(SO_REUSEPORT)
    int reusePort = 1;
    if (::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
                     reinterpret_cast<sockopt_val_t>(&reusePort),
                     sizeof(reusePort)) != 0) {
        LOGW("Warning: setsockopt SO_REUSEPORT failed: %d", GET_SOCKET_ERR());
    }
#endif

    // 4. Set SO_RCVBUF with descending fallback ladder
    constexpr std::array<int, 5> BUFFER_LADDER = {
        4 * 1024 * 1024, // 4 MB target
        2 * 1024 * 1024, // 2 MB
        1 * 1024 * 1024, // 1 MB
        512 * 1024,      // 512 KB
        256 * 1024       // 256 KB
    };

    int targetBuf = static_cast<int>(config.socketBufferSize);
    if (targetBuf <= 0) targetBuf = BUFFER_LADDER[0];

    bool bufSet = false;
    if (::setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<sockopt_val_t>(&targetBuf),
                     sizeof(targetBuf)) == 0) {
        bufSet = true;
    } else {
        for (int fallback : BUFFER_LADDER) {
            if (::setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                             reinterpret_cast<sockopt_val_t>(&fallback),
                             sizeof(fallback)) == 0) {
                bufSet = true;
                break;
            }
        }
    }

    // Query granted buffer size via getsockopt
    int actualBuf = 0;
    sockopt_len_t optLen = sizeof(actualBuf);
    if (::getsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<sockopt_mut_val_t>(&actualBuf),
                     &optLen) == 0) {
        m_grantedBufferSize = static_cast<uint32_t>(actualBuf);
        LOGI("SO_RCVBUF granted size: %u bytes (requested %zu)", m_grantedBufferSize, config.socketBufferSize);
    } else {
        m_grantedBufferSize = bufSet ? static_cast<uint32_t>(targetBuf) : 0;
    }

    // 5. Set SO_RCVTIMEO (100 ms timeout to allow clean thread cancellation)
    const uint32_t timeoutMs = config.receiveTimeoutMs > 0 ? config.receiveTimeoutMs : 100;
#if defined(_WIN32)
    DWORD tv = timeoutMs;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeoutMs / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const void*>(&tv), sizeof(tv));
#endif

    // 6. Bind to port and address
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(config.port);

    const bool isMcast = isMulticastAddress(config.listenIp);
    if (isMcast || config.listenIp.empty() || config.listenIp == "0.0.0.0") {
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (vban_inet_pton(AF_INET, config.listenIp.c_str(), &bindAddr.sin_addr) <= 0) {
            LOGE("Invalid listen IP address: %s", config.listenIp.c_str());
            CLOSESOCKET(sock);
            return false;
        }
    }

    if (::bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        // If binding to specific IP failed (e.g. EADDRNOTAVAIL because config.listenIp is a
        // remote transmitter/sender IP), fallback to INADDR_ANY so unicast reception continues.
        LOGW("Failed to bind UDP socket to %s:%u (error %d), falling back to INADDR_ANY",
             config.listenIp.c_str(), config.port, GET_SOCKET_ERR());
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            LOGE("Failed to bind UDP socket to INADDR_ANY:%u (error %d)",
                 config.port, GET_SOCKET_ERR());
            CLOSESOCKET(sock);
            return false;
        }
    }

    // 7. Join multicast group if applicable
    if (isMcast) {
        struct ip_mreq mreq{};
        if (vban_inet_pton(AF_INET, config.listenIp.c_str(), &mreq.imr_multiaddr) <= 0) {
            LOGE("Failed to parse multicast address for ip_mreq: %s", config.listenIp.c_str());
            CLOSESOCKET(sock);
            return false;
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);

        if (::setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         reinterpret_cast<sockopt_val_t>(&mreq),
                         sizeof(mreq)) != 0) {
            LOGW("Failed to join multicast group %s (error %d)",
                 config.listenIp.c_str(), GET_SOCKET_ERR());
        } else {
            std::memcpy(m_joinedMreq, &mreq, sizeof(mreq));
            m_isMulticastJoined = true;
            LOGI("Joined IPv4 multicast group %s successfully", config.listenIp.c_str());
        }
    }

    m_socketFd.store(static_cast<intptr_t>(sock), std::memory_order_release);
    return true;
}

void VbanReceiver::closeSocket() {
    const intptr_t oldFd = m_socketFd.exchange(-1, std::memory_order_acq_rel);
    if (oldFd != -1) {
        socket_t sock = static_cast<socket_t>(oldFd);

        if (m_isMulticastJoined) {
            ::setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                         reinterpret_cast<sockopt_val_t>(m_joinedMreq),
                         sizeof(m_joinedMreq));
            m_isMulticastJoined = false;
        }

        SHUTDOWNSOCKET(sock);
        CLOSESOCKET(sock);
    }
}

bool VbanReceiver::start(const ReceiverSocketConfig& config) {
    if (m_running.load(std::memory_order_acquire)) {
        LOGW("VbanReceiver already running");
        return true;
    }

    m_config = config;
    m_parser.setTargetStreamName(config.streamName);
    {
        std::lock_guard<std::mutex> lock(m_streamNameMutex);
        m_detectedStreamName = config.streamName;
    }
    m_workerStreamName = config.streamName;
    m_parser.reset();
    m_reorderQueue.reset();
    m_activeSampleRate.store(0, std::memory_order_relaxed);
    m_activeChannels.store(0, std::memory_order_relaxed);
    m_activeBitDepth.store(0, std::memory_order_relaxed);
    m_totalBytesReceived.store(0, std::memory_order_relaxed);
    m_overflowFrames.store(0, std::memory_order_relaxed);
    m_peakLeft.store(0.0f, std::memory_order_relaxed);
    m_peakRight.store(0.0f, std::memory_order_relaxed);
    m_isMulticastJoined = false;

    if (!openSocket(config)) {
        return false;
    }

    m_running.store(true, std::memory_order_release);
    m_workerThread = std::thread(&VbanReceiver::receiveLoop, this);
    LOGI("VbanReceiver worker thread launched on port %u", config.port);
    return true;
}

void VbanReceiver::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return; // Already stopped
    }

    LOGI("Stopping VbanReceiver worker thread...");
    closeSocket(); // Unblocks recvfrom immediately

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    LOGI("VbanReceiver stopped successfully");
}

bool VbanReceiver::isRunning() const noexcept {
    return m_running.load(std::memory_order_acquire);
}

void VbanReceiver::setReconfigureCallback(ReconfigureCallback cb) {
    m_reconfigCb = std::move(cb);
}

bool VbanReceiver::processDatagram(const uint8_t* buffer, size_t size, uint64_t arrivalTimeNs) noexcept {
    if (!buffer || size < VBAN_HEADER_SIZE) [[unlikely]] {
        return false;
    }

    ParsedPacket packet{};
    const ParseResult result = m_parser.parse(buffer, size, arrivalTimeNs, packet);
    if (result != ParseResult::Success) [[unlikely]] {
        return false;
    }

    if (!packet.streamName.empty() && m_workerStreamName != packet.streamName) {
        m_workerStreamName = std::string(packet.streamName);
        std::lock_guard<std::mutex> lock(m_streamNameMutex);
        m_detectedStreamName = m_workerStreamName;
    }

    // 1. Dynamic stream format change detection
    const uint32_t incomingRate = packet.sampleRate;
    const uint16_t incomingChannels = packet.channels;
    const uint16_t incomingBitDepth = static_cast<uint16_t>(packet.sampleSize * 8);
    const uint32_t activeRate = m_activeSampleRate.load(std::memory_order_relaxed);
    const uint16_t activeChannels = m_activeChannels.load(std::memory_order_relaxed);
    const uint16_t activeBitDepth = m_activeBitDepth.load(std::memory_order_relaxed);

    if (incomingBitDepth != activeBitDepth) [[unlikely]] {
        m_activeBitDepth.store(incomingBitDepth, std::memory_order_release);
    }

    if (incomingRate != activeRate || incomingChannels != activeChannels) [[unlikely]] {
        const bool isInitialStream = (activeRate == 0 && activeChannels == 0);
        m_activeSampleRate.store(incomingRate, std::memory_order_release);
        m_activeChannels.store(incomingChannels, std::memory_order_release);

        m_reorderQueue.reset();

        if (m_reconfigCb && (!isInitialStream || incomingRate != 48000 || incomingChannels != 2)) {
            m_reconfigCb(incomingRate, incomingChannels);
        }
    }

    // 2. Audio Dequantization to Float32 [-1.0f, +1.0f]
    const size_t totalSamples = static_cast<size_t>(packet.samplesPerFrame) * packet.channels;
    if (totalSamples > STAGING_MAX_SAMPLES) [[unlikely]] {
        return false;
    }

    const bool converted = convertVbanPayloadToFloat(
        static_cast<uint8_t>(packet.dataType),
        packet.payload,
        m_stagingBuffer.data(),
        totalSamples
    );

    if (!converted) [[unlikely]] {
        return false;
    }

    // 3. Push dequantized samples into UDP Packet Reorder & PLC Queue
    m_reorderQueue.push(packet.frameNumber, m_stagingBuffer.data(), totalSamples);

    // 4. Pop ordered samples from Reorder Queue and write to SPSC Ring Buffer
    constexpr float decayFactor = 0.95f;
    const uint16_t ch = incomingChannels;

    bool wasPlc = false;
    size_t popped = 0;
    while ((popped = m_reorderQueue.popOrdered(m_reorderPopBuffer.data(), m_reorderPopBuffer.size(), wasPlc)) > 0) {
        // Peak level calculation for telemetry
        const float prevL = m_peakLeft.load(std::memory_order_relaxed);
        const float prevR = m_peakRight.load(std::memory_order_relaxed);
        const size_t frames = (ch > 0) ? (popped / ch) : 0;

        if (ch >= 2) {
            float peakL = 0.0f;
            float peakR = 0.0f;
            calculatePeakLevelsStereo(m_reorderPopBuffer.data(), frames, peakL, peakR);
            m_peakLeft.store(std::max(peakL, prevL * decayFactor), std::memory_order_relaxed);
            m_peakRight.store(std::max(peakR, prevR * decayFactor), std::memory_order_relaxed);
        } else if (ch == 1) {
            float maxMono = 0.0f;
            for (size_t i = 0; i < popped; ++i) {
                float v = std::abs(m_reorderPopBuffer[i]);
                if (v > maxMono) maxMono = v;
            }
            m_peakLeft.store(std::max(maxMono, prevL * decayFactor), std::memory_order_relaxed);
            m_peakRight.store(std::max(maxMono, prevR * decayFactor), std::memory_order_relaxed);
        }

        // Write interleaved Float32 whole frames to Lock-Free SPSC Ring Buffer
        const size_t available = m_ringBuffer.availableWrite();
        const size_t maxWholeFrames = (ch > 0) ? (available / ch) : 0;
        const size_t samplesToWrite = (ch > 0) ? std::min(popped, maxWholeFrames * ch) : 0;

        if (samplesToWrite > 0) {
            m_ringBuffer.write(m_reorderPopBuffer.data(), samplesToWrite);
        }

        // Overrun / Overflow Accounting
        if (samplesToWrite < popped) [[unlikely]] {
            const size_t droppedFrames = (ch > 0) ? ((popped - samplesToWrite) / ch) : 0;
            m_overflowFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
        }
    }

    return true;
}

bool VbanReceiver::processDatagram(const uint8_t* buffer, size_t size) noexcept {
    const auto arrivalTime = std::chrono::steady_clock::now();
    const auto arrivalNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            arrivalTime.time_since_epoch()).count()
    );
    return processDatagram(buffer, size, arrivalNs);
}

void VbanReceiver::receiveLoop() {
#if defined(__ANDROID__)
    // Elevate priority to Linux CFS nice -16 (Urgent Audio)
    setpriority(PRIO_PROCESS, 0, -16);
    pthread_setname_np(pthread_self(), "VbanNetWorker");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "VbanNetWorker");
#endif

    LOGI("Network worker thread entering receive loop");

    sockaddr_in srcAddr{};
    sockopt_len_t srcLen = sizeof(srcAddr);

    while (m_running.load(std::memory_order_relaxed)) {
        const intptr_t currentFd = m_socketFd.load(std::memory_order_acquire);
        if (currentFd == -1) {
            break;
        }
        socket_t sock = static_cast<socket_t>(currentFd);

        srcLen = sizeof(srcAddr);
#if defined(_WIN32)
        int bytesRead = ::recvfrom(
            sock,
            reinterpret_cast<char*>(m_rxBuffer.data()),
            static_cast<int>(m_rxBuffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&srcAddr),
            &srcLen
        );
#else
        ssize_t bytesRead = ::recvfrom(
            sock,
            m_rxBuffer.data(),
            m_rxBuffer.size(),
            0,
            reinterpret_cast<sockaddr*>(&srcAddr),
            &srcLen
        );
#endif

        if (bytesRead > 0) {
            const auto arrivalTime = std::chrono::steady_clock::now();
            const auto arrivalNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    arrivalTime.time_since_epoch()).count()
            );

            m_totalBytesReceived.fetch_add(static_cast<uint64_t>(bytesRead), std::memory_order_relaxed);
            processDatagram(m_rxBuffer.data(), static_cast<size_t>(bytesRead), arrivalNs);
        } else if (bytesRead < 0) {
            const int err = GET_SOCKET_ERR();
            if (IS_TIMEOUT_ERR(err)) {
                continue; // Normal timeout, check running flag
            }
            if (!m_running.load(std::memory_order_relaxed)) {
                break;
            }
        }
    }

    LOGI("Network worker thread exiting receive loop cleanly");
}

ReceiverTelemetry VbanReceiver::getTelemetry() const noexcept {
    const auto parserTelem = m_parser.getTelemetry();
    ReceiverTelemetry telem;
    telem.totalBytesReceived = m_totalBytesReceived.load(std::memory_order_relaxed);
    telem.totalPacketsReceived = parserTelem.totalPacketsReceived;
    telem.validPackets = parserTelem.validPackets;
    telem.lostPackets = parserTelem.lostPackets;
    telem.outOfOrderPackets = parserTelem.outOfOrderPackets;
    telem.overflowFrames = m_overflowFrames.load(std::memory_order_relaxed);
    telem.jitterMs = static_cast<float>(parserTelem.jitterMs);
    telem.activeSampleRate = m_activeSampleRate.load(std::memory_order_relaxed);
    telem.activeChannels = m_activeChannels.load(std::memory_order_relaxed);
    telem.activeBitDepth = m_activeBitDepth.load(std::memory_order_relaxed);
    telem.grantedBufferSize = m_grantedBufferSize;
    telem.isMulticast = m_isMulticastJoined;
    telem.isRunning = m_running.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_streamNameMutex);
        std::strncpy(telem.activeStreamName, m_detectedStreamName.c_str(), 16);
    }
    return telem;
}

} // namespace vban
