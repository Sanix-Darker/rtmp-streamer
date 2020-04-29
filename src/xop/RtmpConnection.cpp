#include "RtmpConnection.h"
#include "RtmpServer.h"
#include "RtmpPublisher.h"
#include "RtmpClient.h"
#include "net/Logger.h"
#include <random>

using namespace xop;

RtmpConnection::RtmpConnection(RtmpServer *rtmpServer, TaskScheduler *taskScheduler, SOCKET sockfd)
	: RtmpConnection(taskScheduler, sockfd)
{
	m_rtmpServer = rtmpServer;
	m_connMode = RTMP_SERVER;
	m_connState = HANDSHAKE_C0C1;
	m_chunkParseState = PARSE_HEADER;
	m_peerBandwidth = rtmpServer->getPeerBandwidth();
	m_acknowledgementSize = rtmpServer->getAcknowledgementSize();
	m_maxGopCacheLen = rtmpServer->getGopCacheLen();
	m_maxChunkSize = rtmpServer->getChunkSize();
}

RtmpConnection::RtmpConnection(RtmpPublisher *rtmpPublisher, TaskScheduler *taskScheduler, SOCKET sockfd)
	: RtmpConnection(taskScheduler, sockfd)
{
	m_rtmpPublisher = rtmpPublisher;
	m_connMode = RTMP_PUBLISHER;
	m_connState = HANDSHAKE_S0S1S2;
	m_chunkParseState = PARSE_HEADER;
	m_peerBandwidth = m_rtmpPublisher->getPeerBandwidth();
	m_acknowledgementSize = m_rtmpPublisher->getAcknowledgementSize();
	m_maxChunkSize = m_rtmpPublisher->getChunkSize();
	m_streamPath = m_rtmpPublisher->getStreamPath();
	m_streamName = m_rtmpPublisher->getStreamName();
	m_app = m_rtmpPublisher->getApp();
}

RtmpConnection::RtmpConnection(RtmpClient *rtmpClient, TaskScheduler *taskScheduler, SOCKET sockfd)
	: RtmpConnection(taskScheduler, sockfd)
{
	m_rtmpClient = rtmpClient;
	m_connMode = RTMP_CLIENT;
	m_connState = HANDSHAKE_S0S1S2;
	m_chunkParseState = PARSE_HEADER;
	m_maxChunkSize = m_rtmpClient->getChunkSize();
	m_streamPath = m_rtmpClient->getStreamPath();
	m_streamName = m_rtmpClient->getStreamName();
	m_app = m_rtmpClient->getApp();
}

RtmpConnection::RtmpConnection(TaskScheduler *taskScheduler, SOCKET sockfd)
	: TcpConnection(taskScheduler, sockfd)
	, m_taskScheduler(taskScheduler)
	, m_channelPtr(new Channel(sockfd))
{
	this->setReadCallback([this](std::shared_ptr<TcpConnection> conn, xop::BufferReader& buffer) {
		return this->onRead(buffer);
	});

	this->setCloseCallback([this](std::shared_ptr<TcpConnection> conn) {
		this->onClose();
	});
}

RtmpConnection::~RtmpConnection()
{

}

bool RtmpConnection::onRead(BufferReader& buffer)
{
	bool ret = true;
	if(m_connState >= HANDSHAKE_COMPLETE)
	{
		ret = handleChunk(buffer);
	}
	else if(m_connState == HANDSHAKE_C0C1 || m_connState == HANDSHAKE_C2
			|| m_connState == HANDSHAKE_S0S1S2)
	{
		ret = this->handleHandshake(buffer);

		if (m_connState == HANDSHAKE_COMPLETE && buffer.readableBytes() > 0)
		{
			ret = handleChunk(buffer);
		}

		if (m_connState == HANDSHAKE_COMPLETE && (m_connMode == RTMP_PUBLISHER || m_connMode == RTMP_CLIENT))
		{
			this->setChunkSize();
			this->connect();
		}
	}

	return ret;
}

void RtmpConnection::onClose()
{
	if (m_rtmpServer != nullptr)
	{
		this->handDeleteStream();
	}
	else if (m_rtmpPublisher != nullptr)
	{
		this->deleteStream();
	}
}

bool RtmpConnection::handshake()
{
	std::shared_ptr<char> res;
	uint32_t resSize = 1 + 1536; //COC1

	res.reset(new char[resSize]);
	memset(res.get(), 0, 1537);
	res.get()[0] = RTMP_VERSION;

	std::random_device rd;
	uint8_t *p = (uint8_t *)res.get(); p += 9;
	for (int i = 0; i < 1528; i++)
	{
		*p++ = rd();
	}

	this->send(res, resSize);
	return true;
}

bool RtmpConnection::handleHandshake(BufferReader& buffer)
{
    uint8_t *buf = (uint8_t*)buffer.peek();
    uint32_t bufSize = buffer.readableBytes();
    uint32_t pos = 0;
    std::shared_ptr<char> res;
    uint32_t resSize = 0;
	std::random_device rd;

	if (m_connState == HANDSHAKE_S0S1S2)
	{
		if (bufSize < (1 + 1536 + 1536)) //S0S1S2
		{
			return true;
		}

		if (buf[0] != RTMP_VERSION)
		{
			LOG_ERROR("unsupported rtmp version %x\n", buf[0]);
			return false;
		}
		pos += 1 + 1536 + 1536;
		resSize = 1536;
		res.reset(new char[resSize]); //C2
		memcpy(res.get(), buf + 1, 1536);
		m_connState = HANDSHAKE_COMPLETE;
	}
    else if(m_connState == HANDSHAKE_C0C1)
    {
        if(bufSize < 1537) //c0c1
        {
            return true;
        }
        else
        {
            if(buf[0] != RTMP_VERSION)
            {
               return false;
            }

            pos += 1537;
            resSize = 1+ 1536 + 1536;
            res.reset(new char[resSize]); //S0 S1 S2
            memset(res.get(), 0, 1537);
            res.get()[0] = RTMP_VERSION;

            char *p = res.get(); p += 9;
            for(int i=0; i<1528; i++)
            {
                *p++ = rd();
            }
            memcpy(p, buf+1, 1536);
            m_connState = HANDSHAKE_C2;
        }
    }
    else if(m_connState == HANDSHAKE_C2)
    {
        if(bufSize < 1536) //c2
        {
            return true;
        }
        else
        {
            pos = 1536;
            m_connState = HANDSHAKE_COMPLETE;
        }
    }
    else
    {
        return false;
    }

    buffer.retrieve(pos);
    if(resSize > 0)
    {
        this->send(res, resSize);
    }

    return true;
}

bool RtmpConnection::handleChunk(BufferReader& buffer)
{
	int ret = -1;

	do
	{
		if (m_chunkParseState == PARSE_HEADER)
		{
			ret = parseChunkHeader(buffer);
		}
		else if (m_chunkParseState == PARSE_BODY)
		{
			ret = parseChunkBody(buffer);
			if (ret >= 0 && m_chunkStreamId>=0)
			{
				auto& rtmpMsg = m_rtmpMessasges[m_chunkStreamId];
				if (rtmpMsg.index == rtmpMsg.length)
				{
					if (rtmpMsg.timestamp >= 0xffffff)
					{
						rtmpMsg._timestamp += rtmpMsg.extTimestamp;
					}
					else
					{
						rtmpMsg._timestamp += rtmpMsg.timestamp;
					}

					if (!handleMessage(rtmpMsg))
					{
						return false;
					}
					m_chunkStreamId = -1;
					rtmpMsg.reset();
				}
			}
		}

		if (ret == 0)
		{
			break;
		}
		else if (ret < 0)
		{
			return false;
		}

		//std::cout << "\n\n--\n >>>>> ???? >>>>> buffer.readableBytes(): " << buffer.readableBytes();
		//std::cout << "\n\n--\n >>>>> ???? >>>>> sizeof(buffer.readableBytes()): " << sizeof(buffer.readableBytes()) << std::endl;

	} while (buffer.readableBytes() > 0);

	return true;
}

int RtmpConnection::parseChunkHeader(BufferReader& buffer)
{
	uint8_t *buf = NULL;
	uint32_t bufSize = 0, bytesUsed = 0;
	buf = (uint8_t*)buffer.peek();
	bufSize = buffer.readableBytes();

	if (bufSize == 0)
	{
		return 0;
	}

	uint8_t flags = buf[bytesUsed];
	bytesUsed += 1;

	uint8_t csid = flags & 0x3f; // chunk stream id
	if (csid == 0) // csid [64, 319]
	{
		if (bufSize < (bytesUsed + 2))
		{
			return 0;
		}

		csid += buf[bytesUsed] + 64;
		bytesUsed += 1;
	}
	else if (csid == 1) // csid [64, 65599]
	{
		if (bufSize  < (3 + bytesUsed))
		{
			return 0;
		}
		csid += buf[bytesUsed + 1] * 255 + buf[bytesUsed] + 64;
		bytesUsed += 2;
	}

	uint8_t fmt = (flags >> 6); // message_header_type
	if (fmt >= 4)
	{
		return -1;
	}

	uint32_t headerLen = kChunkMessageLen[fmt]; // basic_header + message_header
	if (bufSize < (headerLen + bytesUsed))
	{
		return 0;
	}

	RtmpMessageHeader header;
	memset(&header, 0, sizeof(RtmpMessageHeader));
	memcpy(&header, buf + bytesUsed, headerLen);
	bytesUsed += headerLen;

	auto& rtmpMsg = m_rtmpMessasges[csid];
	m_chunkStreamId = rtmpMsg.csid = csid;

	if (fmt == RTMP_CHUNK_TYPE_0 || fmt == RTMP_CHUNK_TYPE_1)
	{
		uint32_t length = readUint24BE((char*)header.length);

		if (rtmpMsg.length != length || rtmpMsg.payload == nullptr)
		{
			rtmpMsg.length = length;
			rtmpMsg.payload.reset(new char[rtmpMsg.length]);
		}
		rtmpMsg.index = 0;
		rtmpMsg.typeId = header.typeId;
	}

	if (fmt == RTMP_CHUNK_TYPE_0)
	{
		rtmpMsg.streamId = readUint24LE((char*)header.streamId);
	}

	uint32_t timestamp = readUint24BE((char*)header.timestamp);
	uint32_t extTimestamp = 0;
	if (timestamp >= 0xffffff || rtmpMsg.timestamp >= 0xffffff)
	{
		if (bufSize < (4 + bytesUsed))
		{
			return 0;
		}
		extTimestamp = readUint32BE((char*)buf + bytesUsed);
		bytesUsed += 4;
	}

	if (rtmpMsg.index == 0) /* first chunk */
	{
		if (fmt == RTMP_CHUNK_TYPE_0)
		{
			/* absolute timestamp */
			rtmpMsg._timestamp = 0;
			rtmpMsg.timestamp = timestamp;
			rtmpMsg.extTimestamp = extTimestamp;
		}
		else
		{
			/* relative timestamp (timestamp delta) */
			if (rtmpMsg.timestamp >= 0xffffff)
			{
				rtmpMsg.extTimestamp += extTimestamp;
			}
			else
			{
				rtmpMsg.timestamp += timestamp;
			}
		}
	}

	m_chunkParseState = PARSE_BODY;
	buffer.retrieve(bytesUsed);
	return bytesUsed;
}

int RtmpConnection::parseChunkBody(BufferReader& buffer)
{
	uint8_t *buf = NULL;
	uint32_t bufSize = 0, bytesUsed = 0;
	buf = (uint8_t*)buffer.peek();
	bufSize = buffer.readableBytes();

	if (bufSize == 0)
	{
		return 0;
	}

	if (m_chunkStreamId < 0)
	{
		return -1;
	}

	auto& rtmpMsg = m_rtmpMessasges[m_chunkStreamId];

	uint32_t chunkSize = rtmpMsg.length - rtmpMsg.index;
	if (chunkSize > m_inChunkSize)
	{
		chunkSize = m_inChunkSize;
	}

	if (bufSize < (chunkSize + bytesUsed))
	{
		return 0;
	}

	if (rtmpMsg.index + chunkSize > rtmpMsg.length)
	{
		return -1;
	}

	memcpy(rtmpMsg.payload.get() + rtmpMsg.index, buf + bytesUsed, chunkSize);
	bytesUsed += chunkSize;
	rtmpMsg.index += chunkSize;
	if (rtmpMsg.index >= rtmpMsg.length || rtmpMsg.index%m_inChunkSize == 0)
	{
		m_chunkParseState = PARSE_HEADER;
	}
	buffer.retrieve(bytesUsed);
	return bytesUsed;
}


// int i = 0;
bool RtmpConnection::handleMessage(RtmpMessage& rtmpMsg)
{
    bool ret = true;
	int lenn = rtmpMsg.length;
	// LOG_INFO("Handle message :%d %d\n",lenn, rtmpMsg.typeId);
	// LOG_INFO(rtmpMsg.payload.get());

	// i += 1;
	// FILE* fp = fopen("frame.png", "wb");
	// fwrite(rtmpMsg.payload.get(), sizeof(char), strlen(rtmpMsg.payload.get()), fp);
	// fclose(fp);

	// std::cout << "TYPE: " << typeid(rtmpMsg.payload.get()).name() << '\n';
    switch(rtmpMsg.typeId)
    {
        case RTMP_VIDEO:
			//LOG_INFO(">>>>>>>>> A video\n");
            ret = handleVideo(rtmpMsg);
            break;
        case RTMP_AUDIO:
			//LOG_INFO(">>>>>>>>> An Audio\n");
            ret = handleAudio(rtmpMsg);
            break;
        case RTMP_INVOKE:
			//LOG_INFO(">>>>>>>>> An invoke\n");
            ret = handleInvoke(rtmpMsg);
            break;
        case RTMP_NOTIFY:
			//LOG_INFO(">>>>>>>>> A notify\n");
            ret = handleNotify(rtmpMsg);
            break;
        case RTMP_FLEX_MESSAGE:
			//LOG_INFO(">>>>>>>>> A Flex message\n");
			//LOG_INFO("unsupported rtmp flex message.\n");
			ret = false;
            break;
        case RTMP_SET_CHUNK_SIZE:
			m_inChunkSize = readUint32BE(rtmpMsg.payload.get());
            break;
		case RTMP_BANDWIDTH_SIZE:
			break;
        case RTMP_FLASH_VIDEO:
			//LOG_INFO("unsupported rtmp flash video.\n");
			ret = false;
            break;
        case RTMP_ACK:
            break;
        case RTMP_ACK_SIZE:
            break;
        case RTMP_USER_EVENT:
            break;
        default:
			LOG_INFO("unkonw message type : %d\n", rtmpMsg.typeId);
            break;
    }
	if (!ret) printf("tmpMsg.typeId:%x\n", rtmpMsg.typeId);

    return ret;
}

bool RtmpConnection::handleInvoke(RtmpMessage& rtmpMsg)
{
    bool ret  = true;
    m_amfDec.reset();

	int bytesUsed = m_amfDec.decode((const char *)rtmpMsg.payload.get(), rtmpMsg.length, 1);
	if (bytesUsed < 0)
	{
		return false;
	}

    std::string method = m_amfDec.getString();
	LOG_INFO("[Method] %s\n", method.c_str());

	if (m_connMode == RTMP_PUBLISHER || m_connMode == RTMP_CLIENT)
	{
		bytesUsed += m_amfDec.decode(rtmpMsg.payload.get() + bytesUsed, rtmpMsg.length - bytesUsed);
		if (method == "_result")
		{
			ret = handleResult(rtmpMsg);
		}
		else if (method == "onStatus")
		{
			ret = handleOnStatus(rtmpMsg);
		}
	}
	else if (m_connMode == RTMP_SERVER)
	{
		if(rtmpMsg.streamId == 0)
		{
			bytesUsed += m_amfDec.decode(rtmpMsg.payload.get()+bytesUsed, rtmpMsg.length-bytesUsed);
			if(method == "connect")
			{
				ret = handleConnect();
			}
			else if(method == "createStream")
			{
				ret = handleCreateStream();
			}
		}
		else if(rtmpMsg.streamId == m_streamId)
		{
			bytesUsed += m_amfDec.decode((const char *)rtmpMsg.payload.get()+bytesUsed, rtmpMsg.length-bytesUsed, 3);
			m_streamName = m_amfDec.getString();
			m_streamPath = "/" + m_app + "/" + m_streamName;

			if((int)rtmpMsg.length > bytesUsed)
			{
				bytesUsed += m_amfDec.decode((const char *)rtmpMsg.payload.get()+bytesUsed, rtmpMsg.length-bytesUsed);
			}

			if(method == "publish")
			{
				ret = handlePublish();
			}
			else if(method == "play")
			{
				ret = handlePlay();
			}
			else if(method == "play2")
			{
				ret = handlePlay2();
			}
			else if(method == "deleteStream")
			{
				ret = handDeleteStream();
			}
			else if (method == "releaseStream")
			{

			}
		}
	}
    return ret;
}

bool RtmpConnection::handleNotify(RtmpMessage& rtmpMsg)
{
    //if(m_streamId != rtmpMsg.streamId)
    //{
    //    return false;
    //}

    m_amfDec.reset();
    int bytesUsed = m_amfDec.decode((const char *)rtmpMsg.payload.get(), rtmpMsg.length, 1);
    if(bytesUsed < 0)
    {
        return false;
    }

    if(m_amfDec.getString() == "@setDataFrame")
    {
        m_amfDec.reset();
        bytesUsed = m_amfDec.decode((const char *)rtmpMsg.payload.get()+bytesUsed, rtmpMsg.length-bytesUsed, 1);
        if(bytesUsed < 0)
        {
            return false;
        }

        if(m_amfDec.getString() == "onMetaData")
        {
            m_amfDec.decode((const char *)rtmpMsg.payload.get()+bytesUsed, rtmpMsg.length-bytesUsed);
            m_metaData = m_amfDec.getObjects();

            auto sessionPtr = m_rtmpServer->getSession(m_streamPath);
            if(sessionPtr)
            {
                sessionPtr->setMetaData(m_metaData);
                sessionPtr->sendMetaData(m_metaData);
            }
        }
    }

    return true;
}

bool RtmpConnection::handleVideo(RtmpMessage& rtmpMsg)
{
	uint8_t type = RTMP_VIDEO;
	uint8_t *payload = (uint8_t *)rtmpMsg.payload.get();
	uint32_t length = rtmpMsg.length;
	uint8_t frameType = (payload[0] >> 4) & 0x0f;
	uint8_t codecId = payload[0] & 0x0f;

	// printf("\nRTMP_VIDEO: %d", RTMP_VIDEO);
	// printf("\npayload: %s", payload);
	// printf("\nlength: %d", length);
	// printf("\nframeType: %d", frameType);
	// printf("\ncodecId: %d", codecId);

	if (m_connMode == RTMP_CLIENT && m_connState == START_PLAY && m_isPlaying)
	{
		if (m_playCB)
		{
			m_playCB(payload, length, codecId, (uint32_t)rtmpMsg._timestamp);
		}
	}
	else
	{
		RtmpSession::Ptr sessionPtr = m_rtmpServer->getSession(m_streamPath);
		if (sessionPtr == nullptr)
		{
			return false;
		}

		if (frameType == 1 && codecId == RTMP_CODEC_ID_H264)
		{
			if (payload[1] == 0)
			{
				m_avcSequenceHeaderSize = length;
				m_avcSequenceHeader.reset(new char[length]);
				memcpy(m_avcSequenceHeader.get(), rtmpMsg.payload.get(), length);
				sessionPtr->setAvcSequenceHeader(m_avcSequenceHeader, m_avcSequenceHeaderSize);
				type = RTMP_AVC_SEQUENCE_HEADER;
			}
		}

		sessionPtr->sendMediaData(type, rtmpMsg._timestamp, rtmpMsg.payload, rtmpMsg.length);
	}

    return true;
}

bool RtmpConnection::handleAudio(RtmpMessage& rtmpMsg)
{
	uint8_t type = RTMP_AUDIO;
	uint8_t *payload = (uint8_t *)rtmpMsg.payload.get();
	uint32_t length = rtmpMsg.length;
	uint8_t soundFormat = (payload[0] >> 4) & 0x0f;
	uint8_t soundSize = (payload[0] >> 1) & 0x01;
	uint8_t soundRate = (payload[0] >> 2) & 0x03;
	uint8_t codecId = payload[0] & 0x0f;

	if (m_connMode == RTMP_CLIENT && m_connState == START_PLAY && m_isPlaying)
	{
		if (m_playCB)
		{
			m_playCB(payload, length, codecId, (uint32_t)rtmpMsg._timestamp);
		}
	}
	else
	{
		RtmpSession::Ptr sessionPtr = m_rtmpServer->getSession(m_streamPath);
		if (sessionPtr == nullptr)
		{
			return false;
		}

		if (soundFormat == RTMP_CODEC_ID_AAC && payload[1] == 0)
		{
			m_aacSequenceHeaderSize = rtmpMsg.length;
			m_aacSequenceHeader.reset(new char[rtmpMsg.length]);
			memcpy(m_aacSequenceHeader.get(), rtmpMsg.payload.get(), rtmpMsg.length);
			sessionPtr->setAacSequenceHeader(m_aacSequenceHeader, m_aacSequenceHeaderSize);
			type = RTMP_AAC_SEQUENCE_HEADER;
		}

		sessionPtr->sendMediaData(type, rtmpMsg._timestamp, rtmpMsg.payload, rtmpMsg.length);
	}

    return true;
}

bool RtmpConnection::connect()
{


	AmfObjects objects;
	m_amfEnc.reset();

	m_amfEnc.encodeString("connect", 7);
	m_amfEnc.encodeNumber((double)(++m_number));
	objects["app"] = AmfObject(m_app);
	objects["type"] = AmfObject(std::string("nonprivate"));

	if (m_connMode == RTMP_PUBLISHER)
	{
		objects["swfUrl"] = AmfObject(m_rtmpPublisher->getSwfUrl());
		objects["tcUrl"] = AmfObject(m_rtmpPublisher->getTcUrl());
	}
	else if (m_connMode == RTMP_CLIENT)
	{
		objects["swfUrl"] = AmfObject(m_rtmpClient->getSwfUrl());
		objects["tcUrl"] = AmfObject(m_rtmpClient->getTcUrl());
	}

	m_amfEnc.encodeObjects(objects);
	m_connState = START_CONNECT;
	sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());
	return true;
}

bool RtmpConnection::cretaeStream()
{
	AmfObjects objects;
	m_amfEnc.reset();

	m_amfEnc.encodeString("createStream", 12);
	m_amfEnc.encodeNumber((double)(++m_number));
	m_amfEnc.encodeObjects(objects);

	m_connState = START_CREATE_STREAM;
	sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());
	return true;
}

bool RtmpConnection::publish()
{
	AmfObjects objects;
	m_amfEnc.reset();

	m_amfEnc.encodeString("publish", 7);
	m_amfEnc.encodeNumber((double)(++m_number));
	m_amfEnc.encodeObjects(objects);
	m_amfEnc.encodeString(m_streamName.c_str(), (int)m_streamName.size());

	m_connState = START_PUBLISH;
	sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());
	return true;
}

bool RtmpConnection::play()
{
	AmfObjects objects;
	m_amfEnc.reset();

	m_amfEnc.encodeString("play", 4);
	m_amfEnc.encodeNumber((double)(++m_number));
	m_amfEnc.encodeObjects(objects);
	m_amfEnc.encodeString(m_streamName.c_str(), (int)m_streamName.size());

	m_connState = START_PLAY;
	sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());
	return true;
}

bool RtmpConnection::deleteStream()
{
	AmfObjects objects;
	m_amfEnc.reset();

	m_amfEnc.encodeString("deleteStream", 12);
	m_amfEnc.encodeNumber((double)(++m_number));
	m_amfEnc.encodeObjects(objects);
	m_amfEnc.encodeNumber(m_streamId);

	m_connState = START_DELETE_STREAM;
	sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());
	return true;
}

bool RtmpConnection::handleConnect()
{
    if(!m_amfDec.hasObject("app"))
    {
        return false;
    }

    AmfObject amfObj = m_amfDec.getObject("app");
    m_app = amfObj.amf_string;
    if(m_app == "")
    {
        return false;
    }

    sendAcknowledgement();
    setPeerBandwidth();
    setChunkSize();

    AmfObjects objects;
    m_amfEnc.reset();
    m_amfEnc.encodeString("_result", 7);
    m_amfEnc.encodeNumber(m_amfDec.getNumber());

    objects["fmsVer"] = AmfObject(std::string("FMS/4,5,0,297"));
    objects["capabilities"] = AmfObject(255.0);
    objects["mode"] = AmfObject(1.0);
    m_amfEnc.encodeObjects(objects);
    objects.clear();
    objects["level"] = AmfObject(std::string("status"));
    objects["code"] = AmfObject(std::string("NetConnection.Connect.Success"));
    objects["description"] = AmfObject(std::string("Connection succeeded."));
    objects["objectEncoding"] = AmfObject(0.0);
    m_amfEnc.encodeObjects(objects);

    sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());
    return true;
}

bool RtmpConnection::handleCreateStream()
{
    AmfObjects objects;
    m_amfEnc.reset();
    m_amfEnc.encodeString("_result", 7);
    m_amfEnc.encodeNumber(m_amfDec.getNumber());
    m_amfEnc.encodeObjects(objects);
    m_amfEnc.encodeNumber(kStreamId);

    sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());
    m_streamId = kStreamId;
    return true;
}

bool RtmpConnection::handlePublish()
{
	LOG_INFO("[+++++++] ----------------------------------");
    LOG_INFO("[Publish] app: %s, stream name: %s, stream path: %s", m_app.c_str(), m_streamName.c_str(), m_streamPath.c_str());
	LOG_INFO("[+++++++] ----------------------------------\n");

    AmfObjects objects;
    m_amfEnc.reset();
    m_amfEnc.encodeString("onStatus", 8);
    m_amfEnc.encodeNumber(0);
    m_amfEnc.encodeObjects(objects);

    bool isError = false;
    if(m_rtmpServer->hasPublisher(m_streamPath))
    {
        isError = true;
        objects["level"] = AmfObject(std::string("error"));
        objects["code"] = AmfObject(std::string("NetStream.Publish.BadName"));
        objects["description"] = AmfObject(std::string("Stream already publishing."));
    }
    else if(m_connState == START_PUBLISH)
    {
        isError = true;
        objects["level"] = AmfObject(std::string("error"));
        objects["code"] = AmfObject(std::string("NetStream.Publish.BadConnection"));
        objects["description"] = AmfObject(std::string("Connection already publishing."));
    }
    /* else if(0)
    {
        //认证处理
    } */
    else
    {
        objects["level"] = AmfObject(std::string("status"));
        objects["code"] = AmfObject(std::string("NetStream.Publish.Start"));
        objects["description"] = AmfObject(std::string("Start publising."));
        m_rtmpServer->addSession(m_streamPath);
    }

    m_amfEnc.encodeObjects(objects);
    sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size());

    if(isError)
    {
        // close ?
    }
    else
    {
        m_connState = START_PUBLISH;
		m_isPublishing = true;
    }

    auto sessionPtr = m_rtmpServer->getSession(m_streamPath);
    if(sessionPtr)
    {
		sessionPtr->setGopCache(m_maxGopCacheLen);
        sessionPtr->addRtmpClient(std::dynamic_pointer_cast<RtmpConnection>(shared_from_this()));
    }
    return true;
}

bool RtmpConnection::handlePlay()
{
	LOG_INFO("[Play] app: %s, stream name: %s, stream path: %s\n", m_app.c_str(), m_streamName.c_str(), m_streamPath.c_str());

    AmfObjects objects;
    m_amfEnc.reset();
    m_amfEnc.encodeString("onStatus", 8);
    m_amfEnc.encodeNumber(0);
    m_amfEnc.encodeObjects(objects);
    objects["level"] = AmfObject(std::string("status"));
    objects["code"] = AmfObject(std::string("NetStream.Play.Reset"));
    objects["description"] = AmfObject(std::string("Resetting and playing stream."));
    m_amfEnc.encodeObjects(objects);
    if(!sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size()))
    {
        return false;
    }

    objects.clear(); 
    m_amfEnc.reset(); 
    m_amfEnc.encodeString("onStatus", 8);
    m_amfEnc.encodeNumber(0);    
    m_amfEnc.encodeObjects(objects);
    objects["level"] = AmfObject(std::string("status"));
    objects["code"] = AmfObject(std::string("NetStream.Play.Start"));
    objects["description"] = AmfObject(std::string("Started playing."));   
    m_amfEnc.encodeObjects(objects);
    if(!sendInvokeMessage(RTMP_CHUNK_INVOKE_ID, m_amfEnc.data(), m_amfEnc.size()))
    {
        return false;
    }

    m_amfEnc.reset(); 
    m_amfEnc.encodeString("|RtmpSampleAccess", 17);
    m_amfEnc.encodeBoolean(true);
    m_amfEnc.encodeBoolean(true);
    if(!sendNotifyMessage(RTMP_CHUNK_DATA_ID, m_amfEnc.data(), m_amfEnc.size()))
    {
        return false;
    }
             
    m_connState = START_PLAY; 
    
    auto sessionPtr = m_rtmpServer->getSession(m_streamPath); 
    if(sessionPtr)
    {   
        sessionPtr->addRtmpClient(std::dynamic_pointer_cast<RtmpConnection>(shared_from_this()));
    }  
    
    return true;
}

bool RtmpConnection::handlePlay2()
{
    printf("[Play2] stream path: %s\n", m_streamPath.c_str());
    return false;
}

bool RtmpConnection::handDeleteStream()
{
    if(m_streamPath != "")
    {
        auto sessionPtr = m_rtmpServer->getSession(m_streamPath); 
        if(sessionPtr != nullptr)
        {   
			auto conn = std::dynamic_pointer_cast<RtmpConnection>(shared_from_this());
			_taskScheduler->addTimer([sessionPtr, conn] {
				sessionPtr->removeRtmpClient(conn);
				return false;
			}, 1);
        }  

		m_isPlaying = false;
		m_isPublishing = false;
		m_hasKeyFrame = false;
        m_rtmpMessasges.clear();
    }

	return true;
}

bool RtmpConnection::handleResult(RtmpMessage& rtmpMsg)
{
	bool ret = false;

	if (m_connState == START_CONNECT)
	{
		if (m_amfDec.hasObject("code"))
		{
			AmfObject amfObj = m_amfDec.getObject("code");
			if (amfObj.amf_string == "NetConnection.Connect.Success")
			{
				this->cretaeStream();
				ret = true;
			}
		}
	}
	else if (m_connState == START_CREATE_STREAM)
	{
		if (m_amfDec.getNumber() > 0)
		{
			m_streamId = (uint32_t)m_amfDec.getNumber();
			if (m_connMode == RTMP_PUBLISHER)
			{
				this->publish();
			}
			else if (m_connMode == RTMP_CLIENT)
			{
				this->play();
			}

			ret = true;
		}
	}

	return ret;
}

bool RtmpConnection::handleOnStatus(RtmpMessage& rtmpMsg)
{
	bool ret = true;

	if (m_connState == START_PUBLISH || m_connState == START_PLAY)
	{		
		if (m_amfDec.hasObject("code"))
		{
			AmfObject amfObj = m_amfDec.getObject("code");
			m_status = amfObj.amf_string;
			if (m_connMode == RTMP_PUBLISHER)
			{
				if (m_status == "NetStream.Publish.Start")
				{
					m_isPublishing = true;					
				}		
				else if(m_status == "NetStream.publish.Unauthorized"
						|| m_status == "NetStream.Publish.BadConnection" /*"Connection already publishing"*/
						|| m_status == "NetStream.Publish.BadName")      /*Stream already publishing*/
				{
					ret = false;
				}
			}
			else if (m_connMode == RTMP_CLIENT)
			{			
				if (/*amfObj.amf_string == "NetStream.Play.Reset" || */m_status == "NetStream.Play.Start")
				{
					m_isPlaying = true;
				}
				else if(m_status == "NetStream.play.Unauthorized"
						|| m_status == "NetStream.Play.UnpublishNotify"  /*"stream is now unpublished."*/
						|| m_status == "NetStream.Play.BadConnection")   /*"Connection already playing"*/
				{
					ret = false;
				}
			}
		}
	}
	if (m_connState == START_DELETE_STREAM)
	{
		if (m_amfDec.hasObject("code"))
		{
			AmfObject amfObj = m_amfDec.getObject("code");
			if (amfObj.amf_string != "NetStream.Unpublish.Success")
			{
				ret = false;
			}
		}
	}

	return true;
}

bool RtmpConnection::sendMetaData(AmfObjects metaData)
{
    if(this->isClosed())
    {
        return false;
    }

	if (metaData.size() == 0)
	{
		return false;
	}

    m_amfEnc.reset();
    m_amfEnc.encodeString("onMetaData", 10);
    m_amfEnc.encodeECMA(metaData);
    if(!sendNotifyMessage(RTMP_CHUNK_DATA_ID, m_amfEnc.data(), m_amfEnc.size()))
    {
        return false;
    }
	std::cout << "+----";
	//std::cout << "\nm_amfEnc.data(): " << m_amfEnc.data();
	std::cout << "\nm_amfEnc.size(): " << m_amfEnc.size();
	std::cout << "\n+----";

    return true;
}

void RtmpConnection::setPeerBandwidth()
{
    std::shared_ptr<char> data(new char[5]);
    writeUint32BE(data.get(), m_peerBandwidth);
    data.get()[4] = 2;
    RtmpMessage rtmpMsg;
    rtmpMsg.typeId = RTMP_BANDWIDTH_SIZE;
    rtmpMsg.payload = data;
    rtmpMsg.length = 5;
    sendRtmpChunks(RTMP_CHUNK_CONTROL_ID, rtmpMsg);
}

void RtmpConnection::sendAcknowledgement()
{
    std::shared_ptr<char> data(new char[4]);
    writeUint32BE(data.get(), m_acknowledgementSize);

    RtmpMessage rtmpMsg;
    rtmpMsg.typeId = RTMP_ACK_SIZE;
    rtmpMsg.payload = data;
    rtmpMsg.length = 4;
    sendRtmpChunks(RTMP_CHUNK_CONTROL_ID, rtmpMsg);
}

void RtmpConnection::setChunkSize()
{
    m_outChunkSize = m_maxChunkSize;
    
    std::shared_ptr<char> data(new char[4]);
    writeUint32BE((char*)data.get(), m_outChunkSize);    

    RtmpMessage rtmpMsg;
    rtmpMsg.typeId = RTMP_SET_CHUNK_SIZE;
    rtmpMsg.payload = data;
    rtmpMsg.length = 4;
    sendRtmpChunks(RTMP_CHUNK_CONTROL_ID, rtmpMsg);
}

void RtmpConnection::setPlayCB(const PlayCallback& cb)
{
	m_playCB = cb;
}

bool RtmpConnection::sendInvokeMessage(uint32_t csid, std::shared_ptr<char> payload, uint32_t payloadSize)
{
    if(this->isClosed())
    {
        return false;
    }

    RtmpMessage rtmpMsg;
    rtmpMsg.typeId = RTMP_INVOKE;
    rtmpMsg.timestamp = 0;
    rtmpMsg.streamId = m_streamId;
    rtmpMsg.payload = payload;
    rtmpMsg.length = payloadSize; 
    sendRtmpChunks(csid, rtmpMsg);  
    return true;
}

bool RtmpConnection::sendNotifyMessage(uint32_t csid, std::shared_ptr<char> payload, uint32_t payloadSize)
{
    if(this->isClosed())
    {
        return false;
    }

    RtmpMessage rtmpMsg;
    rtmpMsg.typeId = RTMP_NOTIFY;
    rtmpMsg.timestamp = 0;
    rtmpMsg.streamId = m_streamId;
    rtmpMsg.payload = payload;
    rtmpMsg.length = payloadSize; 
    sendRtmpChunks(csid, rtmpMsg);  
    return true;
}

bool RtmpConnection::isKeyFrame(std::shared_ptr<char> payload, uint32_t payloadSize)
{
	uint8_t frameType = (payload.get()[0] >> 4) & 0x0f;
	uint8_t codecId = payload.get()[0] & 0x0f;
	return (frameType == 1 && codecId == RTMP_CODEC_ID_H264);
}

bool RtmpConnection::sendMediaData(uint8_t type, uint64_t timestamp, std::shared_ptr<char> payload, uint32_t payloadSize)
{
    if(this->isClosed())
    {
        return false;
    }

	if (payloadSize == 0)
	{
		return false;
	}

	m_isPlaying = true;

	if (type == RTMP_AVC_SEQUENCE_HEADER)
	{
		m_avcSequenceHeader = payload;
		m_avcSequenceHeaderSize = payloadSize;
	}
	else if (type == RTMP_AAC_SEQUENCE_HEADER)
	{
		m_aacSequenceHeader = payload;
		m_aacSequenceHeaderSize = payloadSize;
	}

	auto conn = std::dynamic_pointer_cast<RtmpConnection>(shared_from_this());
	m_taskScheduler->addTriggerEvent([conn, type, timestamp, payload, payloadSize] {
		if (!conn->m_hasKeyFrame && conn->m_avcSequenceHeaderSize > 0
			&& (type != RTMP_AVC_SEQUENCE_HEADER)
			&& (type != RTMP_AAC_SEQUENCE_HEADER))
		{
			if (conn->isKeyFrame(payload, payloadSize))
			{
				conn->m_hasKeyFrame = true;
			}
			else
			{
				return ;
			}
		}

		RtmpMessage rtmpMsg;
		rtmpMsg._timestamp = timestamp;
		rtmpMsg.streamId = conn->m_streamId;
		rtmpMsg.payload = payload;
		rtmpMsg.length = payloadSize;

		if (type == RTMP_VIDEO || type == RTMP_AVC_SEQUENCE_HEADER)
		{
			rtmpMsg.typeId = RTMP_VIDEO;
			conn->sendRtmpChunks(RTMP_CHUNK_VIDEO_ID, rtmpMsg);
		}
		else if (type == RTMP_AUDIO || type == RTMP_AAC_SEQUENCE_HEADER)
		{
			rtmpMsg.typeId = RTMP_AUDIO;
			conn->sendRtmpChunks(RTMP_CHUNK_AUDIO_ID, rtmpMsg);
		}
	});
   
    return true;
}

bool RtmpConnection::sendVideoData(uint64_t timestamp, std::shared_ptr<char> payload, uint32_t payloadSize)
{
	if (payloadSize == 0)
	{
		return false;
	}
	
	auto conn = std::dynamic_pointer_cast<RtmpConnection>(shared_from_this());
	m_taskScheduler->addTriggerEvent([conn, timestamp, payload, payloadSize] {
		RtmpMessage rtmpMsg;
		rtmpMsg.typeId = RTMP_VIDEO;
		rtmpMsg._timestamp = timestamp;
		rtmpMsg.streamId = conn->m_streamId;
		rtmpMsg.payload = payload;
		rtmpMsg.length = payloadSize;
		conn->sendRtmpChunks(RTMP_CHUNK_VIDEO_ID, rtmpMsg);
	});

	return true;
}

bool RtmpConnection::sendAudioData(uint64_t timestamp, std::shared_ptr<char> payload, uint32_t payloadSize)
{
	if (payloadSize == 0)
	{
		return false;
	}

	auto conn = std::dynamic_pointer_cast<RtmpConnection>(shared_from_this());
	m_taskScheduler->addTriggerEvent([conn, timestamp, payload, payloadSize] {
		RtmpMessage rtmpMsg;
		rtmpMsg.typeId = RTMP_AUDIO;
		rtmpMsg._timestamp = timestamp;
		rtmpMsg.streamId = conn->m_streamId;
		rtmpMsg.payload = payload;
		rtmpMsg.length = payloadSize;
		conn->sendRtmpChunks(RTMP_CHUNK_AUDIO_ID, rtmpMsg);
	});
	return true;
}

void RtmpConnection::sendRtmpChunks(uint32_t csid, RtmpMessage& rtmpMsg)
{    
    uint32_t bufferOffset = 0, payloadOffset = 0;
    uint32_t capacity = rtmpMsg.length + rtmpMsg.length/m_outChunkSize*5 + 1024; 
    std::shared_ptr<char> bufferPtr(new char[capacity]);
    char* buffer = bufferPtr.get();

    bufferOffset += this->createChunkBasicHeader(0, csid, buffer + bufferOffset); //first chunk
    bufferOffset += this->createChunkMessageHeader(0, rtmpMsg, buffer + bufferOffset);
    if(rtmpMsg._timestamp >= 0xffffff)
    {
        writeUint32BE((char*)buffer + bufferOffset, (uint32_t)rtmpMsg._timestamp);
        bufferOffset += 4;
    }
	int a = 0, p = rtmpMsg.length;
    while(rtmpMsg.length > 0)
    {
		a++;
        if(rtmpMsg.length > m_outChunkSize)
        {
            memcpy(buffer+bufferOffset, rtmpMsg.payload.get()+payloadOffset, m_outChunkSize);         
            payloadOffset += m_outChunkSize;
            bufferOffset += m_outChunkSize;
            rtmpMsg.length -= m_outChunkSize;
            
            bufferOffset += this->createChunkBasicHeader(3, csid, buffer + bufferOffset);
            if(rtmpMsg._timestamp >= 0xffffff)
            {
                writeUint32BE(buffer + bufferOffset, (uint32_t)rtmpMsg._timestamp);
                bufferOffset += 4;
            }
        }
        else
        {
            memcpy(buffer+bufferOffset, rtmpMsg.payload.get()+payloadOffset, rtmpMsg.length);            
            bufferOffset += rtmpMsg.length;
            rtmpMsg.length = 0;            
            break;
        }
    }

    this->send(bufferPtr, bufferOffset);
}

int RtmpConnection::createChunkBasicHeader(uint8_t fmt, uint32_t csid, char* buf)
{
    int len = 0;

    if (csid >= 64 + 255) 
    {
        buf[len++] = (fmt << 6) | 1;
        buf[len++] = (csid - 64) & 0xFF;
        buf[len++] = ((csid - 64) >> 8) & 0xFF;
    } 
    else if (csid >= 64) 
    {
        buf[len++] = (fmt << 6) | 0;
        buf[len++] = (csid - 64) & 0xFF;
    } 
    else 
    {
        buf[len++] = (fmt << 6) | csid;
    }
    return len;
}

int RtmpConnection::createChunkMessageHeader(uint8_t fmt, RtmpMessage& rtmpMsg, char* buf)
{   
    int len = 0;    
    if (fmt <= 2) 
    {
        if(rtmpMsg._timestamp < 0xffffff)
        {
           writeUint24BE((char*)buf, (uint32_t)rtmpMsg._timestamp);
        }
        else
        {
            writeUint24BE((char*)buf, 0xffffff);    
        }
        len += 3;
    }

    if (fmt <= 1) 
    {
        writeUint24BE((char*)buf + len, rtmpMsg.length);
        len += 3;
        buf[len++] = rtmpMsg.typeId;
    }

    if (fmt == 0) 
    {
        writeUint32LE((char*)buf + len, rtmpMsg.streamId);    
        len += 4;
    }

    return len;
}

