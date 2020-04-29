#include "RtmpServer.h"
#include "RtmpConnection.h"
#include "net/SocketUtil.h"
#include "net/Logger.h"

using namespace xop;

RtmpServer::RtmpServer(EventLoop* loop, std::string ip, uint16_t port)
	: TcpServer(loop, ip, port)
	, m_eventLoop(loop)
{
    if (this->start() != 0)
    {
        LOG_INFO("RTSP Server listening on %u failed.", port);
    }
    else
    {
        LOG_INFO("RTMP Server listen on %u.\n", port);
    }

	m_eventLoop->addTimer([this] {
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto iter = m_rtmpSessions.begin(); iter != m_rtmpSessions.end(); )
		{
			if (iter->second->getClients() == 0)
			{
				m_rtmpSessions.erase(iter++);
			}
			else
			{
				iter++;
			}
		}
		return true;
	}, 10000); // 30000
}

RtmpServer::~RtmpServer()
{

}

TcpConnection::Ptr RtmpServer::newConnection(SOCKET sockfd)
{
    return std::make_shared<RtmpConnection>((RtmpServer*)this, _eventLoop->getTaskScheduler().get(), sockfd);
}

void RtmpServer::addSession(std::string streamPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_rtmpSessions.find(streamPath) == m_rtmpSessions.end())
    {
        m_rtmpSessions[streamPath] = std::make_shared<RtmpSession>();
    }
}

void RtmpServer::removeSession(std::string streamPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rtmpSessions.erase(streamPath);
}

bool RtmpServer::hasSession(std::string streamPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_rtmpSessions.find(streamPath) != m_rtmpSessions.end());
}

RtmpSession::Ptr RtmpServer::getSession(std::string streamPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_rtmpSessions.find(streamPath) == m_rtmpSessions.end())
    {
        m_rtmpSessions[streamPath] = std::make_shared<RtmpSession>();
    }

    return m_rtmpSessions[streamPath];
}

bool RtmpServer::hasPublisher(std::string streamPath)
{
    auto sessionPtr = this->getSession(streamPath);
    if(sessionPtr == nullptr)
    {
       return false;
    }

    return (sessionPtr->getPublisher()!=nullptr);
}


