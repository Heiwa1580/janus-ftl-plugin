/**
 * @file FtlStream.cpp
 * @author Hayden McAfee (hayden@outlook.com)
 * @version 0.1
 * @date 2020-08-11
 * 
 * @copyright Copyright (c) 2020 Hayden McAfee
 * 
 */

#include "FtlExceptions.h"
#include "FtlStream.h"
#include "JanusSession.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <poll.h>
extern "C"
{
    #include <unistd.h>
    #include <debug.h>
    #include <sys/time.h>
    #include <rtcp.h>
    #include <utils.h>
}

#pragma region Constructor/Destructor
FtlStream::FtlStream(
    const std::shared_ptr<IngestConnection> ingestConnection,
    const uint16_t mediaPort,
    const std::shared_ptr<RelayThreadPool> relayThreadPool,
    const std::shared_ptr<ServiceConnection> serviceConnection,
    const uint16_t metadataReportIntervalMs,
    const std::string myHostname) : 
    ingestConnection(ingestConnection),
    mediaPort(mediaPort),
    relayThreadPool(relayThreadPool),
    serviceConnection(serviceConnection),
    metadataReportIntervalMs(metadataReportIntervalMs),
    myHostname(myHostname)
{
    // Bind to ingest callbacks
    ingestConnection->SetOnClosed(std::bind(
        &FtlStream::ingestConnectionClosed,
        this,
        std::placeholders::_1));
}
#pragma endregion

#pragma region Public methods
void FtlStream::Start()
{
    // Mark our start time
    streamStartTime = std::time(nullptr);

    // Initialize PreviewGenerator
    switch (GetVideoCodec())
    {
    case VideoCodecKind::H264:
        previewGenerator = std::make_unique<H264PreviewGenerator>();
        break;
    case VideoCodecKind::Unsupported:
    default:
        break;
    }

    // Start listening for incoming packets
    streamThread = std::thread(&FtlStream::startStreamThread, this);
    streamThread.detach();

    // Start thread for reporting stream metadata out
    streamMetadataReportingThread = 
        std::thread(&FtlStream::startStreamMetadataReportingThread, this);
    streamMetadataReportingThread.detach();
}

void FtlStream::Stop()
{
    // Stop the ingest connection, which will end up reporting closed to us
    ingestConnection->Stop();
    
    // Join our outstanding threads
    if (streamThread.joinable())
    {
        streamThread.join();
    }
    if (streamMetadataReportingThread.joinable())
    {
        streamMetadataReportingThread.join();
    }
}

void FtlStream::AddViewer(std::shared_ptr<JanusSession> viewerSession)
{
    std::lock_guard<std::mutex> lock(viewerSessionsMutex);
    viewerSessions.push_back(viewerSession);
}

void FtlStream::RemoveViewer(std::shared_ptr<JanusSession> viewerSession)
{
    std::lock_guard<std::mutex> lock(viewerSessionsMutex);
    std::lock_guard<std::mutex> keyframeLock(keyframeMutex);
    viewerSessions.erase(
        std::remove(viewerSessions.begin(), viewerSessions.end(), viewerSession),
        viewerSessions.end());
    keyframeSentToViewers.erase(viewerSession);
}

void FtlStream::SetOnClosed(std::function<void (FtlStream&)> callback)
{
    onClosed = callback;
}

void FtlStream::SendKeyframeToViewer(std::shared_ptr<JanusSession> viewerSession)
{
    std::lock_guard<std::mutex> keyframeLock(keyframeMutex);
    if (keyframe.rtpPackets.size() > 0)
    {
        if (keyframeSentToViewers.count(viewerSession) == 0)
        {
            JANUS_LOG(
                LOG_INFO,
                "FTL: Channel %u sending %lu keyframe packets to viewer.\n",
                GetChannelId(),
                keyframe.rtpPackets.size());
            for (const auto& packet : keyframe.rtpPackets)
            {
                viewerSession->SendRtpPacket(
                {
                    .rtpPacketPayload = packet,
                    .type = RtpRelayPacketKind::Video,
                    .channelId = GetChannelId()
                });
            }
            keyframeSentToViewers.insert(viewerSession);
        }
    }
}
#pragma endregion

#pragma region Getters/Setters
ftl_channel_id_t FtlStream::GetChannelId()
{
    return ingestConnection->GetChannelId();
}

uint16_t FtlStream::GetMediaPort()
{
    return mediaPort;
}

bool FtlStream::GetHasVideo()
{
    return ingestConnection->GetHasVideo();
}

bool FtlStream::GetHasAudio()
{
    return ingestConnection->GetHasAudio();
}

VideoCodecKind FtlStream::GetVideoCodec()
{
    return ingestConnection->GetVideoCodec();
}

AudioCodecKind FtlStream::GetAudioCodec()
{
    return ingestConnection->GetAudioCodec();
}

uint32_t FtlStream::GetAudioSsrc()
{
    return ingestConnection->GetAudioSsrc();
}

uint32_t FtlStream::GetVideoSsrc()
{
    return ingestConnection->GetVideoSsrc();
}

uint8_t FtlStream::GetAudioPayloadType()
{
    return ingestConnection->GetAudioPayloadType();
}

uint8_t FtlStream::GetVideoPayloadType()
{
    return ingestConnection->GetVideoPayloadType();
}

std::list<std::shared_ptr<JanusSession>> FtlStream::GetViewers()
{
    std::lock_guard<std::mutex> lock(viewerSessionsMutex);
    return viewerSessions;
}
#pragma endregion

#pragma region Private methods
void FtlStream::ingestConnectionClosed(IngestConnection& connection)
{
    // Ingest connection was closed, let's stop this stream.
    stopping = true;
}

void FtlStream::startStreamThread()
{
    sockaddr_in socketAddress;
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    socketAddress.sin_port = htons(mediaPort);

    mediaSocketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    int bindResult = bind(
        mediaSocketHandle,
        (const sockaddr*)&socketAddress,
        sizeof(socketAddress));

    JANUS_LOG(LOG_INFO, "FTL: Started media connection for %d on port %d\n", GetChannelId(), mediaPort);
    switch (bindResult)
    {
    case 0:
        break;
    case EADDRINUSE:
        throw std::runtime_error("FTL stream could not bind to media socket, "
            "this address is already in use.");
    case EACCES:
        throw std::runtime_error("FTL stream could not bind to media socket, "
            "access was denied.");
    default:
        throw std::runtime_error("FTL stream could not bind to media socket.");
    }

    // Let the service know that we're streaming!
    streamId = serviceConnection->StartStream(GetChannelId());

    // Set up some values we'll be using in our read thread
    socklen_t addrlen;
    sockaddr_in remote;
    char buffer[1500] = { 0 };
    int bytesRead = 0;
    // Set up poll
    struct pollfd fds[1];
    fds[0].fd = mediaSocketHandle;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    while (true)
    {
        // Are we stopping?
        if (stopping)
        {
            // Close the socket handle
            close(mediaSocketHandle);
            break;
        }

        int pollResult = poll(fds, 1, 1000);

        if (pollResult < 0)
        {
            // We've lost our socket
            JANUS_LOG(LOG_ERR, "FTL: Unknown media connection read error.\n");
            break;
        }
        else if (pollResult == 0)
        {
            // No new data
            continue;
        }

        // We've got a poll event, let's process it.
        if (fds[0].revents & (POLLERR | POLLHUP))
        {
            JANUS_LOG(LOG_ERR, "FTL: Media connection polling error.\n");
            break;
        }
        else if (fds[0].revents & POLLIN)
        {
            // Ooh, yummy data
            addrlen = sizeof(remote);
            bytesRead = recvfrom(
                mediaSocketHandle,
                buffer,
                sizeof(buffer),
                0,
                (struct sockaddr*)&remote,
                &addrlen);

            if (remote.sin_addr.s_addr != ingestConnection->GetAcceptAddress().sin_addr.s_addr)
            {
                JANUS_LOG(
                    LOG_ERR,
                    "FTL: Channel %u received packet from unexpected address.\n",
                    GetChannelId());
                continue;
            }

            if (bytesRead < 12)
            {
                // This packet is too small to have an RTP header.
                JANUS_LOG(LOG_WARN, "FTL: Channel %u received non-RTP packet.\n", GetChannelId());
                continue;
            }

            // Parse out RTP packet
            janus_rtp_header* rtpHeader = (janus_rtp_header*)buffer;
            rtp_sequence_num_t sequenceNumber = ntohs(rtpHeader->seq_number);

            // Process audio/video packets
            if ((rtpHeader->type == GetAudioPayloadType()) || 
                (rtpHeader->type == GetVideoPayloadType()))
            {
                // Count it!
                ++numPacketsReceived;

                std::shared_ptr<std::vector<unsigned char>> rtpPacket =
                    std::make_shared<std::vector<unsigned char>>(buffer, buffer + bytesRead);

                // Do additional processing on video packets
                if (rtpHeader->type == GetVideoPayloadType())
                {
                    processKeyframePacket(rtpPacket);
                }

                // Relay the packet
                RtpRelayPacketKind packetKind = rtpHeader->type == GetVideoPayloadType() ? 
                    RtpRelayPacketKind::Video : RtpRelayPacketKind::Audio;
                
                relayThreadPool->RelayPacket({
                    .rtpPacketPayload = rtpPacket,
                    .type = packetKind,
                    .channelId = GetChannelId()
                });

                // Check for any lost packets
                markReceivedSequence(rtpHeader->type, sequenceNumber);
                processLostPackets(remote, rtpHeader->type, sequenceNumber, rtpHeader->timestamp);
            }
            else
            {
                // FTL implementation uses the marker bit space for payload types above 127
                // when the payload type is not audio or video. So we need to reconstruct it.
                uint8_t payloadType = 
                    ((static_cast<uint8_t>(rtpHeader->markerbit) << 7) | 
                    static_cast<uint8_t>(rtpHeader->type));
                
                if (payloadType == FTL_PAYLOAD_TYPE_PING)
                {
                    handlePing(rtpHeader, bytesRead);
                }
                else if (payloadType == FTL_PAYLOAD_TYPE_SENDER_REPORT)
                {
                    handleSenderReport(rtpHeader, bytesRead);
                }
                else
                {
                    JANUS_LOG(
                        LOG_WARN,
                        "FTL: Unknown RTP payload type %d (orig %d)\n",
                        payloadType,
                        rtpHeader->type);
                }
            }
        }
    }

    // We're no longer listening to incoming packets.

    // Tell the service this stream has ended.
    serviceConnection->EndStream(streamId);

    // TODO: Tell the sessions that we're going away
    if (onClosed != nullptr)
    {
        onClosed(*this);
    }
}

void FtlStream::startStreamMetadataReportingThread()
{
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(metadataReportIntervalMs));

        // Are we stopping?
        if (stopping)
        {
            break;
        }

        std::time_t currentTime = std::time(nullptr);
        uint32_t streamTimeSeconds = currentTime - streamStartTime;

        serviceConnection->UpdateStreamMetadata(
            streamId,
            {
                .ingestServerHostname         = myHostname,
                .streamTimeSeconds            = streamTimeSeconds,
                .numActiveViewers             = static_cast<uint32_t>(viewerSessions.size()),
                .currentSourceBitrateBps      = currentSourceBitrateBps,
                .numPacketsReceived           = numPacketsReceived,
                .numPacketsNacked             = numPacketsNacked,
                .numPacketsLost               = numPacketsLost,
                .streamerToIngestPingMs       = streamerToIngestPingMs,
                .streamerClientVendorName     = ingestConnection->GetVendorName(),
                .streamerClientVendorVersion  = ingestConnection->GetVendorVersion(),
                .videoCodec                   = SupportedVideoCodecs::VideoCodecString(ingestConnection->GetVideoCodec()),
                .audioCodec                   = SupportedAudioCodecs::AudioCodecString(ingestConnection->GetAudioCodec()),
                .videoWidth                   = 1280,
                .videoHeight                  = 720,
            });

        // Send a preview of the latest keyframe if needed
        if (previewGenerator && (lastKeyframePreviewReported != keyframe.rtpTimestamp))
        {
            // Create our own copy of the keyframe to avoid holding up processing
            // new keyframe packets
            Keyframe previewKeyframe;
            {
                std::lock_guard<std::mutex> lock(keyframeMutex);
                previewKeyframe = keyframe;
            }

            // Generate a JPEG image and send it off to the service connection
            try
            {
                std::vector<uint8_t> jpegData = 
                    previewGenerator->GenerateJpegImage(previewKeyframe);
                serviceConnection->SendJpegPreviewImage(streamId, jpegData);
            }
            catch (const PreviewGenerationFailedException& e)
            {
                JANUS_LOG(
                    LOG_ERR,
                    "FTL: Failed to generate JPEG preview for stream %d. Error: %s\n",
                    streamId,
                    e.what());
            }
            catch (const ServiceConnectionCommunicationFailedException& e)
            {
                JANUS_LOG(
                    LOG_ERR,
                    "FTL: Failed to send JPEG preview for stream %d to service connection. Error: %s\n",
                    streamId,
                    e.what());
            }
        }
    }
}

void FtlStream::processKeyframePacket(std::shared_ptr<std::vector<unsigned char>> rtpPacket)
{
    if (rtpPacket == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(keyframeMutex);

    janus_rtp_header* rtpHeader = reinterpret_cast<janus_rtp_header*>(rtpPacket->data());

    uint16_t sequence = ntohs(rtpHeader->seq_number);
    uint32_t timestamp = ntohl(rtpHeader->timestamp);

    // If we have an ongoing keyframe capture and we see different timestamps come in, we're done
    // capturing that keyframe.
    if ((pendingKeyframe.isCapturing) && (timestamp != pendingKeyframe.rtpTimestamp))
    {
        keyframe = pendingKeyframe;
        pendingKeyframe.isCapturing = false;
        pendingKeyframe.rtpTimestamp = 0;
        pendingKeyframe.rtpPackets.clear();
        keyframeSentToViewers.clear();

        JANUS_LOG(
            LOG_VERB,
            "FTL: Channel %u keyframe complete ts %u w/ %lu packets\n",
            GetChannelId(),
            keyframe.rtpTimestamp,
            keyframe.rtpPackets.size());
    }

    // Determine if this packet is part of a keyframe
    bool isKeyframePacket = false;
    int payloadLength = 0;
    char* payload = janus_rtp_payload(reinterpret_cast<char*>(rtpPacket->data()), rtpPacket->size(), &payloadLength);
    if (payload != nullptr)
    {
        isKeyframePacket = janus_h264_is_keyframe(payload, payloadLength);
    }
    
    // Our first keyframe packet?
    if (isKeyframePacket && !pendingKeyframe.isCapturing)
    {
        JANUS_LOG(
            LOG_VERB,
            "FTL: Channel %u new keyframe seq %u ts %u\n",
            GetChannelId(),
            sequence,
            timestamp);
        pendingKeyframe.isCapturing = true;
        pendingKeyframe.rtpTimestamp = timestamp;
        pendingKeyframe.rtpPackets.push_back(rtpPacket);
    }
    // Part of our current keyframe?
    else if (timestamp == pendingKeyframe.rtpTimestamp && pendingKeyframe.isCapturing)
    {
        pendingKeyframe.rtpPackets.push_back(rtpPacket);
    }
}

void FtlStream::markReceivedSequence(
    rtp_payload_type_t payloadType,
    rtp_sequence_num_t receivedSequence)
{
    // If this sequence was previously marked as lost, remove it
    if (lostPackets.count(payloadType) > 0)
    {
        auto& lostPayloadPackets = lostPackets[payloadType];
        if (lostPayloadPackets.count(receivedSequence) > 0)
        {
            lostPayloadPackets.erase(receivedSequence);
        }
    }

    // If this is the first sequence, record it
    if (latestSequence.count(payloadType) <= 0)
    {
        latestSequence[payloadType] = receivedSequence;
        JANUS_LOG(
            LOG_INFO,
            "FTL: Channel %u first %u sequence: %u\n",
            GetChannelId(),
            payloadType,
            receivedSequence);
    }
    else
    {
        // Make sure we're actually the latest
        auto lastSequence = latestSequence[payloadType];
        if (lastSequence < receivedSequence)
        {
            // Identify any lost packets between the last sequence
            if (lostPackets.count(payloadType) <= 0)
            {
                lostPackets[payloadType] = std::set<rtp_sequence_num_t>();
            }
            for (auto seq = lastSequence + 1; seq < receivedSequence; ++seq)
            {
                lostPackets[payloadType].insert(seq);
            }
            if ((receivedSequence - lastSequence) > 1)
            {
                JANUS_LOG(
                    LOG_WARN,
                    "FTL: Channel %u PL %u lost %u packets.\n",
                    GetChannelId(),
                    payloadType,
                    (receivedSequence - (lastSequence + 1)));

                // Count em!
                numPacketsLost += (receivedSequence - lastSequence);
            }
            latestSequence[payloadType] = receivedSequence;
        }
    }
}

void FtlStream::processLostPackets(
    sockaddr_in remote,
    rtp_payload_type_t payloadType,
    rtp_sequence_num_t currentSequence,
    rtp_timestamp_t currentTimestamp)
{
    if (lostPackets.count(payloadType) > 0)
    {
        auto& lostPayloadPackets = lostPackets[payloadType];
        for (auto it = lostPayloadPackets.cbegin(); it != lostPayloadPackets.cend();)
        {
            const auto lostPacketSequence = *it;

            // If this 'lost' packet came from the future, get rid of it
            if (lostPacketSequence > currentSequence)
            {
                it = lostPayloadPackets.erase(it);
                continue;
            }

            // Otherwise, ask for re-transmission
            rtp_ssrc_t ssrc;
            if (payloadType == GetVideoPayloadType())
            {
                ssrc = GetVideoSsrc();
            }
            else if (payloadType == GetAudioPayloadType())
            {
                ssrc = GetAudioSsrc();
            }
            else
            {
                JANUS_LOG(
                    LOG_ERR,
                    "FTL: Channel %u cannot NACK unknown payload type %u\n",
                    GetChannelId(),
                    payloadType);
                it = lostPayloadPackets.erase(it);
                continue;
            }

            // See https://tools.ietf.org/html/rfc4585 Section 6.1
            // for information on how the nack packet is formed
            char nackBuf[120];
            janus_rtcp_header *rtcpHeader = reinterpret_cast<janus_rtcp_header*>(nackBuf);
            rtcpHeader->version = 2;
            rtcpHeader->type = RTCP_RTPFB;
            rtcpHeader->rc = 1;
            janus_rtcp_fb* rtcpFeedback = reinterpret_cast<janus_rtcp_fb*>(rtcpHeader);
            rtcpFeedback->media = htonl(ssrc);
            rtcpFeedback->ssrc = htonl(ssrc);
            janus_rtcp_nack* rtcpNack = reinterpret_cast<janus_rtcp_nack*>(rtcpFeedback->fci);
            rtcpNack->pid = htons(lostPacketSequence);
            rtcpNack->blp = 0;
            rtcpHeader->length = htons(3);

            int res = sendto(
                mediaSocketHandle,
                nackBuf,
                16,
                0,
                reinterpret_cast<sockaddr*>(&remote),
                sizeof(remote));
            if (res == -1)
            {
                JANUS_LOG(
                    LOG_ERR,
                    "FTL: Channel %u PL %u NACK failed: %d\n",
                    GetChannelId(),
                    payloadType,
                    errno);
            }

            // Sent! Take it out of the list for now.
            it = lostPayloadPackets.erase(it);

            JANUS_LOG(
                    LOG_INFO,
                    "FTL: Channel %u PL %u sent NACK for seq %u\n",
                    GetChannelId(),
                    payloadType,
                    lostPacketSequence);

            // Count number of packets we NACK
            ++numPacketsNacked;
        }
    }
}

void FtlStream::handlePing(janus_rtp_header* rtpHeader, uint16_t length)
{
    // These pings are useless - FTL tries to determine 'ping' by having a timestamp
    // sent across and compared against the remote's clock. This assumes that there is
    // no time difference between the client and server, which is practically never true.

    // We'll just ignore these pings, since they wouldn't give us any useful information
    // anyway.
}

void FtlStream::handleSenderReport(janus_rtp_header* rtpHeader, uint16_t length)
{
    // We expect this packet to be 28 bytes big.
    if (length != 28)
    {
        JANUS_LOG(LOG_WARN, "FTL: Invalid sender report packet of length %d (expect 28)\n", length);
    }
    // char* packet = reinterpret_cast<char*>(rtpHeader);
    // uint32_t ssrc              = ntohl(*reinterpret_cast<uint32_t*>(packet + 4));
    // uint32_t ntpTimestampHigh  = ntohl(*reinterpret_cast<uint32_t*>(packet + 8));
    // uint32_t ntpTimestampLow   = ntohl(*reinterpret_cast<uint32_t*>(packet + 12));
    // uint32_t rtpTimestamp      = ntohl(*reinterpret_cast<uint32_t*>(packet + 16));
    // uint32_t senderPacketCount = ntohl(*reinterpret_cast<uint32_t*>(packet + 20));
    // uint32_t senderOctetCount  = ntohl(*reinterpret_cast<uint32_t*>(packet + 24));

    // uint64_t ntpTimestamp = (static_cast<uint64_t>(ntpTimestampHigh) << 32) | 
    //     static_cast<uint64_t>(ntpTimestampLow);

    // TODO: We don't do anything with this information right now, but we ought to log
    // it away somewhere.
}
#pragma endregion