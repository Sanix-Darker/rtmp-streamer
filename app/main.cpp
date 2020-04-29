#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "xop/RtmpServer.h"
#include "xop/HttpFlvServer.h"
#include "net/TcpServer.h"
#include "net/EventLoop.h"
#include "net/Logger.h"

#define TEST_MULTI_THREAD 1

int main(int argc, char **argv)
{

    LOG_INFO("\n\n");
    LOG_INFO("[+] ------------------------------------------");
    LOG_INFO("[+] RTMP Server started successfully !");
    LOG_INFO("[+] ------------------------------------------");

	int count = 1;
#if TEST_MULTI_THREAD
	count = std::thread::hardware_concurrency();
#endif
	xop::EventLoop eventLoop(count);

	/* rtmp server example */
	// rtmp://127.0.0.1:1935/live/zinzin
	xop::RtmpServer rtmpServer(&eventLoop, "0.0.0.0", 1935);
	rtmpServer.setChunkSize(60000);
	//rtmpServer.setGopCache(); /* enable gop cache */

	/* http-flv server example */
    // http://127.0.0.1:8080/live/zinzin.flv
	xop::HttpFlvServer httpFlvServer(&eventLoop, "0.0.0.0", 8080);
	httpFlvServer.attach(&rtmpServer);


	/* socket server */
	//
	// xop::TcpServer tcpServer(&eventLoop, "0.0.0.0", 70);
	// tcpServer.run();

	while (1)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	return 0;
}
